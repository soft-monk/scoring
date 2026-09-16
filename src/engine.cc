// src/engine.cc · scoring —— 公开入口实现 + JSON 序列化
//
// 权威依据：include/scoring/scoring.h（唯一公开头）与需求专篇 SCD-*（34 条）。
// 三条纪律在本文件体现：
//   · 一切业务取值来自规则包（本文件只搬字段，不判断业务含义 —— P6/P7）
//   · 出口全走注入的反向接口（IPlanStore / IPlanSink / IClock / ILogSink —— P8/P9）
//   · 失败 MUST NOT 抛异常跨边界；统一信封 {code,message,data}（P10 / protocol §3.1）
#include <chrono>
#include <cstdio>
#include <fstream>

#include "internal.h"

namespace scoring {

namespace detail {

static std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    const char last = dir[dir.size() - 1];
    if (last == '/' || last == '\\') return dir + name;
    return dir + "/" + name;
}

}  // namespace detail

// ============================================================================
// JSON 序列化（camelCase；键序 = 成员声明顺序 —— 确定性，SCD-SCORE-06）
// ============================================================================

namespace {

/// **对外标度**：所有对外数值先 half-up 到两位小数，保证同一输入在任何平台输出相同
/// （确定性 —— SCD-SCORE-06 / SCD-NFR-05）。`contributions` 一律由定点整数换算。
constexpr int64_t kOut = detail::kExportScale;

json scaledNum(double v) {
    const int64_t fixed = detail::scaled(v, kOut);
    return static_cast<double>(fixed) / static_cast<double>(kOut);
}

}  // namespace

json LoadIssue::toJson() const {
    return json{{"path", path}, {"field", field}, {"reason", reason}};
}

json LoadResult::toJson() const {
    json d = json::object();
    d["itemCount"] = itemCount;
    d["policiesNamespace"] = policiesNamespace;
    d["schemaVersion"] = schemaVersion;
    d["digest"] = digest;
    d["definitionVersion"] = definitionVersion;
    d["warnings"] = warnings;
    json issueArr = json::array();
    for (const auto& i : issues) issueArr.push_back(i.toJson());
    d["issues"] = issueArr;
    return json{{"code", code}, {"message", message}, {"data", d}};
}

json InapplicableItem::toJson() const {
    json o = json::object();
    o["code"] = code;
    o["field"] = field;
    if (!subject.empty()) o["subject"] = subject;
    if (hasNumbers) {
        o["required"] = required;
        o["actual"] = actual;
    }
    return o;
}

json DuplicateOf::toJson() const {
    return json{{"key", key}, {"name", name}, {"into", into}};
}

json Candidate::toJson() const {
    json o = json::object();
    o["id"] = id;
    o["key"] = key;
    o["side"] = side;
    o["scene"] = scene;
    o["seq"] = seq;
    o["name"] = name;
    o["method"] = method;
    o["clusters"] = clusters;
    o["effect"] = effect;
    o["profileKey"] = profileKey;
    if (hasSuccessRate) o["successRate"] = successRate;
    o["recommendedHint"] = recommendedHint;
    o["applicable"] = applicable;
    json reasons = json::array();
    for (const auto& r : inapplicableReasons) reasons.push_back(r.toJson());
    o["inapplicableReasons"] = reasons;
    json dups = json::array();
    for (const auto& d : duplicates) dups.push_back(d.toJson());
    o["duplicates"] = dups;
    o["source"] = source;
    return o;
}

json MetricScore::toJson() const {
    json o = json::object();
    o["key"] = key;
    o["name"] = name;
    o["unit"] = unit;
    o["direction"] = direction;
    o["normalizeType"] = normalizeType;
    o["source"] = sourceName;
    o["sourceField"] = sourceField;
    o["raw"] = scaledNum(raw);
    o["normalized"] = scaledNum(normalized);
    o["weight"] = scaledNum(weight);
    o["contribution"] = detail::roundedContribution(contributionScaled);
    o["contributionScaled"] = contributionScaled;
    o["contributionScale"] = kScale;
    o["usedBaseline"] = usedBaseline;
    o["missing"] = missing;
    if (!missingMarker.empty()) o["missingMarker"] = missingMarker;
    return o;
}

