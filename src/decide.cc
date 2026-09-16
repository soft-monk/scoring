// src/decide.cc · scoring —— 采纳与确认的状态语义裁决（SCD-DECIDE-01..05）
//
// 权威依据：
//   SCD-DECIDE-01 同侧唯一（互斥；后来者使前者失效 —— 语义由引擎裁决，落库由宿主执行）
//   SCD-DECIDE-02 幂等：重复采纳/确认 MUST 成功并标 idempotent，MUST NOT 报错
//   SCD-DECIDE-03 确认前置校验（策略由规则包声明：拒绝 or 自动采纳）
//   SCD-DECIDE-04 产出 `plan.state` 事件 + 事件日志（既有负载字段只增不改 —— CTR-EV-04）
//   SCD-DECIDE-05 待确认态可查询（未采纳 / 已采纳 / 已确认）
//   SCD-PICK-03   强制选择非推荐方案允许，结果标 deviated + 推荐 id
//   protocol §3.3 CTR-EC-01/02/03 + 冲突裁决 C15/C16/C17：
//     幂等成功 = code 0 + data.idempotent=true；冲突拒绝 = 1002 + conflict=true；前置未满足 = 1003
//     —— MUST NOT 为幂等成功发明非零码。
#include <algorithm>

#include "internal.h"

