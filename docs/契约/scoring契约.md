# scoring · 方案评分与推荐引擎契约（v1.0）

| 项 | 内容 |
|---|---|
| 文档编号 | `CTR-SCD-001` |
| 版本 | **v1.0（首版实现对齐稿）** |
| 适用范围 | `scoring`：**后端 C++17 库**（无 Web 框架、无 SQL、无其它引擎内部依赖）。候选生成、指标归一与加权评分、排序与推荐、结构化可解释输出与审计三件套、受约束的自动优化、采纳/确认的状态语义裁决 |
| 本文件定位 | **接口唯一基准**：公开签名、字段名与取值、错误码、事件负载、规则包形状、机检清单以本文件为准 |
| 需求依据 | [`../需求/scoring需求专篇.md`](../需求/scoring需求专篇.md)（`SCD-*` 共 **34** 条） |
| 上游共享契约 | [`../../../phase-engine/docs/契约/protocol.md`](../../../phase-engine/docs/契约/protocol.md)（P1–P10、§1.4 `PhaseContext`、§3 错误码、§4 事件名、§5 `policies`、§6 反向接口命名） |
| 冲突裁决 | [`../../../phase-engine/docs/契约/冲突裁决.md`](../../../phase-engine/docs/契约/冲突裁决.md)（**C1** 评分数值的唯一权威来源是本引擎、**C2** 方案模板唯一来源是规则包、**C15** 废弃 `1001`、**C16** 收窄 `1002`、**C17** 幂等成功与互斥冲突分开表达） |
| 借鉴范本（只读，MUST NOT 修改） | [`../../../resource-alloc/docs/契约/resource-alloc契约.md`](../../../resource-alloc/docs/契约/resource-alloc契约.md)、`../../../resource-alloc/`（第 2 波实现，结构与验收脚本同构） |
| 状态 | 首版（已实现并验收通过）。破坏性变更 MUST 走 protocol.md §9 的修订流程 |

---

## 0. 这份契约解决什么

`scoring` 回答一句话：**每套方案得几分、为什么、哪套最好、优化能提升多少、采纳/确认该是什么状态**。

| 本引擎负责 | 不负责（归谁） |
|---|---|
| 候选生成（规则驱动、不适用标注、去重） | 方案名称与组成（属规则包 `planTemplates` —— C2） |
| 指标归一、加权求和、舍入规则 | 自然语言推荐理由（`llm-provider`：本引擎只出**结构化条目**） |
| 排序、推荐、次优差值、偏离推荐标注 | 资源分配的合法性校验（`resource-alloc`，本引擎只**读结论**） |
| 结构化可解释输出 + 审计三件套 | 阶段推进（`phase-engine`，本引擎只**读当前阶段**） |
| 受约束的自动优化（单调、幂等、可截断） | 落库与广播（宿主实现 `IPlanStore` / `IPlanSink`） |
| 采纳/确认的**状态语义裁决**（同侧唯一、幂等、前置条件） | 界面方案卡与执行任务生成（宿主 + `phase-engine`） |

### 0.1 三条不可协商的口径

1. **引擎不认识任何方案。** 候选由规则包 `kind:"planTemplates"` 驱动生成；指标、方向、归一方式、权重、基线、理由口径、搜索空间、确认前置策略全部来自 `policies/mapapp/`。引擎产物内 MUST NOT 出现方案名（机检 `acceptance.ps1` C07）与演示数值（C08c）。
2. **纯函数式 + 反向接口。** 引擎不落库、不广播、不取系统时间、不读全局状态。四个出口 `IPlanStore` / `IPlanSink` / `IClock` / `ILogSink` 全部由宿主注入（P8/P9）；未注入时引擎仍 MUST 可工作。
3. **失败 MUST NOT 抛异常跨边界**（P10）。一切裁决走统一信封 `{code, message, data}`；`code` 只取 protocol.md §3.2 的取值，`1001` 保留不用（C15）。

---

## 1. 公开入口与命名