json ReasonItem::toJson() const {
    json o = json::object();
    o["type"] = type;
    o["metricKey"] = metricKey;
    if (!refKey.empty()) o["refKey"] = refKey;
    // valueScaled 是内部定点（scale = kScale）；对外按两位小数输出
    o["value"] = detail::roundedContribution(valueScaled);
    o["valueScaled"] = valueScaled;
    return o;
}

json CandidateScore::toJson() const {
    json o = json::object();
    o["id"] = candidate.id;
    o["name"] = candidate.name;
    o["side"] = candidate.side;
    o["scene"] = candidate.scene;
    o["seq"] = candidate.seq;
    o["profileKey"] = candidate.profileKey;
    if (candidate.hasSuccessRate) o["successRate"] = candidate.successRate;
    o["applicable"] = applicable;
    o["scored"] = scored;
    o["total"] = totalPercent;  // **对外整数百分比**（D6）
    o["totalScaled"] = totalScaled;
    o["rank"] = rank;
    o["recommended"] = recommended;
    o["leadOverNext"] = scaledNum(leadOverNext);
    o["leadOverNextPercent"] = leadOverNextPercent;
    json ms = json::array();
    for (const auto& m : metrics) ms.push_back(m.toJson());
    o["metrics"] = ms;
    return o;
}

json ScoreResult::dataJson() const {
    json d = json::object();
    d["missionId"] = missionId;
    d["side"] = side;
    d["scene"] = scene;
    if (hasRecommended) {
        d["recommendedId"] = recommendedId;
        d["recommendedPercent"] = recommendedPercent;
    }
    if (hasNext) {
        d["nextId"] = nextId;
        d["nextPercent"] = nextPercent;
    }
    d["leadOverNext"] = scaledNum(leadOverNext);
    d["leadOverNextPercent"] = leadOverNextPercent;
    json cands = json::array();
    for (const auto& c : candidates) cands.push_back(c.toJson());
    d["candidates"] = cands;
    json rs = json::array();
    for (const auto& x : reasons) rs.push_back(x.toJson());
    d["reasons"] = rs;  // **结构化条目**（SCD-EXP-02：MUST NOT 成句自然语言）
    d["missingInputs"] = missingInputs;
    d["auditDigest"] = digestHex(auditDigest);
    d["metricsDigest"] = metricsDigest;
    d["templatesDigest"] = templatesDigest;
    d["ts"] = ts;
    return d;
}

json ScoreResult::toJson() const { return json{{"code", code}, {"message", message}, {"data", dataJson()}}; }

json ScoreResult::toAuditJson(bool withMetrics) const {
    json output = json::object();
    output["missionId"] = missionId;
    output["side"] = side;
    output["scene"] = scene;
    output["recommendedId"] = hasRecommended ? json(recommendedId) : json(nullptr);
    output["recommendedPercent"] = recommendedPercent;
    output["nextId"] = hasNext ? json(nextId) : json(nullptr);
    output["nextPercent"] = nextPercent;
    output["leadOverNext"] = scaledNum(leadOverNext);
    output["missingInputs"] = missingInputs;
    output["metricsDigest"] = metricsDigest;
    output["templatesDigest"] = templatesDigest;
    json cands = json::array();
    for (const auto& c : candidates) {
        json co = json::object();
        co["id"] = c.candidate.id;
        co["applicable"] = c.applicable;
        co["scored"] = c.scored;
        co["total"] = c.totalPercent;
        co["totalScaled"] = c.totalScaled;
        co["rank"] = c.rank;
        co["recommended"] = c.recommended;
        if (withMetrics) {
            json ms = json::array();
            for (const auto& m : c.metrics) ms.push_back(m.toJson());
            co["metrics"] = ms;
        }
        cands.push_back(co);
    }
    output["candidates"] = cands;
    json rs = json::array();
    for (const auto& x : reasons) rs.push_back(x.toJson());
    output["reasons"] = rs;
    return json{{"input", auditInput}, {"weights", auditWeights}, {"output", output}};
}

