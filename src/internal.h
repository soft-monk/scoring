// src/internal.h · scoring —— 内部实现细节（**宿主 MUST NOT 包含本文件**）
//
// 公开面只有 include/scoring/scoring.h（P1）。本文件放引擎内部状态与定点工具：
//   · Impl        —— ScoringEngine 的唯一实现（规则包 + 注入依赖 + 进程内状态缓存）
//   · 定点工具    —— half-up 舍入 / 归一 / 定点累加（SCD-SCORE-06、确定性）
//   · 取值来源链  —— 规则包声明的 source 解析（SCD-SCORE-01/02）
//
// 确定性要求（SCD-SCORE-06 / SCD-NFR-05）：本文件内 MUST NOT 出现
//   unordered_map 迭代 / 指针地址比较 / 随机数 / 系统时间 / 浮点累加。
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "scoring/scoring.h"

namespace scoring {
namespace detail {

// ---------------------------------------------------------------- 常量

/// 权重和的比较容差（浮点权重由规则包给出：0.17×4 + 0.16×2 = 1.00）
inline constexpr double kWeightEpsilon = 1e-9;

// ---------------------------------------------------------------- 定点工具

/// 双精度 → 定点整数（half-up）。负值按绝对值舍入后取负，保证对称。
int64_t scaled(double value, int64_t scale);
/// 定点 → 双精度
inline double unscaled(int64_t v, int64_t scale) {
    return scale == 0 ? 0.0 : static_cast<double>(v) / static_cast<double>(scale);
}
/// 截断到 [lo,hi]
inline double clampTo(double v, double lo, double hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}
/// **对外标度**：百分比保留两位小数（对外数值一律先 half-up 到该标度）
inline constexpr int64_t kExportScale = 100;
/// 内部定点（92.96% ↔ 9296）→ 对外数值（百分比，两位小数）
inline double roundedContribution(int64_t scaledValue) {
    return static_cast<double>(scaled(unscaled(scaledValue, kScale) * 100.0, kExportScale)) /
           static_cast<double>(kExportScale);
}/// 数字 → 稳定字符串（整数不带小数点；确定性序列化用）
std::string numToStableString(double v);

// ---------------------------------------------------------------- JSON 取值助手
// 全部**不抛异常**：类型不符按"缺失"处理，由调用方决定是否标注（P10）。

const json* find(const json& obj, const std::string& key);
std::string getString(const json& obj, const std::string& key, const std::string& def = "");
double getNumber(const json& obj, const std::string& key, double def = 0.0);
bool getBool(const json& obj, const std::string& key, bool def = false);
int getInt(const json& obj, const std::string& key, int def = 0);
std::vector<std::string> getStringArray(const json& obj, const std::string& key);
bool hasKey(const json& obj, const std::string& key);

/// 规范化序列化：对象键按 ASCII 升序、数组保序、数字用最短稳定表示、不含空白。
/// 用于 digest（同一份规则内容字节 → 同一 digest）。
std::string canonicalJson(const json& v);

/// 规则包骨架校验（policiesNamespace / schemaVersion MAJOR / kind）—— 供两个 kind 复用
struct SkeletonCheck {
    bool ok = false;
    int code = 0;
    std::string message;
    std::string policiesNamespace;
    std::string schemaVersion;
    std::string digest;
    std::vector<std::string> warnings;
    std::vector<LoadIssue> issues;
};
SkeletonCheck checkSkeleton(const json& pkg, const std::string& expectedKind);

// ---------------------------------------------------------------- 规则模型（引擎内部解析结果）

/// 搜索空间 + 理由口径 + 确认前置策略 + 中性规则（都住在指标/模板包，引擎只实现机制）
struct ReasonSpec {
    int count = 0;                                       // 理由条数（规则包声明）
    std::vector<std::string> types;                      // 理由类型序列（机器可读枚举取值）
};

/// 解析后的指标包（内部形状；对外只暴露 MetricsPack 的只读视图）
struct MetricsModel {
    MetricsPack pack;
    AggregateSpec aggregate;
    std::vector<MetricDef> items;
    SearchSpace search;
    ReasonSpec reasons;
    bool reasonsDeclared = false;
    /// 指标 key → 声明下标（有序 map：迭代顺序 = 声明顺序）
    std::map<std::string, int> index;
};

/// 解析后的模板包
struct TemplatesModel {
    TemplatesPack pack;
    std::vector<ProfileDef> profiles;
    std::vector<PlanTemplate> items;
    std::map<std::string, int> index;
    std::map<std::string, int> profileIndex;
    ConfirmPrecondition confirmMode = ConfirmPrecondition::RejectIfNotAdopted;
    std::string confirmModeName;
    bool confirmModeDeclared = false;
};

// ---- 规则包解析（定义在 src/policies.cc）----

/// 解析指标包到内部模型（**不落状态**；成功时 out 被整体替换）
LoadResult loadMetricsInto(const json& pkg, MetricsModel& out);
/// 解析模板包到内部模型；`metrics` 非空时做交叉校验（剖面必须覆盖模板来源指标）
LoadResult loadTemplatesInto(const json& pkg, TemplatesModel& out, const MetricsModel* metrics);
/// 读 JSON 文件（失败时 err 给可读原因；MUST NOT 抛异常）
bool readJsonFile(const std::string& path, json& out, std::string& err);

// ---------------------------------------------------------------- 取值解析

/// 一次取值的解析结果（可追溯 —— SCD-SCORE-04 的 sourceField）
struct ResolvedValue {
    double raw = 0.0;
    bool found = false;
    bool usedBaseline = false;
    bool missing = false;         // 输入缺失（标注缺失，不阻塞评分）
    bool fromProfile = false;
    bool fromSnapshot = false;
    std::string sourceField;      // 实际取自哪里（可读、可追溯）
    std::string missingMarker;
};

/// 解析某指标在某候选上的原始值（来源链：快照 → 剖面 → 声明来源 → 中性值 → 基线）
/// `lifts`（可空）为优化阶段的参数增益：`{metricKey → 增量}`，按指标归一区间截断。
/// **指标名与增益值全部来自规则包的 searchSpace 声明，引擎不内建任何取值**（SCD-OPT-02）。
ResolvedValue resolveRaw(const MetricsModel& m, const TemplatesModel& t, const PlanTemplate& tmpl,
                         const MetricDef& def, const ScoringSnapshot& snap,
                         const std::map<std::string, double>* lifts = nullptr);

// ---------------------------------------------------------------- 进程内状态（无 DB、无全局单例）

/// 同侧状态的进程内缓存（store 为权威；未注入 store 时它就是权威 —— SCD-DECIDE-01）
struct PlanStateCache {
    /// missionId → side → planId → state
    std::map<std::string, std::map<std::string, std::map<std::string, PlanState>>> byMission;

