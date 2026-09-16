// scoring · scoring.h —— 唯一公开头文件
//
// 权威依据（本文件不新增任何业务取值，一切业务内容来自规则包）：
//   · 需求专篇 docs/需求/scoring需求专篇.md（SCD-CAND 4 / SCD-SCORE 6 / SCD-PICK 5 /
//     SCD-EXP 4 / SCD-OPT 5 / SCD-DECIDE 5 / SCD-NFR 5，共 34 条）
//   · 共享契约 ../phase-engine/docs/契约/protocol.md v1.0
//       P1–P10、§1.4 PhaseContext、§3 错误码、§4 事件名、§5 policies schema、§6 反向接口命名
//   · 冲突裁决 ../phase-engine/docs/契约/冲突裁决.md
//       C1（评分的唯一权威来源是本引擎的计算输出；数值基线住规则包）
//       C2（方案台账唯一来源 = 规则包 kind:"planTemplates"；DB plan 表降级为缓存）
//       C15（废弃 1001：幂等成功 = code 0 + data.idempotent=true）
//       C16（收窄 1002 = 冲突拒绝 + HTTP 409）
//       C17（同一调用方重复提交已完成动作 = 幂等成功，MUST NOT 用 1002）
//
// 四条不可协商的口径：
//   1. 引擎不认识任何方案。候选方案由规则包 kind:"planTemplates" 驱动生成；指标、方向、
//      归一方式、权重、基线、理由口径、搜索空间、确认前置策略全部来自规则包
//      （kind:"scoringMetrics" / "planTemplates"）。本文件与 src/ 内 MUST NOT 出现任何
//      方案名、指标演示数值或硬编码优化（P6 / P7 / SCD-CAND-01 / SCD-OPT-01 / ADR-C1-02）。
//   2. 纯函数式：不落库、不广播、不取系统时间、不读全局状态。出口为反向接口
//      IPlanStore / IPlanSink / IClock / ILogSink（全部由宿主注入；未注入时引擎仍可工作）。
//   3. 失败 MUST NOT 抛异常跨边界（P10）。一切裁决走统一信封 {code, message, data}，
//      code 只取 protocol.md §3.2 的取值；1001 保留不用（C15）。
//   4. 确定性：同输入 MUST 同输出。对外一律整数百分比；内部定点整数累加（无浮点累加），
//      舍入规则显式（half-up，规则包 aggregate.rounding 声明，缺省 half-up）。
//
// 宿主的三个入口：policies()（装载规则包）→ score()（候选/评分/排序/推荐/可解释）
//                 → optimize() / adopt() / confirm()（优化与采纳确认裁决）。
//
// 宿主只允许 #include <scoring/scoring.h>；其余头文件是内部实现细节。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace scoring {

/// 引擎的 JSON 类型。
///
/// 为什么是 `ordered_json`（而不是 nlohmann 默认的 `json`）：
/// SCD-EXP-01/03 要求输出字段完整且**可复算**，SCD-SCORE-06/SCD-NFR-05 要求
/// "同输入跑两次逐字节一致"。nlohmann 默认 `json` 以 `std::map` 为后端，会按键名字典序
/// 重排；`ordered_json`（`std::vector` 后端）保留**成员声明顺序**，使 `dump()` 的字节
/// 完全由本文件的类型定义决定。两者取值语义、`dump()`、`parse()` 行为一致。
using json = nlohmann::ordered_json;

/// 引擎支持的规则包 MAJOR（protocol.md §5.2：不匹配 MUST 拒绝装载 + code 1006，MUST NOT 静默降级）
inline constexpr int kSupportedPoliciesMajor = 1;
/// 引擎版本（capabilities 自述用）
inline constexpr const char* kEngineVersion = "0.1.0";
/// **内部定点标度**：百分比一律用 `round(v × kScale)` 的整数累加（MUST NOT 用浮点累加）。
/// 例：贡献度 16.32% 的内部表示是 `1632`；总分 92.96% 的内部表示是 `9296`。
inline constexpr int64_t kScale = 10000;

// ============================================================================
// §1 protocol.md §1.4 —— “当前阶段”的唯一入参形状（逐字五字段）
// ============================================================================

/// 阶段上下文。**形状与 protocol.md §1.4 逐字一致**。
///
/// 为什么在本仓重新定义而不是 include `phase/phase_engine.h`：protocol.md P1 要求引擎之间
/// MUST NOT 互相 import（跨仓 include 零命中）。因此 `PhaseContext` 由**宿主**从
/// `phase-engine` 取出后按值传入本引擎（phase-engine 契约 §4.5）。本结构只是该形状的本地
/// 镜像，不含任何阶段取值 —— 引擎只当字符串与整数用（SCD-SCORE-05）。
struct PhaseContext {
    std::string phaseKey;     // PhaseDef.key
    int seq = 0;              // PhaseDef.seq
    std::string scenarioKey;  // 场景键（规则包定义取值）
    int64_t enteredAt = 0;    // 进入时刻，epoch ms
    std::string missionId;    // 任务标识
};

// ============================================================================
// §2 错误码（protocol.md §3.2 的取值，逐值对齐；1001 保留不用 —— 冲突裁决 C15）
// ============================================================================

