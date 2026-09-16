// examples/decision/main.cc · 采纳 / 确认裁决示例（SCD-DECIDE-01..05 + SCD-PICK-03）
//
// 演示：
//   ① 强制选择**非推荐**方案 → deviated=true 与推荐 id（SCD-PICK-03）；
//   ② 同侧唯一：采纳第二个方案后第一个不再 adopted（SCD-DECIDE-01）；
//   ③ 幂等：连点两次不报错，第二次 code=0 + idempotent=true（SCD-DECIDE-02 / C15 / C17）；
//   ④ 三态可查（未采纳 / 已采纳 / 已确认 —— SCD-DECIDE-05）；
//   ⑤ 事件负载 = `plan.state` 的既有四字段 + 只增可选字段（SCD-DECIDE-04 / CTR-EV-04）。
//
// 用法：example_decision [规则包目录]   退出码：0 / 1
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../common.h"

namespace {

/// 宿主侧的 IPlanStore / IPlanSink / IClock / ILogSink 测试替身（引擎只认识反向接口）
class MemStore : public scoring::IPlanStore {
public:
    bool save(const scoring::PlanState& st) override {
        rows_[st.missionId][st.planId] = st;
        return true;
    }
    bool load(const std::string& missionId, const std::string& planId,
              scoring::PlanState& out) override {
        const auto m = rows_.find(missionId);
        if (m == rows_.end()) return false;
        const auto p = m->second.find(planId);
        if (p == m->second.end()) return false;
        out = p->second;
        return true;
    }
    std::vector<scoring::PlanState> listBySide(const std::string& missionId,
                                               const std::string& side) override {
        std::vector<scoring::PlanState> out;
        const auto m = rows_.find(missionId);
        if (m == rows_.end()) return out;
        for (const auto& kv : m->second) {
            if (kv.second.side == side) out.push_back(kv.second);
        }
        return out;
    }
    bool supportsList() const override { return true; }
    std::vector<scoring::PlanState> list(const scoring::PlanQuery& q) override {
        std::vector<scoring::PlanState> out;
        for (const auto& m : rows_) {
            if (!q.missionId.empty() && m.first != q.missionId) continue;
            for (const auto& kv : m.second) {
                if (!q.side.empty() && kv.second.side != q.side) continue;
                bool hit = q.stateIn.empty();
                for (const auto& s : q.stateIn) {
                    if (kv.second.state == s) hit = true;
                }
                if (hit) out.push_back(kv.second);
            }
        }
        return out;
    }

private:
    std::map<std::string, std::map<std::string, scoring::PlanState>> rows_;
};

class RecSink : public scoring::IPlanSink {
public:
    std::vector<scoring::json> events;
    void onPlanStateChanged(const scoring::json& e) override { events.push_back(e); }
};

class FakeClock : public scoring::IClock {
public:
    int64_t nowMs() const override { return 1750000000000LL; }
};

class RecLog : public scoring::ILogSink {
public:
    int logs = 0;
    std::vector<scoring::json> audits;
    void log(int, const std::string&, const scoring::json&) override { ++logs; }
    void commandAudit(const scoring::json& e) override { audits.push_back(e); }
};

}  // namespace

