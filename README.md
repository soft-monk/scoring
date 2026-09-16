# scoring · 方案评分与推荐引擎

> **回答"每套方案得几分、为什么、哪套最好、优化能提升多少、采纳/确认该是什么状态"**：
> 一个可独立构建、可独立验收的后端 C++17 库。候选生成、指标归一、加权评分、排序推荐、
> 结构化可解释输出、受约束的自动优化、采纳/确认的状态语义全部由引擎显式守护；
> 方案名称与组成、指标与权重、基线、理由口径、搜索空间、确认前置策略全部来自规则包。

**已实现并验收通过**：构建 0 error，自测 **42 用例 / 731 断言全绿**，
`scripts/acceptance.ps1` **49 项检查全过、退出码 0**。

---

## 它解决什么问题

现状（`Api.cc` `plans()` / `optimizePlan()`）是"SELECT + 排序"与 `success_rate + 3` 的**演示级假优化**：
"推荐评分 93%"是 `Seed.cc` 里的写死值，`ai/rules.py` 又抄了一遍，**一处计算逻辑都没有** ——
现场问"93% 怎么来的"，代码答不出来。

本引擎把这个数字变成**算出来的、可逐项复算的**结论：

| 机制 | 说明 |
|---|---|
| **候选规则驱动** | 候选由规则包 `planTemplates.json` 实例化；引擎产物内**方案名零命中**（机检 C07） |
| **不适用只标注、不丢弃** | 资源不足时该方案标 `applicable=false` + 原因码（`cluster-unavailable` / `insufficient-resources`），**候选总数不减少** |
| **评分可复算** | 导出「原始值 / 归一值 / 权重 / 贡献度 / 定点贡献度 / 取值来源」，`Σ contributionScaled == totalScaled` 手算可核对（ADR-C1-04） |
| **权威来源单一** | 分数只由本引擎算：`Σ baseline × weight = 92.96 → 93`，引擎内**没有** `93` 这个常量（机检 C08c） |
| **确定性** | 内部定点整数累加（标度 10000）+ 显式 half-up；同输入 + 同假时钟 → `dump()` **逐字节一致** |
| **结构化理由，不出话术** | 理由 4 条为 `{type, metricKey, value}`，每条可追溯到指标；无句末标点、无文案字段（`exp02` 129 条断言锁定） |
| **优化不再是假动作** | 基规则声明的搜索空间扰动重评；**单调**（无改进返回 `no-improvement`）、**幂等**（连续 10 次不漂移）、**有界可截断** |
| **采纳/确认裁决** | 同侧唯一（后者使前者失效）、幂等成功走 `code=0 + idempotent=true`（C15/C17）、前置策略由规则包声明、`plan.state` 事件与事件日志齐备 |
| **强制选非推荐方案** | 允许，结果标 `deviated=true` 与推荐 id（SCD-PICK-03） |

## 做 / 不做

| 做 | 不做 |
|---|---|
| 候选生成（规则驱动、不适用标注、去重） | 🚫 不产出自然语言理由（`llm-provider` 把结构化结论转述成人话） |
| 指标归一与加权评分（过程可导出） | 🚫 不做资源合法性校验（`resource-alloc`，本引擎只读结论） |
| 排序、推荐、次优差值、偏离推荐标注 | 🚫 不做阶段推进（`phase-engine`，本引擎只读当前阶段） |
| 结构化可解释输出 + 审计三件套 | 🚫 不做落库与广播（宿主实现 `IPlanStore` / `IPlanSink`） |
| 受约束的自动优化（单调 / 幂等 / 可截断） | 🚫 不做界面方案卡与执行任务生成 |
| 采纳 / 确认的状态语义与幂等 | 🚫 不内建方案名、指标名、权重与任何演示数值 |

## 快速开始