enum class ErrorCode {
    Ok = 0,                 // 唯一成功码；幂等命中也是 0 + data.idempotent=true
    BadRequest = 1000,      // 请求参数错误 / 空体 / 权重非法（全 0 或和不为 1 且未声明归一）
    Conflict = 1002,        // 冲突拒绝（HTTP 409）：同侧并发重入（C16/C17，MUST NOT 用于幂等成功）
    PreconditionUnmet = 1003,  // 前置条件未满足：确认前置校验未过 / 不适用方案被采纳
    NotFound = 1004,        // 方案 / 任务不存在
    Internal = 1005,        // 服务端执行失败（宿主 store 写入失败等）
    VersionMismatch = 1006  // 规则包 schemaVersion 的 MAJOR 不符
};

/// 三态结果（对齐 phase-engine 的 TransitionStatus 口径）
enum class ResultStatus { Ok, Rejected, AlreadyApplied };

/// 码的稳定短名；未知码 → "unknown"
const char* errorCodeName(int code);

// ============================================================================
// §3 规则包类型（protocol.md §5：kind 与清单段 MUST 登记；内容全在 policies/mapapp）
// ============================================================================

/// 指标方向（SCD-SCORE-01：方向由规则注入，引擎 MUST NOT 内建）
enum class MetricDirection { Higher, Lower };
/// 归一方式（SCD-SCORE-02：每个指标 MUST 声明归一方式）
///   · Range     —— 线性区间：`(raw-min)/(max-min)`，`direction=lower` 时取反
///   · Threshold —— 阈值映射：取 `bands[]` 中第一个满足 `raw >= min` 的 `value`
///   · Percent   —— 直接百分比：`raw / scale`
enum class NormalizeType { Percent, Range, Threshold };

/// 归一化声明（`items[].normalize`）
struct NormalizeSpec {
    NormalizeType type = NormalizeType::Percent;
    std::string typeName;  // 规则包原样取值：percent / range / threshold
    double min = 0.0;      // Range 用
    double max = 0.0;      // Range 用
    double scale = 100.0;  // Percent 用（raw / scale）
    /// Threshold 用：分档，按 `min` **降序**求值，取第一个 `raw >= min` 的 `value`
    std::vector<std::pair<double, double>> bands;
};

/// 指标的取值来源（**引擎的机制**；取值由规则包声明，引擎 MUST NOT 内建指标名）
enum class MetricSource {
    Template,  // 候选方案的模板参数（如模板自带的能力基线）
    Snapshot,  // 输入快照（资源台账 / 链路评估 / 目标清单）
    Profile,   // 候选方案所属模板族的指标剖面
    Scalar     // 规则包 aggregate 声明的标量（候选无关的全局取值）
};

/// 指标定义（`kind:"scoringMetrics"` 的条目；SCD-SCORE-01/02/03）
struct MetricDef {
    std::string key;    // 指标标识（唯一）
    std::string name;   // 显示名（属规则包；引擎不解释、不匹配）
    std::string unit;   // 单位（展示用）
    MetricDirection direction = MetricDirection::Higher;
    std::string directionName;      // 规则包原样取值：higher / lower
    NormalizeSpec normalize;        // 归一方式（显式且可复现）
    double weight = 0.0;            // 权重（规则注入；和 MUST 为 1，见 §4 校验）
    bool hasBaseline = false;       // 是否声明了基线
    double baseline = 0.0;          // 基线原始值（住规则包 —— ADR-C1-02）
    bool baselineUsed = false;      // 该基线是否参与评分（false = 仅作演示口径锚点，供复算核对）
    // ---- 取值来源（机制）----
    MetricSource source = MetricSource::Template;
    std::string sourceName;         // 规则包原样取值：template / snapshot / profile / scalar
    std::string sourceField;        // Template 用：模板条目的字段名
    std::string sourceMetric;       // Snapshot/Profile 用：快照字段名 / 剖面指标名
    double scalar = 0.0;            // Scalar 用
    bool neutralOnMissing = false;  // 缺失时是否回落中性值（SCD-SCORE-05 / 风险 R6）
    double neutral = 0.0;           // 中性值（规则包 aggregate.neutralRule）
    std::string missingMarker;      // 缺失标注（规则包声明；引擎只透传，不造成句文案）
    int order = 0;                  // 声明顺序（次级排序键与迭代顺序的唯一依据）
};

/// 聚合口径（`kind:"scoringMetrics"` 的兄弟段 `aggregate`）
struct AggregateSpec {
    std::string method;              // 只支持 linear-weighted-mean（风险 R3：不做帕累托/机器学习）
    std::string rounding;            // half-up（缺省）
    double outputScale = 100.0;      // 对外百分比标度
    bool normalizeWeights = false;   // 权重和不等于 1 时是否**显式**归一（默认 false = 报错）
    bool neutralRuleEnabled = false; // 缺失值中性规则总开关
    std::string neutralSource;       // profile / baseline
    double neutralValue = 0.0;       // scalar（neutralSource=scalar 时）
    bool neutralMarkMissing = false; // 是否标注缺失
    std::string neutralMarker;       // 缺失标注词（规则包声明）
};

