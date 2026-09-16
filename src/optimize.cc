// src/optimize.cc · scoring —— 自动优化（受约束的扰动重评 —— SCD-OPT-01..05）
//
// 替换现状假优化（原实现把成功率写死成一个固定增量并截断上限，见冲突裁决 C1）：
//   · 优化 = **在规则包声明的搜索空间内**扰动参数 → 重评 → 取最优（SCD-OPT-02）
//   · 单调：优化后总分 MUST NOT 低于优化前；无改进 → status="no-improvement" 且总分不变（SCD-OPT-04）
//   · 幂等：同输入连续调用 MUST 收敛且不漂移（步数上界 + 迭代上限使其必然是有限搜索）
//   · 可解释：输出"改了什么参数 → 哪些指标变了 → 总分变化"（SCD-OPT-03）
//   · 有界：maxIterations / timeBudgetMs 双向封顶，超限按时返回并标 truncated（SCD-OPT-05）
//
// 本文件 MUST NOT 出现任何硬编码的指标增益、数值上限或方案名；一切取值来自规则包（P6/P7）。
#include <algorithm>
#include <chrono>
#include <cmath>

#include "internal.h"

namespace scoring {
namespace detail {

namespace {

/// 逐指标峰值搜索：返回"在预算内能把总分抬到的最大步数"。
/// 对每个参数都从 1 步起**逐级试到 maxSteps**（不做步长倍增 —— 避免把最优点跳过去）。
int searchParam(const MetricsModel& m, const TemplatesModel& t, const Candidate& cand,
                const ScoringSnapshot& snap, const AdjustableParam& p, const MetricDef& def,
                std::map<std::string, double>& lifts, int64_t baselineTotal, int budget,
                int64_t timeBudgetMs, int& iterations, bool& truncated, std::string& stopReason,
                const std::function<int64_t()>& elapsed) {
    const int64_t maxSteps = static_cast<int64_t>(p.maxSteps > 0.0 ? p.maxSteps : 0.0);
    if (maxSteps <= 0 || p.step == 0.0) return 0;
    int accepted = 0;
    int64_t best = baselineTotal;
    for (int64_t k = 1; k <= maxSteps; ++k) {
        if (iterations >= budget) {
            truncated = true;
            stopReason = "max-iterations";
            break;
        }
        if (timeBudgetMs > 0 && elapsed() > timeBudgetMs) {
            truncated = true;
            stopReason = "time-budget";
            break;
        }
        ++iterations;
        const double keep = lifts.count(p.metricKey) ? lifts[p.metricKey] : 0.0;
        lifts[p.metricKey] = p.step * static_cast<double>(k);
        const CandidateScore trial = scoreOne(m, t, cand, snap, nullptr, &lifts);
        if (trial.totalScaled > best) {
            best = trial.totalScaled;
            accepted = static_cast<int>(k);
        } else {
            lifts[p.metricKey] = keep;  // 该步无改进：回退（单调性的实现基础）
        }
    }
    // 收敛到"被接受的最大步数"（若中间步被接受而后续步无改进，保留最优步）
    if (accepted > 0) {
        lifts[p.metricKey] = p.step * static_cast<double>(accepted);
    } else if (lifts.count(p.metricKey) && best == baselineTotal) {
        lifts[p.metricKey] = 0.0;
    }
    (void)def;
    return accepted;
}

}  // namespace

OptimizeResult runOptimize(const MetricsModel& m, const TemplatesModel& t,
                           const OptimizeRequest& req, int64_t ts, Metrics* counters) {
    OptimizeResult out;
    out.planId = req.templateKey;
    out.ts = ts;

    const auto it = t.index.find(req.templateKey);
    if (it == t.index.end()) {
        out.code = static_cast<int>(ErrorCode::NotFound);
        out.message = "plan template not found";
        out.status = "rejected";
        out.stopReason = "not-found";
        return out;
    }
    // 适用性走与候选生成**同一套**机械判定（不适用方案默认不参与优化 —— SCD-CAND-02）
    CandidateRequest creq;
    creq.missionId = req.missionId;
    creq.phase = req.phase;
    creq.onlyTemplateKeys = {req.templateKey};
    creq.snapshot = req.snapshot;
    creq.dedupe = false;
    const std::vector<Candidate> cands = generateCandidates(m, t, creq, nullptr);
    if (cands.empty()) {
        out.code = static_cast<int>(ErrorCode::NotFound);
        out.message = "plan template not found";
        out.status = "rejected";
        out.stopReason = "not-found";
        return out;
    }
    Candidate cand = cands.front();
    if (!cand.applicable && !req.includeInapplicable) {
        out.code = static_cast<int>(ErrorCode::PreconditionUnmet);
        out.message = "plan not applicable";
        out.status = "rejected";
        out.stopReason = "not-applicable";
        for (const auto& r : cand.inapplicableReasons) out.unmetReasons.push_back(r.code);
        return out;
    }
    // 不适用但显式要求优化：仍按同一套解析评分（缺失取中性/基线），不静默跳过
    cand.applicable = true;

    const int budget =
        req.maxIterationsOverride > 0 ? req.maxIterationsOverride : m.search.maxIterations;
    const int64_t timeBudgetMs = m.search.timeBudgetMs;

    const auto t0 = std::chrono::steady_clock::now();
    auto elapsed = [&]() -> int64_t {
        return static_cast<int64_t>(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                .count());
    };

    std::map<std::string, double> lifts;  // metricKey → 累计增益（原始值量纲）
    const CandidateScore before = scoreOne(m, t, cand, req.snapshot, nullptr, &lifts);
    int64_t bestTotal = before.totalScaled;
    int iterations = 0;
    bool truncated = false;
    std::string stopReason = "converged";

    // 参数按规则包声明顺序逐个优化；每个参数在 [0, maxSteps] 内取最优（确定性 → 幂等）
    std::map<std::string, int> acceptedSteps;
    std::map<std::string, double> attemptedLift;  // 尝试过的最大增益（供解释输出）
    for (const AdjustableParam& p : m.search.params) {
        const auto mit = m.index.find(p.metricKey);
        if (mit == m.index.end()) continue;
        const MetricDef& def = m.items[static_cast<std::size_t>(mit->second)];
        attemptedLift[p.metricKey] = p.step * (p.maxSteps > 0.0 ? p.maxSteps : 0.0);
        const int64_t startTotal = bestTotal;
        const int acc = searchParam(m, t, cand, req.snapshot, p, def, lifts, startTotal, budget,
                                    timeBudgetMs, iterations, truncated, stopReason, elapsed);
        acceptedSteps[p.metricKey] = acc;
        const CandidateScore probe = scoreOne(m, t, cand, req.snapshot, nullptr, &lifts);
        bestTotal = probe.totalScaled;
        if (truncated) break;
    }

    const CandidateScore after = scoreOne(m, t, cand, req.snapshot, nullptr, &lifts);
    out.before = before;
    out.beforeMetrics = before.metrics;
    out.iterations = iterations;
    out.elapsedMs = elapsed();

    // 单调性：优化后 MUST NOT 低于优化前（SCD-OPT-04）
    if (after.totalScaled < before.totalScaled) {
        out.code = static_cast<int>(ErrorCode::Internal);
        out.message = "optimization produced a worse score";
        out.status = "rejected";
        out.stopReason = "non-monotonic";
        return out;
    }

    out.beforePercent = before.totalPercent;
    out.afterPercent = after.totalPercent;
    out.delta = out.afterPercent - out.beforePercent;
    out.before = before;
    out.afterMetrics = after.metrics;
    out.improved = (after.totalScaled > before.totalScaled);
    out.hasAfter = true;
    out.truncated = truncated;

    if (!out.improved) {
        out.code = 0;
        out.message = "no improvement";
        out.status = "no-improvement";
        out.after = before;  // 无改进时**总分不变**（原样返回）
        out.afterMetrics = before.metrics;
        out.afterPercent = before.totalPercent;
        out.delta = 0;
        if (counters) ++counters->noImprovement;
        return out;
    }

    out.code = 0;
    out.message = truncated ? "improved (truncated)" : "improved";
    out.status = truncated ? "truncated" : "improved";
    out.after = after;

    // 参数变化（改了什么）——仅输出真正被采纳的项
    for (const AdjustableParam& p : m.search.params) {
        const auto mit = m.index.find(p.metricKey);
        if (mit == m.index.end()) continue;
        const auto accIt = acceptedSteps.find(p.metricKey);
        if (accIt == acceptedSteps.end() || accIt->second <= 0) continue;
        ParamChange pc;
        pc.key = p.key;
        pc.metricKey = p.metricKey;
        pc.steps = accIt->second;
        pc.delta = p.step * static_cast<double>(accIt->second);
        pc.before = 0.0;
        pc.after = pc.delta;
        out.paramChanges.push_back(pc);
    }
    // 指标变化（哪些指标变了）
    for (std::size_t i = 0; i < after.metrics.size(); ++i) {
        const MetricScore& am = after.metrics[i];
        const MetricScore* bm = nullptr;
        for (const auto& x : before.metrics) {
            if (x.key == am.key) {
                bm = &x;
                break;
            }
        }
        if (!bm) continue;
        if (bm->raw == am.raw && bm->normalized == am.normalized &&
            bm->contributionScaled == am.contributionScaled) {
            continue;
        }
        MetricChange mc;
        mc.key = am.key;
        mc.rawBefore = bm->raw;
        mc.rawAfter = am.raw;
        mc.normalizedBefore = bm->normalized;
        mc.normalizedAfter = am.normalized;
        mc.contributionBefore = bm->contribution;
        mc.contributionAfter = am.contribution;
        out.metricChanges.push_back(mc);
    }
    out.stopReason = truncated ? stopReason : "converged";
    if (counters) ++counters->optimized;
    if (truncated && counters) ++counters->truncated;
    return out;
}

}  // namespace detail
}  // namespace scoring
