// examples/minimal/main.cc · 最小可用示例（一条命令跑通：装载规则包 → 评分 → 推荐）
//
// 用法：example_minimal [规则包目录]
// 退出码：0 = 通过；1 = 失败
#include <cstdio>

#include "../common.h"

int main(int argc, char** argv) {
    scoring::ScoringEngine engine;
    if (ex::loadPolicies(engine, ex::policyDir(argc, argv)) != 0) return 1;

    const scoring::TemplatesPack pack = engine.templatesPack();
    if (pack.items.empty()) return ex::fail("规则包内没有模板");

    const std::string side = pack.items.front().side;
    const scoring::PhaseContext phase = ex::phaseOf(pack, side, "phase-demo");

    scoring::CandidateRequest req;
    req.missionId = phase.missionId;
    req.phase = phase;
    req.side = side;
    req.snapshot = ex::snapshotWithAllClusters(pack, phase);

    const scoring::ScoreResult res = engine.score(req);
    std::printf("\n评分信封：code=%d（%s）\n", res.code, scoring::errorCodeName(res.code));
    if (res.code != 0) return 1;

    std::printf("候选 %zu 个（侧别 %s，场景 %s）\n", res.candidates.size(), res.side.c_str(),
                res.scene.c_str());
    for (const auto& c : res.candidates) {
        std::printf("  [%s] %-28s 总分 %3d%%  适用=%s\n", c.candidate.id.c_str(),
                    c.candidate.name.c_str(), c.totalPercent, c.applicable ? "是" : "否");
        for (const auto& r : c.candidate.inapplicableReasons) {
            std::printf("        不适用原因：%s（%s）\n", r.code.c_str(), r.field.c_str());
        }
    }
    std::printf("推荐：%s（%d%%），领先次优 %d 个百分点\n", res.recommendedId.c_str(),
                res.recommendedPercent, res.leadOverNextPercent);
    std::printf("结构化推荐理由 %zu 条（无自然语言话术）：\n", res.reasons.size());
    for (const auto& r : res.reasons) {
        std::printf("  {%s, %s, %.4f}\n", r.type.c_str(), r.metricKey.c_str(),
                    scoring::scaledToDouble(r.valueScaled));
    }
    if (!res.missingInputs.empty()) {
        std::printf("缺失输入标注：");
        for (const auto& m : res.missingInputs) std::printf("%s ", m.c_str());
        std::printf("\n");
    }
    return 0;
}
