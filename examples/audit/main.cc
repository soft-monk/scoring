// examples/audit/main.cc · 可复算示例（ADR-C1-04 / SCD-SCORE-04 / SCD-EXP-03）
//
// 打印「原始值 / 归一值 / 权重 / 贡献度」四项，并把导出项**手算累加**与引擎总分逐位比对；
// 同时演示审计三件套（输入快照 + 权重 + 输出）可完整复算当时的总分。
//
// 用法：example_audit [规则包目录]   退出码：0 = 复算一致；1 = 不一致
#include <cstdio>

#include "../common.h"

namespace {

/// **独立于引擎的**手算：Σ round(归一值 × 权重 × 标度) → 整数百分比（只用导出项）
int handRecompute(const scoring::CandidateScore& c) {
    long long sum = 0;
    for (const auto& m : c.metrics) {
        sum += scoring::roundHalfUpScaled(m.normalized * m.weight * scoring::kScale);
    }
    return scoring::roundToPercent(sum);
}

/// 由规则包参数（baseline / weight）独立算出"锚点总分"：不是引擎计算，而是规则的直接推论
int anchorFromRules(const scoring::MetricsPack& pack) {
    long long sum = 0;
    for (const auto& d : pack.items) {
        if (!d.hasBaseline) continue;
        sum += scoring::roundHalfUpScaled(scoring::normalizeValue(d, d.baseline) * d.weight *
                                          scoring::kScale);
    }
    return scoring::roundToPercent(sum);
}

}  // namespace

int main(int argc, char** argv) {
    scoring::ScoringEngine engine;
    if (ex::loadPolicies(engine, ex::policyDir(argc, argv)) != 0) return 1;

    const scoring::TemplatesPack pack = engine.templatesPack();
    const scoring::MetricsPack metrics = engine.metricsPack();
    if (pack.items.empty()) return ex::fail("规则包内没有模板");

    const std::string side = pack.items.front().side;
    const scoring::PhaseContext phase = ex::phaseOf(pack, side, "phase-demo");

    scoring::CandidateRequest req;
    req.missionId = phase.missionId;
    req.phase = phase;
    req.side = side;
    req.snapshot = ex::snapshotWithAllClusters(pack, phase);

    const scoring::ScoreResult res = engine.score(req);
    if (res.code != 0) return ex::fail("评分失败");
    const auto best = res.byId(res.recommendedId);
    if (!best) return ex::fail("推荐项不在候选列表内");

    std::printf("\n===== 推荐项逐项导出（可手算复算）=====\n");
    std::printf("%-24s %10s %10s %8s %12s %s\n", "指标", "原始值", "归一值", "权重", "贡献度",
                "取值来源");
    for (const auto& m : best->metrics) {
        std::printf("%-24s %10.4f %10.4f %8.4f %12.4f %s%s\n", m.key.c_str(), m.raw,
                    m.normalized, m.weight, m.contribution, m.sourceField.c_str(),
                    m.missing ? "（缺失标注）" : "");
    }

    long long scaleSum = 0;
    for (const auto& m : best->metrics) scaleSum += m.contributionScaled;
    std::printf("\n导出项定点累加 contributionScaled = %lld（标度 %lld）\n", scaleSum,
                static_cast<long long>(scoring::kScale));
    std::printf("Σ 贡献度 = %.4f → 四舍五入 = %d%%\n",
                scoring::scaledToDouble(scaleSum),
                scoring::roundToPercent(scaleSum));
    std::printf("引擎总分 = %d%%（totalScaled=%lld）\n", best->totalPercent,
                static_cast<long long>(best->totalScaled));

    // ① 手算（只用导出项）vs 引擎
    const int hand = handRecompute(*best);
    std::printf("\n[检查] 手算（Σ 归一值×权重） = %d%% ；引擎 = %d%% → %s\n", hand,
                best->totalPercent, hand == best->totalPercent ? "一致" : "不一致");
    if (hand != best->totalPercent) return ex::fail("手算与引擎总分不一致");

    // ② 规则包基线锚点（推荐项取值全部来自规则包基线时逐位一致）
    bool allBaseline = true;
    for (const auto& m : best->metrics) {
        if (m.sourceField != "baseline") {
            allBaseline = false;
            break;
        }
    }
    const int anchor = anchorFromRules(metrics);
    std::printf("[检查] 规则包 baseline×weight 的独立推论 = %d%%（推荐项取值%s）\n", anchor,
                allBaseline ? "全部命中基线" : "来自模板剖面 / 快照");
    if (allBaseline && best->totalPercent != anchor) {
        return ex::fail("推荐项取值全部命中基线，但总分与规则包推论不一致");
    }
    // 提示：本波参数设定下推荐项的六项取值与规则包基线一致，因此 anchor == 引擎总分；
    // 若规则包把推荐项改成走模板剖面，anchor 与总分可能不同 —— 这属参数口径，不是引擎差异。
    if (best->totalPercent != anchor) {
        std::printf("[提示] 引擎总分 %d%% 与基线推论 %d%% 不同：推荐项取值来自模板剖面\n",
                    best->totalPercent, anchor);
    }

    // ③ 审计三件套复算（SCD-EXP-03）
    const scoring::json audit = res.toAuditJson(true);
    bool auditOk = false;
    for (const auto& c : audit["output"]["candidates"]) {
        if (c.value("id", std::string()) != res.recommendedId) continue;
        long long s = 0;
        bool hasMetrics = false;
        if (c.contains("metrics")) {
            hasMetrics = true;
            for (const auto& m : c["metrics"]) {
                s += m.value("contributionScaled", 0LL);
            }
        }
        if (!hasMetrics) break;
        const int recomputed = scoring::roundToPercent(s);
        auditOk = (recomputed == c.value("total", -1));
        std::printf("[检查] 审计三件套复算 = %d%%（存档总分 %d%%）→ %s\n", recomputed,
                    c.value("total", -1), auditOk ? "一致" : "不一致");
        break;
    }
    std::printf("审计摘要 auditDigest = %s｜规则包摘要 %s + %s\n",
                scoring::digestHex(res.auditDigest).c_str(), res.metricsDigest.c_str(),
                res.templatesDigest.c_str());
    if (!auditOk) return ex::fail("审计三件套无法复算总分");

    std::printf("\n复算通过（退出码 0）\n");
    return 0;
}