/// 阈值带（归一 threshold 用）
struct NormalizeBand {
    double min = 0.0;
    double value = 0.0;
};

/// 指标包（`kind:"scoringMetrics"` 的装载结果 —— SCD-SCORE-01/02/03/04）
struct MetricsPack {
    std::string policiesNamespace;
    std::string schemaVersion;
    std::string kind;   // 必须是 "scoringMetrics"
    std::string digest; // 规则包字节摘要（ADR-C1-04 的可追溯性）
    AggregateSpec aggregate;
    std::vector<MetricDef> items;              // 声明顺序即迭代顺序
    json raw = json::object();                 // 原样保留（审计三件套用）

    /// 指标的原始值观测量
    bool has(const std::string& key) const;
    /// 指标定义；不存在 → nullopt（MUST NOT 造默认指标）
    std::optional<MetricDef> metric(const std::string& key) const;
    /// 权重和（供校验与复算）
    double weightSum() const;
};

/// 模板族的指标剖面（`kind:"planTemplates"` 的兄弟段 `profiles`）
///
/// 用途：同一族模板共享同一条指标取值向量（如"基础覆盖能力"）。
/// **未在 `values` 中声明的指标，必须由指标自身声明 `baseline` 参与评分**
/// （即"剖面未覆盖 → 取基线"），这样基线锚点与逐项可复算两条要求同时成立。
struct ProfileDef {
    std::string key;
    std::string name;  // 展示名（引擎不解释）
    json values = json::object();  // { "<metricKey>": <rawValue> }
};

/// 候选方案的适用条件（`items[].when`；引擎只做机械比较，不解释业务含义 —— SCD-CAND-02）
struct TemplateWhen {
    std::vector<std::string> clusterKeys;  // 需要的集群 key（字符串 key，MUST NOT 用显示名 —— CTR-PL-08）
    int minClusters = -1;                  // -1 = 未声明
    int minItems = -1;                     // 需要的器材件数下限（所有列出集群合计）
    std::vector<std::string> phaseKeys;    // 允许的阶段 key（空 = 不限）
};

/// 方案模板（`kind:"planTemplates"` 的条目 —— ADR-C2-01：模板唯一权威来源）
struct PlanTemplate {
    std::string key;
    std::string side;   // group / strike（规则包取值；引擎只当字符串用）
    std::string scene;  // 场景键（规则包取值）
    int seq = 0;        // 声明序号（**稳定次级排序键** —— SCD-PICK-01）
    std::string name;
    std::string method;
    std::vector<std::string> clusters;
    std::string effect;
    std::string profileKey;   // 指向 profiles[]
    bool hasSuccessRate = false;
    double successRate = 0.0; // 模板自带成功率基线（住规则包）
    bool recommendedHint = false;  // 规则包的历史标注：**仅作提示**，推荐以排序结果为准
    TemplateWhen when;
};

/// 模板包（`kind:"planTemplates"` 的装载结果 —— C2 的装载与执行者）
struct TemplatesPack {
    std::string policiesNamespace;
    std::string schemaVersion;
    std::string kind;   // 必须是 "planTemplates"
    std::string digest;
    std::vector<ProfileDef> profiles;
    std::vector<PlanTemplate> items;  // 声明顺序即迭代顺序（次级排序键 seq，再退回声明序）
    json raw = json::object();

    bool has(const std::string& key) const;
    std::optional<PlanTemplate> plan(const std::string& key) const;
    std::optional<ProfileDef> profile(const std::string& key) const;
};

/// 逐条装载问题（path 形如 items[3].weight）
struct LoadIssue {
    std::string path;
    std::string field;
    std::string reason;  // 机器可读原因（本引擎不产出成句文案 —— SCD-EXP-02）

    json toJson() const;
};

/// 装载结果（**不抛异常**；校验失败 MUST 拒绝整包 —— CTR-PL-02）
struct LoadResult {
    int code = 0;
    std::string message;
    int itemCount = 0;
    std::string policiesNamespace;
    std::string schemaVersion;
    std::string digest;
    std::string definitionVersion;  // "<ns>:<schemaVersion>:<digest>"
    std::vector<std::string> warnings;  // 未知字段（CTR-PL-03：忽略但计入告警）
    std::vector<LoadIssue> issues;

    json toJson() const;
};

// ============================================================================
// §4 输入快照（SCD-SCORE-05：评分输入为结构化快照；引擎 MUST NOT 读全局状态或数据库）
// ============================================================================

/// 集群占用（镜像 resource-alloc 的分配结论；本引擎**只读**，不做合法性校验）
struct ClusterUsage {
    std::string clusterId;
    std::string phaseKey;      // 分配时所在阶段
    int total = 0;             // 该集群的器材件数（无明细时为 0）
    bool available = true;     // 该集群当前是否可用（宿主判定）
    bool present = true;       // 该集群在快照中是否**出现**（false = 缺失）
};

/// 资源快照（resource-alloc 的只读镜像）
struct ResourceSnapshot {
    PhaseContext phase;
    bool present = false;  // false = 未提供（**缺项按中性值 + 标注缺失处理，不阻塞评分** —— 风险 R6）
    std::vector<ClusterUsage> clusters;

    std::optional<ClusterUsage> cluster(const std::string& clusterId) const;
};