| 项 | 取值 |
|---|---|
| 语言 / 标准 | **C++17** |
| 构建 | **CMake ≥ 3.20**；目标名 `scoring`（静态库；示例/自测独立目标） |
| 唯一公开头文件 | `#include <scoring/scoring.h>`（**公开入口只有它**） |
| 命名空间 | `scoring` |
| 依赖 | **仅 `nlohmann/json`**（内置单头回落 `third_party/nlohmann/json.hpp`）；不依赖 Web 框架 / SQL / 其它引擎 |
| 对外字段命名 | **camelCase**（P4）；JSON 键名与 C++ 成员名一一对应 |
| 时间戳 | **epoch 毫秒**（P5），类型 `int64_t` |
| JSON 类型 | `nlohmann::ordered_json`（保留**成员声明顺序** → `dump()` 逐字节可由类型定义决定） |
| 引擎不产生的 JSON | 引擎**不产出 WS 信封**；产出的是事件的 `data` 与 `ts`（§5） |

> **其余头文件（`src/internal.h`）MUST 为内部实现细节，MUST NOT 被宿主包含。**
> 例外说明：`ScoringEngine::Impl` 作为**嵌套类型**声明在公开头内（供内部源文件使用），
> 但其定义只在 `src/internal.h`，宿主 MUST NOT 使用它。

---

## 2. 标度与舍入（本契约冻结的口径）

| 约定 | 定义 |
|---|---|
| 内部定点标度 | `kScale = 10000`：**百分比 92.96% 的内部表示是 `9296`**（`定点 = round(数值 × kScale)`） |
| 对外整数百分比 | `roundToPercent(scaled, kScale) == round(scaled / kScale × 100)`（整数 half-up） |
| 对外两位小数 | `scaledToDouble(scaled, kScale) == scaled / kScale × 100` |
| 百分比 ↔ 定点 | `roundPercent(percent, outScale) == round(percent / 100 × outScale)`；`outScale` 是**定点标度**（`outScale = 1` 得整数百分比，`= kScale` 得内部定点） |
| 贡献度 | `contributionScaled = round(归一值 × 权重 × kScale)`（**整数累加，MUST NOT 浮点累加**） |
| 总分 | `totalScaled = Σ contributionScaled`；`totalPercent = roundToPercent(totalScaled, kScale)` |

**约束（SCD-SCORE-06 / SCD-NFR-05）**：

| 编号 | 约束 |
|---|---|
| `CTR-SCD-NUM-01` | 一切单价与总分 MUST 用**定点整数**计算与累加；MUST NOT 出现浮点累加 |
| `CTR-SCD-NUM-02` | 对外数值 MUST 先 half-up 到两位小数（`kExportScale = 100`）；对外百分比 MUST 为整数 |
| `CTR-SCD-NUM-03` | 舍入 MUST 为 **half-up**（`.5` 向上，负值对称）；规则包 `aggregate.rounding` 只支持 `half-up` |
| `CTR-SCD-NUM-04` | 同输入 MUST 同输出：`toJson().dump()` 在注入假时钟后 **逐字节一致** |
| `CTR-SCD-NUM-05` | 审计三件套 MUST 逐项导出 `raw` / `normalized` / `weight` / `contribution` / `contributionScaled`，使 `Σ contributionScaled == totalScaled` 可手算核对（ADR-C1-04） |

---

## 3. 公开接口（唯一公开头 `include/scoring/scoring.h`）

### 3.1 规则包类型

| 类型 | 用途 | 关键字段 |
|---|---|---|
| `MetricDef` | 指标定义（SCD-SCORE-01/02） | `key` `name` `unit` `direction` `normalize{type,min,max,scale,bands}` `weight` `baseline` `baselineUsed` `source{kind,field,metric,value}` `neutralOnMissing{enabled,value,missingMarker}` |
| `NormalizeSpec` | 归一声明 | `type ∈ {percent, range, threshold}` + 对应参数 |
| `MetricSource` | 取值来源（**机制**） | `Template` / `Snapshot` / `Profile` / `Scalar` |
| `AggregateSpec` | 聚合口径 | `method`（只支持 `linear-weighted-mean`）`rounding` `outputScale` `normalizeWeights` `neutralRule{...}` |
| `ProfileDef` | 模板族的指标剖面 | `key` `values{metricKey → raw}` |
| `PlanTemplate` | 方案模板（ADR-C2-01 的唯一权威来源） | `key` `side` `scene` `seq` `name` `method` `clusters` `effect` `profileKey` `successRate` `recommended` `when{clusters,minClusters,minItems,phases}` |
| `MetricsPack` / `TemplatesPack` | 装载后的只读视图 | `items` `profiles` `aggregate` `digest` `raw`（原样保留供审计） |
| `LoadResult` | 装载结果（**不抛异常**） | `code` `message` `issues[]{path,field,reason}` `warnings[]` `digest` `definitionVersion` |