    void put(const PlanState& st);
    std::optional<PlanState> get(const std::string& missionId, const std::string& planId) const;
    std::vector<PlanState> listSide(const std::string& missionId, const std::string& side) const;
    std::vector<PlanState> list(const PlanQuery& q) const;
    std::vector<std::string> clearSide(const std::string& missionId, const std::string& side,
                                      const std::string& exceptPlanId);
};

}  // namespace detail

/// 引擎实现（PIMPL：公开头不暴露任何实现细节）
struct ScoringEngine::Impl {
    Impl() = default;
    // ---- 规则包 ----
    detail::MetricsModel metrics;
    detail::TemplatesModel templates;
    bool metricsLoaded = false;
    bool templatesLoaded = false;

    // ---- 注入依赖 ----
    std::shared_ptr<IPlanStore> store;
    std::shared_ptr<IPlanSink> sink;
    std::shared_ptr<IClock> clock;
    std::shared_ptr<ILogSink> log;

    // ---- 进程内状态 ----
    detail::PlanStateCache cache;
    /// "missionId|side" → 持有计数（同侧互斥闸门 —— C16/C17；计数为 0 时移除）
    std::map<std::string, int> inFlight;
    mutable std::recursive_mutex mu;
    Metrics counters;

    // ---- 时钟（至多一次） ----
    int64_t nowMs();
};

namespace detail {

/// 同侧互斥闸门（try_lock 语义：拿不到立刻返回 false，MUST NOT 阻塞 —— CTR-EC-03）
///
/// 语义：`inFlight` 里**是否存在**该 key 就是"同侧是否被持有"的唯一判据；
/// 析构时按**引用计数**递减（计数归零才移除），因此嵌套/重入不会互相清掉闸门。
class SideGate {
public:
    /// `take=false` 表示调用方已持有同一把闸门（复用，不重复计数）
    SideGate(std::recursive_mutex& mu, std::map<std::string, int>& inFlight, const std::string& key,
             Metrics& counters, bool take = true);
    ~SideGate();
    SideGate(const SideGate&) = delete;
    SideGate& operator=(const SideGate&) = delete;
    bool acquired() const { return acquired_; }

private:
    std::recursive_mutex& mu_;
    std::map<std::string, int>& inFlight_;
    std::string key_;
    Metrics& counters_;
    bool acquired_ = false;  // 闸门可用（"同侧没被别人持有"）
    bool owned_ = false;     // 本对象是否真的给自己记了一笔（take=true 且拿到时）
};

/// 引擎核心：候选生成（不改状态，const）
std::vector<Candidate> generateCandidates(const MetricsModel& m, const TemplatesModel& t,
                                          const CandidateRequest& req, Metrics* counters);

/// 引擎核心：对一组候选评分 + 排序 + 推荐 + 理由（纯计算）
ScoreResult scoreCandidates(const MetricsModel& m, const TemplatesModel& t,
                            const CandidateRequest& req, int64_t ts, Metrics* counters);

/// 引擎核心：自动优化（受约束的扰动重评 —— SCD-OPT）
OptimizeResult runOptimize(const MetricsModel& m, const TemplatesModel& t,
                           const OptimizeRequest& req, int64_t ts, Metrics* counters);

/// 引擎核心：采纳 / 确认裁决（SCD-DECIDE）
/// `autoConfirm=true` 表示"自动采纳并确认"（confirm 的 auto-adopt 路径）。
/// **两侧都 MUST 由调用方（ScoringEngine::adopt / ::confirm）先持有同侧闸门** ——
/// 本函数不碰闸门，以便 confirm 的 auto-adopt 路径在同一次持锁内复用裁决。
DecideResult runAdopt(ScoringEngine::Impl& impl, const AdoptRequest& req, bool autoConfirm);
DecideResult runConfirm(ScoringEngine::Impl& impl, const ConfirmRequest& req);

/// 单候选评分（供评分与优化共用）
CandidateScore scoreOne(const MetricsModel& m, const TemplatesModel& t, const Candidate& c,
                        const ScoringSnapshot& snap, std::vector<std::string>* missingInputs,
                        const std::map<std::string, double>* lifts = nullptr);

/// 快照 ⇄ JSON（宿主投喂与审计三件套共用；camelCase，键序 = 成员声明顺序）
json toJson(const PhaseContext& v);
json toJson(const ClusterUsage& v);
json toJson(const ResourceSnapshot& v);
json toJson(const LinkEval& v);
json toJson(const TopologySnapshot& v);
json toJson(const TargetList& v);
json toJson(const ScoringSnapshot& v);
/// 由宿主投喂的 JSON 构造快照（缺项一律按"缺失"处理 —— 不阻塞评分）
ScoringSnapshot snapshotFromJson(const json& v);
/// 资源/链路/目标清单的分段解析（宿主可用于分别投喂）
ResourceSnapshot resourceFromJson(const json& v);
TopologySnapshot topologyFromJson(const json& v);
TargetList targetsFromJson(const json& v);

/// 比较器：总分降序 → 声明序号 → 声明下标 → id（**稳定次级排序键** —— SCD-PICK-01）
struct ScoreOrder {
    bool operator()(const CandidateScore& a, const CandidateScore& b) const;
};

}  // namespace detail
}  // namespace scoring
