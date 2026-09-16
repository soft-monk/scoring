// examples/full_flow/main.cc · 全流程示例（候选 → 评分 → 推荐 → 优化 → 可解释）
//
// 演示四件事：
//   ① 候选数随模板数变化（不写死套数）；
//   ② 资源不足时方案标"不适用"且**候选总数不减少**（SCD-CAND-02）；
//   ③ 自动优化在规则声明的搜索空间内扰动重评，输出"改了什么 → 哪些指标变了 → 总分变化"（SCD-OPT-03）；
//   ④ 同输入双跑逐字节一致（SCD-SCORE-06）。
//
// 用法：example_full_flow [规则包目录]   退出码：0 / 1
#include <cstdio>
#include <memory>
#include <string>

#include "../common.h"

namespace {

/// 固定时钟：让"同输入双跑逐字节一致"可被断言（ts 也参与比对 —— SCD-SCORE-06 / P9）
class FixedClock : public scoring::IClock {
public:
    int64_t nowMs() const override { return 1750000000000LL; }
};

}  // namespace

int main(int argc, char** argv) {
    scoring::ScoringEngineOptions opts;
    opts.clock = std::make_shared<FixedClock>();
    scoring::ScoringEngine engine(opts);
    if (ex::loadPolicies(engine, ex::policyDir(argc, argv)) != 0) return 1;

    const scoring::TemplatesPack pack = engine.templatesPack();
    if (pack.items.empty()) return ex::fail("规则包内没有模板");
    const std::string side = pack.items.front().side;
    const scoring::PhaseContext phase = ex::phaseOf(pack, side, "phase-demo");

    // ---- ① 完整资源：全部候选适用 ----
    scoring::CandidateRequest full;
    full.missionId = phase.missionId;
    full.phase = phase;
    full.side = side;
    full.snapshot = ex::snapshotWithAllClusters(pack, phase);

    const scoring::ScoreResult r1 = engine.score(full);
    if (r1.code != 0) return ex::fail("评分失败");
    std::printf("\n①  候选 %zu 个 → 推荐 %s（%d%%），领先次优 %d 个百分点\n",
                r1.candidates.size(), r1.recommendedId.c_str(), r1.recommendedPercent,
                r1.leadOverNextPercent);

    // ---- ② 资源不足：候选总数不变，不适用项带原因 ----
    scoring::CandidateRequest scarce = full;
    if (scarce.snapshot.resources) {
        for (auto& c : scarce.snapshot.resources->clusters) c.available = false;
        for (auto& c : scarce.snapshot.resources->clusters) c.total = 0;
    }
    const scoring::ScoreResult r2 = engine.score(scarce);
    int inapplicable = 0;
    for (const auto& c : r2.candidates) {
        if (!c.applicable) ++inapplicable;
    }
    std::printf("②  资源不足：候选仍 %zu 个（不减少）｜不适用 %d 个｜推荐 %s\n",
                r2.candidates.size(), inapplicable, r2.recommendedId.c_str());
    for (const auto& c : r2.candidates) {
        if (c.applicable) continue;
        std::printf("      %s → %s（%s）\n", c.candidate.id.c_str(),
                    c.candidate.inapplicableReasons.empty()
                        ? "-"
                        : c.candidate.inapplicableReasons.front().code.c_str(),
                    c.candidate.inapplicableReasons.empty()
                        ? "-"
                        : c.candidate.inapplicableReasons.front().field.c_str());
    }
    if (r2.candidates.size() != r1.candidates.size()) {
        return ex::fail("不适用方案被静默丢弃（候选总数应不减少）");
    }

    // ---- ③ 自动优化（规则声明的搜索空间；单调、幂等）----
    scoring::OptimizeRequest oreq;
    oreq.missionId = phase.missionId;
    oreq.phase = phase;
    oreq.templateKey = r1.recommendedId;
    oreq.snapshot = full.snapshot;
    const scoring::OptimizeResult o1 = engine.optimize(oreq);
    std::printf("③  优化：status=%s %d%% → %d%%（Δ%d）｜迭代 %d｜耗时 %lld ms｜停止原因 %s\n",
                o1.status.c_str(), o1.beforePercent, o1.afterPercent, o1.delta, o1.iterations,
                static_cast<long long>(o1.elapsedMs), o1.stopReason.c_str());
    for (const auto& p : o1.paramChanges) {
        std::printf("      参数 %s → %s：%+.2f（%d 步）\n", p.key.c_str(), p.metricKey.c_str(),
                    p.delta, p.steps);
    }
    for (const auto& m : o1.metricChanges) {
        std::printf("      指标 %s：归一值 %.4f → %.4f，贡献度 %.4f → %.4f\n", m.key.c_str(),
                    m.normalizedBefore, m.normalizedAfter, m.contributionBefore,
                    m.contributionAfter);
    }
    if (o1.delta < 0) return ex::fail("优化后总分低于优化前（违反单调性）");
    const scoring::OptimizeResult o2 = engine.optimize(oreq);
    if (o2.beforePercent != o1.beforePercent || o2.afterPercent != o1.afterPercent) {
        return ex::fail("优化不幂等（重复调用结果漂移）");
    }
    std::printf("      幂等：重复调用 %d%% → %d%%（与首次一致）\n", o2.beforePercent,
                o2.afterPercent);

    // ---- ④ 同输入双跑逐字节一致（ts 由注入时钟决定，两次调用之间不推进时钟）----
    const scoring::ScoreResult a = engine.score(full);
    const scoring::ScoreResult b = engine.score(full);
    const std::string ja = a.toJson().dump();
    const std::string jb = b.toJson().dump();
    std::printf("④  双跑字节一致：%s（输出 %zu 字节）\n", ja == jb ? "是" : "否", ja.size());
    if (ja != jb) return ex::fail("同输入双跑输出不一致");

    std::printf("\n全流程通过（退出码 0）\n");
    return 0;
}