std::optional<CandidateScore> ScoreResult::byId(const std::string& id) const {
    for (const auto& c : candidates) {
        if (c.candidate.id == id) return c;
    }
    return std::nullopt;
}

json ParamChange::toJson() const {
    return json{{"key", key},           {"metricKey", metricKey}, {"before", scaledNum(before)},
                {"after", scaledNum(after)}, {"delta", scaledNum(delta)}, {"steps", steps}};
}

json MetricChange::toJson() const {
    return json{{"key", key},
                {"rawBefore", scaledNum(rawBefore)},
                {"rawAfter", scaledNum(rawAfter)},
                {"normalizedBefore", scaledNum(normalizedBefore)},
                {"normalizedAfter", scaledNum(normalizedAfter)},
                {"contributionBefore", scaledNum(contributionBefore)},
                {"contributionAfter", scaledNum(contributionAfter)}};
}

json OptimizeResult::toJson() const {
    json d = json::object();
    d["status"] = status;
    d["planId"] = planId;
    d["improved"] = improved;
    d["truncated"] = truncated;
    d["stopReason"] = stopReason;
    d["iterations"] = iterations;
    d["elapsedMs"] = elapsedMs;
    d["beforePercent"] = beforePercent;
    d["afterPercent"] = afterPercent;
    d["delta"] = delta;
    json pcs = json::array();
    for (const auto& p : paramChanges) pcs.push_back(p.toJson());
    d["paramChanges"] = pcs;
    json mcs = json::array();
    for (const auto& m : metricChanges) mcs.push_back(m.toJson());
    d["metricChanges"] = mcs;
    json bms = json::array();
    for (const auto& m : beforeMetrics) bms.push_back(m.toJson());
    d["beforeMetrics"] = bms;
    json ams = json::array();
    for (const auto& m : afterMetrics) ams.push_back(m.toJson());
    d["afterMetrics"] = ams;
    d["unmetReasons"] = unmetReasons;
    d["ts"] = ts;
    return json{{"code", code}, {"message", message}, {"data", d}};
}

json PlanState::toJson() const {
    json o = json::object();
    o["missionId"] = missionId;
    o["planId"] = planId;
    o["side"] = side;
    o["state"] = state;
    o["updatedAt"] = updatedAt;
    return o;
}

json DecideResult::dataJson() const {
    json d = json::object();
    d["status"] = toString(status);
    d["missionId"] = missionId;
    d["planId"] = planId;
    d["side"] = side;
    d["action"] = action;
    d["planState"] = planState;
    d["idempotent"] = idempotent;
    d["conflict"] = conflict;
    d["deviated"] = deviated;
    if (hasRecommendedId) d["recommendedId"] = recommendedId;
    d["autoAdopted"] = autoAdopted;
    d["invalidated"] = invalidated;
    d["unmet"] = unmet;
    d["event"] = event;
    d["ts"] = ts;
    return d;
}

json DecideResult::toJson() const {
    return json{{"code", code}, {"message", message}, {"data", dataJson()}};
}

json Capabilities::toJson() const {
    json o = json::object();
    o["metricsLoaded"] = metricsLoaded;
    o["templatesLoaded"] = templatesLoaded;
    o["policiesNamespace"] = policiesNamespace;
    o["metricsSchemaVersion"] = metricsSchemaVersion;
    o["templatesSchemaVersion"] = templatesSchemaVersion;
    o["policiesMajor"] = policiesMajor;
    o["metricCount"] = metricCount;
    o["templateCount"] = templateCount;
    o["profileCount"] = profileCount;
    o["searchParamCount"] = searchParamCount;
    o["maxIterations"] = maxIterations;
    o["timeBudgetMs"] = timeBudgetMs;
    o["reasonCount"] = reasonCount;
    o["confirmPrecondition"] = confirmPrecondition;
    o["weightSum"] = scaledNum(weightSum);
    o["storeInjected"] = storeInjected;
    o["storeList"] = storeList;
    o["sinkInjected"] = sinkInjected;
    o["clockInjected"] = clockInjected;
    o["logInjected"] = logInjected;
    o["metricsDigest"] = metricsDigest;
    o["templatesDigest"] = templatesDigest;
    o["engineVersion"] = engineVersion;
    return o;
}