/// 链路评估（镜像 topology 的链路评估结论）
struct LinkEval {
    std::string linkId;
    std::string from;
    std::string to;
    std::string state;  // 规则包枚举取值（引擎不解释）
    double signal = 0.0;
    double bandwidthMbps = 0.0;
    double latencyMs = 0.0;
    double lossRate = 0.0;
    double coverageKm2 = 0.0;
    bool present = true;
};

/// 链路评估快照（topology 的只读镜像；**可缺省** —— 风险 R6）
struct TopologySnapshot {
    PhaseContext phase;
    bool present = false;  // false = 未提供 → 相关指标按中性值 + 标注缺失
    std::vector<LinkEval> links;
};

/// 目标清单（镜像 entity-ledger 的目标清单；本引擎只读）
struct TargetList {
    PhaseContext phase;
    bool present = false;
    std::vector<std::string> targetIds;  // 一律用 id 引用（protocol §2：MUST NOT 用 no 作引用键）
    std::vector<int> confidences;
    int modelCount = 0;
};

/// 评分输入快照（**唯一的输入形状**；SCD-SCORE-05 的显式声明）
struct ScoringSnapshot {
    std::string missionId;
    PhaseContext phase;
    std::optional<ResourceSnapshot> resources;
    std::optional<TopologySnapshot> topology;  // 缺省 = 缺链路评估输入
    std::optional<TargetList> targets;
    double resourceUtilization = 0.0;  // 外部直接给出的利用率（resource-alloc 的结论）
    bool hasResourceUtilization = false;
    json extra = json::object();  // 宿主附加数据（引擎不解释，只进审计三件套）
};

/// 候选选择请求
struct CandidateRequest {
    std::string missionId;
    PhaseContext phase;
    std::string side;   // 空 = 两侧都要
    std::string scene;  // 空 = 用 phase.scenarioKey
    std::vector<std::string> onlyTemplateKeys;  // 非空 = 只取这些模板（顺序即结果顺序）
    bool includeInapplicable = true;  // SCD-CAND-02：不适用 MUST 标注而非静默丢弃
    bool dedupe = true;               // SCD-CAND-04：内容等价的候选合并并标注
    ScoringSnapshot snapshot;         // 适用性判定的输入（可为空快照）
};

// ============================================================================
// §5 候选与评分结果（SCD-SCORE-04 / SCD-EXP-01）
// ============================================================================

/// 不适用条目（SCD-CAND-02：MUST 标注原因，候选总数不减少）
struct InapplicableItem {
    std::string code;    // 机器可读原因码
    std::string field;   // 相关字段（clusterKey / phaseKey / items …）
    std::string subject; // 相关主体（集群 key / 阶段 key）
    double required = 0.0;
    double actual = 0.0;
    bool hasNumbers = false;

    json toJson() const;
};

/// 去重标注（SCD-CAND-04：内容等价的候选被合并，合并去向可追溯）
struct DuplicateOf {
    std::string key;   // 被合并掉的模板 key
    std::string name;  // 被合并掉的模板名
    std::string into;  // 保留的候选 id

    json toJson() const;
};

/// 候选（由模板实例化；引擎不内建任何方案名 —— SCD-CAND-01）
struct Candidate {
    std::string id;    // 候选 id（= 模板 key；实例 id 由宿主另存）
    std::string key;   // 模板 key
    std::string side;
    std::string scene;
    int seq = 0;
    std::string name;
    std::string method;
    std::vector<std::string> clusters;
    std::string effect;
    std::string profileKey;
    bool hasSuccessRate = false;
    double successRate = 0.0;
    bool recommendedHint = false;
    bool applicable = true;
    std::vector<InapplicableItem> inapplicableReasons;
    std::vector<DuplicateOf> duplicates;
    std::string source;  // 生成来源（模板 key；可追溯 —— ADR-C2 的可追溯性）

    json toJson() const;
};

/// 逐项评分（SCD-SCORE-04 / SCD-EXP-01：原始值 / 归一值 / 权重 / 贡献度）
struct MetricScore {
    std::string key;
    std::string name;
    std::string unit;
    std::string direction;
    std::string normalizeType;
    MetricSource source = MetricSource::Template;
    std::string sourceName;
    std::string sourceField;   // 本项取值实际来自哪里（可追溯）
    double raw = 0.0;          // 原始值
    double normalized = 0.0;   // 归一值（0..1）
    double weight = 0.0;       // 权重
    double contribution = 0.0; // 贡献度 = 归一值 × 权重 × 100（对外百分比）
    int64_t contributionScaled = 0;  // 定点贡献度（scale = kScale），累加即总分
    bool usedBaseline = false; // 本项是否取了规则包基线
    bool missing = false;      // 本项输入缺失（SCD-SCORE-05 / 风险 R6）
    std::string missingMarker; // 缺失标注（规则包声明；引擎只透传）

    json toJson() const;
};

