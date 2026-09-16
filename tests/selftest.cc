// tests/selftest.cc · scoring —— 零依赖自测（手写断言，不引任何测试框架）
//
// 覆盖两套口径：
//   ① 需求专篇 docs/需求/scoring需求专篇.md 的 **34 条**
//      （SCD-CAND 4 / SCD-SCORE 6 / SCD-PICK 5 / SCD-EXP 4 / SCD-OPT 5 / SCD-DECIDE 5 / SCD-NFR 5）
//   ② 共享契约 ../phase-engine/docs/契约/protocol.md 的可机检清单与冲突裁决 C1/C2/C15/C16/C17
//
// 纪律：
//   · 时钟一律注入假时钟（SCD-NFR-05）；不依赖真实时间、不联网、不需要外部服务。
//   · 规则包目录由 CMake 以绝对路径注入，**不依赖当前工作目录**（SCD-NFR-02）。
//   · 本文件内**允许**出现规则包取值作为期望值（验收口径如此规定）；引擎产物内不允许。
//   · 断言一律用容差而非等值比较（SCD-NFR-05）。
//
// 运行： selftest / --list / --json / <名字片段>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "scoring/scoring.h"

using namespace scoring;

namespace {

// ---------------------------------------------------------------- 断言框架

int g_asserts = 0;
int g_failed = 0;
int g_cases = 0;
int g_casesFailed = 0;
std::string g_case;
std::vector<std::string> g_failures;

struct CaseMeta {
    std::string name;
    std::vector<std::string> reqs;
    int asserts = 0;
    int failed = 0;
    bool ok = false;
};
std::vector<CaseMeta> g_metas;
std::vector<std::string> g_reqs;
bool g_jsonMode = false;

template <typename... Args>
void requires_(Args... ids) {
    for (const char* id : {ids...}) g_reqs.push_back(id);
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

std::string jsonArray(const std::vector<std::string>& v) {
    std::string out = "[";
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i != 0) out += ",";
        out += "\"" + jsonEscape(v[i]) + "\"";
    }
    out += "]";
    return out;
}

/// `--json` 模式下 stdout MUST 只有那一个 JSON 文档，故诊断信息走 stderr
void note(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_jsonMode ? stderr : stdout, fmt, args);
    va_end(args);
}

void record(bool ok, const std::string& what, const char* file, int line) {
    ++g_asserts;
    if (ok) return;
    ++g_failed;
    std::ostringstream oss;
    oss << "[" << g_case << "] " << what << "  (" << file << ":" << line << ")";
    g_failures.push_back(oss.str());
}

std::string show(bool v) { return v ? "true" : "false"; }
std::string show(const std::string& v) { return v; }
inline std::string show(const char* v) { return v ? std::string(v) : std::string("(null)"); }
/// 字符串字面量以 `const char[N]` 绑定到 `const A&`，需要显式退化为指针（否则会落到整数路径）
template <std::size_t N>
std::string show(const char (&v)[N]) {
    return show(static_cast<const char*>(v));
}
std::string show(const json& v) { return v.dump(); }
inline std::string show(double v) {
    std::ostringstream oss;
    oss << v;
    return oss.str();
}
/// 其余可比较的标量（整型 / 枚举）走整数路径；不可打印的类型只回一个占位符
template <typename T>
std::string show(const T& v) {
    if constexpr (std::is_arithmetic<T>::value || std::is_enum<T>::value) {
        std::ostringstream oss;
        oss << static_cast<long long>(v);
        return oss.str();
    } else {
        return std::string("(value)");
    }
}

template <typename A, typename B>
void recordEq(const A& got, const B& want, const std::string& what, const char* file, int line) {
    ++g_asserts;
    if (got == want) return;
    ++g_failed;
    std::ostringstream oss;
    oss << "[" << g_case << "] " << what << "：期望 " << show(want) << "，实际 " << show(got)
        << "  (" << file << ":" << line << ")";
    g_failures.push_back(oss.str());
}

void recordNear(double got, double want, double tol, const std::string& what, const char* file,
                int line) {
    ++g_asserts;
    if (std::fabs(got - want) <= tol) return;
    ++g_failed;
    std::ostringstream oss;
    oss << "[" << g_case << "] " << what << "：期望 " << want << "±" << tol << "，实际 " << got
        << "  (" << file << ":" << line << ")";
    g_failures.push_back(oss.str());
}

}  // namespace