// ============================================================================
// 规则包视图
// ============================================================================

bool MetricsPack::has(const std::string& key) const {
    for (const auto& m : items) {
        if (m.key == key) return true;
    }
    return false;
}

std::optional<MetricDef> MetricsPack::metric(const std::string& key) const {
    for (const auto& m : items) {
        if (m.key == key) return m;
    }
    return std::nullopt;
}

double MetricsPack::weightSum() const {
    double s = 0.0;
    for (const auto& m : items) s += m.weight;
    return s;
}

bool TemplatesPack::has(const std::string& key) const {
    for (const auto& t : items) {
        if (t.key == key) return true;
    }
    return false;
}

std::optional<PlanTemplate> TemplatesPack::plan(const std::string& key) const {
    for (const auto& t : items) {
        if (t.key == key) return t;
    }
    return std::nullopt;
}

std::optional<ProfileDef> TemplatesPack::profile(const std::string& key) const {
    for (const auto& p : profiles) {
        if (p.key == key) return p;
    }
    return std::nullopt;
}

// ============================================================================
// ScoringEngine
// ============================================================================

ScoringEngine::ScoringEngine() : impl_(new Impl()) {}
ScoringEngine::ScoringEngine(const ScoringEngineOptions& opts) : impl_(new Impl()) {
    impl_->store = opts.store;
    impl_->sink = opts.sink;
    impl_->clock = opts.clock;
    impl_->log = opts.log;
}
ScoringEngine::~ScoringEngine() = default;
ScoringEngine::ScoringEngine(ScoringEngine&&) noexcept = default;
ScoringEngine& ScoringEngine::operator=(ScoringEngine&&) noexcept = default;

int64_t ScoringEngine::Impl::nowMs() {
    if (clock) return clock->nowMs();
    SystemClock sc;
    return sc.nowMs();
}