/// 推荐理由条目（**结构化，MUST NOT 成句自然语言** —— SCD-EXP-02 / 风险 R4）
///
/// 每条含 `{type, metricKey, value, refKey}`：`type` 与 `metricKey` 为机器可读枚举，
/// `value` 为数值。**引擎不产出任何自然语言文本**，人话由 `llm-provider` 生成。
struct ReasonItem {
    std::string type;        // 机器可读类型（规则包声明：top-score / top-contribution / lead / advantage）
    std::string metricKey;   // **可追溯到具体指标**（SCD-PICK-04）
    std::string refKey;      // 参照候选 id（lead / advantage 用）
    double value = 0.0;
    int64_t valueScaled = 0;
    int order = 0;

    json toJson() const;
};

/// 候选的完整评分
struct CandidateScore {
    Candidate candidate;
    bool applicable = true;
    bool scored = false;   // 不适用的候选不参与评分（但仍出现在列表里）
    int64_t totalScaled = 0;  // 定点总分（scale = kScale）
    int totalPercent = 0;     // **对外整数百分比**（half-up 舍入 —— SCD-SCORE-06 / D6）
    std::vector<MetricScore> metrics;
    int rank = 0;             // 1-based 名次
    bool recommended = false;
    double leadOverNext = 0.0;    // 领先次优的百分点（SCD-PICK-05）
    int leadOverNextPercent = 0;

    json toJson() const;
};

/// 评分结果（= 输出信封的 data —— SCD-EXP-01）
struct ScoreResult {
    int code = 0;
    std::string message;
    std::string missionId;
    std::string side;
    std::string scene;
    std::string recommendedId;
    bool hasRecommended = false;
    int recommendedPercent = 0;
    bool hasNext = false;
    std::string nextId;
    int nextPercent = 0;
    double leadOverNext = 0.0;
    int leadOverNextPercent = 0;
    std::vector<CandidateScore> candidates;  // **按总分降序、同分按 seq/声明序稳定排序**
    std::vector<ReasonItem> reasons;         // 结构化推荐理由（条数由规则包声明）
    std::vector<std::string> missingInputs;  // 缺失输入标注（缺链路/资源时不阻塞评分）
    int64_t auditDigest = 0;                 // 输入快照 + 权重 + 输出的摘要（SCD-EXP-03）
    std::string metricsDigest;
    std::string templatesDigest;
    int64_t ts = 0;  // 取自注入的 IClock（一次调用至多一次）
    /// **审计三件套之输入**：本次评分的输入快照（原样保留，供事后完整复算 —— SCD-EXP-03）
    json auditInput = json::object();
    /// **审计三件套之权重**：本次评分的逐指标权重
    json auditWeights = json::array();

    std::optional<CandidateScore> byId(const std::string& id) const;
    /// 统一信封 {code, message, data}
    json toJson() const;
    /// 评分结果的 data 部分（键序 = 成员声明顺序）
    json dataJson() const;
    /// **审计三件套**：{input, weights, output}（SCD-EXP-03）；
    /// `withMetrics=true` 时输出含逐项得分明细（可完整复算 —— ADR-C1-04）
    json toAuditJson(bool withMetrics = false) const;
};

// ============================================================================
// §6 自动优化（SCD-OPT）
// ============================================================================

/// 可调参数（`kind:"scoringMetrics"` 兄弟段 `searchSpace` 的条目）
///
/// 语义（引擎只实现这一条机制，取值全部来自规则包 —— SCD-OPT-02）：
///   对目标指标 raw 施加 `k * step` 的扰动，`k ∈ [0, maxSteps]`，结果按该指标的归一区间截断。
/// 这样"改了什么参数 → 哪些指标变了 → 总分变化"三者都可逐项核对（SCD-OPT-03）。
struct AdjustableParam {
    std::string key;
    std::string name;      // 展示名（引擎不解释）
    std::string metricKey; // 作用指标
    double step = 0.0;     // 步长
    double maxSteps = 0.0; // 上界（以步长为单位的最大步数）
};

/// 搜索空间（`kind:"scoringMetrics"` 兄弟段 `searchSpace`，SCD-OPT-02）
struct SearchSpace {
    int maxIterations = 0;        // 迭代上限（SCD-OPT-05：有界）
    int64_t timeBudgetMs = 0;     // 时限（超时按截断返回并标注）
    int maxTotalSteps = 0;        // 单次优化的总步数上限
    std::vector<AdjustableParam> params;
};

/// 参数变化（SCD-OPT-03：改了什么参数）
struct ParamChange {
    std::string key;
    std::string metricKey;
    double before = 0.0;
    double after = 0.0;
    double delta = 0.0;
    int steps = 0;

    json toJson() const;
};

/// 指标变化（SCD-OPT-03：哪些指标变了）
struct MetricChange {
    std::string key;
    double rawBefore = 0.0;
    double rawAfter = 0.0;
    double normalizedBefore = 0.0;
    double normalizedAfter = 0.0;
    double contributionBefore = 0.0;
    double contributionAfter = 0.0;

    json toJson() const;
};

/// 优化请求
struct OptimizeRequest {
    std::string missionId;
    PhaseContext phase;
    std::string templateKey;   // 要优化的方案（模板 key）
    ScoringSnapshot snapshot;
    bool includeInapplicable = false;  // 不适用方案默认不参与优化
    int maxIterationsOverride = 0;     // 0 = 用规则包
};

