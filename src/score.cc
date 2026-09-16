// src/score.cc · scoring —— 候选生成 / 归一 / 加权评分 / 排序 / 推荐 / 可解释输出 / 审计三件套
//
// 权威依据：
//   SCD-CAND-01..04（规则驱动生成、不适用标注、候选数可变、去重）
//   SCD-SCORE-01..06（指标可配、归一显式、权重校验、计算过程可导出、快照输入、确定性）
//   SCD-PICK-01..05（稳定排序、推荐标记、强制非推荐、理由 4 条、次优差值）
//   SCD-EXP-01..04（结构化输出、无话术、审计三件套、敏感性排序）
//   ADR-C1-04（本文件 MUST NOT 出现任何演示评分常量；一切取值来自规则包）
//
// 确定性：全部单价用**定点整数**累加（scale = kScale），对外整数百分比（half-up）。
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "internal.h"

namespace scoring {
namespace detail {

// ---------------------------------------------------------------- 定点累加

/// 单价的定点标度：contributionScaled = round(raw × weight × kScale)
/// 其中 raw = 归一值（0..1）。整数除法实现 half-up，**不做浮点累加**。
static int64_t unitScaledFromNormalized(double normalized, double weight) {
    const double v = normalized * weight * static_cast<double>(kScale);
    const bool neg = v < 0;
    const double mag = neg ? -v : v;
    const int64_t q = static_cast<int64_t>(mag + 0.5);
    return neg ? -q : q;
}

// ---------------------------------------------------------------- 取值解析

namespace {

/// 数值序列的简单聚合（确定性：按声明顺序，无并行归约）
struct Series {
    double sum = 0.0;
    double mean = 0.0;
    int count = 0;
};

Series seriesOf(const std::vector<double>& v) {
    Series s;
    s.count = static_cast<int>(v.size());
    for (double x : v) s.sum += x;
    if (s.count > 0) s.mean = s.sum / static_cast<double>(s.count);
    return s;
}

}  // namespace

ResolvedValue resolveRaw(const MetricsModel& m, const TemplatesModel& t, const PlanTemplate& tmpl,
                         const MetricDef& def, const ScoringSnapshot& snap,
                         const std::map<std::string, double>* lifts) {
    ResolvedValue r;
    const AggregateSpec& agg = m.aggregate;
    const std::string field = def.sourceMetric.empty() ? def.key : def.sourceMetric;

    /// 应用规则包声明的参数增益（优化阶段）；按该指标的归一区间截断
    auto applyLift = [&](ResolvedValue in) -> ResolvedValue {
        if (!lifts || !in.found) return in;
        const auto it = lifts->find(def.key);
        if (it == lifts->end() || it->second == 0.0) return in;
        double lo = 0.0;
        double hi = 0.0;
        switch (def.normalize.type) {
            case NormalizeType::Percent:
                lo = 0.0;
                hi = def.normalize.scale;
                break;
            case NormalizeType::Range:
                lo = def.normalize.min;
                hi = def.normalize.max;
                break;
            case NormalizeType::Threshold:
                if (def.normalize.bands.empty()) return in;
                hi = def.normalize.bands.front().first;
                lo = def.normalize.bands.back().first;
                break;
        }
        in.raw = clampTo(in.raw + it->second, lo, hi);
        in.sourceField += "+searchSpace";
        return in;
    };

    // ---- 1. 快照显式给出的指标值（宿主可直接投喂任一指标的观测量）----
    const json* sm = find(snap.extra, "metrics");
    if (sm && sm->is_object()) {
        const json* v = find(*sm, def.key);
        if (v && v->is_number()) {
            r.raw = v->get<double>();
            r.found = true;
            r.fromSnapshot = true;
            r.sourceField = "snapshot.metrics." + def.key;
            return applyLift(r);
        }
    }

    // ---- 2. 模板剖面（source.kind = "profile"）----
    if (def.source == MetricSource::Profile) {
        if (!tmpl.profileKey.empty()) {
            const auto pit = t.profileIndex.find(tmpl.profileKey);
            if (pit != t.profileIndex.end()) {
                const ProfileDef& prof = t.profiles[static_cast<std::size_t>(pit->second)];
                const json* v = find(prof.values, field);
                if (v && v->is_number()) {
                    r.raw = v->get<double>();
                    r.found = true;
                    r.fromProfile = true;
                    r.sourceField = "profile." + tmpl.profileKey + "." + field;
                    return applyLift(r);
                }
            }
        }
    }

    // ---- 3. 指标声明的来源 ----
    switch (def.source) {
        case MetricSource::Template: {
            // 模板字段（如模板自带的成功率基线）。字段名由规则包声明，引擎不认识它。
            if (!def.sourceField.empty() && def.sourceField != "profile") {
                const json* raw = nullptr;
                if (!t.pack.raw.is_null() && t.pack.raw.is_object()) {
                    const json* items = find(t.pack.raw, "items");
                    if (items && items->is_array()) {
                        for (const auto& it : *items) {
                            if (getString(it, "key") == tmpl.key) {
                                raw = find(it, def.sourceField);
                                break;
                            }
                        }
                    }
                }
                if (raw && raw->is_number()) {
                    r.raw = raw->get<double>();
                    r.found = true;
                    r.sourceField = "template." + def.sourceField;
                    return applyLift(r);
                }
            } else if (def.sourceField == "profile") {
                if (!tmpl.profileKey.empty()) {
                    const auto pit = t.profileIndex.find(tmpl.profileKey);
                    if (pit != t.profileIndex.end()) {
                        const ProfileDef& prof = t.profiles[static_cast<std::size_t>(pit->second)];
                        const json* v = find(prof.values, field);
                        if (v && v->is_number()) {
                            r.raw = v->get<double>();
                            r.found = true;
                            r.fromProfile = true;
                            r.sourceField = "profile." + tmpl.profileKey + "." + field;
                            return applyLift(r);
                        }
                    }
                }
            }
            break;
        }
        case MetricSource::Snapshot: {
            if (field == "coverageRate" || field == "coverage") {
                if (snap.topology && snap.topology->present) {
                    std::vector<double> v;
                    v.reserve(snap.topology->links.size());
                    for (const auto& l : snap.topology->links) v.push_back(l.coverageKm2);
                    if (!v.empty()) {
                        r.raw = seriesOf(v).sum;  // 覆盖率量纲由规则包 normalize 声明
                        r.found = true;
                        r.fromSnapshot = true;
                        r.sourceField = "topology.links[].coverageKm2";
                        return applyLift(r);
                    }
                }
            } else if (field == "linkStability" || field == "link") {
                if (snap.topology && snap.topology->present) {
                    std::vector<double> v;
                    v.reserve(snap.topology->links.size());
                    for (const auto& l : snap.topology->links) v.push_back(1.0 - l.lossRate);
                    if (!v.empty()) {
                        r.raw = seriesOf(v).mean;
                        r.found = true;
                        r.fromSnapshot = true;
                        r.sourceField = "topology.links[].lossRate";
                        return applyLift(r);
                    }
                }
            } else if (field == "targetDetection" || field == "detection") {
                if (snap.targets && snap.targets->present && !snap.targets->confidences.empty()) {
                    std::vector<double> v;
                    v.reserve(snap.targets->confidences.size());
                    for (int c : snap.targets->confidences) v.push_back(static_cast<double>(c));
                    r.raw = seriesOf(v).mean;
                    r.found = true;
                    r.fromSnapshot = true;
                    r.sourceField = "targets.confidences[]";
                    return applyLift(r);
                }
            } else if (field == "resourceUtilization" || field == "utilization") {
                if (snap.hasResourceUtilization) {
                    r.raw = snap.resourceUtilization;
                    r.found = true;
                    r.fromSnapshot = true;
                    r.sourceField = "snapshot.resourceUtilization";
                    return applyLift(r);
                }
            } else if (field == "electronicSuppression" || field == "suppression") {
                if (snap.extra.is_object()) {
                    const json* v = find(snap.extra, "electronicSuppression");
                    if (v && v->is_number()) {
                        r.raw = v->get<double>();
                        r.found = true;
                        r.fromSnapshot = true;
                        r.sourceField = "snapshot.extra.electronicSuppression";
                        return applyLift(r);
                    }
                }
            } else if (snap.extra.is_object()) {
                const json* v = find(snap.extra, field);
                if (v && v->is_number()) {
                    r.raw = v->get<double>();
                    r.found = true;
                    r.fromSnapshot = true;
                    r.sourceField = "snapshot.extra." + field;
                    return applyLift(r);
                }
            }
            break;
        }
        case MetricSource::Scalar: {
            r.raw = def.scalar;
            r.found = true;
            r.sourceField = "scoringMetrics.items[].source.value";
            return r;
        }
        case MetricSource::Profile:
            break;  // 已在上一步处理
    }

    // ---- 4. 缺失：中性值 / 基线（**标注缺失，不阻塞评分** —— SCD-SCORE-05 / 风险 R6）----
    const bool neutralActive =
        def.neutralOnMissing || (agg.neutralRuleEnabled && agg.neutralSource != "baseline");
    if (neutralActive && def.neutralOnMissing) {
        r.raw = def.neutral;
        r.found = true;
        r.missing = agg.neutralMarkMissing;
        r.sourceField = "neutral";
        r.missingMarker = def.missingMarker.empty() ? agg.neutralMarker : def.missingMarker;
        return r;
    }
    if (def.hasBaseline && def.baselineUsed) {
        r.raw = def.baseline;
        r.found = true;
        r.usedBaseline = true;
        r.missing = true;  // 输入缺失 → 走规则包基线；评分继续
        r.sourceField = "baseline";
        r.missingMarker = def.missingMarker.empty() ? agg.neutralMarker : def.missingMarker;
        return r;
    }
    if (def.hasBaseline) {
        r.raw = def.baseline;
        r.found = true;
        r.usedBaseline = true;
        r.sourceField = "baseline";
        return r;
    }
    // 既无输入也无基线：按 0 处理并标注缺失（仍不阻塞）
    r.raw = 0.0;
    r.found = false;
    r.missing = true;
    r.sourceField = "none";
    r.missingMarker = def.missingMarker.empty() ? agg.neutralMarker : def.missingMarker;
    return r;
}

// ---------------------------------------------------------------- 单候选评分

CandidateScore scoreOne(const MetricsModel& m, const TemplatesModel& t, const Candidate& c,
                        const ScoringSnapshot& snap, std::vector<std::string>* missingInputs,
                        const std::map<std::string, double>* lifts) {
    CandidateScore out;
    out.candidate = c;
    out.applicable = c.applicable;
    if (!c.applicable) return out;

    PlanTemplate tmpl;
    bool haveTmpl = false;
    const auto it = t.index.find(c.key);
    if (it != t.index.end()) {
        tmpl = t.items[static_cast<std::size_t>(it->second)];
        haveTmpl = true;
    }
    if (!haveTmpl) return out;

    int64_t total = 0;
    for (const MetricDef& def : m.items) {
        const ResolvedValue rv = resolveRaw(m, t, tmpl, def, snap, lifts);
        MetricScore ms;
        ms.key = def.key;
        ms.name = def.name;
        ms.unit = def.unit;
        ms.direction = toString(def.direction);
        ms.normalizeType = toString(def.normalize.type);
        ms.source = def.source;
        ms.sourceName = def.sourceName.empty() ? toString(def.source) : def.sourceName;
        ms.sourceField = rv.sourceField;
        ms.raw = rv.raw;
        ms.normalized = normalizeValue(def, rv.raw);
        ms.weight = def.weight;
        ms.usedBaseline = rv.usedBaseline;
        ms.missing = rv.missing;
        ms.missingMarker = rv.missingMarker;
        ms.contributionScaled = unitScaledFromNormalized(ms.normalized, def.weight);
        total += ms.contributionScaled;
        if (missingInputs && rv.missing && !rv.missingMarker.empty()) {
            if (std::find(missingInputs->begin(), missingInputs->end(), rv.missingMarker) ==
                missingInputs->end()) {
                missingInputs->push_back(rv.missingMarker);
            }
        }
        out.metrics.push_back(ms);
    }
    out.totalScaled = total;
    out.totalPercent = roundToPercent(total, kScale);
    for (MetricScore& ms : out.metrics) {
        ms.contribution = roundedContribution(ms.contributionScaled);
    }
    out.scored = true;
    return out;
}

// ---------------------------------------------------------------- 排序（稳定次级排序键）

bool ScoreOrder::operator()(const CandidateScore& a, const CandidateScore& b) const {
    if (a.applicable != b.applicable) return a.applicable;  // 适用优先
    if (a.totalScaled != b.totalScaled) return a.totalScaled > b.totalScaled;  // 总分降序
    if (a.candidate.seq != b.candidate.seq) return a.candidate.seq < b.candidate.seq;
    if (a.candidate.profileKey != b.candidate.profileKey) {
        return a.candidate.profileKey < b.candidate.profileKey;
    }
    return a.candidate.id < b.candidate.id;  // 最终稳定键
}

// ---------------------------------------------------------------- 候选生成

namespace {

/// 内容键（SCD-CAND-04 的"内容等价"判据：方法 + 集群集合（保序去重）+ 成功率）
std::string contentKey(const PlanTemplate& t) {
    std::vector<std::string> clusters = t.clusters;
    std::sort(clusters.begin(), clusters.end());
    clusters.erase(std::unique(clusters.begin(), clusters.end()), clusters.end());
    std::string s = t.method;
    for (const auto& c : clusters) {
        s += "|";
        s += c;
    }
    s += "|";
    s += t.hasSuccessRate ? numToStableString(t.successRate) : std::string("-");
    return s;
}

void applicability(const PlanTemplate& t, const ScoringSnapshot& snap,
                   std::vector<InapplicableItem>& out) {
    const ResourceSnapshot* res = nullptr;
    if (snap.resources && snap.resources->present) res = &(*snap.resources);

    if (t.when.minClusters > 0 || !t.when.clusterKeys.empty()) {
        int availableClusters = 0;
        int availableItems = 0;
        for (const auto& c : (res ? res->clusters : std::vector<ClusterUsage>{})) {
            if (!c.available) continue;
            ++availableClusters;
            availableItems += c.total;
        }
        if (t.when.minClusters > 0 && availableClusters < t.when.minClusters) {
            InapplicableItem it;
            it.code = "insufficient-resources";
            it.field = "when.minClusters";
            it.required = t.when.minClusters;
            it.actual = availableClusters;
            it.hasNumbers = true;
            out.push_back(it);
        }
        for (const auto& need : t.when.clusterKeys) {
            bool hit = false;
            for (const auto& c : (res ? res->clusters : std::vector<ClusterUsage>{})) {
                if (c.clusterId == need && c.available) {
                    hit = true;
                    break;
                }
            }
            if (!hit) {
                InapplicableItem it;
                it.code = "cluster-unavailable";
                it.field = "when.clusters";
                it.subject = need;  // 字符串 key（MUST NOT 用显示名 —— CTR-PL-08）
                out.push_back(it);
            }
        }
        if (t.when.minItems > 0 && availableItems < t.when.minItems) {
            InapplicableItem it;
            it.code = "insufficient-items";
            it.field = "when.minItems";
            it.required = t.when.minItems;
            it.actual = availableItems;
            it.hasNumbers = true;
            out.push_back(it);
        }
    }
    if (!t.when.phaseKeys.empty()) {
        bool hit = false;
        for (const auto& p : t.when.phaseKeys) {
            if (p == snap.phase.phaseKey) {
                hit = true;
                break;
            }
        }
        if (!hit) {
            InapplicableItem it;
            it.code = "phase-not-allowed";
            it.field = "when.phases";
            it.subject = snap.phase.phaseKey;
            out.push_back(it);
        }
    }
}

}  // namespace

std::vector<Candidate> generateCandidates(const MetricsModel& m, const TemplatesModel& t,
                                          const CandidateRequest& req, Metrics* counters) {
    (void)m;
    std::vector<Candidate> out;
    const std::string side = req.side;
    const std::string scene = req.scene.empty() ? req.phase.scenarioKey : req.scene;

    // 声明顺序即输出顺序（确定性 —— SCD-PICK-01 的次级键来源）
    std::vector<const PlanTemplate*> picked;
    if (!req.onlyTemplateKeys.empty()) {
        for (const auto& k : req.onlyTemplateKeys) {
            const auto it = t.index.find(k);
            if (it != t.index.end()) picked.push_back(&t.items[static_cast<std::size_t>(it->second)]);
        }
    } else {
        for (const auto& item : t.items) {
            if (!side.empty() && item.side != side) continue;
            if (!scene.empty() && !item.scene.empty() && item.scene != scene) continue;
            picked.push_back(&item);
        }
    }

    std::map<std::string, std::string> firstByContent;  // contentKey → 保留的候选 id
    for (const PlanTemplate* tp : picked) {
        Candidate c;
        c.id = tp->key;
        c.key = tp->key;
        c.side = tp->side;
        c.scene = tp->scene;
        c.seq = tp->seq;
        c.name = tp->name;
        c.method = tp->method;
        c.clusters = tp->clusters;
        c.effect = tp->effect;
        c.profileKey = tp->profileKey;
        c.hasSuccessRate = tp->hasSuccessRate;
        c.successRate = tp->successRate;
        c.recommendedHint = tp->recommendedHint;
        c.source = tp->key;

        applicability(*tp, req.snapshot, c.inapplicableReasons);
        c.applicable = c.inapplicableReasons.empty();
        if (counters) {
            ++counters->candidatesGenerated;
            if (!c.applicable) ++counters->candidatesInapplicable;
        }

        if (req.dedupe) {
            const std::string ck = contentKey(*tp);
            const auto it = firstByContent.find(ck);
            if (it != firstByContent.end()) {
                // 内容等价：**合并进先出现者**并标注（候选总数不因去重而减少到 0，去向可追溯）
                DuplicateOf dup;
                dup.key = tp->key;
                dup.name = tp->name;
                dup.into = it->second;
                for (auto& kept : out) {
                    if (kept.id == it->second) {
                        kept.duplicates.push_back(dup);
                        break;
                    }
                }
                if (counters) ++counters->duplicatesMerged;
                continue;
            }
            firstByContent[ck] = c.id;
        }
        out.push_back(c);
    }
    return out;
}

// ---------------------------------------------------------------- 推荐理由（结构化，非话术）

namespace {

std::vector<ReasonItem> buildReasons(const MetricsModel& m, const ScoreResult& res,
                                     const CandidateScore& best,
                                     const std::optional<CandidateScore>& next) {
    std::vector<ReasonItem> out;
    if (best.metrics.empty()) return out;

    const int want = m.reasons.count > 0 ? m.reasons.count : 0;
    const std::vector<std::string>& types = m.reasons.types;
    auto typeAt = [&](int i) -> std::string {
        if (i < static_cast<int>(types.size())) return types[static_cast<std::size_t>(i)];
        return std::string("reason-") + std::to_string(i);
    };

    // 指标按归一值降序选"得分最高的指标"；同值取声明顺序（稳定）
    std::vector<std::size_t> byNormalized(best.metrics.size());
    for (std::size_t i = 0; i < byNormalized.size(); ++i) byNormalized[i] = i;
    std::stable_sort(byNormalized.begin(), byNormalized.end(),
                     [&](std::size_t a, std::size_t b) {
                         if (best.metrics[a].normalized != best.metrics[b].normalized) {
                             return best.metrics[a].normalized > best.metrics[b].normalized;
                         }
                         return a < b;
                     });
    // 指标按贡献度降序选"贡献最大的指标"（敏感性 —— SCD-EXP-04）
    std::vector<std::size_t> byContribution(best.metrics.size());
    for (std::size_t i = 0; i < byContribution.size(); ++i) byContribution[i] = i;
    std::stable_sort(byContribution.begin(), byContribution.end(),
                     [&](std::size_t a, std::size_t b) {
                         if (best.metrics[a].contributionScaled !=
                             best.metrics[b].contributionScaled) {
                             return best.metrics[a].contributionScaled >
                                    best.metrics[b].contributionScaled;
                         }
                         return a < b;
                     });

    for (int i = 0; i < want; ++i) {
        ReasonItem r;
        r.type = typeAt(i);
        r.order = i;
        if (r.type == "top-score" && !byNormalized.empty()) {
            const MetricScore& ms = best.metrics[byNormalized[0]];
            r.metricKey = ms.key;
            r.value = ms.normalized;
            r.valueScaled = scaled(ms.normalized, kScale);
        } else if (r.type == "top-contribution" && !byContribution.empty()) {
            const MetricScore& ms = best.metrics[byContribution[0]];
            r.metricKey = ms.key;
            r.value = ms.contribution;
            r.valueScaled = ms.contributionScaled;
        } else if (r.type == "lead") {
            r.value = res.leadOverNext;
            r.valueScaled = scaled(res.leadOverNext, kScale);
            r.refKey = res.nextId;
            r.metricKey = m.items.empty() ? std::string() : m.items.front().key;
        } else if (r.type == "advantage") {
            // 关键优势项：与次优差值最大的指标（逐项可比 —— SCD-PICK-04）
            std::size_t bestIdx = 0;
            double bestDelta = -1.0;
            const CandidateScore* other = next ? &(*next) : nullptr;
            for (std::size_t k = 0; k < best.metrics.size(); ++k) {
                double delta = 0.0;
                if (other) {
                    for (const auto& om : other->metrics) {
                        if (om.key == best.metrics[k].key) {
                            delta = best.metrics[k].normalized - om.normalized;
                            break;
                        }
                    }
                } else {
                    delta = best.metrics[k].normalized;
                }
                if (delta > bestDelta) {
                    bestDelta = delta;
                    bestIdx = k;
                }
            }
            const MetricScore& ms = best.metrics[bestIdx];
            r.metricKey = ms.key;
            r.value = bestDelta < 0.0 ? 0.0 : bestDelta;
            r.valueScaled = scaled(r.value, kScale);
            r.refKey = res.nextId;
        } else {
            // 规则包声明了更多类型：仍产出结构化条目（可追溯到指标），不造文案
            const std::size_t k = (byContribution.empty() ? 0 : byContribution[0]);
            if (!best.metrics.empty()) {
                r.metricKey = best.metrics[k].key;
                r.value = best.metrics[k].contribution;
                r.valueScaled = best.metrics[k].contributionScaled;
            }
        }
        out.push_back(r);
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------- 评分主流程

ScoreResult scoreCandidates(const MetricsModel& m, const TemplatesModel& t,
                            const CandidateRequest& req, int64_t ts, Metrics* counters) {
    ScoreResult res;
    res.missionId = req.missionId;
    res.side = req.side;
    res.scene = req.scene.empty() ? req.phase.scenarioKey : req.scene;
    res.metricsDigest = m.pack.digest;
    res.templatesDigest = t.pack.digest;
    res.ts = ts;
    res.auditInput = toJson(req.snapshot);
    for (const auto& d : m.items) {
        res.auditWeights.push_back(json{{"key", d.key}, {"weight", d.weight},
                                        {"direction", toString(d.direction)},
                                        {"normalize", toString(d.normalize.type)}});
    }

    std::vector<Candidate> cands = generateCandidates(m, t, req, counters);
    for (const Candidate& c : cands) {
        CandidateScore cs = scoreOne(m, t, c, req.snapshot, &res.missingInputs);
        res.candidates.push_back(std::move(cs));
    }
    std::stable_sort(res.candidates.begin(), res.candidates.end(), ScoreOrder());

    int rank = 0;
    for (auto& cs : res.candidates) {
        if (!cs.applicable) continue;
        cs.rank = ++rank;
    }

    // 推荐 = 排序结果的第一名（适用者）；规则包的 recommended 标注只作提示（SCD-PICK-02）
    const CandidateScore* best = nullptr;
    const CandidateScore* second = nullptr;
    for (const auto& cs : res.candidates) {
        if (!cs.applicable) continue;
        if (!best) best = &cs;
        else if (!second) {
            second = &cs;
            break;
        }
    }
    if (best) {
        res.hasRecommended = true;
        res.recommendedId = best->candidate.id;
        res.recommendedPercent = best->totalPercent;
        for (auto& cs : res.candidates) {
            cs.recommended = (cs.candidate.id == res.recommendedId);
        }
        if (second) {
            res.hasNext = true;
            res.nextId = second->candidate.id;
            res.nextPercent = second->totalPercent;
            // 领先次优的百分点（SCD-PICK-05）：内部定点 → 对外百分比（`scaled / scale × 100`）
            res.leadOverNext =
                scaledToDouble(best->totalScaled - second->totalScaled, kScale);
            res.leadOverNextPercent = best->totalPercent - second->totalPercent;
        } else {
            res.leadOverNext = 0.0;
            res.leadOverNextPercent = 0;
        }
        for (auto& cs : res.candidates) {
            if (cs.candidate.id == res.recommendedId && second) {
                cs.leadOverNext = res.leadOverNext;
                cs.leadOverNextPercent = res.leadOverNextPercent;
            }
        }
        std::optional<CandidateScore> nextCopy;
        if (second) nextCopy = *second;
        res.reasons = buildReasons(m, res, *best, nextCopy);
    }

    // 审计摘要：输入快照 + 权重 + 输出的规范化字节（SCD-EXP-03 的可追溯性）
    res.auditDigest = fnv1a64(canonicalJson(res.toAuditJson()));
    return res;
}

}  // namespace detail
}  // namespace scoring