int64_t SystemClock::nowMs() const {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

LoadResult ScoringEngine::loadMetrics(const json& pkg) {
    std::lock_guard<std::recursive_mutex> lock(impl_->mu);
    detail::MetricsModel model;
    LoadResult r = detail::loadMetricsInto(pkg, model);
    r.definitionVersion = r.policiesNamespace + ":" + r.schemaVersion + ":" + r.digest;
    if (r.code == 0) {
        // 原子替换：失败时保留上一次成功装载（CTR-PHE-DEF-02 的同口径）
        impl_->metrics = model;
        impl_->metricsLoaded = true;
    }
    return r;
}

LoadResult ScoringEngine::loadMetricsFile(const std::string& path) {
    json pkg;
    std::string err;
    if (!detail::readJsonFile(path, pkg, err)) {
        LoadResult r;
        r.code = static_cast<int>(ErrorCode::BadRequest);
        r.message = err;
        r.issues.push_back({path, "", err});
        return r;
    }
    return loadMetrics(pkg);
}

LoadResult ScoringEngine::loadTemplates(const json& pkg) {
    std::lock_guard<std::recursive_mutex> lock(impl_->mu);
    detail::TemplatesModel model;
    const detail::MetricsModel* m = impl_->metricsLoaded ? &impl_->metrics : nullptr;
    LoadResult r = detail::loadTemplatesInto(pkg, model, m);
    r.definitionVersion = r.policiesNamespace + ":" + r.schemaVersion + ":" + r.digest;
    if (r.code == 0) {
        impl_->templates = model;
        impl_->templatesLoaded = true;
    }
    return r;
}

LoadResult ScoringEngine::loadTemplatesFile(const std::string& path) {
    json pkg;
    std::string err;
    if (!detail::readJsonFile(path, pkg, err)) {
        LoadResult r;
        r.code = static_cast<int>(ErrorCode::BadRequest);
        r.message = err;
        r.issues.push_back({path, "", err});
        return r;
    }
    return loadTemplates(pkg);
}

LoadResult ScoringEngine::policies(const json& metricsPkg, const json& templatesPkg) {
    std::lock_guard<std::recursive_mutex> lock(impl_->mu);
    const LoadResult mr = loadMetrics(metricsPkg);
    if (mr.code != 0) return mr;
    const LoadResult tr = loadTemplates(templatesPkg);
    if (tr.code != 0) return tr;
    LoadResult r;
    r.code = 0;
    r.message = "ok";
    r.policiesNamespace = mr.policiesNamespace;
    r.schemaVersion = mr.schemaVersion;
    r.digest = mr.digest + "+" + tr.digest;
    r.definitionVersion = r.policiesNamespace + ":" + r.schemaVersion + ":" + r.digest;
    r.itemCount = mr.itemCount + tr.itemCount;
    for (const auto& w : mr.warnings) r.warnings.push_back("metrics:" + w);
    for (const auto& w : tr.warnings) r.warnings.push_back("templates:" + w);
    return r;
}

LoadResult ScoringEngine::policiesFromDir(const std::string& dir) {
    json metricsPkg;
    json templatesPkg;
    std::string err;
    const std::string mp = detail::joinPath(dir, "scoringMetrics.json");
    const std::string tp = detail::joinPath(dir, "planTemplates.json");
    if (!detail::readJsonFile(mp, metricsPkg, err)) {
        LoadResult r;
        r.code = static_cast<int>(ErrorCode::BadRequest);
        r.message = "scoringMetrics.json:" + err;
        r.issues.push_back({mp, "", err});
        return r;
    }
    if (!detail::readJsonFile(tp, templatesPkg, err)) {
        LoadResult r;
        r.code = static_cast<int>(ErrorCode::BadRequest);
        r.message = "planTemplates.json:" + err;
        r.issues.push_back({tp, "", err});
        return r;
    }
    return policies(metricsPkg, templatesPkg);
}

LoadResult ScoringEngine::validateMetrics(const json& pkg) {
    detail::MetricsModel model;
    return detail::loadMetricsInto(pkg, model);
}

LoadResult ScoringEngine::validateTemplates(const json& pkg) {
    detail::TemplatesModel model;
    return detail::loadTemplatesInto(pkg, model, nullptr);
}

MetricsPack ScoringEngine::metricsPack() const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mu);
    return impl_->metrics.pack;
}

TemplatesPack ScoringEngine::templatesPack() const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mu);
    return impl_->templates.pack;
}

std::vector<std::string> ScoringEngine::metricKeys() const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mu);
    std::vector<std::string> out;
    for (const auto& m : impl_->metrics.items) out.push_back(m.key);
    return out;
}

std::vector<std::string> ScoringEngine::templateKeys() const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mu);
    std::vector<std::string> out;
    for (const auto& t : impl_->templates.items) out.push_back(t.key);
    return out;
}

Capabilities ScoringEngine::capabilities() const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mu);
    Capabilities c;
    c.metricsLoaded = impl_->metricsLoaded;
    c.templatesLoaded = impl_->templatesLoaded;
    c.policiesNamespace = impl_->metrics.pack.policiesNamespace.empty()
                              ? impl_->templates.pack.policiesNamespace
                              : impl_->metrics.pack.policiesNamespace;
    c.metricsSchemaVersion = impl_->metrics.pack.schemaVersion;
    c.templatesSchemaVersion = impl_->templates.pack.schemaVersion;
    c.policiesMajor = kSupportedPoliciesMajor;
    c.metricCount = static_cast<int>(impl_->metrics.items.size());
    c.templateCount = static_cast<int>(impl_->templates.items.size());
    c.profileCount = static_cast<int>(impl_->templates.profiles.size());
    c.searchParamCount = static_cast<int>(impl_->metrics.search.params.size());
    c.maxIterations = impl_->metrics.search.maxIterations;
    c.timeBudgetMs = impl_->metrics.search.timeBudgetMs;
    c.reasonCount = impl_->metrics.reasons.count;
    c.confirmPrecondition = toString(impl_->templates.confirmMode);
    c.weightSum = impl_->metrics.pack.weightSum();
    c.storeInjected = static_cast<bool>(impl_->store);
    c.storeList = impl_->store ? impl_->store->supportsList() : false;
    c.sinkInjected = static_cast<bool>(impl_->sink);
    c.clockInjected = static_cast<bool>(impl_->clock);
    c.logInjected = static_cast<bool>(impl_->log);
    c.metricsDigest = impl_->metrics.pack.digest;
    c.templatesDigest = impl_->templates.pack.digest;
    c.engineVersion = kEngineVersion;
    return c;
}