/// 优化结果（SCD-OPT-03/04/05）
struct OptimizeResult {
    int code = 0;
    std::string message;
    std::string status;   // improved / no-improvement / truncated / rejected
    std::string planId;
    bool improved = false;
    bool truncated = false;      // 触发迭代/时限上限，按时返回并说明已截断
    std::string stopReason;      // 机器可读停止原因
    int iterations = 0;
    int64_t elapsedMs = 0;
    int beforePercent = 0;
    int afterPercent = 0;
    int delta = 0;               // afterPercent - beforePercent（MUST >= 0 —— 单调性）
    std::vector<MetricScore> beforeMetrics;
    std::vector<MetricScore> afterMetrics;
    std::vector<MetricChange> metricChanges;
    std::vector<ParamChange> paramChanges;
    std::vector<std::string> unmetReasons;  // 不适用而未优化时的原因码（SCD-CAND-02）
    CandidateScore before;
    CandidateScore after;
    bool hasAfter = false;
    int64_t ts = 0;              // 取自注入的 IClock

    json toJson() const;
};

// ============================================================================
// §7 采纳与确认裁决（SCD-DECIDE）
// ============================================================================

/// 方案实例状态（同步自 IPlanStore；SCD-DECIDE-05 的三态）
struct PlanState {
    std::string missionId;
    std::string planId;   // = 模板 key（本引擎的方案标识）
    std::string side;
    std::string state;    // pending / adopted / confirmed
    int64_t updatedAt = 0;

    json toJson() const;
};

/// 方案状态查询（SCD-DECIDE-05：三态可分别列出）
struct PlanQuery {
    std::string missionId;
    std::string side;          // 空 = 两侧
    std::vector<std::string> stateIn;  // 空 = 全部三态
};

/// 采纳请求
struct AdoptRequest {
    std::string missionId;
    PhaseContext phase;
    std::string planId;   // 模板 key
    std::string side;     // 空 = 由模板推导
    std::string operatorId;
    std::string reason;
    bool hasRecommendedContext = false;  // 是否带推荐上下文（用于 deviated 标注 —— SCD-PICK-03）
    std::string recommendedId;
    int recommendedPercent = 0;
    int planPercent = 0;
    ScoringSnapshot snapshot;
};

/// 确认请求（前置校验策略由规则包声明 —— SCD-DECIDE-03）
struct ConfirmRequest {
    std::string missionId;
    PhaseContext phase;
    std::string planId;
    std::string side;
    std::string operatorId;
    std::string reason;
};

/// 采纳/确认裁决结果
struct DecideResult {
    int code = 0;
    std::string message;
    ResultStatus status = ResultStatus::Rejected;
    std::string missionId;
    std::string planId;
    std::string side;
    std::string action;        // adopted / confirmed
    std::string planState;     // pending / adopted / confirmed
    bool idempotent = false;   // 幂等命中 → true，且 code MUST 为 0（C15/C17）
    bool conflict = false;     // 互斥冲突 → true，且 code MUST 为 1002（C16/C17）
    bool deviated = false;     // 强制选择非推荐方案（SCD-PICK-03）
    bool hasRecommendedId = false;
    std::string recommendedId;
    bool autoAdopted = false;  // 确认前置策略 = autoAdopt 且本次自动采纳
    std::vector<std::string> invalidated;  // 同侧被他方取代的方案 id（SCD-DECIDE-01）
    std::vector<std::string> unmet;        // 前置未满足项（code=1003）
    json event = json::object();           // plan.state 事件的 data（宿主负责广播）
    int64_t ts = 0;

    json toJson() const;
    json dataJson() const;
};

// ============================================================================
// §8 反向接口（宿主 MUST 实现 —— P8/P9；命名遵循 protocol.md §6）
// ============================================================================

/// 方案状态持久化出口（引擎不接触 SQL —— P3）
class IPlanStore {
public:
    virtual ~IPlanStore() = default;
    /// 全量落盘该方案的**实例结果**（DB plan 表为引擎写入的缓存 —— ADR-C2-03）
    virtual bool save(const PlanState& st) = 0;
    /// false = 不存在（MUST NOT 用空记录当命中 —— protocol §2.3）
    virtual bool load(const std::string& missionId, const std::string& planId, PlanState& out) = 0;
    /// **同侧查询**：返回该任务该侧的全部状态（SCD-DECIDE-01 的互斥依据）
    /// 宿主未实现时返回空数组（引擎回落"仅按本次请求裁决"，并在 capabilities 如实标注）
    virtual std::vector<PlanState> listBySide(const std::string& missionId, const std::string& side) {
        (void)missionId;
        (void)side;
        return {};
    }
    /// 可选：三态列表（SCD-DECIDE-05）
    virtual bool supportsList() const { return false; }
    virtual std::vector<PlanState> list(const PlanQuery& q) {
        (void)q;
        return {};
    }
};

/// 方案变更通知出口（→ 广播 `plan.state`；protocol §4.2 事件名冻结）
///
/// CTR-EV-04：负载**只增不改** —— 既有 `{missionId, side, planId, action}` 逐字保留，
/// 新增字段全部可选（`recommendedId` / `deviated` / `idempotent`）。
class IPlanSink {
public:
    virtual ~IPlanSink() = default;
    virtual void onPlanStateChanged(const json& event) = 0;  // plan.state 的 data（P4：camelCase）
};