### 3.2 输入快照（SCD-SCORE-05）

| 类型 | 内容 |
|---|---|
| `PhaseContext` | protocol §1.4 逐字五字段（`phaseKey` / `seq` / `scenarioKey` / `enteredAt` / `missionId`） |
| `ResourceSnapshot` | `present` + `clusters[]{clusterId, phaseKey, total, available, present}` |
| `TopologySnapshot` | `present` + `links[]{linkId, from, to, state, signal, bandwidthMbps, latencyMs, lossRate, coverageKm2}` |
| `TargetList` | `present` + `targetIds[]`（**只带 id** —— CTR-EN-01）+ `confidences[]` + `modelCount` |
| `ScoringSnapshot` | `missionId` `phase` `resources?` `topology?` `targets?` `resourceUtilization` `hasResourceUtilization` `extra` |

> **缺项处理**：任一分段缺失时相关指标按**中性值**处理并**标注缺失**，评分 MUST NOT 失败（风险 R6）。

### 3.3 评分结果（SCD-EXP-01）

```jsonc
{ "code": 0, "message": "ok",
  "data": {
    "missionId": "...", "side": "group", "scene": "scenario-1",
    "recommendedId": "grp-s1-b", "recommendedPercent": 93,
    "nextId": "grp-s1-a", "nextPercent": 80,
    "leadOverNext": 13.0, "leadOverNextPercent": 13,
    "candidates": [ { "id": "...", "name": "...", "side": "...", "scene": "...", "seq": 2,
                      "profileKey": "...", "successRate": 93.0, "applicable": true, "scored": true,
                      "total": 93, "totalScaled": 9280, "rank": 1, "recommended": true,
                      "leadOverNext": 13.0, "leadOverNextPercent": 13,
                      "metrics": [ { "key": "coverageRate", "name": "区域覆盖率", "unit": "%",
                                     "direction": "higher", "normalizeType": "range",
                                     "source": "profile", "sourceField": "profile.multi-domain.coverageRate",
                                     "raw": 96.0, "normalized": 0.96, "weight": 0.17,
                                     "contribution": 16.32, "contributionScaled": 1632,
                                     "contributionScale": 10000,
                                     "usedBaseline": false, "missing": false } ] } ],
    "reasons": [ { "type": "top-score", "metricKey": "coverageRate", "value": 96.0, "valueScaled": 9600 } ],
    "missingInputs": ["link-eval-missing"],
    "auditDigest": "d780bb78d6b7f408",
    "metricsDigest": "84f52edb6dd14ae8", "templatesDigest": "16f2c1ceaf612e11",
    "ts": 1750000000000 } }
```

**约束**

| 编号 | 约束 |
|---|---|
| `CTR-SCD-JS-01` | 信封字段**恰为** `{code, message, data}`；`data` 键序 MUST 与成员声明顺序一致 |
| `CTR-SCD-JS-02` | 不适用候选 MUST 出现在 `candidates[]` 内并带 `applicable=false` + `inapplicableReasons[]`（**候选总数不减少** —— SCD-CAND-02） |
| `CTR-SCD-JS-03` | `reasons[]` MUST 为**结构化条目** `{type, metricKey, value, refKey?}`；引擎 MUST NOT 产出成句自然语言（SCD-EXP-02） |
| `CTR-SCD-JS-04` | `auditInput` / `auditWeights` / `auditOutput` 构成**审计三件套**，`toAuditJson(true)` 含逐项明细，可完整复算当时总分（SCD-EXP-03 / ADR-C1-04） |

### 3.4 引擎方法（`class ScoringEngine`）