Metrics ScoringEngine::metrics() const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mu);
    return impl_->counters;
}

std::vector<Candidate> ScoringEngine::generateCandidates(const CandidateRequest& req) const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mu);
    if (!impl_->templatesLoaded) return {};
    return detail::generateCandidates(impl_->metrics, impl_->templates, req, &impl_->counters);
}

ScoreResult ScoringEngine::score(const CandidateRequest& req) {
    std::lock_guard<std::recursive_mutex> lock(impl_->mu);
    ScoreResult r;
    r.missionId = req.missionId;
    r.side = req.side;
    r.scene = req.scene.empty() ? req.phase.scenarioKey : req.scene;
    if (!impl_->metricsLoaded || !impl_->templatesLoaded) {
        r.code = static_cast<int>(ErrorCode::Internal);
        r.message = "policies not loaded";
        ++impl_->counters.rejected;
        return r;
    }
    const int64_t ts = impl_->nowMs();
    // 非 const 版本：评分会写入计数器（因此不能直接 const 调用，但语义上与 const 等价）
    ScoreResult out =
        detail::scoreCandidates(impl_->metrics, impl_->templates, req, ts, &impl_->counters);
    if (out.code == 0) ++impl_->counters.scores;
    else ++impl_->counters.rejected;
    return out;
}

OptimizeResult ScoringEngine::optimize(const OptimizeRequest& req) {
    std::lock_guard<std::recursive_mutex> lock(impl_->mu);
    OptimizeResult r;
    r.planId = req.templateKey;
    if (!impl_->metricsLoaded || !impl_->templatesLoaded) {
        r.code = static_cast<int>(ErrorCode::Internal);
        r.message = "policies not loaded";
        r.status = "rejected";
        return r;
    }
    const int64_t ts = impl_->nowMs();
    r = detail::runOptimize(impl_->metrics, impl_->templates, req, ts, &impl_->counters);
    ++impl_->counters.optimizations;
    if (r.code != 0) ++impl_->counters.rejected;
    return r;
}

namespace {

/// 事件与日志出口（SCD-DECIDE-04：事件名 `plan.state`，负载含 {missionId, side, planId, action}）
/// 两个回调 MUST 立即返回；抛出的异常 MUST 被吞掉，MUST NOT 影响裁决结果（P10 精神）
void publish(const std::shared_ptr<IPlanSink>& sink, const std::shared_ptr<ILogSink>& log,
             Metrics& counters, const DecideResult& r, const std::string& operatorId,
             const std::string& reason) {
    if (sink) {
        try {
            sink->onPlanStateChanged(r.event);
        } catch (...) {
            ++counters.sinkErrors;
        }
    }
    if (log) {
        try {
            log->log(1, "plan." + r.action, r.event);
            json audit = json::object();
            audit["at"] = r.ts;
            audit["actor"] = operatorId;
            audit["action"] = r.action;
            audit["target"] = r.planId;
            audit["detail"] = reason;
            audit["violation"] = false;
            audit["deviated"] = r.deviated;
            audit["idempotent"] = r.idempotent;
            log->commandAudit(audit);
        } catch (...) {
            ++counters.sinkErrors;
        }
    }
}

}  // namespace