/// 时间注入（epoch 毫秒 —— P5）；一次调用至多取一次 nowMs()
class IClock {
public:
    virtual ~IClock() = default;
    virtual int64_t nowMs() const = 0;
};

/// 可选日志出口（缺省 = 静默）。事件日志由宿主在 Sink 内落库（SCD-DECIDE-04）
class ILogSink {
public:
    virtual ~ILogSink() = default;
    virtual void log(int level, const std::string& event, const json& data) {
        (void)level;
        (void)event;
        (void)data;
    }
    virtual void commandAudit(const json& entry) { (void)entry; }
};

/// 内置系统时钟（引擎唯一允许的非确定性来源，仅在未注入 IClock 时使用）
class SystemClock : public IClock {
public:
    SystemClock() = default;
    int64_t nowMs() const override;
};

// ============================================================================
// §9 引擎
// ============================================================================

/// 引擎依赖注入结构（只有四个依赖，**没有业务配置** —— 业务全在规则包）
struct ScoringEngineOptions {
    std::shared_ptr<IPlanStore> store;  // 可空 → 纯内存
    std::shared_ptr<IPlanSink> sink;    // 可空 → 空 Sink
    std::shared_ptr<IClock> clock;      // 可空 → SystemClock
    std::shared_ptr<ILogSink> log;      // 可空 → 静默
};

/// 确认前置策略（**规则包声明**，`planTemplates.confirmPrecondition.mode` —— SCD-DECIDE-03）
enum class ConfirmPrecondition {
    RejectIfNotAdopted,  // 未采纳直接确认 → 1003（默认）
    AutoAdopt            // 未采纳直接确认 → 自动采纳后确认
};

/// 引擎能力自述
struct Capabilities {
    bool metricsLoaded = false;
    bool templatesLoaded = false;
    std::string policiesNamespace;
    std::string metricsSchemaVersion;
    std::string templatesSchemaVersion;
    int policiesMajor = kSupportedPoliciesMajor;
    int metricCount = 0;
    int templateCount = 0;
    int profileCount = 0;
    int searchParamCount = 0;
    int maxIterations = 0;
    int64_t timeBudgetMs = 0;
    int reasonCount = 0;
    std::string confirmPrecondition;  // reject-if-not-adopted / auto-adopt
    double weightSum = 0.0;
    bool storeInjected = false;
    bool storeList = false;
    bool sinkInjected = false;
    bool clockInjected = false;
    bool logInjected = false;
    std::string metricsDigest;
    std::string templatesDigest;
    std::string engineVersion;

    json toJson() const;
};

/// 引擎计数（可观测；供验收）
struct Metrics {
    int64_t scores = 0;
    int64_t rejected = 0;
    int64_t candidatesGenerated = 0;
    int64_t candidatesInapplicable = 0;
    int64_t duplicatesMerged = 0;
    int64_t optimizations = 0;
    int64_t optimized = 0;
    int64_t noImprovement = 0;
    int64_t truncated = 0;
    int64_t adopts = 0;
    int64_t confirms = 0;
    int64_t idempotentHits = 0;
    int64_t conflicts = 0;
    int64_t invalidated = 0;
    int64_t autoAdopted = 0;
    int64_t storeErrors = 0;
    int64_t sinkErrors = 0;
    int64_t unknownFields = 0;
};

/// 方案评分与推荐引擎（纯函数式 + 反向接口注入）。
///
/// 三个入口：`policies()` 装载规则包 → `score()` 出候选/评分/排序/推荐/可解释
/// → `optimize()` / `adopt()` / `confirm()`。
///
/// **实现结构 `Impl` 是公开的**：它声明在唯一公开头内，但**只有引擎内部源文件**会用到它
/// （`src/internal.h` 为它提供全部字段）。宿主永远不需要、也 MUST NOT 直接使用它 ——
/// 公开头的这一处"实现暴露"是为了让内部自由函数能读写引擎状态，而不必把字段塞进公开接口。
class ScoringEngine {
public:
    struct Impl;  // 定义在 src/internal.h（宿主 MUST NOT 使用）

    ScoringEngine();
    explicit ScoringEngine(const ScoringEngineOptions& opts);
    ~ScoringEngine();
    ScoringEngine(const ScoringEngine&) = delete;
    ScoringEngine& operator=(const ScoringEngine&) = delete;
    ScoringEngine(ScoringEngine&&) noexcept;
    ScoringEngine& operator=(ScoringEngine&&) noexcept;

    // ---- 规则包装载（protocol §5；装载失败 MUST 拒绝整包并逐条给原因） ----

    /// 装载指标包（kind:"scoringMetrics"）。失败时保留上一次成功装载（原子替换）。
    LoadResult loadMetrics(const json& pkg);
    /// 从文件装载指标包（便利方法；读不到 / 非法 JSON → 1000）
    LoadResult loadMetricsFile(const std::string& path);
    /// 装载模板包（kind:"planTemplates"）
    LoadResult loadTemplates(const json& pkg);
    /// 从文件装载模板包
    LoadResult loadTemplatesFile(const std::string& path);
    /// **一次装载两份规则包**（推荐的宿主用法：任一失败即拒绝，已装载侧保留原值）
    LoadResult policies(const json& metricsPkg, const json& templatesPkg);
    /// 从目录装载 `scoringMetrics.json` + `planTemplates.json`
    LoadResult policiesFromDir(const std::string& dir);