| 分组 | 方法 |
|---|---|
| 规则包装载 | `loadMetrics` / `loadMetricsFile` / `loadTemplates` / `loadTemplatesFile` / `policies` / `policiesFromDir`；**纯函数** `validateMetrics` / `validateTemplates` |
| 规则视图 | `metricsPack` / `templatesPack` / `metricKeys` / `templateKeys` / `capabilities` / `metrics` |
| 候选与评分 | `generateCandidates(CandidateRequest) -> Candidate[]`（**不改状态**）/ `score(CandidateRequest) -> ScoreResult` |
| 自动优化 | `optimize(OptimizeRequest) -> OptimizeResult` |
| 采纳确认 | `adopt(AdoptRequest)` / `confirm(ConfirmRequest)` / `plans(PlanQuery)` |
| 依赖注入 | `setStore` / `setSink` / `setClock` / `setLog` + `ScoringEngineOptions{store,sink,clock,log}` |

### 3.5 自由纯函数（宿主 / 测试可直接调用）

`roundHalfUpScaled` / `roundPercent` / `roundToPercent` / `scaledToDouble` / `normalizeValue` /
`fnv1a64` / `digestHex` / `errorCodeName` / `toString(枚举)` / `metricDirectionFromString` /
`normalizeTypeFromString` / `metricSourceFromString` / `confirmPreconditionFromString` / `majorOfVersion`

---

## 4. 反向接口（宿主 MUST 实现 —— P8/P9）

```cpp
class IPlanStore {                      // 持久化（引擎不接触 SQL —— P3）
  virtual bool save(const PlanState& st) = 0;                                   // 全量落盘
  virtual bool load(const std::string& missionId, const std::string& planId,
                    PlanState& out) = 0;                                        // false = 不存在
  virtual std::vector<PlanState> listBySide(const std::string& missionId,
                                            const std::string& side) { ... }     // 同侧查询（互斥依据）
  virtual bool supportsList() const { return false; }
  virtual std::vector<PlanState> list(const PlanQuery& q) { ... }                // 三态列表（可选）
};

class IPlanSink {                       // 事件出口（→ 广播 plan.state）
  virtual void onPlanStateChanged(const json& event) = 0;                        // 立即返回，MUST NOT 阻塞
};

class IClock   { virtual int64_t nowMs() const = 0; };                           // epoch 毫秒
class ILogSink { virtual void log(int, const std::string&, const json&) {}
                 virtual void commandAudit(const json&) {} };                    // 可选；缺省静默
```

| 编号 | 约束 |
|---|---|
| `CTR-SCD-SK-01` | `IPlanSink::onPlanStateChanged` MUST 立即返回，MUST NOT 阻塞（不得在其中做网络 IO / 等锁 / 落库） |
| `CTR-SCD-SK-02` | 回调抛出的异常 MUST 被引擎吞掉并计入 `metrics.sinkErrors`；**MUST NOT 影响裁决结果**（SCD-NFR-03） |
| `CTR-SCD-ST-01` | `save()` 返回 `false` → 本次裁决整体失败（`1005`）且**内存状态不变** |
| `CTR-SCD-ST-02` | `IPlanStore` 未注入时引擎 MUST 以纯内存模式工作（`capabilities().storeInjected=false`） |
| `CTR-SCD-CK-01` | 一次裁决 MUST **恰好调用一次** `nowMs()`；未注入时回落内置 `SystemClock` |

---

## 5. 事件（复用 protocol.md §4.2，冻结）

| 项 | 内容 |
|---|---|
| 事件名 | **`plan.state`**（既有，MUST NOT 改名） |
| 产生时机 | 采纳成功、确认成功 |
| 负载（既有四字段**逐字保留**） | `{missionId, side, planId, action}` |
| 新增可选字段（`CTR-EV-04` 只增不改） | `planState`（`pending/adopted/confirmed`）、`deviated`、`idempotent`、`recommendedId`（带推荐上下文时）、`ts` |
| `action` 取值 | `adopted` / `confirmed`（取自 protocol §4.3 冻结表；`generated` / `recommended` / `optimized` 为本引擎之外或后续产生方） |
| 事件日志 | 采纳与确认 MUST 经 `ILogSink::log` + `commandAudit` 落痕（SCD-DECIDE-04） |

**幂等命中 MUST NOT 重发事件、MUST NOT 重复落库**（零副作用）。

---

## 6. 规则包（`policies/mapapp/`）

| 文件 | `kind` | 内容 |
|---|---|---|
| `scoringMetrics.json` | `scoringMetrics` | 指标清单 + `aggregate` + `reasons` + `searchSpace` |
| `planTemplates.json` | `planTemplates` | 方案模板 + `profiles` + `confirmPrecondition` |