```powershell
# 一条命令构建（仅需 CMake ≥ 3.20 + C++17 编译器；nlohmann/json 优先系统包，缺失时用内置单头）
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

# 跑示例
build\bin\Release\example_minimal.exe      # 最小：装载 → 评分 → 推荐 → 理由
build\bin\Release\example_audit.exe        # 可复算：逐项导出 + 手算比对 + 审计三件套复算
build\bin\Release\example_full_flow.exe    # 全流程：候选 → 不适用 → 优化 → 幂等 → 双跑一致
build\bin\Release\example_decision.exe     # 裁决：偏离推荐 → 同侧互斥 → 幂等 → 三态

# 零依赖自测（42 用例 / 731 断言）
build\bin\Release\selftest.exe

# 独立验收（构建 + 自测 + §7 逐条 + 结构纪律 + 需求对账，退出码 0/1）
powershell -ExecutionPolicy Bypass -File scripts\acceptance.ps1
```

最小用法（宿主只包含一个头文件）：

```cpp
#include <scoring/scoring.h>
using namespace scoring;

ScoringEngineOptions opts;                    // 四个出口可空：未注入也能工作
opts.store = myPlanStore;                     // IPlanStore：落库（引擎不接触 SQL）
opts.sink  = myPlanSink;                      // IPlanSink ：广播 plan.state
opts.clock = myClock;                         // IClock    ：确定性验收用假时钟
ScoringEngine engine(opts);

engine.policiesFromDir("policies/mapapp");    // 规则包：指标 + 权重 + 模板 + 剖面 + 搜索空间

ScoringSnapshot snap;                         // 唯一的评分输入（引擎不读全局状态）
snap.missionId = missionId;
snap.phase = phaseContextFromHost;            // 由宿主从 phase-engine 取出后按值传入
snap.resources = resourceSnapshot;            // resource-alloc 的只读镜像（可缺省）
// snap.topology / snap.targets 可再缺省：缺项按中性值处理并标注缺失，不阻塞评分

CandidateRequest req;
req.missionId = missionId; req.phase = snap.phase;
req.side = "group"; req.snapshot = snap;

ScoreResult r = engine.score(req);            // 候选 / 归一 / 加权 / 排序 / 推荐 / 理由
if (r.code == 0) {
    std::printf("推荐 %s：%d%%（领先次优 %d）\n",
                r.recommendedId.c_str(), r.recommendedPercent, r.leadOverNextPercent);
    for (const auto& m : r.byId(r.recommendedId)->metrics)   // 逐项可手算复算
        std::printf("  %-22s raw=%.2f norm=%.4f w=%.2f 贡献=%.2f (%s)\n",
                    m.key.c_str(), m.raw, m.normalized, m.weight, m.contribution,
                    m.sourceField.c_str());
}

json audit = r.toAuditJson(true);             // 审计三件套：输入 + 权重 + 输出（含逐项明细）
```

### 93 是怎么算出来的（本引擎的验收锚点）

```
规则包 baseline × weight 的独立推论：
  0.17 × (96 + 93 + 91 + 88) + 0.16 × (94 + 96) = 62.56 + 30.40 = 92.96 → 93（half-up）

引擎导出的逐项贡献度（编组侧推荐方案）：
  coverageRate 16.32 ｜ linkStability 15.81 ｜ targetDetection 15.47
  electronicSuppression 14.96 ｜ missionSuccess 14.88 ｜ resourceUtilization 15.36
  定点累加：1632 + 1581 + 1547 + 1496 + 1488 + 1536 = 9280（标度 10000）→ 92.80% → 93
```

## 接口一览（唯一公开头 `include/scoring/scoring.h`）

| 分类 | 入口 |
|---|---|
| 规则包 | `loadMetrics` / `loadTemplates` / `policies` / `policiesFromDir`；纯函数 `validateMetrics` / `validateTemplates` |
| 规则视图 | `metricsPack` / `templatesPack` / `metricKeys` / `templateKeys` / `capabilities` / `metrics` |
| 候选与评分 | `generateCandidates`（不改状态）/ `score` |
| 自动优化 | `optimize` |
| 采纳确认 | `adopt` / `confirm` / `plans`（三态查询） |
| 注入 | `setStore` / `setSink` / `setClock` / `setLog` + `ScoringEngineOptions` |
| 反向接口（宿主实现） | `IPlanStore` / `IPlanSink` / `IClock` / `ILogSink` |
| 纯函数（可直接调用） | `normalizeValue` / `roundPercent` / `roundToPercent` / `scaledToDouble` / `roundHalfUpScaled` / `fnv1a64` / `digestHex` |

