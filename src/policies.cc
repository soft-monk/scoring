// src/policies.cc · scoring —— 规则包装载与校验（protocol.md §5；SCD-SCORE-01/02/03、SCD-OPT-02）
//
// 纪律：
//   · 引擎 MUST NOT 内建指标名 / 方案名 / 权重 / 演示数值（P6/P7、ADR-C1-02）。
//     本文件只认识**形状**：key / name / direction / normalize / weight / baseline / source …
//   · 校验失败 MUST 拒绝**整包**并给出**逐条**可读原因（含条目下标与字段名）—— CTR-PL-02。
//   · 未知字段 MUST 被忽略并计入告警统计（CTR-PL-03）；缺必填字段 MUST 拒绝（CTR-PL-05）。
//   · MAJOR 不支持 → 1006（protocol §5.2），MUST NOT 静默降级。
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include "internal.h"

namespace scoring {
namespace detail {

// ---------------------------------------------------------------- 校验助手

static void issue(std::vector<LoadIssue>& out, const std::string& path, const std::string& field,
                  const std::string& reason) {
    out.push_back({path, field, reason});
}

static std::string itemsPath(int idx) { return "items[" + std::to_string(idx) + "]"; }

/// 权重和校验（SCD-SCORE-03）
struct WeightCheck {
    bool ok = false;
    bool allZero = true;
    double sum = 0.0;
    std::string reason;
};

static WeightCheck checkWeights(const std::vector<MetricDef>& items, bool autoNormalize) {
    WeightCheck w;
    for (const auto& it : items) w.sum += it.weight;
    for (const auto& it : items) {
        if (it.weight != 0.0) {
            w.allZero = false;
            break;
        }
    }
    if (items.empty()) {
        w.reason = "no-metrics";
        return w;
    }
    if (w.allZero) {
        w.reason = "weights-all-zero";
        return w;
    }
    for (const auto& it : items) {
        if (it.weight < 0.0) {
            w.reason = "weight-negative:" + it.key;
            return w;
        }
    }
    if (std::fabs(w.sum - 1.0) > kWeightEpsilon) {
        if (!autoNormalize) {
            w.reason = "weight-sum-not-one";
            return w;
        }
        w.reason = "weight-sum-normalized";
    }
    w.ok = true;
    return w;
}

// ---------------------------------------------------------------- 指标包解析

/// 解析 aggregate 段（可选；缺省 = linear-weighted-mean + half-up）
static void parseAggregate(const json& pkg, AggregateSpec& agg, std::vector<std::string>& warnings) {
    const json* a = find(pkg, "aggregate");
    if (!a || !a->is_object()) return;
    std::string method = getString(*a, "method", "linear-weighted-mean");
    if (method != agg.method) {
        // 只支持线性加权平均（风险 R3：不做帕累托 / 不做机器学习）
        warnings.push_back("aggregate.method-unsupported-fallback:" + method);
    }
    const std::string rounding = getString(*a, "rounding", "half-up");
    if (rounding != "half-up") warnings.push_back("aggregate.rounding-unsupported-fallback:" + rounding);
    agg.outputScale = getNumber(*a, "outputScale", agg.outputScale);
    if (agg.outputScale <= 0.0) agg.outputScale = 100.0;
    agg.normalizeWeights = getBool(*a, "normalizeWeights", false);

    const json* nr = find(*a, "neutralRule");
    if (nr && nr->is_object()) {
        agg.neutralRuleEnabled = getBool(*nr, "enabled", false);
        agg.neutralSource = getString(*nr, "valueSource", "baseline");
        agg.neutralValue = getNumber(*nr, "value", 0.0);
        agg.neutralMarkMissing = getBool(*nr, "markMissing", false);
        agg.neutralMarker = getString(*nr, "missingMarker");
    }
}

/// 解析 normalize 段（SCD-SCORE-02：归一方式 MUST 显式声明）
static bool parseNormalize(const json& item, NormalizeSpec& spec, std::vector<LoadIssue>& issues,
                           const std::string& path) {
    const json* n = find(item, "normalize");
    if (!n || !n->is_object()) {
        issue(issues, path, "normalize", "missing-required-field");
        return false;
    }
    const std::string t = getString(*n, "type");
    const auto parsed = normalizeTypeFromString(t);
    if (!parsed) {
        issue(issues, path, "normalize.type", "unknown-enum-value:" + t);
        return false;
    }
    spec.type = *parsed;
    spec.typeName = t;
    switch (spec.type) {
        case NormalizeType::Percent: {
            spec.scale = getNumber(*n, "scale", 100.0);
            if (spec.scale == 0.0) {
                issue(issues, path, "normalize.scale", "must-not-be-zero");
                return false;
            }
            break;
        }
        case NormalizeType::Range: {
            if (!hasKey(*n, "min") || !hasKey(*n, "max")) {
                issue(issues, path, "normalize.min|normalize.max", "missing-required-field");
                return false;
            }
            spec.min = getNumber(*n, "min", 0.0);
            spec.max = getNumber(*n, "max", 0.0);
            if (spec.max <= spec.min) {
                issue(issues, path, "normalize.max", "must-be-greater-than-min");
                return false;
            }
            break;
        }
        case NormalizeType::Threshold: {
            const json* bands = find(*n, "bands");
            if (!bands || !bands->is_array() || bands->empty()) {
                issue(issues, path, "normalize.bands", "missing-or-empty");
                return false;
            }
            for (std::size_t i = 0; i < bands->size(); ++i) {
                const json& b = (*bands)[i];
                const std::string bp = path + ".normalize.bands[" + std::to_string(i) + "]";
                if (!b.is_object() || !hasKey(b, "min") || !hasKey(b, "value")) {
                    issue(issues, bp, "min|value", "missing-required-field");
                    return false;
                }
                spec.bands.emplace_back(getNumber(b, "min", 0.0), getNumber(b, "value", 0.0));
            }
            // 归一值 MUST 落在 0..1；按 min 降序求值（确定性）
            for (const auto& b : spec.bands) {
                if (b.second < 0.0 || b.second > 1.0) {
                    issue(issues, path, "normalize.bands[].value", "must-be-within-0..1");
                    return false;
                }
            }
            std::sort(spec.bands.begin(), spec.bands.end(),
                      [](const std::pair<double, double>& a, const std::pair<double, double>& b) {
                          return a.first > b.first;
                      });
            break;
        }
    }
    return true;
}

LoadResult loadMetricsInto(const json& pkg, MetricsModel& out) {
    LoadResult r;
    SkeletonCheck sk = checkSkeleton(pkg, "scoringMetrics");
    r.warnings = sk.warnings;
    r.issues = sk.issues;
    r.policiesNamespace = sk.policiesNamespace;
    r.schemaVersion = sk.schemaVersion;
    r.digest = sk.digest;
    if (!sk.ok) {
        r.code = sk.code;
        r.message = sk.message;
        return r;
    }

    MetricsModel model;
    model.pack.policiesNamespace = sk.policiesNamespace;
    model.pack.schemaVersion = sk.schemaVersion;
    model.pack.kind = "scoringMetrics";
    model.pack.digest = sk.digest;
    model.pack.raw = pkg;
    parseAggregate(pkg, model.aggregate, r.warnings);
    model.pack.aggregate = model.aggregate;

    const json* items = find(pkg, "items");
    if (!items || !items->is_array() || items->empty()) {
        // §5.3：scoringMetrics 的清单段是 items
        issue(r.issues, "items", "items", "missing-or-empty");
        r.code = static_cast<int>(ErrorCode::BadRequest);
        r.message = "items MUST be a non-empty array";
        return r;
    }

    int idx = 0;
    for (const auto& item : *items) {
        const std::string path = itemsPath(idx);
        if (!item.is_object()) {
            issue(r.issues, path, "", "not-an-object");
            ++idx;
            continue;
        }
        MetricDef d;
        d.order = idx;
        d.key = getString(item, "key");
        d.name = getString(item, "name");
        if (d.key.empty()) issue(r.issues, path, "key", "missing-required-field");
        if (d.name.empty()) issue(r.issues, path, "name", "missing-required-field");
        if (model.index.count(d.key)) issue(r.issues, path, "key", "duplicate-key:" + d.key);
        d.unit = getString(item, "unit");

        const std::string dirName = getString(item, "direction", "higher");
        const auto dir = metricDirectionFromString(dirName);
        if (!dir) {
            issue(r.issues, path, "direction", "unknown-enum-value:" + dirName);
        } else {
            d.direction = *dir;
            d.directionName = dirName;
        }

        if (!parseNormalize(item, d.normalize, r.issues, path)) {
            ++idx;
            continue;
        }

        if (!hasKey(item, "weight")) {
            issue(r.issues, path, "weight", "missing-required-field");
        } else {
            d.weight = getNumber(item, "weight", 0.0);
        }

        if (hasKey(item, "baseline")) {
            d.hasBaseline = true;
            d.baseline = getNumber(item, "baseline", 0.0);
        }
        d.baselineUsed = getBool(item, "baselineUsed", true);
        if (!hasKey(item, "baselineUsed") && hasKey(item, "source")) d.baselineUsed = false;

        const json* src = find(item, "source");
        if (src && src->is_object()) {
            const std::string sName = getString(*src, "kind");
            const auto s = metricSourceFromString(sName);
            if (!s) {
                issue(r.issues, path, "source.kind", "unknown-enum-value:" + sName);
            } else {
                d.source = *s;
                d.sourceName = sName;
            }
            d.sourceField = getString(*src, "field");
            d.sourceMetric = getString(*src, "metric");
            d.scalar = getNumber(*src, "value", 0.0);
        }

        const json* nm = find(item, "neutralOnMissing");
        if (nm && nm->is_boolean()) d.neutralOnMissing = nm->get<bool>();
        else if (nm && nm->is_object()) {
            d.neutralOnMissing = getBool(*nm, "enabled", false);
            d.neutral = getNumber(*nm, "value", 0.0);
            d.missingMarker = getString(*nm, "missingMarker");
        }

        // 有效基线：规则包显式声明，否则回落中性规则声明的中性值
        if (!d.hasBaseline && model.aggregate.neutralRuleEnabled &&
            model.aggregate.neutralSource == "scalar") {
            d.hasBaseline = true;
            d.baseline = model.aggregate.neutralValue;
        }
        if (!d.hasBaseline && model.aggregate.neutralRuleEnabled &&
            model.aggregate.neutralSource == "baseline" && hasKey(item, "neutral")) {
            d.hasBaseline = true;
            d.baseline = getNumber(item, "neutral", 0.0);
        }
        if (d.missingMarker.empty() && model.aggregate.neutralMarkMissing) {
            d.missingMarker = model.aggregate.neutralMarker;
        }

        model.items.push_back(d);
        model.index[d.key] = static_cast<int>(model.items.size()) - 1;
        ++idx;
    }

    // 权重校验（SCD-SCORE-03：全 0 或和不为 1 → 报错，除非规则包**显式**声明自动归一）
    const WeightCheck wc = checkWeights(model.items, model.aggregate.normalizeWeights);
    if (!wc.ok) {
        issue(r.issues, "items[].weight", "weight", wc.reason);
        r.code = static_cast<int>(ErrorCode::BadRequest);
        r.message = "weight check failed:" + wc.reason;
        r.itemCount = static_cast<int>(model.items.size());
        return r;
    }
    if (wc.reason == "weight-sum-normalized") {
        // 显式归一：按声明顺序逐项除以和（确定性）
        for (auto& it : model.items) it.weight = it.weight / wc.sum;
        r.warnings.push_back("weight-sum-normalized-from:" + numToStableString(wc.sum));
    }

    // 理由口径（SCD-PICK-04：条数与类型序列由规则包声明）
    const json* rs = find(pkg, "reasons");
    if (rs && rs->is_object()) {
        model.reasonsDeclared = true;
        model.reasons.count = getInt(*rs, "count", 0);
        const json* types = find(*rs, "types");
        if (types && types->is_array()) {
            for (const auto& t : *types) {
                if (t.is_object()) {
                    const std::string ty = getString(t, "type");
                    if (!ty.empty()) model.reasons.types.push_back(ty);
                } else if (t.is_string()) {
                    model.reasons.types.push_back(t.get<std::string>());
                }
            }
        }
        if (model.reasons.count <= 0) {
            issue(r.issues, "reasons", "reasons.count", "must-be-positive");
        }
        if (!model.reasons.types.empty() &&
            static_cast<int>(model.reasons.types.size()) < model.reasons.count) {
            issue(r.issues, "reasons", "reasons.types", "fewer-types-than-count");
        }
    } else {
        issue(r.issues, "reasons", "reasons", "missing-required-section");
    }

    // 搜索空间（SCD-OPT-02：可调参数、步长、上下限由规则声明）
    const json* ss = find(pkg, "searchSpace");
    if (ss && ss->is_object()) {
        model.search.maxIterations = getInt(*ss, "maxIterations", 0);
        model.search.timeBudgetMs = static_cast<int64_t>(getNumber(*ss, "timeBudgetMs", 0.0));
        model.search.maxTotalSteps = getInt(*ss, "maxTotalSteps", 0);
        const json* params = find(*ss, "params");
        if (params && params->is_array()) {
            int pi = 0;
            for (const auto& p : *params) {
                const std::string pp = "searchSpace.params[" + std::to_string(pi) + "]";
                if (!p.is_object()) {
                    issue(r.issues, pp, "", "not-an-object");
                    ++pi;
                    continue;
                }
                AdjustableParam ap;
                ap.key = getString(p, "key");
                ap.name = getString(p, "name");
                ap.metricKey = getString(p, "metricKey");
                ap.step = getNumber(p, "step", 0.0);
                ap.maxSteps = getNumber(p, "maxSteps", 0.0);
                if (ap.key.empty()) issue(r.issues, pp, "key", "missing-required-field");
                if (ap.metricKey.empty()) issue(r.issues, pp, "metricKey", "missing-required-field");
                if (!model.index.count(ap.metricKey)) {
                    issue(r.issues, pp, "metricKey", "unknown-metric:" + ap.metricKey);
                }
                if (ap.step == 0.0) issue(r.issues, pp, "step", "must-not-be-zero");
                if (ap.maxSteps < 0.0) issue(r.issues, pp, "maxSteps", "must-not-be-negative");
                model.search.params.push_back(ap);
                ++pi;
            }
        }
        if (model.search.maxIterations <= 0) {
            issue(r.issues, "searchSpace", "searchSpace.maxIterations", "must-be-positive");
        }
    } else {
        issue(r.issues, "searchSpace", "searchSpace", "missing-required-section");
    }

    if (!r.issues.empty()) {
        r.code = static_cast<int>(ErrorCode::BadRequest);
        r.message = "metrics package rejected";
        r.itemCount = static_cast<int>(model.items.size());
        return r;
    }

    model.pack.items = model.items;  // 对外只读视图（排序与查找请用 model.index）
    out = model;
    r.code = 0;
    r.message = "ok";
    r.itemCount = static_cast<int>(model.items.size());
    return r;
}

// ---------------------------------------------------------------- 模板包解析

LoadResult loadTemplatesInto(const json& pkg, TemplatesModel& out, const MetricsModel* metrics) {
    LoadResult r;
    SkeletonCheck sk = checkSkeleton(pkg, "planTemplates");
    r.warnings = sk.warnings;
    r.issues = sk.issues;
    r.policiesNamespace = sk.policiesNamespace;
    r.schemaVersion = sk.schemaVersion;
    r.digest = sk.digest;
    if (!sk.ok) {
        r.code = sk.code;
        r.message = sk.message;
        return r;
    }

    TemplatesModel model;
    model.pack.policiesNamespace = sk.policiesNamespace;
    model.pack.schemaVersion = sk.schemaVersion;
    model.pack.kind = "planTemplates";
    model.pack.digest = sk.digest;
    model.pack.raw = pkg;

    // profiles（兄弟段：模板族的指标剖面 —— 与模板同版本演进）
    const json* profiles = find(pkg, "profiles");
    if (profiles && profiles->is_array()) {
        int pi = 0;
        for (const auto& p : *profiles) {
            const std::string pp = "profiles[" + std::to_string(pi) + "]";
            if (!p.is_object()) {
                issue(r.issues, pp, "", "not-an-object");
                ++pi;
                continue;
            }
            ProfileDef d;
            d.key = getString(p, "key");
            d.name = getString(p, "name");
            if (d.key.empty()) issue(r.issues, pp, "key", "missing-required-field");
            if (model.profileIndex.count(d.key)) {
                issue(r.issues, pp, "key", "duplicate-key:" + d.key);
            }
            const json* vals = find(p, "values");
            if (!vals || !vals->is_object()) {
                issue(r.issues, pp, "values", "missing-required-field");
            } else {
                d.values = *vals;
            }
            model.profiles.push_back(d);
            model.profileIndex[d.key] = static_cast<int>(model.profiles.size()) - 1;
            ++pi;
        }
    }

    // confirmPrecondition（SCD-DECIDE-03 的策略由规则包声明）
    const json* cp = find(pkg, "confirmPrecondition");
    if (cp && cp->is_object()) {
        model.confirmModeDeclared = true;
        model.confirmModeName = getString(*cp, "mode", "reject-if-not-adopted");
        const auto mode = confirmPreconditionFromString(model.confirmModeName);
        if (!mode) {
            issue(r.issues, "confirmPrecondition", "confirmPrecondition.mode",
                  "unknown-enum-value:" + model.confirmModeName);
        } else {
            model.confirmMode = *mode;
        }
    }

    const json* items = find(pkg, "items");
    if (!items || !items->is_array() || items->empty()) {
        issue(r.issues, "items", "items", "missing-or-empty");
        r.code = static_cast<int>(ErrorCode::BadRequest);
        r.message = "items MUST be a non-empty array";
        return r;
    }

    int idx = 0;
    for (const auto& item : *items) {
        const std::string path = itemsPath(idx);
        if (!item.is_object()) {
            issue(r.issues, path, "", "not-an-object");
            ++idx;
            continue;
        }
        PlanTemplate d;
        d.key = getString(item, "key");
        d.name = getString(item, "name");
        d.side = getString(item, "side");
        d.scene = getString(item, "scene");
        d.method = getString(item, "method");
        d.effect = getString(item, "effect");
        d.clusters = getStringArray(item, "clusters");
        d.profileKey = getString(item, "profileKey");
        d.seq = getInt(item, "seq", idx);
        d.recommendedHint = getBool(item, "recommended", false);
        if (d.key.empty()) issue(r.issues, path, "key", "missing-required-field");
        if (d.name.empty()) issue(r.issues, path, "name", "missing-required-field");
        if (d.side.empty()) issue(r.issues, path, "side", "missing-required-field");
        if (model.index.count(d.key)) issue(r.issues, path, "key", "duplicate-key:" + d.key);
        if (!d.profileKey.empty() && !model.profileIndex.count(d.profileKey)) {
            issue(r.issues, path, "profileKey", "unknown-profile:" + d.profileKey);
        }
        if (hasKey(item, "successRate")) {
            d.hasSuccessRate = true;
            d.successRate = getNumber(item, "successRate", 0.0);
        }

        const json* when = find(item, "when");
        if (when && when->is_object()) {
            d.when.clusterKeys = getStringArray(*when, "clusters");
            if (hasKey(*when, "minClusters")) d.when.minClusters = getInt(*when, "minClusters", -1);
            if (hasKey(*when, "minItems")) d.when.minItems = getInt(*when, "minItems", -1);
            d.when.phaseKeys = getStringArray(*when, "phases");
        }

        model.items.push_back(d);
        model.index[d.key] = static_cast<int>(model.items.size()) - 1;
        ++idx;
    }

    // 交叉校验：模板来源指标 MUST 能被该模板的剖面解析（否则该候选永远取不到值）
    if (metrics) {
        for (std::size_t i = 0; i < model.items.size(); ++i) {
            const PlanTemplate& t = model.items[i];
            const std::string path = itemsPath(static_cast<int>(i));
            if (t.profileKey.empty()) continue;
            const auto pit = model.profileIndex.find(t.profileKey);
            if (pit == model.profileIndex.end()) continue;
            const ProfileDef& prof = model.profiles[static_cast<std::size_t>(pit->second)];
            for (const auto& m : metrics->items) {
                if (m.source != MetricSource::Profile) continue;
                const std::string want = m.sourceMetric.empty() ? m.key : m.sourceMetric;
                if (!hasKey(prof.values, want)) {
                    issue(r.issues, path, "profileKey", "profile-missing-metric:" + want);
                }
            }
        }
    }

    if (!r.issues.empty()) {
        r.code = static_cast<int>(ErrorCode::BadRequest);
        r.message = "templates package rejected";
        r.itemCount = static_cast<int>(model.items.size());
        return r;
    }

    model.pack.profiles = model.profiles;  // 对外只读视图（查找请用 model.profileIndex）
    model.pack.items = model.items;
    out = model;
    r.code = 0;
    r.message = "ok";
    r.itemCount = static_cast<int>(model.items.size());
    return r;
}

// ---------------------------------------------------------------- 文件装载

bool readJsonFile(const std::string& path, json& out, std::string& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "cannot-open-file";
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    try {
        out = json::parse(text);
    } catch (const std::exception& ex) {
        err = std::string("invalid-json:") + ex.what();
        return false;
    }
    return true;
}

}  // namespace detail
}  // namespace scoring