namespace scoring {
namespace detail {

// ---------------------------------------------------------------- 进程内状态缓存

void PlanStateCache::put(const PlanState& st) {
    if (st.missionId.empty() || st.side.empty() || st.planId.empty()) return;
    byMission[st.missionId][st.side][st.planId] = st;
}

std::optional<PlanState> PlanStateCache::get(const std::string& missionId,
                                             const std::string& planId) const {
    const auto m = byMission.find(missionId);
    if (m == byMission.end()) return std::nullopt;
    for (const auto& side : m->second) {
        const auto p = side.second.find(planId);
        if (p != side.second.end()) return p->second;
    }
    return std::nullopt;
}

std::vector<PlanState> PlanStateCache::listSide(const std::string& missionId,
                                                const std::string& side) const {
    std::vector<PlanState> out;
    const auto m = byMission.find(missionId);
    if (m == byMission.end()) return out;
    const auto s = m->second.find(side);
    if (s == m->second.end()) return out;
    for (const auto& kv : s->second) out.push_back(kv.second);
    return out;
}

std::vector<PlanState> PlanStateCache::list(const PlanQuery& q) const {
    std::vector<PlanState> out;
    auto stateOk = [&](const std::string& st) {
        if (q.stateIn.empty()) return true;
        return std::find(q.stateIn.begin(), q.stateIn.end(), st) != q.stateIn.end();
    };
    for (const auto& m : byMission) {
        if (!q.missionId.empty() && m.first != q.missionId) continue;
        for (const auto& side : m.second) {
            if (!q.side.empty() && side.first != q.side) continue;
            for (const auto& kv : side.second) {
                if (stateOk(kv.second.state)) out.push_back(kv.second);
            }
        }
    }
    return out;
}

std::vector<std::string> PlanStateCache::clearSide(const std::string& missionId,
                                                   const std::string& side,
                                                   const std::string& exceptPlanId) {
    std::vector<std::string> cleared;
    auto m = byMission.find(missionId);
    if (m == byMission.end()) return cleared;
    auto s = m->second.find(side);
    if (s == m->second.end()) return cleared;
    for (auto& kv : s->second) {
        if (kv.first == exceptPlanId) continue;
        kv.second.state = "pending";
        cleared.push_back(kv.first);
    }
    return cleared;
}

// ---------------------------------------------------------------- 同侧互斥闸门

SideGate::SideGate(std::recursive_mutex& mu, std::map<std::string, int>& inFlight,
                   const std::string& key, Metrics& counters, bool take)
    : mu_(mu), inFlight_(inFlight), key_(key), counters_(counters) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    const auto it = inFlight_.find(key_);
    if (it != inFlight_.end() && it->second > 0) {
        // 同侧已被持有：**立刻返回**，MUST NOT 阻塞等待（CTR-EC-03 / C16）
        ++counters_.conflicts;
        return;
    }
    acquired_ = true;
    if (take) {
        ++inFlight_[key_];
        owned_ = true;
    }
}

SideGate::~SideGate() {
    if (!owned_) return;
    std::lock_guard<std::recursive_mutex> lock(mu_);
    const auto it = inFlight_.find(key_);
    if (it == inFlight_.end()) return;
    if (--it->second <= 0) inFlight_.erase(it);
}

namespace {

/// `plan.state` 事件的 data（既有四字段逐字保留 + 只增可选字段 —— CTR-EV-04）
json planStateEvent(const std::string& missionId, const std::string& side,
                    const std::string& planId, const std::string& action, const std::string& state,
                    bool recommendedSet, const std::string& recommendedId, bool deviated,
                    bool idempotent, int64_t ts) {
    json e = json::object();
    e["missionId"] = missionId;
    e["side"] = side;
    e["planId"] = planId;
    e["action"] = action;  // adopted / optimized / confirmed（protocol §4.3 取值表）
    e["planState"] = state;
    e["deviated"] = deviated;
    e["idempotent"] = idempotent;
    if (recommendedSet) e["recommendedId"] = recommendedId;
    e["ts"] = ts;
    return e;
}

}  // namespace

// ---------------------------------------------------------------- 采纳

DecideResult runAdopt(ScoringEngine::Impl& impl, const AdoptRequest& req, bool autoConfirm) {
    DecideResult r;
    r.missionId = req.missionId;
    r.planId = req.planId;
    r.ts = impl.nowMs();

    if (req.missionId.empty() || req.planId.empty()) {
        r.code = static_cast<int>(ErrorCode::BadRequest);
        r.message = "missionId and planId are required";
        r.status = ResultStatus::Rejected;
        return r;
    }
    if (!impl.templatesLoaded) {
        r.code = static_cast<int>(ErrorCode::Internal);
        r.message = "policies not loaded";
        r.status = ResultStatus::Rejected;
        return r;
    }
    const auto pit = impl.templates.index.find(req.planId);
    if (pit == impl.templates.index.end()) {
        r.code = static_cast<int>(ErrorCode::NotFound);
        r.message = "plan template not found";
        r.status = ResultStatus::Rejected;
        return r;
    }
    const PlanTemplate& tmpl = impl.templates.items[static_cast<std::size_t>(pit->second)];
    r.side = req.side.empty() ? tmpl.side : req.side;

    // 注意：**同侧互斥闸门由调用方（ScoringEngine::confirm）持有**，这里只做裁决。

    // 当前状态：store 为权威，缓存兜底（SCD-DECIDE-05 的三态）
    PlanState cur;
    cur.missionId = req.missionId;
    cur.planId = req.planId;
    cur.side = r.side;
    cur.state = "pending";
    bool known = false;
    if (impl.store) {
        PlanState loaded;
        if (impl.store->load(req.missionId, req.planId, loaded)) {
            cur = loaded;
            cur.missionId = req.missionId;
            known = true;
        }
    }
    if (!known) {
        if (const auto c = impl.cache.get(req.missionId, req.planId)) {
            cur = *c;
            known = true;
        }
    }

    // 偏离推荐标注（SCD-PICK-03）
    const bool deviated = req.hasRecommendedContext && !req.recommendedId.empty() &&
                          req.recommendedId != req.planId;

    // 幂等命中：已是 adopted → code=0 + idempotent=true，**零副作用**（C15/C17）
    if (known && cur.state == "adopted") {
        r.code = 0;
        r.message = "already adopted";
        r.status = ResultStatus::AlreadyApplied;
        r.idempotent = true;
        r.action = "adopted";
        r.planState = "adopted";
        r.deviated = deviated;
        r.hasRecommendedId = req.hasRecommendedContext && !req.recommendedId.empty();
        r.recommendedId = req.recommendedId;
        r.event = planStateEvent(req.missionId, r.side, req.planId, "adopted", "adopted",
                                 r.hasRecommendedId, r.recommendedId, deviated, true, r.ts);
        ++impl.counters.idempotentHits;
        return r;
    }
    // 已确认 → 采纳属重复提交（幂等：状态不变，仍成功 —— SCD-DECIDE-02）
    if (known && cur.state == "confirmed") {
        r.code = 0;
        r.message = "already confirmed";
        r.status = ResultStatus::AlreadyApplied;
        r.idempotent = true;
        r.action = "adopted";
        r.planState = "confirmed";
        r.deviated = deviated;
        r.hasRecommendedId = req.hasRecommendedContext && !req.recommendedId.empty();
        r.recommendedId = req.recommendedId;
        r.event = planStateEvent(req.missionId, r.side, req.planId, "adopted", "confirmed",
                                 r.hasRecommendedId, r.recommendedId, deviated, true, r.ts);
        ++impl.counters.idempotentHits;
        return r;
    }

    // ---- 同侧唯一：先把同侧其它方案置回 pending（SCD-DECIDE-01）----
    std::vector<std::string> invalidated = impl.cache.clearSide(req.missionId, r.side, req.planId);
    if (impl.store) {
        std::vector<PlanState> side = impl.store->listBySide(req.missionId, r.side);
        for (const auto& s : side) {
            if (s.planId == req.planId) continue;
            if (s.state == "pending") continue;
            PlanState demoted = s;
            demoted.missionId = req.missionId;
            demoted.state = "pending";
            demoted.updatedAt = r.ts;
            if (!impl.store->save(demoted)) {
                ++impl.counters.storeErrors;
                r.code = static_cast<int>(ErrorCode::Internal);
                r.message = "store save failed";
                r.status = ResultStatus::Rejected;
                return r;
            }
            if (std::find(invalidated.begin(), invalidated.end(), s.planId) == invalidated.end()) {
                invalidated.push_back(s.planId);
            }
            impl.cache.put(demoted);
        }
    }

    // ---- 落库（引擎裁决，宿主执行 —— D7 / SCD-NFR-03）----
    PlanState next;
    next.missionId = req.missionId;
    next.planId = req.planId;
    next.side = r.side;
    next.state = autoConfirm ? "confirmed" : "adopted";
    next.updatedAt = r.ts;
    if (impl.store && !impl.store->save(next)) {
        ++impl.counters.storeErrors;
        r.code = static_cast<int>(ErrorCode::Internal);
        r.message = "store save failed";
        r.status = ResultStatus::Rejected;
        return r;
    }
    impl.cache.put(next);

    r.code = 0;
    r.message = "ok";
    r.status = ResultStatus::Ok;
    r.autoAdopted = autoConfirm;
    r.action = autoConfirm ? "confirmed" : "adopted";
    r.planState = next.state;
    r.deviated = deviated;
    r.hasRecommendedId = req.hasRecommendedContext && !req.recommendedId.empty();
    r.recommendedId = req.recommendedId;
    r.invalidated = invalidated;
    impl.counters.invalidated += static_cast<int64_t>(invalidated.size());
    r.event = planStateEvent(req.missionId, r.side, req.planId, r.action, next.state,
                             r.hasRecommendedId, r.recommendedId, deviated, false, r.ts);
    return r;
}

// ---------------------------------------------------------------- 确认

DecideResult runConfirm(ScoringEngine::Impl& impl, const ConfirmRequest& req) {
    DecideResult r;
    r.missionId = req.missionId;
    r.planId = req.planId;
    r.action = "confirmed";
    r.ts = impl.nowMs();

    if (req.missionId.empty() || req.planId.empty()) {
        r.code = static_cast<int>(ErrorCode::BadRequest);
        r.message = "missionId and planId are required";
        r.status = ResultStatus::Rejected;
        return r;
    }
    if (!impl.templatesLoaded) {
        r.code = static_cast<int>(ErrorCode::Internal);
        r.message = "policies not loaded";
        r.status = ResultStatus::Rejected;
        return r;
    }
    const auto pit = impl.templates.index.find(req.planId);
    if (pit == impl.templates.index.end()) {
        r.code = static_cast<int>(ErrorCode::NotFound);
        r.message = "plan template not found";
        r.status = ResultStatus::Rejected;
        return r;
    }
    const PlanTemplate& tmpl = impl.templates.items[static_cast<std::size_t>(pit->second)];
    r.side = req.side.empty() ? tmpl.side : req.side;

    // 注意：**同侧互斥闸门由调用方（ScoringEngine::confirm）持有**，这里只做裁决。

    PlanState cur;
    cur.missionId = req.missionId;
    cur.planId = req.planId;
    cur.side = r.side;
    cur.state = "pending";
    bool known = false;
    if (impl.store) {
        PlanState loaded;
        if (impl.store->load(req.missionId, req.planId, loaded)) {
            cur = loaded;
            cur.missionId = req.missionId;
            known = true;
        }
    }
    if (!known) {
        if (const auto c = impl.cache.get(req.missionId, req.planId)) {
            cur = *c;
            known = true;
        }
    }

    // 幂等命中：已确认 → code=0 + idempotent=true（C15/C17；**不得报错** —— 否则前端流程卡住）
    if (known && cur.state == "confirmed") {
        r.code = 0;
        r.message = "already confirmed";
        r.status = ResultStatus::AlreadyApplied;
        r.idempotent = true;
        r.planState = "confirmed";
        r.event = planStateEvent(req.missionId, r.side, req.planId, "confirmed", "confirmed", false,
                                 "", false, true, r.ts);
        ++impl.counters.idempotentHits;
        return r;
    }

    // ---- 确认前置校验（SCD-DECIDE-03：策略由规则包声明）----
    if (!known || cur.state != "adopted") {
        if (impl.templates.confirmMode == ConfirmPrecondition::AutoAdopt) {
            AdoptRequest areq;
            areq.missionId = req.missionId;
            areq.phase = req.phase;
            areq.planId = req.planId;
            areq.side = r.side;
            areq.operatorId = req.operatorId;
            areq.reason = req.reason;
            DecideResult adopted = runAdopt(impl, areq, true);
            if (adopted.code != 0) return adopted;
            adopted.autoAdopted = true;
            ++impl.counters.autoAdopted;
            return adopted;
        }
        r.code = static_cast<int>(ErrorCode::PreconditionUnmet);
        r.message = "plan is not adopted";
        r.status = ResultStatus::Rejected;
        r.planState = cur.state;
        r.unmet.push_back("not-adopted");
        return r;
    }

    PlanState next;
    next.missionId = req.missionId;
    next.planId = req.planId;
    next.side = r.side;
    next.state = "confirmed";
    next.updatedAt = r.ts;
    if (impl.store && !impl.store->save(next)) {
        ++impl.counters.storeErrors;
        r.code = static_cast<int>(ErrorCode::Internal);
        r.message = "store save failed";
        r.status = ResultStatus::Rejected;
        return r;
    }
    impl.cache.put(next);
    r.code = 0;
    r.message = "ok";
    r.status = ResultStatus::Ok;
    r.planState = "confirmed";
    r.event = planStateEvent(req.missionId, r.side, req.planId, "confirmed", "confirmed", false, "",
                             false, false, r.ts);
    return r;
}

}  // namespace detail
}  // namespace scoring