### 6.1 骨架（protocol §5.1）

```jsonc
{ "policiesNamespace": "mapapp", "schemaVersion": "1.0.0", "kind": "scoringMetrics", "items": [ ... ] }
```

`MAJOR` 不匹配 → 拒绝装载并返回 **`1006`**，MUST NOT 静默降级（protocol §5.2）。

### 6.2 取值来源的解析顺序（引擎机制，取值全在规则包）

```
① snapshot.extra.metrics.<metricKey>        （宿主直接投喂观测量）
② profile（模板族剖面）：source.kind = "profile"
③ 指标声明的来源：template 字段 / snapshot 字段 / scalar
④ 中性值（neutralOnMissing.enabled）→ 标注 missingMarker
⑤ 规则包基线 baseline（baselineUsed = true）
```

**任何一步命中即终止**；一次取值的实际来源写入 `metrics[].sourceField`，使"这个分数从哪来"可逐项追溯（SCD-SCORE-04）。

### 6.3 搜索空间（SCD-OPT-02）

```jsonc
"searchSpace": { "maxIterations": 240, "timeBudgetMs": 100, "maxTotalSteps": 12,
  "params": [ { "key": "lift-coverageRate", "metricKey": "coverageRate", "step": 1, "maxSteps": 4 } ] }
```

语义：对目标指标 `raw` 施加 `k × step`（`k ∈ [0, maxSteps]`）后**重评**，结果按该指标的归一区间截断。
引擎 MUST NOT 内建任何增益或上限（SCD-OPT-01）。

---

## 7. 错误码（protocol §3.2 的子集，逐值对齐；`1001` 保留不用）

| code | 本引擎的用法 | HTTP |
|---|---|---|
| `0` | 成功；**幂等命中也是 `0` + `data.idempotent=true`**（C15/C17） | 200 |
| `1000` | 请求参数错误 / 规则包非法（骨架、字段、枚举、引用不完整） | 400 |
| `1002` | **互斥冲突**：同侧已被他方持有（持闸门期间的重入）（C16/C17） | **409** |
| `1003` | **前置条件未满足**：确认前置校验未过（未采纳）、不适用方案参与优化 | 409 |
| `1004` | 方案模板不存在 | 404 |
| `1005` | 规则包未装载 / 宿主 store 写入失败 | 500 |
| `1006` | 规则包 `schemaVersion` 的 `MAJOR` 不受支持 | 409 |
| `1001` | **保留不用**（不分配给新用途 —— C15） | — |

| 编号 | 约束 |
|---|---|
| `CTR-SCD-EC-01` | 幂等成功 MUST 用 `code=0` + `data.idempotent=true`；MUST NOT 为非零码（C15/C17） |
| `CTR-SCD-EC-02` | 互斥冲突 MUST 用 `1002` + `data.conflict=true`；与幂等成功 MUST 分开表达（ADR-C17-02） |
| `CTR-SCD-EC-03` | 前置条件未满足 MUST 用 `1003`（MUST NOT 塞进 `1002`） |

---

## 8. 采纳与确认的裁决顺序（MUST 按此顺序，结果可复现）

### 8.1 `adopt`

| 步 | 检查 | 不通过 → |
|---|---|---|
| 1 | `missionId` / `planId` 非空 | `1000` |
| 2 | 规则包已装载 | `1005` |
| 3 | 模板存在 | `1004` |
| 4 | 取得**同侧闸门**（try_lock 语义，MUST NOT 阻塞） | `1002` + `conflict=true` |
| 5 | 当前状态（store 为权威，进程内缓存兜底）已是 `adopted` | **幂等成功**：`0` + `idempotent=true`（零副作用） |
| 6 | 当前状态已是 `confirmed` | **幂等成功**：`0` + `idempotent=true`（状态不变） |
| 7 | 同侧其它方案置回 `pending` → 落库 → 通知 Sink → 落日志 | `1005`（store 失败时内存不变） |

### 8.2 `confirm`

| 步 | 检查 | 不通过 → |
|---|---|---|
| 1–4 | 同上（含同侧闸门） | 同上 |
| 5 | 当前状态已是 `confirmed` | **幂等成功**：`0` + `idempotent=true` |
| 6 | 前置校验：当前状态 **不是** `adopted` | 规则包 `confirmPrecondition.mode` = `reject-if-not-adopted` → `1003` + `unmet[]`；= `auto-adopt` → 自动采纳后确认（`autoAdopted=true`） |
| 7 | 状态置 `confirmed` → 落库 → 通知 → 日志 | `1005` |