    /// 纯函数：只校验不装载（供 CI / 宿主预检）
    static LoadResult validateMetrics(const json& pkg);
    static LoadResult validateTemplates(const json& pkg);

    // ---- 规则视图与自述 ----

    MetricsPack metricsPack() const;
    TemplatesPack templatesPack() const;
    std::vector<std::string> metricKeys() const;
    std::vector<std::string> templateKeys() const;
    Capabilities capabilities() const;
    Metrics metrics() const;

    // ---- 候选生成与评分（SCD-CAND / SCD-SCORE / SCD-PICK / SCD-EXP） ----

    /// 候选生成（规则驱动；不适用标注；去重）—— 不改动任何状态
    std::vector<Candidate> generateCandidates(const CandidateRequest& req) const;
    /// 评分（候选 → 归一 → 加权 → 排序 → 推荐 → 结构化理由 → 审计三件套）
    ScoreResult score(const CandidateRequest& req);

    // ---- 自动优化（SCD-OPT） ----

    /// 在**可声明搜索空间**内扰动重评；单调（无改进返回 no-improvement）；幂等；有界可截断
    OptimizeResult optimize(const OptimizeRequest& req);

    // ---- 采纳与确认裁决（SCD-DECIDE） ----

    /// 采纳（同侧唯一 + 互斥；幂等成功走 code=0 + idempotent；强制非推荐方案标 deviated）
    DecideResult adopt(const AdoptRequest& req);
    /// 确认（前置校验按规则包策略；幂等成功走 code=0 + idempotent）
    DecideResult confirm(const ConfirmRequest& req);
    /// 三态查询（SCD-DECIDE-05）
    std::vector<PlanState> plans(const PlanQuery& q) const;

    // ---- 依赖注入（P8/P9） ----

    void setStore(std::shared_ptr<IPlanStore> store);  // nullptr = 纯内存
    void setSink(std::shared_ptr<IPlanSink> sink);     // nullptr = 空 Sink
    void setClock(std::shared_ptr<IClock> clock);      // nullptr = SystemClock
    void setLog(std::shared_ptr<ILogSink> log);        // nullptr = 静默

private:
    std::unique_ptr<Impl> impl_;
};

// ============================================================================
// §10 自由函数（纯函数：可被宿主 / 测试直接调用，不依赖引擎状态）
// ============================================================================

/// **half-up 舍入**的唯一定点实现（SCD-SCORE-06：避免显示成 92.99999 的误差）
/// 语义：`roundHalfUpScaled(v, scale) == round(v × scale)`（整数 half-up，对称）
int64_t roundHalfUpScaled(double value, int64_t scale = 1);
/// **百分比 ↔ 定点的唯一换算实现**（互为逆运算；`outScale` 是"100% 对应的定点基数"）：
/// 例：`scaledToDouble(roundPercent(92.96, kScale), kScale) == 92.96`。
/// `outScale` 取 `1` 时得到整数百分比、取 `kScale` 时得到引擎内部定点值。
/// 全程 half-up，避免浮点尾差把 `92.99999` 显示成两位数（SCD-SCORE-06）。
int64_t roundPercent(double percent, int64_t outScale = 1);
/// 内部定点值（92.96% ↔ 9296）→ **对外整数百分比**
int roundToPercent(int64_t scaledValue, int64_t scale = kScale);
/// 内部定点值（92.96% ↔ 9296）→ **对外百分比（两位小数）**
double scaledToDouble(int64_t scaledValue, int64_t scale = kScale);

/// 归一化（SCD-SCORE-02 的唯一定点实现）
///   · Percent   → raw / scale
///   · Range     → (raw-min)/(max-min)，direction=lower 时取反
///   · Threshold → bands 按 min 降序，取第一个 raw >= min 的 value
/// 结果一律截断到 [0,1]；区间退化（max<=min）时按 0 处理并在 missing 中标注
double normalizeValue(const MetricDef& def, double raw);

/// FNV-1a 64（零外部依赖的摘要；规范化字节 → 同一 digest）
int64_t fnv1a64(const std::string& bytes);
/// 输出 16 位小写十六进制
std::string digestHex(int64_t digest);

/// 枚举 ⇄ JSON 字符串
const char* toString(MetricDirection d);
const char* toString(NormalizeType t);
const char* toString(MetricSource s);
const char* toString(ConfirmPrecondition p);
const char* toString(ResultStatus s);
std::optional<MetricDirection> metricDirectionFromString(const std::string& s);
std::optional<NormalizeType> normalizeTypeFromString(const std::string& s);
std::optional<MetricSource> metricSourceFromString(const std::string& s);
std::optional<ConfirmPrecondition> confirmPreconditionFromString(const std::string& s);

/// 版本串解析（MAJOR.MINOR.PATCH）；非法 → nullopt
std::optional<int> majorOfVersion(const std::string& v);

}  // namespace scoring
