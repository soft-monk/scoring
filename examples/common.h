// examples/common.h · 示例共用脚手架（**只读规则包，不内建任何业务取值**）
//
// 纪律（P6/P7）：示例属引擎产物，内 MUST NOT 出现方案名、指标演示数值或期望分数。
// 一切取值从注入的规则包读；场景键与集群 key 由**规则包自身**推出，不写死。
#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "scoring/scoring.h"

namespace ex {

inline std::string policyDir(int argc, char** argv) {
#ifdef SCORING_POLICY_DIR
    const std::string def = SCORING_POLICY_DIR;
#else
    const std::string def = "policies/mapapp";
#endif
    if (argc > 1) return std::string(argv[1]);
    return def;
}

/// 装载规则包；失败时打印逐条原因并返回非零
inline int loadPolicies(scoring::ScoringEngine& engine, const std::string& dir) {
    const scoring::LoadResult r = engine.policiesFromDir(dir);
    std::printf("规则包目录：%s\n", dir.c_str());
    std::printf("装载结果：code=%d（%s）%s\n", r.code, scoring::errorCodeName(r.code),
                r.message.c_str());
    if (r.code != 0) {
        for (const auto& i : r.issues) {
            std::printf("  问题：%s %s → %s\n", i.path.c_str(), i.field.c_str(), i.reason.c_str());
        }
        return 1;
    }
    const scoring::Capabilities cap = engine.capabilities();
    std::printf("指标 %d 项（权重和 %.4f）｜模板 %d 套｜剖面 %d 个｜可调参数 %d 个\n",
                cap.metricCount, cap.weightSum, cap.templateCount, cap.profileCount,
                cap.searchParamCount);
    std::printf("理由口径 %d 条｜确认前置策略 %s｜迭代上限 %d｜时限 %lld ms\n", cap.reasonCount,
                cap.confirmPrecondition.c_str(), cap.maxIterations,
                static_cast<long long>(cap.timeBudgetMs));
    return 0;
}

/// 只取规则包里**实际存在**的某一侧模板（键与侧别都来自规则包）
inline std::vector<std::string> keysOfSide(const scoring::TemplatesPack& pack,
                                           const std::string& side) {
    std::vector<std::string> out;
    for (const auto& t : pack.items) {
        if (t.side == side) out.push_back(t.key);
    }
    return out;
}

/// 由某一侧的第一个模板推出场景键与集群 key（不写死任何业务取值）
inline scoring::PhaseContext phaseOf(const scoring::TemplatesPack& pack, const std::string& side,
                                     const std::string& phaseKey) {
    scoring::PhaseContext p;
    for (const auto& t : pack.items) {
        if (t.side == side) {
            p.scenarioKey = t.scene;
            break;
        }
    }
    p.phaseKey = phaseKey;
    p.seq = 1;
    p.enteredAt = 1750000000000LL;
    p.missionId = "demo-mission";
    return p;
}

/// 造一个**完整可用**的资源快照：集群 key 全部来自规则包（示例不内建业务词）
inline scoring::ScoringSnapshot snapshotWithAllClusters(const scoring::TemplatesPack& pack,
                                                        const scoring::PhaseContext& phase) {
    scoring::ScoringSnapshot snap;
    snap.missionId = phase.missionId;
    snap.phase = phase;
    scoring::ResourceSnapshot res;
    res.present = true;
    res.phase = phase;
    for (const auto& t : pack.items) {
        for (const auto& c : t.clusters) {
            bool seen = false;
            for (const auto& u : res.clusters) {
                if (u.clusterId == c) {
                    seen = true;
                    break;
                }
            }
            if (seen) continue;
            scoring::ClusterUsage u;
            u.clusterId = c;
            u.phaseKey = phase.phaseKey;
            u.total = 4;
            u.available = true;
            res.clusters.push_back(u);
        }
    }
    snap.resources = res;
    // 链路评估与目标清单**故意不给**：验证"缺项按中性值 + 标注缺失，不阻塞评分"（风险 R6）
    return snap;
}

inline int fail(const char* what) {
    std::printf("失败：%s\n", what);
    return 1;
}

}  // namespace ex