---

## 9. 可机检清单（本契约的验收）

| # | 断言 | 方式 |
|---|---|---|
| 1 | 引擎产物内方案名零命中；规则包内命中 > 0（反证） | `acceptance.ps1` C07 / C07b |
| 2 | 引擎产物内演示数值零命中；规则包内命中 > 0（反证） | C08c / C08b |
| 3 | 引擎产物内无硬编码优化增益（写死的 `+ N`） | C08 |
| 4 | 跨仓 import 零命中；仅 `nlohmann/json` | C09 / C10 |
| 5 | 不产生保留码 `1001` | C11 |
| 6 | 引擎内无落库 / 无广播 / 无网络调用 | C12 |
| 7 | 演示基线由规则包 `baseline × weight` 独立复算（不经过引擎常量） | C13（抓 selftest 锚点行） |
| 8 | 四个反向接口在公开头声明且全部经 `ScoringEngineOptions` 注入 | C14 / C15 |
| 9 | 唯一公开头；内部头标注宿主不可包含 | C16 / C17 |
| 10 | `PhaseContext` 五字段逐字一致 | C18 |
| 11 | 错误码逐值对齐且不含保留码 | C19 |
| 12 | 规则包骨架 + `kind` + `MAJOR` 一致；权重和 = 1.0000 | C22 / C23 / C23b |
| 13 | 34 条需求每条都有用例引用且至少一个通过 | C05 / C06 |
| 14 | 示例独立运行退出码 0；ctest 通过；脚本以 0/1 收口 | C25 / C26 / C27 |

---

## 10. 开放问题（不阻塞本波，需宿主裁决）

| # | 问题 | 影响 | 现状 |
|---|---|---|---|
| 1 | **确认前置的默认策略**：未采纳时拒绝还是自动采纳（专篇 §9 待确认 5） | `confirmPrecondition.mode` | 规则包取 `reject-if-not-adopted`；引擎两种都已实现，改一行规则即可 |
| 2 | **评分指标口径与权重取值**（需求初稿 §10.2；专篇 §9 待确认 1/2） | 指标 `normalize` 区间与 `weight` | 按演示级口径落地；换规则包即换口径，引擎零改动 |
| 3 | **"自动优化"期望的可感知幅度**（专篇 §9 待确认 3） | `searchSpace` 的步长与上界 | 现规则包下推荐项可从 93% 提升到 96%；幅度由规则包调 |
| 4 | **是否保留"演示模式"假优化**（专篇 §9 待确认 4 / D3 例外） | `SCD-OPT-01` | **未保留**：假优化已彻底移除（机检 C08 零命中） |
| 5 | **是否需要多目标 / 帕累托**（专篇 §9 待确认 6） | `SCD-SCORE-*` | 按"不需要"实现：只支持线性加权 + 归一 + 排序（风险 R3） |
| 6 | **规则包两份文件的参数追加**（本波为"基线可复算"追加了声明式来源与剖面） | `scoringMetrics.json` / `planTemplates.json` | 见 [`../实现报告.md`](../实现报告.md) §6 —— 权重、基线、名称、成功率、`when` 阈值**全部未改** |
| 7 | `plan` 表是否新增 `templateKey` 字段（C2 可追溯性） | 宿主数据模型 | 本引擎已把模板 key 作为候选 id 与 `source` 输出，可直接用 |
| 8 | `protocol.md` §4.3 的 `generated` / `recommended` 两个 `action` 的产生方 | 事件完备性 | 本引擎按需可发（`IPlanSink` 已在位），登记为宿主决定项 |

---

## 11. 变更记录

| 版本 | 变更 |
|---|---|
| v1.0 | 首版：冻结公开接口（40 类型 / 37 函数量级）、标度与舍入口径（定点 10000 + half-up）、四个反向接口、`plan.state` 负载只增不改、错误码子集（`1001` 保留不用）、规则包形状（`scoringMetrics` + `planTemplates` 与其兄弟段）、采纳/确认裁决顺序、可机检清单 14 条、开放问题 8 条 |