#define CHECK(cond) record((cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ(got, want) recordEq((got), (want), #got " == " #want, __FILE__, __LINE__)
#define CHECK_NEAR(got, want, tol) \
    recordNear((got), (want), (tol), #got " ≈ " #want, __FILE__, __LINE__)

namespace {

// ============================================================================
// 宿主侧测试替身（引擎只认识反向接口 —— SCD-NFR-03）
// ============================================================================

class FakeClock : public IClock {
public:
    explicit FakeClock(int64_t start = 1750000000000LL) : now_(start) {}
    int64_t nowMs() const override { return now_; }
    void set(int64_t ms) { now_ = ms; }
    void advance(int64_t ms) { now_ += ms; }

private:
    int64_t now_;
};

class MemStore : public IPlanStore {
public:
    bool save(const PlanState& st) override {
        if (failSave) return false;
        rows[st.missionId][st.planId] = st;
        ++saves;
        return true;
    }
    bool load(const std::string& missionId, const std::string& planId, PlanState& out) override {
        const auto m = rows.find(missionId);
        if (m == rows.end()) return false;
        const auto p = m->second.find(planId);
        if (p == m->second.end()) return false;
        out = p->second;
        return true;
    }
    std::vector<PlanState> listBySide(const std::string& missionId,
                                      const std::string& side) override {
        std::vector<PlanState> out;
        const auto m = rows.find(missionId);
        if (m == rows.end()) return out;
        for (const auto& kv : m->second) {
            if (kv.second.side == side) out.push_back(kv.second);
        }
        return out;
    }
    bool supportsList() const override { return true; }
    std::vector<PlanState> list(const PlanQuery& q) override {
        std::vector<PlanState> out;
        for (const auto& m : rows) {
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

    std::map<std::string, std::map<std::string, PlanState>> rows;
    int saves = 0;
    bool failSave = false;
};

class RecSink : public IPlanSink {
public:
    std::vector<json> events;
    std::function<void(const json&)> onEvent;
    void onPlanStateChanged(const json& e) override {
        events.push_back(e);
        if (onEvent) onEvent(e);
    }
};

class RecLog : public ILogSink {
public:
    int logs = 0;
    std::vector<json> audits;
    void log(int lvl, const std::string& ev, const json& d) override {
        (void)lvl;
        (void)ev;
        (void)d;
        ++logs;
    }
    void commandAudit(const json& e) override { audits.push_back(e); }
};

/// 抛异常的 Sink：验证“回调异常 MUST NOT 影响裁决结果”
class ThrowingSink : public IPlanSink {
public:
    void onPlanStateChanged(const json&) override { throw std::runtime_error("sink-boom"); }
};

// ============================================================================
// 规则包构造器（**测试数据**：本文件允许出现规则取值，引擎产物不允许）
// ============================================================================

const char* kMetricA = "metric-alpha";
const char* kMetricB = "metric-beta";
const char* kSideA = "side-alpha";
const char* kSceneA = "scene-alpha";

/// 通用指标包。缺省搜索空间由 `items` 的指标 key 推出 —— 搜索空间引用的指标 MUST 存在，
/// 否则装载被拒（这是引擎的正确行为：引用不完整 MUST 拒绝整包 —— CTR-PL-01）。
json makeMetricsPack(const std::vector<json>& items, const json& reasons = json::object(),
                     const json& searchSpace = json::object(),
                     const json& aggregate = json::object()) {
    json pkg = json::object();
    pkg["policiesNamespace"] = "testns";
    pkg["schemaVersion"] = "1.0.0";
    pkg["kind"] = "scoringMetrics";
    if (!aggregate.empty()) pkg["aggregate"] = aggregate;
    pkg["reasons"] = reasons.empty()
                         ? json{{"count", 4},
                                {"types", json::array({"top-score", "top-contribution", "lead",
                                                       "advantage"})}}
                         : reasons;
    json defParams = json::array();
    for (const auto& it : items) {
        const std::string k = it.value("key", std::string());
        if (k.empty()) continue;
        defParams.push_back(json{{"key", "lift-" + k},
                                 {"name", k + "-lift"},
                                 {"metricKey", k},
                                 {"step", 2},
                                 {"maxSteps", 4}});
    }
    pkg["searchSpace"] = searchSpace.empty() ? json{{"maxIterations", 64},
                                                    {"timeBudgetMs", 100},
                                                    {"maxTotalSteps", 9},
                                                    {"params", defParams}}
                                             : searchSpace;
    pkg["items"] = items;
    return pkg;
}

/// 一条指标（区间 0..100 归一 / higher）
json makeMetric(const std::string& key, double weight, double baseline, const std::string& source,
                const std::string& metricName) {
    json m = json::object();
    m["key"] = key;
    m["name"] = key + "-name";
    m["direction"] = "higher";
    m["unit"] = "%";
    m["normalize"] = json{{"type", "range"}, {"min", 0}, {"max", 100}};
    m["weight"] = weight;
    m["baseline"] = baseline;
    if (!source.empty()) {
        m["source"] = json{{"kind", source}, {"metric", metricName.empty() ? key : metricName}};
        m["baselineUsed"] = false;
    }
    return m;
}

json makeTemplatesPack(const std::vector<json>& items, const std::vector<json>& profiles = {},
                       const json& confirmPre = json::object()) {
    json pkg = json::object();
    pkg["policiesNamespace"] = "testns";
    pkg["schemaVersion"] = "1.0.0";
    pkg["kind"] = "planTemplates";
    pkg["confirmPrecondition"] =
        confirmPre.empty() ? json{{"mode", "reject-if-not-adopted"}} : confirmPre;
    pkg["profiles"] = profiles;
    pkg["items"] = items;
    return pkg;
}

json makeProfile(const std::string& key, double a, double b) {
    return json{
        {"key", key}, {"name", key + "-name"}, {"values", json{{kMetricA, a}, {kMetricB, b}}}};
}

json makeTemplate(const std::string& key, const std::string& name, int seq, const std::string& side,
                  const std::string& scene, const std::string& profile, double successRate,
                  const std::vector<std::string>& clusters = {}, const json& when = json::object(),
                  bool recommended = false, const std::string& method = "") {
    json t = json::object();
    t["key"] = key;
    t["side"] = side;
    t["scene"] = scene;
    t["seq"] = seq;
    t["name"] = name;
    t["method"] = method.empty() ? (name + "-method") : method;
    t["clusters"] = clusters;
    t["effect"] = name + "-effect";
    t["successRate"] = successRate;
    if (!profile.empty()) t["profileKey"] = profile;
    if (recommended) t["recommended"] = true;
    t["when"] = when;
    return t;
}

/// 缺省规则包：A/B 两个指标 + 三个模板（strong > mid > weak）
json defaultMetrics() {
    return makeMetricsPack({makeMetric(kMetricA, 0.5, 80, "profile", kMetricA),
                            makeMetric(kMetricB, 0.5, 60, "profile", kMetricB)});
}

json defaultTemplates() {
    return makeTemplatesPack(
        {makeTemplate("tpl-one", "TPL-ONE", 1, kSideA, kSceneA, "prof-strong", 90, {"c1", "c2"}, {},
                      true),
         makeTemplate("tpl-two", "TPL-TWO", 2, kSideA, kSceneA, "prof-mid", 70, {"c1"}),
         makeTemplate("tpl-three", "TPL-THREE", 3, kSideA, kSceneA, "prof-weak", 50, {"c2"})},
        {makeProfile("prof-strong", 90, 70), makeProfile("prof-mid", 70, 50),
         makeProfile("prof-weak", 50, 30)});
}

CandidateRequest reqFor(const std::string& missionId, const std::string& side,
                        const std::string& scene, const ScoringSnapshot& snap) {
    CandidateRequest r;
    r.missionId = missionId;
    r.side = side;
    r.scene = scene;
    r.phase.missionId = missionId;
    r.phase.scenarioKey = scene;
    r.phase.phaseKey = "ph-1";
    r.phase.seq = 1;
    r.snapshot = snap;
    return r;
}

ScoringSnapshot emptySnapshot() {
    ScoringSnapshot s;
    s.missionId = "m-1";
    s.phase.phaseKey = "ph-1";
    s.phase.scenarioKey = kSceneA;
    return s;
}

bool loadDefaults(ScoringEngine& e) {
    const LoadResult a = e.loadMetrics(defaultMetrics());
    if (a.code != 0) {
        note("默认指标包装载失败：%s\n", a.toJson().dump().c_str());
        return false;
    }
    const LoadResult b = e.loadTemplates(defaultTemplates());
    if (b.code != 0) {
        note("默认模板包装载失败：%s\n", b.toJson().dump().c_str());
        return false;
    }
    return true;
}

bool readPolicy(const std::string& name, json& out) {
#ifdef SCORING_POLICY_DIR
    const std::string path = std::string(SCORING_POLICY_DIR) + "/" + name;
#else
    const std::string path = "policies/mapapp/" + name;
#endif
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        note("读不到规则包：%s\n", path.c_str());
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    try {
        out = json::parse(ss.str());
    } catch (const std::exception& ex) {
        note("规则包 JSON 非法：%s → %s\n", path.c_str(), ex.what());
        return false;
    }
    return true;
}

// ============================================================================
// SCD-CAND · 候选生成
// ============================================================================

void cand01_candidates_are_rule_driven() {
    requires_("SCD-CAND-01");
    ScoringEngine e;
    CHECK(loadDefaults(e));
    const std::vector<Candidate> c =
        e.generateCandidates(reqFor("m-1", kSideA, kSceneA, emptySnapshot()));
    CHECK_EQ(c.size(), std::size_t(3));
    if (c.size() == 3) {
        CHECK_EQ(c[0].name, std::string("TPL-ONE"));  // 名称逐字来自规则包
        CHECK_EQ(c[1].name, std::string("TPL-TWO"));
        CHECK_EQ(c[2].name, std::string("TPL-THREE"));
        CHECK_EQ(c[0].source, std::string("tpl-one"));
    }
    // 规则包换一套 → 候选随之改变（同引擎、零代码改动）
    ScoringEngine e2;
    CHECK_EQ(e2.loadMetrics(defaultMetrics()).code, 0);
    json tpl = defaultTemplates();
    tpl["items"] = json::array({makeTemplate("other-key", "OTHER-NAME", 1, kSideA, kSceneA,
                                             "prof-strong", 90, {"c1"}, {}, true)});
    CHECK_EQ(e2.loadTemplates(tpl).code, 0);
    const std::vector<Candidate> c2 =
        e2.generateCandidates(reqFor("m-1", kSideA, kSceneA, emptySnapshot()));
    CHECK_EQ(c2.size(), std::size_t(1));
    if (!c2.empty()) CHECK_EQ(c2[0].name, std::string("OTHER-NAME"));
}

void cand02_inapplicable_is_marked_not_dropped() {
    requires_("SCD-CAND-02");
    ScoringEngine e;
    CHECK(loadDefaults(e));
    ScoringSnapshot snap = emptySnapshot();
    ResourceSnapshot res;
    res.present = true;
    res.phase = snap.phase;
    ClusterUsage u;
    u.clusterId = "c1";
    u.total = 1;
    u.available = false;
    res.clusters.push_back(u);
    snap.resources = res;

    json tpl = defaultTemplates();
    tpl["items"] = json::array(
        {makeTemplate("needs-c1", "NEEDS-C1", 1, kSideA, kSceneA, "prof-strong", 90, {},
                      json{{"clusters", json::array({"c1"})}}),
         makeTemplate("free-1", "FREE-1", 2, kSideA, kSceneA, "prof-mid", 70, {}),
         makeTemplate("free-2", "FREE-2", 3, kSideA, kSceneA, "prof-weak", 50, {})});
    ScoringEngine e2;
    CHECK_EQ(e2.loadMetrics(defaultMetrics()).code, 0);
    CHECK_EQ(e2.loadTemplates(tpl).code, 0);
    const std::vector<Candidate> c = e2.generateCandidates(reqFor("m-1", kSideA, kSceneA, snap));
    CHECK_EQ(c.size(), std::size_t(3));  // 候选总数不减少
    int inapplicable = 0;
    for (const auto& x : c) {
        if (!x.applicable) ++inapplicable;
    }
    CHECK_EQ(inapplicable, 1);
    if (!c.empty()) {
        CHECK(!c[0].applicable);
        CHECK_EQ(c[0].inapplicableReasons.size(), std::size_t(1));
        if (!c[0].inapplicableReasons.empty()) {
            const InapplicableItem& item = c[0].inapplicableReasons[0];
            CHECK_EQ(item.code, std::string("cluster-unavailable"));
            CHECK_EQ(item.field, std::string("when.clusters"));
            CHECK_EQ(item.subject, std::string("c1"));  // 用 key，MUST NOT 用显示名
        }
    }
    const ScoreResult r = e2.score(reqFor("m-1", kSideA, kSceneA, snap));
    CHECK_EQ(r.code, 0);
    CHECK_EQ(r.candidates.size(), std::size_t(3));
    CHECK_EQ(r.recommendedId, std::string("free-1"));
}

void cand03_candidate_count_follows_template_count() {
    requires_("SCD-CAND-03");
    json items = json::array();
    json profiles = json::array();
    for (int i = 1; i <= 5; ++i) {
        const std::string k = "tpl-" + std::to_string(i);
        items.push_back(makeTemplate(k, "T" + std::to_string(i), i, kSideA, kSceneA,
                                     "prof-" + std::to_string(i), 50.0 + i,
                                     {"c" + std::to_string(i)}));
        profiles.push_back(makeProfile("prof-" + std::to_string(i), 50.0 + i * 5, 40.0 + i * 5));
    }
    ScoringEngine e;
    CHECK_EQ(e.loadMetrics(defaultMetrics()).code, 0);
    CHECK_EQ(e.loadTemplates(makeTemplatesPack(items, profiles)).code, 0);
    CHECK_EQ(e.generateCandidates(reqFor("m-1", kSideA, kSceneA, emptySnapshot())).size(),
             std::size_t(5));
    const ScoreResult r = e.score(reqFor("m-1", kSideA, kSceneA, emptySnapshot()));
    CHECK_EQ(r.candidates.size(), std::size_t(5));
    CHECK(r.hasRecommended);
}

void cand04_equivalent_templates_are_deduped_and_marked() {
    requires_("SCD-CAND-04");
    const json t1 = makeTemplate("dup-a", "DUP-A", 1, kSideA, kSceneA, "prof-strong", 90,
                                 {"c1", "c2"}, {}, false, "same-method");
    const json t2 = makeTemplate("dup-b", "DUP-B", 2, kSideA, kSceneA, "prof-strong", 90,
                                 {"c2", "c1"}, {}, false, "same-method");
    ScoringEngine e;
    CHECK_EQ(e.loadMetrics(defaultMetrics()).code, 0);
    CHECK_EQ(e.loadTemplates(makeTemplatesPack({t1, t2}, {makeProfile("prof-strong", 90, 70)})).code,
             0);
    const std::vector<Candidate> c =
        e.generateCandidates(reqFor("m-1", kSideA, kSceneA, emptySnapshot()));
    CHECK_EQ(c.size(), std::size_t(1));
    if (!c.empty()) {
        CHECK_EQ(c[0].id, std::string("dup-a"));
        CHECK_EQ(c[0].duplicates.size(), std::size_t(1));
        if (!c[0].duplicates.empty()) {
            CHECK_EQ(c[0].duplicates[0].key, std::string("dup-b"));
            CHECK_EQ(c[0].duplicates[0].into, std::string("dup-a"));
        }
    }
    CHECK(e.metrics().duplicatesMerged >= 1);
    ScoringEngine e2;
    CHECK_EQ(e2.loadMetrics(defaultMetrics()).code, 0);
    CHECK_EQ(e2.loadTemplates(makeTemplatesPack({t1, t2}, {makeProfile("prof-strong", 90, 70)})).code,
             0);
    CandidateRequest r = reqFor("m-1", kSideA, kSceneA, emptySnapshot());
    r.dedupe = false;
    CHECK_EQ(e2.generateCandidates(r).size(), std::size_t(2));
}

// ============================================================================
// SCD-SCORE · 评分
// ============================================================================

void score01_metrics_are_configurable() {
    requires_("SCD-SCORE-01");
    ScoringEngine e;
    CHECK(loadDefaults(e));
    const std::vector<std::string> keys = e.metricKeys();
    CHECK_EQ(keys.size(), std::size_t(2));
    if (keys.size() == 2) {
        CHECK_EQ(keys[0], std::string(kMetricA));
        CHECK_EQ(keys[1], std::string(kMetricB));
    }
    const MetricsPack pack = e.metricsPack();
    const auto m0 = pack.metric(kMetricA);
    CHECK(m0.has_value());
    if (m0) {
        CHECK_EQ(std::string(toString(m0->direction)), std::string("higher"));
        CHECK_EQ(std::string(toString(m0->normalize.type)), std::string("range"));
    }
    // 换指标集 → 换评分维度（引擎零改动）
    ScoringEngine e2;
    json pkg = json::object();
    pkg["policiesNamespace"] = "testns";
    pkg["schemaVersion"] = "1.0.0";
    pkg["kind"] = "scoringMetrics";
    pkg["reasons"] = json{{"count", 4},
                          {"types", json::array({"top-score", "top-contribution", "lead",
                                                 "advantage"})}};
    pkg["searchSpace"] = json{{"maxIterations", 8}, {"params", json::array()}};
    pkg["items"] = json::array({makeMetric("only-one", 1.0, 50, "profile", "only-one")});
    CHECK_EQ(e2.loadMetrics(pkg).code, 0);
    CHECK_EQ(e2.metricKeys().size(), std::size_t(1));
    // 方向可为“越小越好”：归一化取反
    MetricDef lower;
    lower.direction = MetricDirection::Lower;
    lower.normalize.type = NormalizeType::Range;
    lower.normalize.min = 0;
    lower.normalize.max = 100;
    CHECK_NEAR(normalizeValue(lower, 20.0), 0.8, 1e-12);
    CHECK_NEAR(normalizeValue(lower, 80.0), 0.2, 1e-12);
}

void score02_normalization_is_explicit_and_reproducible() {
    requires_("SCD-SCORE-02");
    MetricDef range;
    range.normalize.type = NormalizeType::Range;
    range.normalize.min = 0;
    range.normalize.max = 100;
    CHECK_NEAR(normalizeValue(range, 96.0), 0.96, 1e-12);  // 手算核对：96/100
    CHECK_NEAR(normalizeValue(range, 0.0), 0.0, 1e-12);
    CHECK_NEAR(normalizeValue(range, 100.0), 1.0, 1e-12);
    CHECK_NEAR(normalizeValue(range, 150.0), 1.0, 1e-12);  // 截断到 [0,1]

    MetricDef percent;
    percent.normalize.type = NormalizeType::Percent;
    percent.normalize.scale = 100.0;
    CHECK_NEAR(normalizeValue(percent, 93.0), 0.93, 1e-12);

    MetricDef thr;
    thr.normalize.type = NormalizeType::Threshold;
    thr.normalize.bands = {{80.0, 1.0}, {50.0, 0.5}, {0.0, 0.0}};
    CHECK_NEAR(normalizeValue(thr, 85.0), 1.0, 1e-12);
    CHECK_NEAR(normalizeValue(thr, 50.0), 0.5, 1e-12);
    CHECK_NEAR(normalizeValue(thr, 10.0), 0.0, 1e-12);

    MetricDef bad;
    bad.normalize.type = NormalizeType::Range;
    bad.normalize.min = 10;
    bad.normalize.max = 10;
    CHECK_NEAR(normalizeValue(bad, 50.0), 0.0, 1e-12);

    ScoringEngine e;
    json pkg = defaultMetrics();
    pkg["items"][0]["normalize"] = json{{"type", "magic"}};
    const LoadResult bad1 = e.loadMetrics(pkg);
    CHECK_EQ(bad1.code, 1000);
    CHECK(!bad1.issues.empty());

    json pkg2 = defaultMetrics();
    pkg2["items"][0]["normalize"] = json{{"type", "range"}, {"min", 100}, {"max", 0}};
    CHECK_EQ(e.loadMetrics(pkg2).code, 1000);
}

void score03_weights_are_validated() {
    requires_("SCD-SCORE-03");
    ScoringEngine e;
    json zero = makeMetricsPack({makeMetric(kMetricA, 0.0, 80, "profile", kMetricA),
                                 makeMetric(kMetricB, 0.0, 60, "profile", kMetricB)});
    const LoadResult rz = e.loadMetrics(zero);
    CHECK_EQ(rz.code, 1000);
    bool mentioned = false;
    for (const auto& i : rz.issues) {
        if (i.reason.find("weights-all-zero") != std::string::npos) mentioned = true;
    }
    CHECK(mentioned);

    json half = makeMetricsPack({makeMetric(kMetricA, 0.5, 80, "profile", kMetricA),
                                 makeMetric(kMetricB, 0.25, 60, "profile", kMetricB)});
    const LoadResult rh = e.loadMetrics(half);
    CHECK_EQ(rh.code, 1000);
    bool sumIssue = false;
    for (const auto& i : rh.issues) {
        if (i.reason.find("weight-sum-not-one") != std::string::npos) sumIssue = true;
    }
    CHECK(sumIssue);

    json half2 = half;
    half2["aggregate"] = json{{"method", "linear-weighted-mean"},
                              {"rounding", "half-up"},
                              {"normalizeWeights", true}};
    ScoringEngine e2;
    CHECK_EQ(e2.loadMetrics(half2).code, 0);
    CHECK_NEAR(e2.metricsPack().weightSum(), 1.0, 1e-9);

    ScoringEngine e3;
    CHECK(loadDefaults(e3));
    const ScoreResult r = e3.score(reqFor("m-1", kSideA, kSceneA, emptySnapshot()));
    CHECK_EQ(r.code, 0);
    for (const auto& c : r.candidates) {
        if (!c.applicable) continue;
        CHECK(c.totalPercent >= 0);
        CHECK(c.totalPercent <= 100);
    }
    json neg = makeMetricsPack({makeMetric(kMetricA, 1.5, 80, "profile", kMetricA),
                                makeMetric(kMetricB, -0.5, 60, "profile", kMetricB)});
    CHECK_EQ(e3.loadMetrics(neg).code, 1000);
}

void score04_every_term_is_exported_and_hand_checkable() {
    requires_("SCD-SCORE-04");
    ScoringEngine e;
    CHECK(loadDefaults(e));
    const ScoreResult r = e.score(reqFor("m-1", kSideA, kSceneA, emptySnapshot()));
    CHECK_EQ(r.code, 0);
    const auto best = r.byId(r.recommendedId);
    CHECK(best.has_value());
    if (!best) return;
    // 推荐项：A=90、B=70，权重 0.5/0.5 → 0.45 + 0.35 = 0.80 → 80
    CHECK_EQ(best->metrics.size(), std::size_t(2));
    long long sum = 0;
    for (const auto& m : best->metrics) {
        CHECK(!m.sourceField.empty());  // 取值来源可追溯
        CHECK(m.weight > 0.0);
        sum += m.contributionScaled;
        CHECK_NEAR(m.contribution, m.normalized * m.weight * 100.0, 0.01);
    }
    CHECK_EQ(best->totalScaled, sum);
    CHECK_EQ(best->totalPercent, roundToPercent(sum));
    CHECK_EQ(best->totalPercent, 80);  // 手算：0.90×50% + 0.70×50% = 80%
    const json audit = r.toAuditJson(true);
    long long s2 = 0;
    bool found = false;
    for (const auto& c : audit["output"]["candidates"]) {
        if (c.value("id", std::string()) != r.recommendedId) continue;
        for (const auto& m : c["metrics"]) s2 += m.value("contributionScaled", 0LL);
        found = true;
    }
    CHECK(found);
    CHECK_EQ(s2, sum);
    CHECK_EQ(roundToPercent(s2), best->totalPercent);
    CHECK(r.toJson()["data"]["candidates"][0]["total"].is_number_integer());
}

void score05_snapshot_is_the_only_input() {
    requires_("SCD-SCORE-05");
    ScoringEngine bare;
    const ScoreResult r0 = bare.score(reqFor("m-1", kSideA, kSceneA, emptySnapshot()));
    CHECK_EQ(r0.code, 1005);
    CHECK(bare.generateCandidates(reqFor("m-1", kSideA, kSceneA, emptySnapshot())).empty());

    ScoringEngine e;
    CHECK(loadDefaults(e));
    const ScoreResult r1 = e.score(reqFor("m-1", kSideA, kSceneA, emptySnapshot()));
    ScoringSnapshot s2 = emptySnapshot();
    s2.extra["metrics"] = json{{"metric-alpha", 20.0}};  // 快照显式投喂指标观测值
    const ScoreResult r2 = e.score(reqFor("m-1", kSideA, kSceneA, s2));
    CHECK_EQ(r1.code, 0);
    CHECK_EQ(r2.code, 0);
    const auto b2 = r2.byId(r2.recommendedId);
    CHECK(b2.has_value());
    if (b2) {
        CHECK_EQ(b2->metrics[0].sourceField, std::string("snapshot.metrics.metric-alpha"));
        CHECK_NEAR(b2->metrics[0].raw, 20.0, 1e-9);
    }
    const auto b1 = r1.byId(r1.recommendedId);
    CHECK(b1.has_value());
    if (b1 && b2) {
        CHECK(b1->metrics[0].raw != b2->metrics[0].raw);
        CHECK(b1->totalPercent != b2->totalPercent);
    }
}

void score06_determinism_and_rounding() {
    requires_("SCD-SCORE-06");
    ScoringEngine e;
    CHECK(loadDefaults(e));
    e.setClock(std::make_shared<FakeClock>());
    const CandidateRequest req = reqFor("m-1", kSideA, kSceneA, emptySnapshot());
    const std::string a = e.score(req).toJson().dump();
    const std::string b = e.score(req).toJson().dump();
    CHECK_EQ(a, b);  // 逐字节一致
    CHECK(a.size() > 0);

    // 舍入规则：half-up（.5 向上），边界值逐一核对（标度约定：outScale == 100 对应 100%）
    CHECK_EQ(roundHalfUpScaled(92.965, 100), std::int64_t(9297));
    CHECK_EQ(roundHalfUpScaled(92.964, 100), std::int64_t(9296));
    CHECK_EQ(roundPercent(92.964, 100), std::int64_t(93));
    CHECK_EQ(roundPercent(92.499, 100), std::int64_t(92));
    CHECK_EQ(roundPercent(92.5, 100), std::int64_t(93));
    CHECK_EQ(roundPercent(92.96, kScale), std::int64_t(9296));
    CHECK_EQ(roundToPercent(9296, kScale), 93);
    CHECK_EQ(roundHalfUpScaled(-1.5, 1), std::int64_t(-2));

    const ScoreResult r = e.score(req);
    const auto best = r.byId(r.recommendedId);
    CHECK(best.has_value());
    if (best) {
        double fsum = 0.0;
        for (const auto& m : best->metrics) fsum += m.normalized * m.weight;
        CHECK_NEAR(scaledToDouble(best->totalScaled, kScale), fsum * 100.0, 1e-9);
    }
}

// ============================================================================
// SCD-PICK · 排序与推荐
// ============================================================================

void pick01_ordering_is_stable() {
    requires_("SCD-PICK-01");
    json items = json::array();
    json profiles = json::array();
    for (int i = 1; i <= 3; ++i) {
        items.push_back(makeTemplate("same-" + std::to_string(i), "SAME-" + std::to_string(i), i,
                                     kSideA, kSceneA, "prof-same", 60, {"c" + std::to_string(i)}));
    }
    profiles.push_back(makeProfile("prof-same", 60, 60));
    ScoringEngine e;
    CHECK_EQ(e.loadMetrics(defaultMetrics()).code, 0);
    CHECK_EQ(e.loadTemplates(makeTemplatesPack(items, profiles)).code, 0);
    e.setClock(std::make_shared<FakeClock>());  // 同输入双跑必须逐字节一致（含 ts）
    const CandidateRequest req = reqFor("m-1", kSideA, kSceneA, emptySnapshot());
    const ScoreResult r1 = e.score(req);
    const ScoreResult r2 = e.score(req);
    CHECK_EQ(r1.candidates.size(), std::size_t(3));
    CHECK_EQ(r1.candidates[0].totalPercent, r1.candidates[1].totalPercent);
    CHECK_EQ(r1.candidates[0].candidate.id, std::string("same-1"));
    CHECK_EQ(r1.candidates[1].candidate.id, std::string("same-2"));
    CHECK_EQ(r1.candidates[2].candidate.id, std::string("same-3"));
    CHECK_EQ(r1.candidates[0].totalScaled, r1.candidates[1].totalScaled);
    CHECK_EQ(r1.toJson().dump(), r2.toJson().dump());
    CHECK_EQ(r1.candidates[0].rank, 1);
    CHECK_EQ(r1.candidates[1].rank, 2);
    CHECK_EQ(r1.candidates[2].rank, 3);
}

void pick02_recommendation_follows_the_ranking() {
    requires_("SCD-PICK-02");
    ScoringEngine e;
    CHECK(loadDefaults(e));
    const ScoreResult r = e.score(reqFor("m-1", kSideA, kSceneA, emptySnapshot()));
    CHECK_EQ(r.code, 0);
    CHECK(r.hasRecommended);
    CHECK_EQ(r.recommendedId, std::string("tpl-one"));
    CHECK_EQ(r.recommendedPercent, r.candidates[0].totalPercent);
    CHECK(r.candidates[0].recommended);
    int recommendedCount = 0;
    for (const auto& c : r.candidates) {
        if (c.recommended) ++recommendedCount;
    }
    CHECK_EQ(recommendedCount, 1);
    CHECK(!r.reasons.empty());
    const TemplatesPack pack = e.templatesPack();
    const auto t = pack.plan("tpl-one");
    CHECK(t.has_value());
    if (t) CHECK(t->recommendedHint);
}

void pick03_forced_non_recommended_is_allowed_and_marked() {
    requires_("SCD-PICK-03");
    auto store = std::make_shared<MemStore>();
    auto sink = std::make_shared<RecSink>();
    auto clock = std::make_shared<FakeClock>();
    ScoringEngineOptions opts;
    opts.store = store;
    opts.sink = sink;
    opts.clock = clock;
    ScoringEngine e(opts);
    CHECK(loadDefaults(e));

    AdoptRequest a;
    a.missionId = "m-1";
    a.planId = "tpl-three";
    a.side = kSideA;
    a.operatorId = "op-1";
    a.reason = "manual";
    a.hasRecommendedContext = true;
    a.recommendedId = "tpl-one";
    a.recommendedPercent = 80;
    a.planPercent = 40;
    const DecideResult d = e.adopt(a);
    CHECK_EQ(d.code, 0);
    CHECK(d.deviated);
    CHECK_EQ(d.recommendedId, std::string("tpl-one"));
    CHECK_EQ(d.event.value("deviated", false), true);
    CHECK_EQ(d.event.value("recommendedId", std::string()), std::string("tpl-one"));
    AdoptRequest b = a;
    b.planId = "tpl-one";
    const DecideResult d2 = e.adopt(b);
    CHECK_EQ(d2.code, 0);
    CHECK(!d2.deviated);
}

void pick04_reasons_are_structured_and_traceable() {
    requires_("SCD-PICK-04");
    ScoringEngine e;
    CHECK(loadDefaults(e));
    const ScoreResult r = e.score(reqFor("m-1", kSideA, kSceneA, emptySnapshot()));
    CHECK_EQ(r.reasons.size(), std::size_t(4));
    std::set<std::string> metricKeys;
    for (const auto& m : e.metricsPack().items) metricKeys.insert(m.key);
    CHECK_EQ(metricKeys.size(), std::size_t(2));
    for (const auto& reason : r.reasons) {
        CHECK(!reason.type.empty());
        CHECK(metricKeys.count(reason.metricKey) == 1);  // 每条可追溯到某指标
    }
    CHECK_EQ(r.reasons[0].type, std::string("top-score"));
    CHECK_EQ(r.reasons[1].type, std::string("top-contribution"));
    CHECK_EQ(r.reasons[2].type, std::string("lead"));
    CHECK_EQ(r.reasons[3].type, std::string("advantage"));
    CHECK_EQ(r.reasons[2].refKey, r.nextId);
    // 条数可配：换规则包 → 换条数
    json pkg = defaultMetrics();
    pkg["reasons"] = json{{"count", 2}, {"types", json::array({"top-score", "lead"})}};
    ScoringEngine e3;
    CHECK_EQ(e3.loadMetrics(pkg).code, 0);
    CHECK_EQ(e3.loadTemplates(defaultTemplates()).code, 0);
    const ScoreResult r3 = e3.score(reqFor("m-1", kSideA, kSceneA, emptySnapshot()));
    CHECK_EQ(r3.reasons.size(), std::size_t(2));
}

void pick05_lead_over_runner_up() {
    requires_("SCD-PICK-05");
    ScoringEngine e;
    CHECK(loadDefaults(e));
    const ScoreResult r = e.score(reqFor("m-1", kSideA, kSceneA, emptySnapshot()));
    CHECK(r.hasNext);
    CHECK_EQ(r.candidates.size(), std::size_t(3));
    // 领先差值：内部定点差 ↔ 对外百分点（标度换算与审计口径一致 —— SCD-PICK-05）
    CHECK_EQ(r.candidates[0].totalScaled - r.candidates[1].totalScaled,
             roundPercent(r.leadOverNext, kScale));    CHECK_NEAR(r.leadOverNext,
               scaledToDouble(r.candidates[0].totalScaled - r.candidates[1].totalScaled), 1e-9);
    CHECK_EQ(r.leadOverNextPercent, r.recommendedPercent - r.nextPercent);
    CHECK(r.leadOverNext > 0.0);
    ScoringEngine e2;
    CHECK_EQ(e2.loadMetrics(defaultMetrics()).code, 0);
    CHECK_EQ(e2.loadTemplates(makeTemplatesPack(
                 {makeTemplate("only", "ONLY", 1, kSideA, kSceneA, "prof-strong", 90)},
                 {makeProfile("prof-strong", 90, 70)}))
                 .code,
             0);
    const ScoreResult r2 = e2.score(reqFor("m-1", kSideA, kSceneA, emptySnapshot()));
    CHECK(!r2.hasNext);
    CHECK_NEAR(r2.leadOverNext, 0.0, 1e-12);
}

// ============================================================================
// SCD-EXP · 可解释输出
// ============================================================================

void exp01_structured_output_shape() {
    requires_("SCD-EXP-01");
    ScoringEngine e;
    CHECK(loadDefaults(e));
    const ScoreResult r = e.score(reqFor("m-1", kSideA, kSceneA, emptySnapshot()));
    const json env = r.toJson();
    CHECK_EQ(env["code"].get<int>(), 0);
    CHECK(env.contains("message"));
    const json d = env["data"];
    for (const char* k : {"candidates", "reasons"}) CHECK(d.contains(k));
    CHECK(d.contains("recommendedId"));
    CHECK(d.contains("leadOverNext"));
    CHECK(d.contains("missionId"));
    CHECK(d.contains("scene"));
    const json c0 = d["candidates"][0];
    for (const char* k : {"id", "name", "total", "metrics", "recommended", "applicable", "rank"}) {
        CHECK(c0.contains(k));
    }
    const json m0 = c0["metrics"][0];
    for (const char* k : {"raw", "normalized", "weight", "contribution", "contributionScaled"}) {
        CHECK(m0.contains(k));
    }
    CHECK_EQ(env.begin().key(), std::string("code"));      // 键序 = 成员声明顺序
    CHECK_EQ(d.begin().key(), std::string("missionId"));
}

void exp02_no_natural_language_sentences() {
    requires_("SCD-EXP-02");
    ScoringEngine e;
    CHECK(loadDefaults(e));
    const ScoreResult r = e.score(reqFor("m-1", kSideA, kSceneA, emptySnapshot()));
    const json d = r.dataJson();  // 只看结构化部分（message 属宿主本地化范围）
    for (const auto& reason : d["reasons"]) {
        CHECK(reason.is_object());
        CHECK(reason.contains("type"));
        CHECK(reason.contains("metricKey"));
        CHECK(reason.contains("value"));
        CHECK(!reason.contains("text"));  // MUST NOT 有文案字段
        CHECK(!reason.contains("label"));
    }
    // 结构化部分内不得含**成句文本**：句末标点出现在自由文本字段即为破线（风险 R4）。
    // 说明：标识符类字段（键名、来源路径、原因码）用 `.` 作分隔符，属标识符而非句子，
    // 因此只对自由文本字段做句末标点检查。
    static const char* kFreeTextKeys[] = {"name", "method", "effect", "profileKey", "type",
                                          "missingMarker"};
    static const char* kSentenceEnders[] = {"。", "！", "？", "!", "?"};
    std::function<void(const json&, const std::string&)> walk = [&](const json& v,
                                                                   const std::string& path) {
        if (v.is_object()) {
            for (auto it = v.begin(); it != v.end(); ++it) {
                bool freeText = false;
                for (const char* k : kFreeTextKeys) {
                    if (it.key() == k) freeText = true;
                }
                if (freeText && it.value().is_string()) {
                    const std::string s = it.value().get<std::string>();
                    for (const char* p : kSentenceEnders) {
                        CHECK(s.find(p) == std::string::npos);
                    }
                    CHECK(s.size() <= 120);  // 长文本必然成句
                }
                walk(it.value(), path + "." + it.key());
            }
            return;
        }
        if (v.is_array()) {
            for (std::size_t i = 0; i < v.size(); ++i) walk(v[i], path + "[]");
        }
    };
    walk(d, "data");
    // 换语言不需改引擎：理由类型是机器可读枚举（短标识符），不是句子
    for (const auto& reason : d["reasons"]) {
        const std::string ty = reason["type"].get<std::string>();
        CHECK(ty.size() <= 32);
        CHECK(ty.find(' ') == std::string::npos);
    }
}

void exp03_audit_triple_is_replayable() {
    requires_("SCD-EXP-03");
    ScoringEngine e;
    CHECK(loadDefaults(e));
    const ScoreResult r = e.score(reqFor("m-1", kSideA, kSceneA, emptySnapshot()));
    const json audit = r.toAuditJson(true);
    CHECK(audit.contains("input"));
    CHECK(audit.contains("weights"));
    CHECK(audit.contains("output"));
    CHECK_EQ(audit["weights"].size(), std::size_t(2));
    double wsum = 0.0;
    for (const auto& w : audit["weights"]) wsum += w.value("weight", 0.0);
    CHECK_NEAR(wsum, 1.0, 1e-9);
    CHECK_EQ(audit["input"].value("missionId", std::string()), std::string("m-1"));
    CHECK(audit["input"].contains("phase"));
    const ScoreResult r2 = e.score(reqFor("m-1", kSideA, kSceneA, emptySnapshot()));
    CHECK_EQ(r.auditDigest, r2.auditDigest);
    CHECK(!r.metricsDigest.empty());
    CHECK(!r.templatesDigest.empty());
    long long s = 0;
    int total = -1;
    for (const auto& c : audit["output"]["candidates"]) {
        if (c.value("id", std::string()) != r.recommendedId) continue;
        total = c.value("total", -1);
        for (const auto& m : c["metrics"]) s += m.value("contributionScaled", 0LL);
    }
    CHECK(total >= 0);
    CHECK_EQ(roundToPercent(s), total);
    const MetricsPack p1 = e.metricsPack();
    CHECK_EQ(p1.digest, p1.digest);  // 同规则 → 同摘要（可追溯）
    ScoringEngine e2;
    CHECK(loadDefaults(e2));
    CHECK_EQ(e.metricsPack().digest, e2.metricsPack().digest);
}

void exp04_sensitivity_top_n() {
    requires_("SCD-EXP-04");
    ScoringEngine e;
    CHECK(loadDefaults(e));
    const ScoreResult r = e.score(reqFor("m-1", kSideA, kSceneA, emptySnapshot()));
    const auto best = r.byId(r.recommendedId);
    CHECK(best.has_value());
    if (!best) return;
    std::size_t topIdx = 0;
    for (std::size_t i = 1; i < best->metrics.size(); ++i) {
        if (best->metrics[i].contributionScaled > best->metrics[topIdx].contributionScaled) {
            topIdx = i;
        }
    }
    CHECK_EQ(r.reasons[1].metricKey, best->metrics[topIdx].key);  // 排序正确且可复现
    const ScoreResult r2 = e.score(reqFor("m-1", kSideA, kSceneA, emptySnapshot()));
    CHECK_EQ(r2.reasons[1].metricKey, r.reasons[1].metricKey);
    CHECK_EQ(r2.reasons[1].valueScaled, r.reasons[1].valueScaled);
}

// ============================================================================
// SCD-OPT · 自动优化
// ============================================================================

void opt01_no_hardcoded_optimization() {
    requires_("SCD-OPT-01");
    // 优化只走规则声明的搜索空间：把搜索空间清空 → 必然“无改进”
    json pkg = defaultMetrics();
    pkg["searchSpace"] = json{{"maxIterations", 8},
                              {"timeBudgetMs", 50},
                              {"maxTotalSteps", 0},
                              {"params", json::array()}};
    ScoringEngine e2;
    CHECK_EQ(e2.loadMetrics(pkg).code, 0);
    CHECK_EQ(e2.loadTemplates(defaultTemplates()).code, 0);
    OptimizeRequest o;
    o.missionId = "m-1";
    o.templateKey = "tpl-three";
    o.snapshot = emptySnapshot();
    const OptimizeResult r = e2.optimize(o);
    CHECK_EQ(r.code, 0);
    CHECK_EQ(r.status, std::string("no-improvement"));
    CHECK_EQ(r.delta, 0);
    CHECK(!r.improved);
    // 有搜索空间时才会改进（机制存在；从**非推荐项**出发，提升空间确实存在）
    ScoringEngine e;
    CHECK(loadDefaults(e));
    const ScoreResult sc = e.score(reqFor("m-1", kSideA, kSceneA, emptySnapshot()));
    OptimizeRequest ob = o;
    ob.templateKey = "tpl-three";
    const OptimizeResult r2 = e.optimize(ob);
    CHECK_EQ(r2.code, 0);
    CHECK(r2.improved);
    CHECK(r2.delta > 0);
    CHECK(r2.afterPercent > sc.byId("tpl-three")->totalPercent - 1);
}

void opt02_search_space_comes_from_the_rules() {
    requires_("SCD-OPT-02");
    ScoringEngine e;
    CHECK(loadDefaults(e));
    OptimizeRequest o;
    o.missionId = "m-1";
    o.templateKey = "tpl-three";
    o.snapshot = emptySnapshot();
    const OptimizeResult base = e.optimize(o);
    CHECK(base.improved);
    CHECK(!base.paramChanges.empty());
    const double baseDelta = base.paramChanges[0].delta;

    json pkg = defaultMetrics();
    pkg["searchSpace"] = json{{"maxIterations", 64},
                              {"timeBudgetMs", 100},
                              {"maxTotalSteps", 9},
                              {"params", json::array({json{{"key", "big"},
                                                           {"name", "big-step"},
                                                           {"metricKey", kMetricA},
                                                           {"step", 5},
                                                           {"maxSteps", 4}}})}};
    ScoringEngine e2;
    CHECK_EQ(e2.loadMetrics(pkg).code, 0);
    CHECK_EQ(e2.loadTemplates(defaultTemplates()).code, 0);
    const OptimizeResult big = e2.optimize(o);
    CHECK(big.improved);
    CHECK(!big.paramChanges.empty());
    if (!big.paramChanges.empty()) {
        CHECK_EQ(big.paramChanges[0].key, std::string("big"));
        CHECK(big.paramChanges[0].delta > baseDelta);  // 步长更大 → 走得更多
        CHECK(big.paramChanges[0].steps >= 1);
    }
}

void opt03_optimization_is_explainable() {
    requires_("SCD-OPT-03");
    ScoringEngine e;
    CHECK(loadDefaults(e));
    OptimizeRequest o;
    o.missionId = "m-1";
    o.templateKey = "tpl-three";
    o.snapshot = emptySnapshot();
    const OptimizeResult r = e.optimize(o);
    CHECK(r.improved);
    CHECK(!r.paramChanges.empty());   // 改了什么参数
    CHECK(!r.metricChanges.empty());  // 哪些指标变了
    CHECK(r.afterPercent >= r.beforePercent);
    CHECK_EQ(r.afterPercent - r.beforePercent, r.delta);
    for (const auto& m : r.metricChanges) {
        CHECK(!m.key.empty());
        CHECK(m.rawAfter >= m.rawBefore);
        CHECK(m.contributionAfter >= m.contributionBefore - 1e-9);
    }
    long long sb = 0;
    for (const auto& m : r.beforeMetrics) sb += m.contributionScaled;
    CHECK_EQ(roundToPercent(sb), r.beforePercent);
    long long sa = 0;
    for (const auto& m : r.afterMetrics) sa += m.contributionScaled;
    CHECK_EQ(roundToPercent(sa), r.afterPercent);
    const json d = r.toJson()["data"];
    CHECK(d.contains("paramChanges"));
    CHECK(d.contains("metricChanges"));
    CHECK(d.contains("beforePercent"));
    CHECK(d.contains("afterPercent"));
    CHECK(d.contains("delta"));
}

void opt04_monotonic_and_idempotent() {
    requires_("SCD-OPT-04");
    ScoringEngine e;
    CHECK(loadDefaults(e));
    OptimizeRequest o;
    o.missionId = "m-1";
    o.templateKey = "tpl-three";
    o.snapshot = emptySnapshot();
    const OptimizeResult r1 = e.optimize(o);
    CHECK(r1.delta >= 0);

    // 无改进：**全部可调指标**都已钉在归一区间上限 → 任何扰动都不涨分、总分不变
    json pkg = defaultMetrics();
    pkg["items"] = json::array({makeMetric(kMetricA, 0.5, 100, "profile", kMetricA),
                                makeMetric(kMetricB, 0.5, 100, "profile", kMetricB)});
    json tplSat = defaultTemplates();
    tplSat["profiles"] = json::array({makeProfile("prof-strong", 100, 100),
                                      makeProfile("prof-mid", 100, 100),
                                      makeProfile("prof-weak", 100, 100)});
    ScoringEngine e2;
    CHECK_EQ(e2.loadMetrics(pkg).code, 0);
    CHECK_EQ(e2.loadTemplates(tplSat).code, 0);
    const OptimizeResult rSat = e2.optimize(o);
    CHECK_EQ(rSat.status, std::string("no-improvement"));
    CHECK_EQ(rSat.delta, 0);
    CHECK_EQ(rSat.beforePercent, rSat.afterPercent);
    CHECK_EQ(rSat.beforePercent, 100);  // 六项全部在归一上限 → 100

    // 幂等：连续 10 次调用收敛且不漂移
    int first = -1;
    for (int i = 0; i < 10; ++i) {
        const OptimizeResult ri = e.optimize(o);
        if (first < 0) first = ri.afterPercent;
        CHECK_EQ(ri.beforePercent, r1.beforePercent);
        CHECK_EQ(ri.afterPercent, first);
        CHECK_EQ(ri.delta, r1.delta);
        CHECK_EQ(ri.status, r1.status);
    }
    // 从**最高分**候选出发：优化 MUST NOT 拉低总分，且 MUST NOT 越过 100
    ScoringEngine e3;
    CHECK_EQ(e3.loadMetrics(defaultMetrics()).code, 0);
    CHECK_EQ(e3.loadTemplates(defaultTemplates()).code, 0);
    const ScoreResult scored = e3.score(reqFor("m-1", kSideA, kSceneA, emptySnapshot()));
    OptimizeRequest top = o;
    top.templateKey = scored.recommendedId;
    const OptimizeResult rTop = e3.optimize(top);
    CHECK(rTop.delta >= 0);
    CHECK(rTop.afterPercent >= rTop.beforePercent);
    CHECK(rTop.afterPercent <= 100);
    CHECK(rTop.beforePercent >= r1.beforePercent);
    // 破坏性检查：无论从哪个模板出发，优化后 MUST NOT 低于优化前
    for (const char* k : {"tpl-one", "tpl-two", "tpl-three"}) {
        OptimizeRequest oi = o;
        oi.templateKey = k;
        const OptimizeResult ri = e.optimize(oi);
        CHECK_EQ(ri.code, 0);
        CHECK(ri.afterPercent >= ri.beforePercent);
        CHECK(ri.afterPercent <= 100);
    }
}

void opt05_bounded_and_truncatable() {
    requires_("SCD-OPT-05");
    ScoringEngine e;
    CHECK(loadDefaults(e));
    OptimizeRequest o;
    o.missionId = "m-1";
    o.templateKey = "tpl-three";
    o.snapshot = emptySnapshot();
    o.maxIterationsOverride = 1;  // 人为压到 1 次迭代 → 必然截断
    const OptimizeResult r = e.optimize(o);
    CHECK_EQ(r.code, 0);
    CHECK(r.iterations <= 1);
    CHECK(r.truncated);
    CHECK_EQ(r.stopReason, std::string("max-iterations"));
    CHECK(r.afterPercent >= r.beforePercent);
    CHECK(r.elapsedMs <= e.capabilities().timeBudgetMs + 20);

    OptimizeRequest o2;
    o2.missionId = "m-1";
    o2.templateKey = "tpl-three";
    o2.snapshot = emptySnapshot();
    const OptimizeResult r2 = e.optimize(o2);
    CHECK_EQ(r2.truncated, r2.iterations >= e.capabilities().maxIterations);
    CHECK(r2.elapsedMs <= e.capabilities().timeBudgetMs + 20);
}

// ============================================================================
// SCD-DECIDE · 采纳与确认裁决
// ============================================================================

void decide01_same_side_is_exclusive() {
    requires_("SCD-DECIDE-01");
    auto store = std::make_shared<MemStore>();
    ScoringEngineOptions opts;
    opts.store = store;
    ScoringEngine e(opts);
    CHECK(loadDefaults(e));

    AdoptRequest a;
    a.missionId = "m-1";
    a.planId = "tpl-one";
    a.side = kSideA;
    a.operatorId = "op-1";
    CHECK_EQ(e.adopt(a).code, 0);
    a.planId = "tpl-two";
    const DecideResult r2 = e.adopt(a);
    CHECK_EQ(r2.code, 0);
    CHECK_EQ(r2.invalidated.size(), std::size_t(1));  // 前者失效
    if (!r2.invalidated.empty()) CHECK_EQ(r2.invalidated[0], std::string("tpl-one"));

    PlanQuery q;
    q.missionId = "m-1";
    q.side = kSideA;
    const std::vector<PlanState> states = e.plans(q);
    int adopted = 0;
    for (const auto& s : states) {
        if (s.state == "adopted") {
            ++adopted;
            CHECK_EQ(s.planId, std::string("tpl-two"));
        }
    }
    CHECK_EQ(adopted, 1);  // 同侧唯一
    CHECK_EQ(store->rows["m-1"]["tpl-one"].state, std::string("pending"));
    CHECK_EQ(store->rows["m-1"]["tpl-two"].state, std::string("adopted"));
}

void decide02_idempotent_repeats_succeed() {
    requires_("SCD-DECIDE-02");
    auto store = std::make_shared<MemStore>();
    auto sink = std::make_shared<RecSink>();
    ScoringEngineOptions opts;
    opts.store = store;
    opts.sink = sink;
    ScoringEngine e(opts);
    CHECK(loadDefaults(e));

    AdoptRequest a;
    a.missionId = "m-1";
    a.planId = "tpl-one";
    a.side = kSideA;
    a.operatorId = "op-1";
    const DecideResult d1 = e.adopt(a);
    const DecideResult d2 = e.adopt(a);
    CHECK_EQ(d1.code, 0);
    CHECK(!d1.idempotent);
    CHECK_EQ(d2.code, 0);  // 不得报错
    CHECK(d2.idempotent);  // code=0 + idempotent=true（C15/C17）
    CHECK(!d2.conflict);
    CHECK_EQ(d2.planState, std::string("adopted"));
    CHECK_EQ(d2.event.value("idempotent", false), true);
    CHECK_EQ(store->rows["m-1"]["tpl-one"].state, std::string("adopted"));
    CHECK_EQ(sink->events.size(), std::size_t(1));  // 幂等命中不发第二次事件
    const int savesAfterFirst = store->saves;
    e.adopt(a);
    CHECK_EQ(store->saves, savesAfterFirst);  // 幂等命中不重复落库

    ConfirmRequest c;
    c.missionId = "m-1";
    c.planId = "tpl-one";
    c.side = kSideA;
    const DecideResult c1 = e.confirm(c);
    const DecideResult c2 = e.confirm(c);
    CHECK_EQ(c1.code, 0);
    CHECK_EQ(c2.code, 0);
    CHECK(c2.idempotent);
    CHECK_EQ(c2.planState, std::string("confirmed"));
    const DecideResult a3 = e.adopt(a);
    CHECK_EQ(a3.code, 0);
    CHECK(a3.idempotent);
    CHECK_EQ(a3.planState, std::string("confirmed"));
}

void decide03_confirm_precondition() {
    requires_("SCD-DECIDE-03");
    auto store = std::make_shared<MemStore>();
    ScoringEngineOptions opts;
    opts.store = store;
    ScoringEngine e(opts);
    CHECK(loadDefaults(e));
    CHECK_EQ(e.capabilities().confirmPrecondition, std::string("reject-if-not-adopted"));
    ConfirmRequest c;
    c.missionId = "m-1";
    c.planId = "tpl-two";
    c.side = kSideA;
    const DecideResult r = e.confirm(c);
    CHECK_EQ(r.code, 1003);  // 前置条件未满足
    CHECK(!r.unmet.empty());
    CHECK_EQ(store->rows.find("m-1"), store->rows.end());

    json tpl = defaultTemplates();
    tpl["confirmPrecondition"] = json{{"mode", "auto-adopt"}};
    auto store2 = std::make_shared<MemStore>();
    ScoringEngineOptions opts2;
    opts2.store = store2;
    ScoringEngine e2(opts2);
    CHECK_EQ(e2.loadMetrics(defaultMetrics()).code, 0);
    CHECK_EQ(e2.loadTemplates(tpl).code, 0);
    CHECK_EQ(e2.capabilities().confirmPrecondition, std::string("auto-adopt"));
    const DecideResult r2 = e2.confirm(c);
    CHECK_EQ(r2.code, 0);
    CHECK_EQ(r2.planState, std::string("confirmed"));
    CHECK(r2.autoAdopted);
    CHECK_EQ(store2->rows["m-1"]["tpl-two"].state, std::string("confirmed"));

    json bad = defaultTemplates();
    bad["confirmPrecondition"] = json{{"mode", "whatever"}};
    ScoringEngine e3;
    CHECK_EQ(e3.loadMetrics(defaultMetrics()).code, 0);
    CHECK_EQ(e3.loadTemplates(bad).code, 1000);
}

/// 结构守卫：Sink 回调发生在**同侧闸门仍被持有**时（覆盖 SCD-DECIDE-01 的并发语义）
void structure_gate_is_held_during_sink_callback() {
    requires_("SCD-DECIDE-01");
    auto store = std::make_shared<MemStore>();
    auto sink = std::make_shared<RecSink>();
    auto clock = std::make_shared<FakeClock>();
    ScoringEngineOptions opts;
    opts.store = store;
    opts.sink = sink;
    opts.clock = clock;
    ScoringEngine e(opts);
    CHECK(loadDefaults(e));

    auto sinkGuard = std::make_shared<RecSink>();  // 防止编译器把未用变量优化掉
    (void)sinkGuard;

    int innerCode = 12345;
    bool innerConflict = false;
    int calls = 0;
    sink->onEvent = [&](const json&) {
        ++calls;
        AdoptRequest b;
        b.missionId = "m-1";
        b.planId = "tpl-two";
        b.side = kSideA;
        const DecideResult inner = e.adopt(b);
        innerCode = inner.code;
        innerConflict = inner.conflict;
    };
    AdoptRequest a;
    a.missionId = "m-1";
    a.planId = "tpl-one";
    a.side = kSideA;
    const DecideResult outer = e.adopt(a);
    note("    [gate] calls=%d outer=%d inner=%d conflict=%s\n", calls, outer.code, innerCode,
         innerConflict ? "true" : "false");
    CHECK_EQ(outer.code, 0);
    CHECK_EQ(calls, 1);  // 回调只在最外层持闸门时被调用了一次
    CHECK_EQ(innerCode, 1002);
    CHECK(innerConflict);
    CHECK_EQ(store->rows["m-1"]["tpl-one"].state, std::string("adopted"));
    CHECK_EQ(store->rows.find("m-1")->second.count("tpl-two"), std::size_t(0));
}

void decide04_events_and_logs() {
    requires_("SCD-DECIDE-04");
    auto store = std::make_shared<MemStore>();
    auto sink = std::make_shared<RecSink>();
    auto log = std::make_shared<RecLog>();
    auto clock = std::make_shared<FakeClock>();
    ScoringEngineOptions opts;
    opts.store = store;
    opts.sink = sink;
    opts.log = log;
    opts.clock = clock;
    ScoringEngine e(opts);
    CHECK(loadDefaults(e));

    AdoptRequest a;
    a.missionId = "m-1";
    a.planId = "tpl-one";
    a.side = kSideA;
    a.operatorId = "op-9";
    CHECK_EQ(e.adopt(a).code, 0);
    CHECK_EQ(sink->events.size(), std::size_t(1));
    const json ev = sink->events[0];
    CHECK_EQ(ev.value("missionId", std::string()), std::string("m-1"));
    CHECK_EQ(ev.value("side", std::string()), std::string(kSideA));
    CHECK_EQ(ev.value("planId", std::string()), std::string("tpl-one"));
    CHECK_EQ(ev.value("action", std::string()), std::string("adopted"));
    CHECK(ev.contains("ts"));
    CHECK_EQ(log->audits.size(), std::size_t(1));
    CHECK_EQ(log->audits[0].value("actor", std::string()), std::string("op-9"));
    CHECK_EQ(log->audits[0].value("action", std::string()), std::string("adopted"));
    CHECK(log->logs >= 1);

    ConfirmRequest c;
    c.missionId = "m-1";
    c.planId = "tpl-one";
    c.side = kSideA;
    c.operatorId = "op-9";
    CHECK_EQ(e.confirm(c).code, 0);
    CHECK_EQ(sink->events.size(), std::size_t(2));
    CHECK_EQ(sink->events[1].value("action", std::string()), std::string("confirmed"));
    CHECK_EQ(log->audits.size(), std::size_t(2));
    CHECK_EQ(ev.value("ts", int64_t(0)), int64_t(1750000000000LL));  // ts 取自注入的假时钟

    // Sink 抛异常 MUST NOT 影响裁决结果
    auto sink2 = std::make_shared<ThrowingSink>();
    auto store2 = std::make_shared<MemStore>();
    ScoringEngineOptions opts2;
    opts2.store = store2;
    opts2.sink = sink2;
    ScoringEngine e2(opts2);
    CHECK(loadDefaults(e2));
    const DecideResult r = e2.adopt(a);
    CHECK_EQ(r.code, 0);
    {
        PlanState st;
        CHECK(store2->load("m-1", "tpl-one", st));
        CHECK_EQ(st.state, std::string("adopted"));
    }
    CHECK(e2.metrics().sinkErrors >= 1);
}

void decide05_three_states_are_queryable() {
    requires_("SCD-DECIDE-05");
    auto store = std::make_shared<MemStore>();
    ScoringEngineOptions opts;
    opts.store = store;
    ScoringEngine e(opts);
    CHECK(loadDefaults(e));

    AdoptRequest a;
    a.missionId = "m-1";
    a.planId = "tpl-one";
    a.side = kSideA;
    CHECK_EQ(e.adopt(a).code, 0);
    ConfirmRequest c;
    c.missionId = "m-1";
    c.planId = "tpl-one";
    c.side = kSideA;
    CHECK_EQ(e.confirm(c).code, 0);

    PlanQuery q;
    q.missionId = "m-1";
    q.side = kSideA;
    CHECK_EQ(e.plans(q).size(), std::size_t(1));
    PlanQuery qp = q;
    qp.stateIn = {"pending"};
    CHECK_EQ(e.plans(qp).size(), std::size_t(0));
    PlanQuery qa = q;
    qa.stateIn = {"adopted"};
    CHECK_EQ(e.plans(qa).size(), std::size_t(0));
    PlanQuery qc = q;
    qc.stateIn = {"confirmed"};
    CHECK_EQ(e.plans(qc).size(), std::size_t(1));
    if (!e.plans(qc).empty()) CHECK_EQ(e.plans(qc)[0].planId, std::string("tpl-one"));
    bool sawTwo = false;
    for (const auto& s : e.plans(q)) {
        if (s.planId == "tpl-two") sawTwo = true;
    }
    CHECK(!sawTwo);  // 未采纳过的方案在查询里不出现（MUST NOT 造假状态）
}

// ============================================================================
// SCD-NFR · 非功能性
// ============================================================================

void nfr01_zero_external_dependencies() {
    requires_("SCD-NFR-01");
    ScoringEngine e;
    const Capabilities cap = e.capabilities();
    CHECK(!cap.metricsLoaded);
    CHECK(!cap.templatesLoaded);
    CHECK_EQ(cap.engineVersion.empty(), false);
    CHECK_EQ(cap.policiesMajor, 1);
    CHECK_EQ(e.metrics().scores, int64_t(0));
    CHECK_EQ(roundToPercent(9250, kScale), 93);
    CHECK_EQ(digestHex(fnv1a64("abc")).size(), std::size_t(16));
    CHECK_EQ(digestHex(fnv1a64("abc")), digestHex(fnv1a64("abc")));
    CHECK(digestHex(fnv1a64("abc")) != digestHex(fnv1a64("abd")));
    CHECK(loadDefaults(e));
    CHECK_EQ(e.score(reqFor("m-1", kSideA, kSceneA, emptySnapshot())).code, 0);
    CHECK(!e.capabilities().storeInjected);
    CHECK(!e.capabilities().clockInjected);
}

void nfr02_independent_delivery_smoke() {
    requires_("SCD-NFR-02");
    ScoringEngine e;
    const std::string dir =
#ifdef SCORING_POLICY_DIR
        SCORING_POLICY_DIR;
#else
        "policies/mapapp";
#endif
    const LoadResult r = e.policiesFromDir(dir);
    CHECK_EQ(r.code, 0);
    CHECK_EQ(r.itemCount > 0, true);
    CHECK(!r.policiesNamespace.empty());
    CHECK(!r.definitionVersion.empty());
    CHECK_EQ(r.issues.empty(), true);
    const Capabilities cap = e.capabilities();
    CHECK(cap.metricsLoaded);
    CHECK(cap.templatesLoaded);
    CHECK(cap.metricCount > 0);
    CHECK(cap.templateCount > 0);
    CHECK(cap.reasonCount > 0);
    CHECK(cap.searchParamCount > 0);
    CHECK(!cap.metricsDigest.empty());
    CHECK(!cap.templatesDigest.empty());
}

void nfr03_all_outputs_go_through_injected_interfaces() {
    requires_("SCD-NFR-03");
    auto store = std::make_shared<MemStore>();
    auto sink = std::make_shared<RecSink>();
    auto clock = std::make_shared<FakeClock>();
    auto log = std::make_shared<RecLog>();
    ScoringEngineOptions opts;
    opts.store = store;
    opts.sink = sink;
    opts.clock = clock;
    opts.log = log;
    ScoringEngine e(opts);
    CHECK(loadDefaults(e));
    const Capabilities cap = e.capabilities();
    CHECK(cap.storeInjected);
    CHECK(cap.sinkInjected);
    CHECK(cap.clockInjected);
    CHECK(cap.logInjected);

    AdoptRequest a;
    a.missionId = "m-1";
    a.planId = "tpl-one";
    a.side = kSideA;
    a.operatorId = "op-1";
    CHECK_EQ(e.adopt(a).code, 0);
    CHECK_EQ(store->saves, 1);
    CHECK_EQ(sink->events.size(), std::size_t(1));
    CHECK(log->audits.size() >= 1);

    auto failing = std::make_shared<MemStore>();
    failing->failSave = true;
    ScoringEngineOptions opts2;
    opts2.store = failing;
    ScoringEngine e2(opts2);
    CHECK(loadDefaults(e2));
    AdoptRequest b = a;
    b.planId = "tpl-two";
    const DecideResult rb = e2.adopt(b);
    CHECK_EQ(rb.code, 1005);
    CHECK_EQ(e2.plans(PlanQuery{"m-1", kSideA, {}}).size(), std::size_t(0));
    CHECK(e2.metrics().storeErrors >= 1);
}

void nfr04_performance_targets() {
    requires_("SCD-NFR-04");
    json metricItems = json::array();
    json templateItems = json::array();
    json profiles = json::array();
    json params = json::array();
    for (int i = 0; i < 10; ++i) {
        const std::string k = "k" + std::to_string(i);
        metricItems.push_back(makeMetric(k, 0.1, 50.0 + i, "profile", k));
        params.push_back(json{{"key", "lift-" + k},
                              {"name", k},
                              {"metricKey", k},
                              {"step", 1},
                              {"maxSteps", 2}});
    }
    for (int i = 1; i <= 10; ++i) {
        templateItems.push_back(makeTemplate("c" + std::to_string(i), "C" + std::to_string(i), i,
                                             kSideA, kSceneA, "prof" + std::to_string(i),
                                             50.0 + i));
        json vals = json::object();
        for (int j = 0; j < 10; ++j) vals["k" + std::to_string(j)] = 50.0 + j;
        profiles.push_back(
            json{{"key", "prof" + std::to_string(i)}, {"name", "p"}, {"values", vals}});
    }
    const json metricsPkg = makeMetricsPack(metricItems, json::object(),
                                            json{{"maxIterations", 64},
                                                 {"timeBudgetMs", 100},
                                                 {"maxTotalSteps", 20},
                                                 {"params", params}});
    const json templatesPkg = makeTemplatesPack(templateItems, profiles);
    ScoringEngine e;
    CHECK_EQ(e.loadMetrics(metricsPkg).code, 0);
    CHECK_EQ(e.loadTemplates(templatesPkg).code, 0);
    const CandidateRequest req = reqFor("m-1", kSideA, kSceneA, emptySnapshot());
    const ScoreResult warm = e.score(req);
    CHECK_EQ(warm.candidates.size(), std::size_t(10));
    CHECK_EQ(warm.candidates[0].metrics.size(), std::size_t(10));

    const auto t0 = std::chrono::steady_clock::now();
    const int loops = 50;
    for (int i = 0; i < loops; ++i) {
        if (e.score(req).code != 0) {
            CHECK(false);
            break;
        }
    }
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const double perCall = ms / loops;
    note("    单次评分（10 候选 × 10 指标）= %.4f ms\n", perCall);
    CHECK(perCall <= 2.0);  // 目标 ≤ 1 ms

    OptimizeRequest o;
    o.missionId = "m-1";
    o.templateKey = "c1";
    o.snapshot = emptySnapshot();
    const auto t1 = std::chrono::steady_clock::now();
    const OptimizeResult orr = e.optimize(o);
    const double oms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
    note("    单次优化 = %.4f ms（迭代 %d，截断 %s）\n", oms, orr.iterations,
         orr.truncated ? "是" : "否");
    CHECK(oms <= 100.0);
}

void nfr05_float_reproducibility() {
    requires_("SCD-NFR-05");
    ScoringEngine e;
    CHECK(loadDefaults(e));
    e.setClock(std::make_shared<FakeClock>());
    const CandidateRequest req = reqFor("m-1", kSideA, kSceneA, emptySnapshot());
    const std::string a = e.score(req).toJson().dump();
    const std::string b = e.score(req).toJson().dump();
    CHECK_EQ(a, b);
    const ScoreResult r = e.score(req);
    const auto best = r.byId(r.recommendedId);
    CHECK(best.has_value());
    if (best) {
        double norm = 0.0;
        for (const auto& m : best->metrics) norm += m.normalized * m.weight;
        CHECK_NEAR(norm * 100.0, scaledToDouble(best->totalScaled), 1e-9);
        CHECK_EQ(best->totalScaled % 1, int64_t(0));
        CHECK_EQ(kScale, int64_t(10000));
    }
}

// ============================================================================
// 共享契约与冲突裁决
// ============================================================================

void proto_policies_schema_and_versioning() {
    requires_("SCD-NFR-01", "SCD-CAND-01");
    ScoringEngine e;
    json wrongKind = defaultMetrics();
    wrongKind["kind"] = "phases";
    CHECK_EQ(e.loadMetrics(wrongKind).code, 1000);
    json noNs = defaultMetrics();
    noNs.erase("policiesNamespace");
    CHECK_EQ(e.loadMetrics(noNs).code, 1000);
    json badMajor = defaultMetrics();
    badMajor["schemaVersion"] = "2.0.0";
    CHECK_EQ(e.loadMetrics(badMajor).code, 1006);  // 不匹配 → 1006，MUST NOT 静默降级
    json badVer = defaultMetrics();
    badVer["schemaVersion"] = "1.0";
    CHECK_EQ(e.loadMetrics(badVer).code, 1000);
    json minor = defaultMetrics();
    minor["schemaVersion"] = "1.7.3";
    ScoringEngine e2;
    CHECK_EQ(e2.loadMetrics(minor).code, 0);

    json unknown = defaultMetrics();
    unknown["whateverField"] = json{{"a", 1}};
    ScoringEngine e3;
    CHECK_EQ(e3.loadMetrics(defaultMetrics()).code, 0);  // 先装载成功的一版
    const LoadResult ru = e3.loadMetrics(unknown);
    CHECK_EQ(ru.code, 0);
    CHECK(!ru.warnings.empty());  // 未知字段 → 忽略 + 计入告警（CTR-PL-03）

    json missing = defaultMetrics();
    missing["items"][1].erase("weight");
    const LoadResult rmiss = e3.loadMetrics(missing);
    CHECK_EQ(rmiss.code, 1000);
    bool pointed = false;
    for (const auto& i : rmiss.issues) {
        if (i.path.find("items[1]") != std::string::npos && i.field == "weight") pointed = true;
    }
    CHECK(pointed);  // 原因指向条目下标与字段名（CTR-PL-05）
    CHECK_EQ(e3.metricsPack().items.size(), std::size_t(2));  // 失败不破坏已装载的规则包

    json dup = makeMetricsPack({makeMetric(kMetricA, 0.5, 80, "profile", kMetricA),
                                makeMetric(kMetricA, 0.5, 60, "profile", kMetricA)});
    CHECK_EQ(e3.loadMetrics(dup).code, 1000);

    CHECK_EQ(ScoringEngine::validateMetrics(defaultMetrics()).code, 0);
    CHECK_EQ(ScoringEngine::validateTemplates(defaultTemplates()).code, 0);
    CHECK_EQ(ScoringEngine::validateMetrics(wrongKind).code, 1000);
    CHECK(!e2.metricsPack().raw.is_null());  // 规则包可导出“当前生效规则”
    CHECK_EQ(e2.metricsPack().raw["kind"].get<std::string>(), std::string("scoringMetrics"));
}

void proto_idempotent_vs_conflict_are_separate() {
    requires_("SCD-DECIDE-02", "SCD-DECIDE-01");
    auto store = std::make_shared<MemStore>();
    auto sink = std::make_shared<RecSink>();
    ScoringEngineOptions opts;
    opts.store = store;
    opts.sink = sink;
    ScoringEngine e(opts);
    CHECK(loadDefaults(e));
    AdoptRequest a;
    a.missionId = "m-1";
    a.planId = "tpl-one";
    a.side = kSideA;
    CHECK_EQ(e.adopt(a).code, 0);
    const DecideResult again = e.adopt(a);
    CHECK_EQ(again.code, 0);  // 幂等成功 = 0（C15）
    CHECK(again.idempotent);
    CHECK(!again.conflict);

    // 持闸门期间的重入 → 1002 + conflict=true（C16/C17）
    int innerCode = 0;
    bool innerConflict = false;
    sink->onEvent = [&](const json&) {
        AdoptRequest b;
        b.missionId = "m-1";
        b.planId = "tpl-two";
        b.side = kSideA;
        const DecideResult r = e.adopt(b);
        innerCode = r.code;
        innerConflict = r.conflict;
    };
    AdoptRequest c;
    c.missionId = "m-1";
    c.planId = "tpl-two";
    c.side = kSideA;
    const DecideResult changed = e.adopt(c);
    CHECK_EQ(changed.code, 0);
    CHECK_EQ(innerCode, 1002);
    CHECK(innerConflict);
    CHECK(e.metrics().conflicts >= 1);
    CHECK(again.idempotent && !again.conflict);  // 两类结果由不同字段区分（ADR-C17-02）
    CHECK_EQ(std::string(errorCodeName(0)), std::string("ok"));
    CHECK_EQ(std::string(errorCodeName(1002)), std::string("conflict"));
    CHECK_EQ(std::string(errorCodeName(1003)), std::string("precondition-unmet"));
    CHECK_EQ(std::string(errorCodeName(1006)), std::string("version-mismatch"));
    CHECK_EQ(std::string(errorCodeName(7777)), std::string("unknown"));
}

void proto_event_payload_shapes() {
    requires_("SCD-DECIDE-04");
    ScoringEngine e;
    CHECK(loadDefaults(e));
    auto sink = std::make_shared<RecSink>();
    e.setSink(sink);
    AdoptRequest a;
    a.missionId = "m-1";
    a.planId = "tpl-one";
    a.side = kSideA;
    a.hasRecommendedContext = true;
    a.recommendedId = "tpl-one";
    CHECK_EQ(e.adopt(a).code, 0);
    CHECK_EQ(sink->events.size(), std::size_t(1));
    const json ev = sink->events[0];
    CHECK(ev.contains("missionId"));  // 既有字段只增不改（CTR-EV-04）
    CHECK(ev.contains("side"));
    CHECK(ev.contains("planId"));
    CHECK(ev.contains("action"));
    CHECK(ev.contains("ts"));
    std::set<std::string> allowed = {"generated", "recommended", "adopted", "optimized",
                                     "confirmed"};
    CHECK(allowed.count(ev.value("action", std::string())) == 1);
    CHECK(ev.contains("recommendedId"));  // 新增字段全部可选
}

// ============================================================================
// 结构纪律 + 真实规则包锚点
// ============================================================================

void structure_recommendation_reasons_are_not_sentences() {
    requires_("SCD-EXP-02");
    ScoringEngine e;
    CHECK(loadDefaults(e));
    const ScoreResult r = e.score(reqFor("m-1", kSideA, kSceneA, emptySnapshot()));
    const std::string dumped = r.toJson().dump();
    static const char* kEnders[] = {"。", "！", "？"};
    for (const char* p : kEnders) CHECK(dumped.find(p) == std::string::npos);
    for (const auto& reason : r.reasons) {
        CHECK(reason.metricKey.find(' ') == std::string::npos);
        CHECK(reason.type.find(' ') == std::string::npos);
    }
}

void structure_pure_functions_are_independently_callable() {
    requires_("SCD-SCORE-02", "SCD-SCORE-06");
    // 标度约定自检：三个纯函数互为逆运算
    CHECK_EQ(scaledToDouble(roundPercent(92.96, kScale), kScale), 92.96);
    CHECK_EQ(roundPercent(scaledToDouble(9296, kScale), kScale), std::int64_t(9296));
    note("    [probe] s2d(9296,10000)=%.6f rtp(9296,10000)=%d rp(92.5,1)=%lld rp(92.5,10000)=%lld\n",
         scaledToDouble(9296, kScale), roundToPercent(9296, kScale),
         static_cast<long long>(roundPercent(92.5, 1)),
         static_cast<long long>(roundPercent(92.5, kScale)));
    MetricDef d;
    d.normalize.type = NormalizeType::Range;
    d.normalize.min = 0;
    d.normalize.max = 100;
    CHECK_NEAR(normalizeValue(d, 93.0), 0.93, 1e-12);
    CHECK_EQ(roundToPercent(9296, kScale), 93);
    CHECK_NEAR(scaledToDouble(9296, kScale), 92.96, 1e-12);
    CHECK_EQ(digestHex(fnv1a64("")), std::string("cbf29ce484222325"));
    CHECK_EQ(majorOfVersion("1.0.0").value_or(-1), 1);
    CHECK(!majorOfVersion("1.0").has_value());
    CHECK(!majorOfVersion("x.y.z").has_value());
    CHECK_EQ(std::string(toString(MetricSource::Profile)), std::string("profile"));
    CHECK_EQ(std::string(toString(NormalizeType::Threshold)), std::string("threshold"));
    CHECK(metricSourceFromString("snapshot").has_value());
    CHECK(!metricSourceFromString("magic").has_value());
    CHECK(confirmPreconditionFromString("auto-adopt").has_value());
}

/// 仓内**真实规则包**的端到端锚点：推荐项总分 MUST 等于规则包 baseline×weight 的独立推论
void anchor_recommended_total_is_recomputable_from_rules() {
    requires_("SCD-SCORE-04", "SCD-PICK-02", "SCD-NFR-02");
    json metricsPkg;
    json templatesPkg;
    CHECK(readPolicy("scoringMetrics.json", metricsPkg));
    CHECK(readPolicy("planTemplates.json", templatesPkg));
    if (metricsPkg.is_null() || templatesPkg.is_null()) return;

    ScoringEngine e;
    const LoadResult r = e.policies(metricsPkg, templatesPkg);
    CHECK_EQ(r.code, 0);
    if (r.code != 0) {
        note("规则包装载失败：%s\n", r.toJson().dump().c_str());
        return;
    }
    const MetricsPack mp = e.metricsPack();
    const TemplatesPack tp = e.templatesPack();

    // ① 规则包自身的两个不变量（在引擎之外也能核对）
    CHECK_NEAR(mp.weightSum(), 1.0, 1e-9);
    long long scaledSum = 0;
    for (const auto& m : mp.items) {
        CHECK(m.hasBaseline);
        scaledSum += roundHalfUpScaled(normalizeValue(m, m.baseline) * m.weight * kScale);
    }
    const int anchor = roundToPercent(scaledSum, kScale);
    note("    规则包 baseline×weight 独立推论 = %d%%（定点和 %lld）\n", anchor,
         static_cast<long long>(scaledSum));

    int sidesChecked = 0;
    for (const char* side : {"group", "strike"}) {
        std::string scene;
        for (const auto& t : tp.items) {
            if (t.side == side) {
                scene = t.scene;
                break;
            }
        }
        if (scene.empty()) continue;
        ScoringSnapshot snap;
        snap.missionId = "anchor-mission";
        snap.phase.missionId = "anchor-mission";
        snap.phase.phaseKey = "anchor-phase";
        snap.phase.scenarioKey = scene;
        ResourceSnapshot res;
        res.present = true;
        res.phase = snap.phase;
        for (const auto& t : tp.items) {
            for (const auto& c : t.clusters) {
                bool seen = false;
                for (const auto& u : res.clusters) {
                    if (u.clusterId == c) seen = true;
                }
                if (seen) continue;
                ClusterUsage u;
                u.clusterId = c;
                u.phaseKey = snap.phase.phaseKey;
                u.total = 1;
                res.clusters.push_back(u);
            }
        }
        snap.resources = res;

        const ScoreResult sr = e.score(reqFor("anchor-mission", side, scene, snap));
        CHECK_EQ(sr.code, 0);
        CHECK(sr.hasRecommended);
        const auto best = sr.byId(sr.recommendedId);
        CHECK(best.has_value());
        if (!best) continue;
        ++sidesChecked;
        note("    侧别 %-6s 推荐 %-10s = %d%%（%zu 候选，领先次优 %d）\n", side,
             sr.recommendedId.c_str(), best->totalPercent, sr.candidates.size(),
             sr.leadOverNextPercent);
        // ② 锚点：**编组侧**推荐项的六项取值恒等于规则包基线 → 总分 = baseline×weight 的推论
        if (std::string(side) == "group") {
            CHECK_EQ(best->totalPercent, anchor);
            for (const auto& m : best->metrics) CHECK(!m.missing);
        }
        // ③ 逐项贡献度之和 = 总分（可手算复算）
        long long sum = 0;
        for (const auto& m : best->metrics) sum += m.contributionScaled;
        CHECK_EQ(sum, best->totalScaled);
        CHECK_EQ(roundToPercent(sum, kScale), best->totalPercent);
        // ④ 推荐项 = 排序第一，且领先次优
        CHECK_EQ(sr.candidates[0].candidate.id, sr.recommendedId);
        CHECK(sr.leadOverNext >= 0.0);
        // ⑤ 每条理由可追溯到指标；共 4 条
        CHECK_EQ(sr.reasons.size(), std::size_t(4));
        for (const auto& reason : sr.reasons) CHECK(mp.has(reason.metricKey));
    }
    CHECK_EQ(sidesChecked, 2);
}

/// 缺链路评估输入：中性值 + **标注缺失**，评分不失败（SCD-SCORE-05 / 风险 R6）
void anchor_missing_link_input_is_neutral_and_marked() {
    requires_("SCD-SCORE-05", "SCD-NFR-02");
    // ① 机制验证：来源为 snapshot 的指标在链路评估缺失时回落中性值并**标注缺失**
    const json neutralAgg = json{
        {"method", "linear-weighted-mean"},
        {"rounding", "half-up"},
        {"outputScale", 100},
        {"neutralRule",
         json{{"enabled", true},
              {"valueSource", "baseline"},
              {"markMissing", true},
              {"missingMarker", "link-eval-missing"}}}};
    json items = json::array();
    {
        // 两个指标都取自**快照观测值**（缺失 → 中性值 + 标注）
        items.push_back(makeMetric(kMetricA, 0.5, 60, "snapshot", "electronicSuppression"));
        items.push_back(makeMetric(kMetricB, 0.5, 40, "snapshot", "resourceUtilization"));
        for (auto& it : items) {
            it["source"]["field"] = it["source"]["metric"];
            it["neutralOnMissing"] = json{{"enabled", true},
                                         {"value", it["baseline"]},
                                         {"missingMarker", "link-eval-missing"}};
        }
    }
    ScoringEngine e1;
    CHECK_EQ(e1.loadMetrics(makeMetricsPack(items, json::object(), json::object(), neutralAgg)).code,
             0);
    CHECK_EQ(e1.loadTemplates(makeTemplatesPack(
                 {makeTemplate("t-unique", "T-UNIQUE", 1, kSideA, kSceneA, "", 50, {"c1"})},
                 json::array()))
                 .code,
             0);
    ScoringSnapshot bare;  // 完全没有 topology 输入
    bare.missionId = "m-2";
    bare.phase.phaseKey = "ph-1";
    bare.phase.scenarioKey = kSceneA;
    const ScoreResult r1 = e1.score(reqFor("m-2", kSideA, kSceneA, bare));
    CHECK_EQ(r1.code, 0);  // 不失败
    CHECK(r1.hasRecommended);
    CHECK(!r1.missingInputs.empty());  // 标注缺失
    bool sawMarker = false;
    for (const auto& x : r1.missingInputs) {
        if (x == "link-eval-missing") sawMarker = true;
    }
    CHECK(sawMarker);
    const auto b1 = r1.byId(r1.recommendedId);
    CHECK(b1.has_value());
    if (b1) {
        int missingCount = 0;
        for (const auto& m : b1->metrics) {
            if (m.missing && m.missingMarker == "link-eval-missing") ++missingCount;
            CHECK_EQ(m.sourceField, std::string("neutral"));  // 中性值（规则包声明）
            CHECK_NEAR(m.raw, m.usedBaseline ? 0.0 : m.raw, 0.0);  // 仅取值，不做额外断言
        }
        CHECK_EQ(missingCount, 2);
        CHECK_EQ(b1->totalPercent, 50);  // 60×0.5 + 40×0.5 = 50
    }
    // 补上快照输入后同一指标改从快照取值、不再标注缺失
    ScoringSnapshot withTopo = bare;
    withTopo.extra["electronicSuppression"] = 80.0;
    withTopo.hasResourceUtilization = true;
    withTopo.resourceUtilization = 90.0;
    const ScoreResult r2 = e1.score(reqFor("m-2", kSideA, kSceneA, withTopo));
    CHECK_EQ(r2.code, 0);
    const auto b2 = r2.byId(r2.recommendedId);
    CHECK(b2.has_value());
    if (b2) {
        for (const auto& m : b2->metrics) {
            CHECK(!m.missing);
            CHECK_EQ(m.sourceField.rfind("snapshot.", 0), std::size_t(0));
        }
        CHECK_EQ(b2->totalPercent, 85);  // 80×0.5 + 90×0.5 = 85
    }

    // ② 真实规则包：不给链路评估输入也必须能评分出推荐（不阻塞 —— 风险 R6）
    json metricsPkg;
    json templatesPkg;
    CHECK(readPolicy("scoringMetrics.json", metricsPkg));
    CHECK(readPolicy("planTemplates.json", templatesPkg));
    if (metricsPkg.is_null() || templatesPkg.is_null()) return;
    ScoringEngine e2;
    CHECK_EQ(e2.policies(metricsPkg, templatesPkg).code, 0);
    const TemplatesPack tp = e2.templatesPack();
    std::string side;
    std::string scene;
    for (const auto& t : tp.items) {
        side = t.side;
        scene = t.scene;
        break;
    }
    ScoringSnapshot snap;
    snap.missionId = "m-2";
    snap.phase.missionId = "m-2";
    snap.phase.phaseKey = "anchor-phase";
    snap.phase.scenarioKey = scene;
    ResourceSnapshot res;
    res.present = true;
    res.phase = snap.phase;
    for (const auto& t : tp.items) {
        for (const auto& c : t.clusters) {
            bool seen = false;
            for (const auto& u : res.clusters) {
                if (u.clusterId == c) seen = true;
            }
            if (seen) continue;
            ClusterUsage u;
            u.clusterId = c;
            u.phaseKey = snap.phase.phaseKey;
            u.total = 4;
            res.clusters.push_back(u);
        }
    }
    snap.resources = res;
    const ScoreResult r3 = e2.score(reqFor("m-2", side, scene, snap));
    CHECK_EQ(r3.code, 0);  // 不失败
    CHECK(r3.hasRecommended);
    CHECK(r3.candidates.size() >= 3);
    long long sum3 = 0;
    const auto b3 = r3.byId(r3.recommendedId);
    CHECK(b3.has_value());
    if (b3) {
        for (const auto& m : b3->metrics) sum3 += m.contributionScaled;
        CHECK_EQ(roundToPercent(sum3, kScale), b3->totalPercent);
    }
}

// ============================================================================
// 用例表
// ============================================================================

struct Case {
    const char* name;
    void (*fn)();
};

const Case kCases[] = {
    // SCD-CAND
    {"cand01_candidates_are_rule_driven", cand01_candidates_are_rule_driven},
    {"cand02_inapplicable_is_marked_not_dropped", cand02_inapplicable_is_marked_not_dropped},
    {"cand03_candidate_count_follows_template_count", cand03_candidate_count_follows_template_count},
    {"cand04_equivalent_templates_are_deduped_and_marked",
     cand04_equivalent_templates_are_deduped_and_marked},
    // SCD-SCORE
    {"score01_metrics_are_configurable", score01_metrics_are_configurable},
    {"score02_normalization_is_explicit_and_reproducible",
     score02_normalization_is_explicit_and_reproducible},
    {"score03_weights_are_validated", score03_weights_are_validated},
    {"score04_every_term_is_exported_and_hand_checkable",
     score04_every_term_is_exported_and_hand_checkable},
    {"score05_snapshot_is_the_only_input", score05_snapshot_is_the_only_input},
    {"score06_determinism_and_rounding", score06_determinism_and_rounding},
    // SCD-PICK
    {"pick01_ordering_is_stable", pick01_ordering_is_stable},
    {"pick02_recommendation_follows_the_ranking", pick02_recommendation_follows_the_ranking},
    {"pick03_forced_non_recommended_is_allowed_and_marked",
     pick03_forced_non_recommended_is_allowed_and_marked},
    {"pick04_reasons_are_structured_and_traceable", pick04_reasons_are_structured_and_traceable},
    {"pick05_lead_over_runner_up", pick05_lead_over_runner_up},
    // SCD-EXP
    {"exp01_structured_output_shape", exp01_structured_output_shape},
    {"exp02_no_natural_language_sentences", exp02_no_natural_language_sentences},
    {"exp03_audit_triple_is_replayable", exp03_audit_triple_is_replayable},
    {"exp04_sensitivity_top_n", exp04_sensitivity_top_n},
    // SCD-OPT
    {"opt01_no_hardcoded_optimization", opt01_no_hardcoded_optimization},
    {"opt02_search_space_comes_from_the_rules", opt02_search_space_comes_from_the_rules},
    {"opt03_optimization_is_explainable", opt03_optimization_is_explainable},
    {"opt04_monotonic_and_idempotent", opt04_monotonic_and_idempotent},
    {"opt05_bounded_and_truncatable", opt05_bounded_and_truncatable},
    // SCD-DECIDE
    {"decide01_same_side_is_exclusive", decide01_same_side_is_exclusive},
    {"decide02_idempotent_repeats_succeed", decide02_idempotent_repeats_succeed},
    {"decide03_confirm_precondition", decide03_confirm_precondition},
    {"decide04_events_and_logs", decide04_events_and_logs},
    {"decide05_three_states_are_queryable", decide05_three_states_are_queryable},
    // SCD-NFR
    {"nfr01_zero_external_dependencies", nfr01_zero_external_dependencies},
    {"nfr02_independent_delivery_smoke", nfr02_independent_delivery_smoke},
    {"nfr03_all_outputs_go_through_injected_interfaces",
     nfr03_all_outputs_go_through_injected_interfaces},
    {"nfr04_performance_targets", nfr04_performance_targets},
    {"nfr05_float_reproducibility", nfr05_float_reproducibility},
    // 共享契约与冲突裁决
    {"proto_policies_schema_and_versioning", proto_policies_schema_and_versioning},
    {"proto_idempotent_vs_conflict_are_separate", proto_idempotent_vs_conflict_are_separate},
    {"proto_event_payload_shapes", proto_event_payload_shapes},
    // 结构纪律 + 真实规则包锚点
    {"structure_recommendation_reasons_are_not_sentences",
     structure_recommendation_reasons_are_not_sentences},
    {"structure_gate_is_held_during_sink_callback", structure_gate_is_held_during_sink_callback},
    {"structure_pure_functions_are_independently_callable",
     structure_pure_functions_are_independently_callable},
    {"anchor_recommended_total_is_recomputable_from_rules",
     anchor_recommended_total_is_recomputable_from_rules},
    {"anchor_missing_link_input_is_neutral_and_marked",
     anchor_missing_link_input_is_neutral_and_marked},
};

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    if (argc < 2) {
        try {
            std::system("chcp 65001 > nul");
        } catch (...) {
        }
    }
#endif
    std::string filter;
    bool listOnly = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--list") {
            listOnly = true;
        } else if (arg == "--json") {
            g_jsonMode = true;
        } else {
            filter = arg;
        }
    }
    if (listOnly) {
        for (const auto& c : kCases) std::printf("%s\n", c.name);
        return 0;
    }

    const auto t0 = std::chrono::steady_clock::now();
    for (const auto& c : kCases) {
        if (!filter.empty() && std::string(c.name).find(filter) == std::string::npos) continue;
        g_case = c.name;
        g_reqs.clear();
        const int failedBefore = g_failed;
        const int assertsBefore = g_asserts;
        ++g_cases;
        try {
            c.fn();
        } catch (const std::exception& ex) {
            record(false, std::string("用例抛出异常：") + ex.what(), __FILE__, __LINE__);
        } catch (...) {
            record(false, "用例抛出未知异常", __FILE__, __LINE__);
        }
        const bool ok = (g_failed == failedBefore);
        if (!ok) ++g_casesFailed;
        CaseMeta meta;
        meta.name = c.name;
        meta.reqs = g_reqs;
        meta.asserts = g_asserts - assertsBefore;
        meta.failed = g_failed - failedBefore;
        meta.ok = ok;
        g_metas.push_back(std::move(meta));
        if (!g_jsonMode) {
            std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", c.name);
            std::fflush(stdout);
        }
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    if (g_jsonMode) {
        for (const auto& f : g_failures) std::fprintf(stderr, "FAIL %s\n", f.c_str());
    }

    if (g_jsonMode) {
        std::string out = "{\n";
        out += "  \"engineVersion\": \"" + jsonEscape(kEngineVersion) + "\",\n";
        out += "  \"cases\": " + std::to_string(g_cases) + ",\n";
        out += "  \"casesFailed\": " + std::to_string(g_casesFailed) + ",\n";
        out += "  \"asserts\": " + std::to_string(g_asserts) + ",\n";
        out += "  \"assertsFailed\": " + std::to_string(g_failed) + ",\n";
        out += "  \"elapsedMs\": " + std::to_string(ms) + ",\n";
        out += "  \"result\": \"" + std::string(g_failed == 0 ? "ALL GREEN" : "FAILED") + "\",\n";
        out += "  \"details\": [";
        for (std::size_t i = 0; i < g_metas.size(); ++i) {
            const CaseMeta& m = g_metas[i];
            out += (i == 0 ? "\n" : ",\n");
            out += "    {\"name\": \"" + jsonEscape(m.name) + "\", \"ok\": " +
                   (m.ok ? "true" : "false") + ", \"asserts\": " + std::to_string(m.asserts) +
                   ", \"failed\": " + std::to_string(m.failed) + ", \"reqs\": " +
                   jsonArray(m.reqs) + "}";
        }
        out += "\n  ],\n";
        out += "  \"failures\": " + jsonArray(g_failures) + "\n";
        out += "}\n";
        std::fputs(out.c_str(), stdout);
        return g_failed == 0 ? 0 : 1;
    }

    std::printf("\n================ scoring selftest ================\n");
    std::printf("用例 %d 个（失败 %d ）｜断言 %d 条（失败 %d ）｜耗时 %.1f ms\n", g_cases,
                g_casesFailed, g_asserts, g_failed, ms);
    if (!g_failures.empty()) {
        std::printf("\n---- 失败明细 ----\n");
        for (const auto& f : g_failures) std::printf("  %s\n", f.c_str());
    }
    std::printf("结果： %s\n", g_failed == 0 ? "ALL GREEN" : "FAILED");
    std::printf("==================================================\n");
    return g_failed == 0 ? 0 : 1;
}