**四个出口全部由宿主注入，未注入时引擎仍 MUST 可工作**（纯内存 + 空 Sink + 系统时钟）。
事件名与负载、错误码、规则包骨架遵守 [`protocol.md`](../phase-engine/docs/契约/protocol.md) 与
[冲突裁决](../phase-engine/docs/契约/冲突裁决.md)：
幂等命中 = `code=0` + `data.idempotent=true`（C15/C17），互斥冲突 = `1002` + `conflict=true`，
前置未满足 = `1003`，`1001` 保留不用。

## 规则包（`policies/mapapp/`）

| 文件 | `kind` | 内容 |
|---|---|---|
| `scoringMetrics.json` | `scoringMetrics` | 6 个指标（方向 / 归一方式 / 权重 / 基线 / 取值来源 / 中性规则）+ `aggregate` + `reasons` + `searchSpace` |
| `planTemplates.json` | `planTemplates` | 12 套方案模板（名称逐字取自契约 §7.3/§7.4）+ 7 个模板族剖面 + `confirmPrecondition` |

> **引擎内没有方案名、没有指标演示数值、没有写死的优化增益** —— 机检确认
> （`acceptance.ps1` C07 / C08 / C08c），且每条"零命中"都配了**反证**（C07b / C08b：同一模式在规则包内必须命中）。
> 换一份规则包 = 换一套方案体系与评分数值，引擎零改动。

## 文档

| 文档 | 内容 |
|---|---|
| [`docs/契约/scoring契约.md`](docs/契约/scoring契约.md) | **接口唯一基准**（`CTR-SCD-001`）：公开签名、标度与舍入口径、错误码、事件、规则包形状、裁决顺序、机检清单 14 条 |
| [`docs/实现报告.md`](docs/实现报告.md) | 构建 / 自测 / 验收结果（原样输出）、34 条需求落点、**对预置规则包的参数追加说明**、修正留痕、开放问题 10 条 |
| [`docs/需求/scoring需求专篇.md`](docs/需求/scoring需求专篇.md) | 需求专篇（唯一权威）：**34 条**（CAND 4 / SCORE 6 / PICK 5 / EXP 4 / OPT 5 / DECIDE 5 / NFR 5） |

## 状态

| 项 | 状态 |
|---|---|
| 需求 | 已冻结（34 条） |
| 契约（接口） | ✅ v1.0（`docs/契约/`），含 8 条开放问题待宿主裁决 |
| 实现 | ✅ 首版（库 + 4 示例 + 自测 42 用例 / 731 断言） |
| 验收 | ✅ `scripts/acceptance.ps1` 49 项全过，退出码 0 |

| 实测指标 | 取值 | 门槛 |
|---|---|---|
| 单次评分（10 候选 × 10 指标） | 约 **0.25 ms**（50 次取均值） | ≤ 1 ms |
| 单次优化（4 参数 × 4 步，迭代 16） | < **0.5 ms** | ≤ 100 ms 或按时截断 |
| 确定性 | 同输入 + 同假时钟 → `dump()` **逐字节一致** | 一致 |
| 演示基线复算 | 规则包推论 = 引擎总分 = 审计复算 | 三者一致 |

## 定位与依赖纪律

这是一个**业务引擎**，与其它模块**互不 import**，只通过注入的反向接口与宿主装配：

- **不 import** `phase-engine`：`PhaseContext` 由宿主按值传入（形状与 protocol.md §1.4 逐字一致）。
- **不认识** `resource-alloc` / `topology` / `entity-ledger`：资源台账、链路评估、目标清单只以**只读快照**形式传入。
- **不认识** `realtime-hub` / `telemetry-store`：广播与落库由宿主的 `IPlanSink` / `IPlanStore` 实现负责。
- **不产出自然语言**：`llm-provider` 负责把结构化结论转成人话（本引擎的边界）。
- 仅依赖 `nlohmann/json`（内置单头回落 `third_party/nlohmann/json.hpp`）。

## 许可

Apache License 2.0，见 [LICENSE](LICENSE)。