int main(int argc, char** argv) {
    auto store = std::make_shared<MemStore>();
    auto sink = std::make_shared<RecSink>();
    auto clock = std::make_shared<FakeClock>();
    auto log = std::make_shared<RecLog>();

    scoring::ScoringEngineOptions opts;
    opts.store = store;
    opts.sink = sink;
    opts.clock = clock;
    opts.log = log;
    scoring::ScoringEngine engine(opts);
    if (ex::loadPolicies(engine, ex::policyDir(argc, argv)) != 0) return 1;

    const scoring::TemplatesPack pack = engine.templatesPack();
    if (pack.items.size() < 2) return ex::fail("模板不足两个，无法演示同侧互斥");
    const std::string side = pack.items.front().side;
    const scoring::PhaseContext phase = ex::phaseOf(pack, side, "phase-demo");

    std::vector<std::string> keys = ex::keysOfSide(pack, side);
    if (keys.size() < 2) return ex::fail("该侧模板不足两个");
    const std::string recommended = keys[0];
    const std::string other = keys[1];

    // ---- ① 强制选择非推荐方案 ----
    scoring::AdoptRequest ar;
    ar.missionId = phase.missionId;
    ar.phase = phase;
    ar.planId = other;
    ar.side = side;
    ar.operatorId = "operator-demo";
    ar.reason = "manual-override";
    ar.hasRecommendedContext = true;
    ar.recommendedId = recommended;
    const scoring::DecideResult d1 = engine.adopt(ar);
    std::printf("①  采纳非推荐 %s：code=%d deviated=%s recommendedId=%s\n", other.c_str(), d1.code,
                d1.deviated ? "true" : "false", d1.recommendedId.c_str());
    if (d1.code != 0 || !d1.deviated || d1.recommendedId != recommended) {
        return ex::fail("强制选择非推荐方案未按 SCD-PICK-03 标注");
    }
    const scoring::json ev = d1.event;
    std::printf("    plan.state 负载：%s\n", ev.dump().c_str());
    for (const char* k : {"missionId", "side", "planId", "action"}) {
        if (!ev.contains(k)) return ex::fail("plan.state 负载缺既有字段");
    }

    // ---- ② 幂等：重复采纳同一方案 ----
    const scoring::DecideResult d2 = engine.adopt(ar);
    std::printf("②  重复采纳：code=%d idempotent=%s（MUST NOT 报错）\n", d2.code,
                d2.idempotent ? "true" : "false");
    if (d2.code != 0 || !d2.idempotent) return ex::fail("幂等成功未走 code=0 + idempotent");

    // ---- ③ 同侧唯一：改采纳推荐方案，前一个不再 adopted ----
    ar.planId = recommended;
    const scoring::DecideResult d3 = engine.adopt(ar);
    std::printf("③  改采纳 %s：code=%d invalidated=%zu", recommended.c_str(), d3.code,
                d3.invalidated.size());
    for (const auto& x : d3.invalidated) std::printf(" %s", x.c_str());
    std::printf("\n");
    if (d3.code != 0) return ex::fail("采纳失败");

    // ---- ④ 确认 + 确认幂等 + 三态查询 ----
    scoring::ConfirmRequest cr;
    cr.missionId = phase.missionId;
    cr.phase = phase;
    cr.planId = recommended;
    cr.side = side;
    cr.operatorId = "operator-demo";
    const scoring::DecideResult c1 = engine.confirm(cr);
    const scoring::DecideResult c2 = engine.confirm(cr);
    std::printf("④  确认：code=%d state=%s｜重复确认：code=%d idempotent=%s\n", c1.code,
                c1.planState.c_str(), c2.code, c2.idempotent ? "true" : "false");
    if (c1.code != 0 || c2.code != 0 || !c2.idempotent) return ex::fail("确认或确认幂等不成立");

    scoring::PlanQuery q;
    q.missionId = phase.missionId;
    q.side = side;
    const std::vector<scoring::PlanState> states = engine.plans(q);
    std::printf("⑤  三态查询 %zu 条：", states.size());
    for (const auto& s : states) std::printf("%s=%s ", s.planId.c_str(), s.state.c_str());
    std::printf("\n");

    std::printf("    事件 %zu 条｜日志 %d 条｜审计 %zu 条\n", sink->events.size(), log->logs,
                log->audits.size());
    if (sink->events.empty() || log->audits.empty()) return ex::fail("事件或事件日志缺失");

    const scoring::Metrics m = engine.metrics();
    std::printf("    计数：采纳 %lld｜确认 %lld｜幂等命中 %lld｜冲突 %lld｜被他方取代 %lld\n",
                static_cast<long long>(m.adopts), static_cast<long long>(m.confirms),
                static_cast<long long>(m.idempotentHits), static_cast<long long>(m.conflicts),
                static_cast<long long>(m.invalidated));

    std::printf("\n裁决示例通过（退出码 0）\n");
    return 0;
}