DecideResult ScoringEngine::adopt(const AdoptRequest& req) {
    std::lock_guard<std::recursive_mutex> lock(impl_->mu);
    // 同侧互斥闸门：**在这一层持有**（runAdopt 只做裁决，不碰闸门，
    // 这样 confirm 的 auto-adopt 路径可以安全复用同一次裁决）
    std::string side = req.side;
    if (side.empty() && impl_->templatesLoaded) {
        const auto it = impl_->templates.index.find(req.planId);
        if (it != impl_->templates.index.end()) {
            side = impl_->templates.items[static_cast<std::size_t>(it->second)].side;
        }
    }
    detail::SideGate gate(impl_->mu, impl_->inFlight, req.missionId + "|" + side, impl_->counters,
                          true);
    if (!gate.acquired()) {
        DecideResult r;
        r.code = static_cast<int>(ErrorCode::Conflict);
        r.message = "another action holds this side";
        r.status = ResultStatus::Rejected;
        r.missionId = req.missionId;
        r.planId = req.planId;
        r.side = side;
        r.conflict = true;
        ++impl_->counters.adopts;
        ++impl_->counters.conflicts;
        return r;
    }
    DecideResult r = detail::runAdopt(*impl_, req, false);
    ++impl_->counters.adopts;
    if (r.code == 0 && r.status == ResultStatus::Ok) {
        publish(impl_->sink, impl_->log, impl_->counters, r, req.operatorId, req.reason);
    }
    if (r.conflict) ++impl_->counters.conflicts;
    return r;
}

DecideResult ScoringEngine::confirm(const ConfirmRequest& req) {
    std::lock_guard<std::recursive_mutex> lock(impl_->mu);
    // 闸门在本层持有：runConfirm 内部的 auto-adopt 会用 take=false 复用同一把闸门
    std::string side = req.side;
    if (side.empty() && impl_->templatesLoaded) {
        const auto it = impl_->templates.index.find(req.planId);
        if (it != impl_->templates.index.end()) {
            side = impl_->templates.items[static_cast<std::size_t>(it->second)].side;
        }
    }
    detail::SideGate gate(impl_->mu, impl_->inFlight, req.missionId + "|" + side, impl_->counters,
                          true);
    if (!gate.acquired()) {
        DecideResult r;
        r.code = static_cast<int>(ErrorCode::Conflict);
        r.message = "another action holds this side";
        r.status = ResultStatus::Rejected;
        r.missionId = req.missionId;
        r.planId = req.planId;
        r.side = side;
        r.conflict = true;
        ++impl_->counters.confirms;
        ++impl_->counters.conflicts;
        return r;
    }
    DecideResult r = detail::runConfirm(*impl_, req);
    ++impl_->counters.confirms;
    if (r.code == 0 && r.status == ResultStatus::Ok) {
        publish(impl_->sink, impl_->log, impl_->counters, r, req.operatorId, req.reason);
    }
    if (r.conflict) ++impl_->counters.conflicts;
    return r;
}

std::vector<PlanState> ScoringEngine::plans(const PlanQuery& q) const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mu);
    // 三态查询（SCD-DECIDE-05）：store 支持 list 时以 store 为权威，否则用进程内缓存
    std::vector<PlanState> out;
    if (impl_->store && impl_->store->supportsList()) out = impl_->store->list(q);
    if (out.empty()) out = impl_->cache.list(q);
    return out;
}

void ScoringEngine::setStore(std::shared_ptr<IPlanStore> store) {
    std::lock_guard<std::recursive_mutex> lock(impl_->mu);
    impl_->store = std::move(store);
}
void ScoringEngine::setSink(std::shared_ptr<IPlanSink> sink) {
    std::lock_guard<std::recursive_mutex> lock(impl_->mu);
    impl_->sink = std::move(sink);
}
void ScoringEngine::setClock(std::shared_ptr<IClock> clock) {
    std::lock_guard<std::recursive_mutex> lock(impl_->mu);
    impl_->clock = std::move(clock);
}
void ScoringEngine::setLog(std::shared_ptr<ILogSink> log) {
    std::lock_guard<std::recursive_mutex> lock(impl_->mu);
    impl_->log = std::move(log);
}

}  // namespace scoring
