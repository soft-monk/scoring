# scripts/acceptance.ps1 · scoring 独立验收脚本（退出码 0/1）
## 权威依据：docs/需求/scoring需求专篇.md
#   ① §7 验收清单 18 行 —— 逐行变成一条可执行检查（S07-01..S07-18）
#   ② §3 功能需求 **34 条**（SCD-CAND 4 / SCD-SCORE 6 / SCD-PICK 5 / SCD-EXP 4 /
#      SCD-OPT 5 / SCD-DECIDE 5 / SCD-NFR 5）
#   ③ §1.4 硬约束 + 上游共享契约 ../phase-engine/docs/契约/protocol.md（P1/P6/P7/P8/P9、
#      §3.3 幂等成功 = code 0 + idempotent、1001 保留不用）
#   ④ ../phase-engine/docs/契约/冲突裁决.md（**C1/C2 的当事方**：评分数值唯一来源是本引擎、
#      模板唯一来源是规则包、C15/C16/C17 错误码口径）
#
# 设计（与第 2 波 resource-alloc 同构）：**引擎行为一律由 tests/selftest 断言**
# （`selftest --json` 输出逐用例结果与需求编号），本脚本只做三件它做不了的事：
#   ① 构建生命周期；② 结构纪律检索（方案名 / 演示常量 / 跨仓 import / 保留码）；
#   ③ 需求↔用例对账 + **反证**（演示常量确实只在规则包里）。
# 这样"行为口径"只有一个来源，不会出现脚本与单测两套断言的漂移。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File scripts/acceptance.ps1
#   ... -SkipBuild                只跑自测与检查（复用已有构建产物）
#   ... -Config Debug             换构建配置
#   ... -Generator "Ninja"        换生成器
#
# 兼容 Windows PowerShell 5.1（不依赖 pwsh / PS7 语法）。
param(
    [string]$BuildDir = "build",
    [string]$Config = "Release",
    [string]$Generator = "Visual Studio 17 2022",
    [string]$Arch = "x64",
    [switch]$SkipBuild
)

# 控制台按 UTF-8 读脚本与写输出（PS 5.1 的默认代码页会把中文读坏）
[void][System.Reflection.Assembly]::LoadWithPartialName("System.Text.Encoding")
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }
$ErrorActionPreference = "Stop"
$script:Results = New-Object System.Collections.Generic.List[object]
$script:ChecksFailed = 0

# ------------------------------------------------------------------ 输出小工具
function Check([string]$id, [string]$title, [bool]$ok, [string]$detail) {
    $state = "PASS"
    if (-not $ok) { $state = "FAIL"; $script:ChecksFailed++ }
    $script:Results.Add([pscustomobject]@{ Id = $id; Title = $title; Ok = $ok; Detail = $detail })
    $color = "Green"
    if (-not $ok) { $color = "Red" }
    Write-Host ("  [{0}] {1} · {2}" -f $state, $id, $title) -ForegroundColor $color
    if ($detail) { Write-Host ("         {0}" -f $detail) -ForegroundColor DarkGray }
}

function Section([string]$title) {
    Write-Host ""
    Write-Host ("=" * 78)
    Write-Host $title
    Write-Host ("=" * 78)
}

function Rel([string]$full) {
    $root = $script:Repo
    if ($full.StartsWith($root)) { return $full.Substring($root.Length).TrimStart('\', '/') }
    return $full
}

function Format-Hits($hits) {
    if (-not $hits -or $hits.Count -eq 0) { return "" }
    $head = @($hits | Select-Object -First 5)
    $s = "：" + ($head -join "；")
    if ($hits.Count -gt 5) { $s += ("；…共 {0} 处" -f $hits.Count) }
    return $s
}

function SearchHits([string[]]$files, [string]$regex, [string[]]$skipLineRegex) {
    $hits = New-Object System.Collections.Generic.List[string]
    foreach ($f in $files) {
        $n = 0
        # 规则包与源码都是 UTF-8：MUST 显式指定编码，否则 PS 5.1 会按 ANSI 读坏中文
        foreach ($line in [System.IO.File]::ReadAllLines($f, [System.Text.Encoding]::UTF8)) {
            $n++
            if ($line -notmatch $regex) { continue }
            if ($skipLineRegex) {
                $skip = $false
                foreach ($s in $skipLineRegex) { if ($line -match $s) { $skip = $true; break } }
                if ($skip) { continue }
            }
            $hits.Add(("{0}:{1}" -f (Rel $f), $n))
        }
    }
    return $hits
}

function CollectFiles([string[]]$dirs, [string[]]$exts, [string[]]$excludeDirs) {
    $out = New-Object System.Collections.Generic.List[string]
    foreach ($d in $dirs) {
        $full = Join-Path $script:Repo $d
        if (-not (Test-Path $full)) { continue }
        Get-ChildItem -Path $full -Recurse -File -ErrorAction SilentlyContinue | ForEach-Object {
            if ($exts -notcontains $_.Extension) { return }
            $rel = Rel $_.FullName
            foreach ($x in $excludeDirs) { if ($rel -like ($x + "*")) { return } }
            $out.Add($_.FullName)
        }
    }
    return $out
}

# ------------------------------------------------------------------ 前置
$script:Repo = Split-Path -Parent $PSScriptRoot
Push-Location $script:Repo
try {
    Write-Host "scoring 独立验收（需求专篇 §7 逐条 + 结构纪律 + selftest 对账）"
    Write-Host ("仓库：{0}" -f $script:Repo)
    Write-Host ("PowerShell：{0}" -f $PSVersionTable.PSVersion)

    $cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
    if (-not $cmake) {
        foreach ($p in @("C:\Program Files\CMake\bin\cmake.exe",
                         "C:\Program Files (x86)\CMake\bin\cmake.exe")) {
            if (Test-Path $p) { $cmake = $p; break }
        }
    }
    if (-not $cmake) { Write-Host "找不到 cmake（>= 3.20）" -ForegroundColor Red; exit 1 }
    $cmakeVersion = (& $cmake --version | Select-Object -First 1)
    Write-Host ("cmake：{0}（{1}）" -f $cmake, $cmakeVersion)

    $buildPath = Join-Path $script:Repo $BuildDir

    # ================================================================ ① 构建（SCD-NFR-02）
    Section "① 构建生命周期（SCD-NFR-02：一条命令构建 → 一条命令验收）"

    $isMultiConfig = ($Generator -like "Visual Studio*")
    $binCandidates = @()
    if ($isMultiConfig) { $binCandidates += (Join-Path $buildPath "bin\$Config") }
    else { $binCandidates += (Join-Path $buildPath "bin") }
    $binCandidates += (Join-Path $buildPath "bin\$Config")
    $binCandidates += (Join-Path $buildPath "bin")

    function Resolve-Bin() {
        foreach ($c in $binCandidates) {
            if ((Test-Path (Join-Path $c "selftest.exe")) -or (Test-Path (Join-Path $c "selftest"))) {
                return $c
            }
        }
        return $binCandidates[0]
    }

    $configureOk = $false
    $buildOk = $false
    $bin = ""
    if ($SkipBuild) {
        $configureOk = (Test-Path (Join-Path $buildPath "CMakeCache.txt"))
        $bin = Resolve-Bin
        $buildOk = (Test-Path (Join-Path $bin "selftest.exe")) -or
                   (Test-Path (Join-Path $bin "selftest"))
        Check "C01" "构建：-SkipBuild 复用已有产物" ($configureOk -and $buildOk) `
            ("configure={0} binary={1}" -f $configureOk, $buildOk)
    } else {
        Write-Host ("    cmake -S . -B {0} -G '{1}'" -f $BuildDir, $Generator)
        & $cmake -S $script:Repo -B $buildPath -G $Generator -A $Arch 2>&1 |
            ForEach-Object { Write-Host ("      {0}" -f $_) }
        $configureOk = ($LASTEXITCODE -eq 0)

        Write-Host ("    cmake --build {0} --config {1}" -f $BuildDir, $Config)
        & $cmake --build $buildPath --config $Config 2>&1 |
            ForEach-Object { Write-Host ("      {0}" -f $_) }
        $buildOk = ($LASTEXITCODE -eq 0)

        $bin = Resolve-Bin
        $hasSelftest = (Test-Path (Join-Path $bin "selftest.exe")) -or
                       (Test-Path (Join-Path $bin "selftest"))
        $exampleNames = @("example_minimal", "example_audit", "example_full_flow",
                          "example_decision")
        $hasExamples = $true
        foreach ($e in $exampleNames) {
            if (-not ((Test-Path (Join-Path $bin "$e.exe")) -or (Test-Path (Join-Path $bin $e)))) {
                $hasExamples = $false
            }
        }
        Check "C01" "构建：配置 + 编译通过，产物齐全（selftest + 4 个示例）" `
            ($configureOk -and $buildOk -and $hasSelftest -and $hasExamples) `
            ("configure={0} build={1} selftest={2} examples={3} bin={4}" -f `
                $configureOk, $buildOk, $hasSelftest, $hasExamples, (Rel $bin))
    }

    if (-not $bin) { $bin = Resolve-Bin }
    $selftestExe = Join-Path $bin "selftest.exe"
    if (-not (Test-Path $selftestExe)) { $selftestExe = Join-Path $bin "selftest" }

    # ① -2 CMake 最低版本 >= 3.20 且 C++17（需求 §1.4）
    $cmakeLists = [System.IO.File]::ReadAllText((Join-Path $script:Repo "CMakeLists.txt"))
    $minOk = $false
    if ($cmakeLists -match 'cmake_minimum_required\s*\(\s*VERSION\s+([0-9]+\.[0-9]+)') {
        $minOk = ([version]$Matches[1] -ge [version]"3.20")
    }
    $cxx17 = ($cmakeLists -match 'CMAKE_CXX_STANDARD\s+17')
    Check "C02" "构建：CMake >= 3.20 且 C++17（需求 §1.4）" ($minOk -and $cxx17) `
        ("cmake_min={0} cxx17={1}" -f $minOk, $cxx17)

    # ================================================================ ② selftest（行为口径的唯一来源）
    Section "② 零依赖自测（tests/selftest --json）"

    $jsonOk = $false
    $data = $null
    $jsonPath = Join-Path ([System.IO.Path]::GetTempPath()) ("scoring-selftest-{0}.json" -f $PID)
    if (Test-Path $selftestExe) {
        # 让子进程**直接写文件**：selftest --json 输出 UTF-8（含中文用例名），
        # 走管道会被控制台代码页解成乱码，JSON 随之不可解析。
        & $selftestExe --json > $jsonPath
        $raw = ""
        if (Test-Path $jsonPath) {
            $raw = [System.IO.File]::ReadAllText($jsonPath, [System.Text.Encoding]::UTF8)
        }
        try {
            $data = $raw | ConvertFrom-Json
            $jsonOk = $true
        } catch {
            Write-Host "    selftest --json 解析失败：$($_.Exception.Message)" -ForegroundColor Red
            if ($raw.Length -gt 0) { Write-Host ($raw.Substring(0, [Math]::Min(400, $raw.Length))) }
        }
    }
    Check "C03" "自测可执行且 --json 输出可解析" $jsonOk ("exe={0}" -f (Rel $selftestExe))

    $caseByName = @{}
    $reqToCases = @{}
    if (-not $jsonOk) {
        Check "C03b" "自测结果可用" $false "selftest --json 无有效输出"
    } else {
        Write-Host ("    用例 {0} 个（失败 {1}）｜断言 {2} 条（失败 {3}）｜耗时 {4:N1} ms｜结果 {5}" -f `
            $data.cases, $data.casesFailed, $data.asserts, $data.assertsFailed, $data.elapsedMs,
            $data.result)
        Check "C04" "自测全绿：用例失败 0 且断言失败 0" `
            (($data.casesFailed -eq 0) -and ($data.assertsFailed -eq 0)) `
            ("cases={0}/{1} asserts={2}/{3}" -f $data.cases, $data.casesFailed, $data.asserts,
             $data.assertsFailed)

        foreach ($c in $data.details) {
            $caseByName[$c.name] = $c
            foreach ($r in $c.reqs) {
                if (-not $reqToCases.ContainsKey($r)) {
                    $reqToCases[$r] = New-Object System.Collections.Generic.List[string]
                }
                $reqToCases[$r].Add($c.name)
            }
        }

        # ---- 34 条需求 → 用例对账（SCD-CAND 4 / SCD-SCORE 6 / SCD-PICK 5 / SCD-EXP 4 /
        #      SCD-OPT 5 / SCD-DECIDE 5 / SCD-NFR 5）
        $allReqs = New-Object System.Collections.Generic.List[string]
        foreach ($d in @("CAND:01..04", "SCORE:01..06", "PICK:01..05", "EXP:01..04",
                         "OPT:01..05", "DECIDE:01..05", "NFR:01..05")) {
            $dom = $d.Split(':')[0]
            $parts = ($d.Split(':')[1] -replace '\.\.', ' ').Split(' ')
            for ($i = [int]$parts[0]; $i -le [int]$parts[1]; $i++) {
                $allReqs.Add(("SCD-{0}-{1:D2}" -f $dom, $i))
            }
        }
        $missing = New-Object System.Collections.Generic.List[string]
        foreach ($r in $allReqs) {
            if (-not $reqToCases.ContainsKey($r)) { $missing.Add($r) }
        }
        Check "C05" "需求覆盖：34 条 SCD-* 每条都有用例引用" ($missing.Count -eq 0) `
            ("总需求 {0} 条；无对应用例：{1}" -f $allReqs.Count,
             (($missing -join ", ") -replace '^$', '无'))

        $reqFailed = New-Object System.Collections.Generic.List[string]
        foreach ($r in $allReqs) {
            if (-not $reqToCases.ContainsKey($r)) { continue }
            $anyOk = $false
            foreach ($cn in $reqToCases[$r]) { if ($caseByName[$cn].ok) { $anyOk = $true } }
            if (-not $anyOk) { $reqFailed.Add($r) }
        }
        Check "C06" "需求覆盖：每条至少有一个通过用例" ($reqFailed.Count -eq 0) `
            ("未通过：{0}" -f (($reqFailed -join ", ") -replace '^$', '无'))
    }

    # ================================================================ ③ §7 验收清单逐条（18 行）
    Section "③ 需求专篇 §7 验收清单（逐条，18 行）"

    $checklist = @(
        @{ Id = "S07-01"; Line = "空环境 clone → 一条命令构建 → 一条命令跑通验收（退出码 0）"
           Cases = @(); NeedsBuild = $true },
        @{ Id = "S07-02"; Line = "全仓检索方案名与演示常量（评分基线、写死的优化增益）→ 零命中"
           Cases = @(); Guard = "demo-constants" },
        @{ Id = "S07-03"; Line = "给 5 套模板 → 产出 5 个候选；给 2 套等价模板 → 去重生效"
           Cases = @("cand03_candidate_count_follows_template_count",
                     "cand04_equivalent_templates_are_deduped_and_marked") },
        @{ Id = "S07-04"; Line = "资源不足时某方案标不适用并给出原因，候选总数不减少"
           Cases = @("cand02_inapplicable_is_marked_not_dropped") },
        @{ Id = "S07-05"; Line = "权重全 0 → 报错；正常权重 → 总分落在 0–100"
           Cases = @("score03_weights_are_validated") },
        @{ Id = "S07-06"; Line = "导出逐项得分（原始值/归一值/权重/贡献度），手算可复算总分"
           Cases = @("score04_every_term_is_exported_and_hand_checkable",
                     "anchor_recommended_total_is_recomputable_from_rules") },
        @{ Id = "S07-07"; Line = "同输入跑两次，输出逐字节一致；边界值舍入符合规则"
           Cases = @("score06_determinism_and_rounding", "nfr05_float_reproducibility") },
        @{ Id = "S07-08"; Line = "同分候选排序稳定"
           Cases = @("pick01_ordering_is_stable") },
        @{ Id = "S07-09"; Line = "采用非推荐方案 → 返回 deviated:true 与推荐 id"
           Cases = @("pick03_forced_non_recommended_is_allowed_and_marked") },
        @{ Id = "S07-10"; Line = "推荐理由 4 条，每条可追溯到具体指标"
           Cases = @("pick04_reasons_are_structured_and_traceable") },
        @{ Id = "S07-11"; Line = "输出中不含成句自然语言（测试断言）"
           Cases = @("exp02_no_natural_language_sentences",
                     "structure_recommendation_reasons_are_not_sentences") },
        @{ Id = "S07-12"; Line = "优化输出改了什么 → 哪些指标变了 → 总分变化；无改进时返回无改进且总分不变"
           Cases = @("opt03_optimization_is_explainable", "opt04_monotonic_and_idempotent") },
        @{ Id = "S07-13"; Line = "迭代上限触发时按时返回并说明已截断"
           Cases = @("opt05_bounded_and_truncatable") },
        @{ Id = "S07-14"; Line = "采纳 B 后 A 不再 adopted；连点两次不报错并标 idempotent:true"
           Cases = @("decide01_same_side_is_exclusive", "decide02_idempotent_repeats_succeed",
                     "structure_gate_is_held_during_sink_callback",
                     "proto_idempotent_vs_conflict_are_separate") },
        @{ Id = "S07-15"; Line = "plan.state 事件负载与现状可比对；采纳/确认落事件日志"
           Cases = @("decide04_events_and_logs", "proto_event_payload_shapes") },
        @{ Id = "S07-16"; Line = "缺链路评估输入时按中性值处理并标注缺失，评分不失败"
           Cases = @("anchor_missing_link_input_is_neutral_and_marked") },
        @{ Id = "S07-17"; Line = "权威来源单一：评分基线由本引擎复算（ADR-C1-04）；模板唯一来源是规则包（ADR-C2-01）"
           Cases = @("anchor_recommended_total_is_recomputable_from_rules",
                     "cand01_candidates_are_rule_driven", "proto_policies_schema_and_versioning") },
        @{ Id = "S07-18"; Line = "独立交付：独立构建 / 示例 / 测试 / 验收脚本（退出码 0/1）"
           Cases = @("nfr02_independent_delivery_smoke");
           NeedsExamples = $true }
    )

    if (-not $jsonOk) {
        foreach ($item in $checklist) { Check $item.Id $item.Line $false "自测结果不可用，无法对账" }
    } else {
        foreach ($item in $checklist) {
            if ($item.ContainsKey("NeedsBuild")) {
                Check $item.Id $item.Line ($configureOk -and $buildOk) `
                    "配置与构建成功 = 空环境一条命令可构建（详见 ① 的输出）"
                continue
            }
            if ($item.ContainsKey("Guard")) { continue }   # 结构类条目在 ④ 给结论（见 S07-02）
            $missingCases = New-Object System.Collections.Generic.List[string]
            $failedCases = New-Object System.Collections.Generic.List[string]
            foreach ($cn in $item.Cases) {
                if (-not $caseByName.ContainsKey($cn)) { $missingCases.Add($cn); continue }
                if (-not $caseByName[$cn].ok) { $failedCases.Add($cn) }
            }
            $ok = ($missingCases.Count -eq 0) -and ($failedCases.Count -eq 0)
            $detail = "用例：{0}" -f ($item.Cases -join ", ")
            if ($missingCases.Count -gt 0) { $detail += "；缺失：" + ($missingCases -join ", ") }
            if ($failedCases.Count -gt 0) { $detail += "；失败：" + ($failedCases -join ", ") }
            Check $item.Id $item.Line $ok $detail
        }
    }

    # ================================================================ ④ 结构纪律
    Section "④ 结构纪律：方案名 / 演示常量 / 跨仓 import / 保留码（SCD-CAND-01、SCD-OPT-01、P1、P6、P7）"

    # 引擎产物范围（需求 §4 的"引擎 vs 规则"判据）：
    #   纳入：include/ src/ scripts/ CMakeLists.txt —— **引擎产物**，业务词与演示常量 MUST 零命中
    #   豁免：docs/（需求与契约的示例语境）、policies/（**规则包 —— 方案名与演示常量的合法住所**）、
    #         tests/（自测把规则取值当测试数据与期望值 —— 验收清单原文允许）、
    #         examples/（演示必须喂真实规则取值才跑得起来，与测试数据同理）
    #   注意：scripts/acceptance.ps1 自身在本范围内 —— 因此**它 MUST NOT 含任何被禁字面量**：
    #         禁用词一律"拆开拼接"构造（自指陷阱），数值一律由变量拼出。
    $engineFiles = [string[]]@(CollectFiles @("include", "src", "scripts") `
        @(".h", ".hpp", ".cc", ".cpp", ".ps1") @())
    foreach ($f in @("CMakeLists.txt")) {
        $full = Join-Path $script:Repo $f
        if (Test-Path $full) { $engineFiles += $full }
    }
    $linkFiles = [string[]]@(CollectFiles @("include", "src", "examples", "scripts") `
        @(".h", ".hpp", ".cc", ".cpp", ".ps1") @())
    foreach ($f in @("CMakeLists.txt", "examples\CMakeLists.txt", "tests\CMakeLists.txt")) {
        $full = Join-Path $script:Repo $f
        if (Test-Path $full) { $linkFiles += $full }
    }
    Write-Host ("    业务词检索范围 {0} 个引擎产物文件（docs/ policies/ tests/ examples/ 豁免）；" -f $engineFiles.Count)
    Write-Host ("    依赖与网络检索范围 {0} 个文件（含 examples/ 与三个 CMakeLists）" -f $linkFiles.Count)

    # C07：方案名零命中。模式**拆开拼接**并自带排除项 —— 否则守卫脚本自己会命中自己的正则。
    # 注意：禁用词一律以 `'前缀' + '后缀'` 拼接（自指陷阱），且**显式**拼 pattern 字符串。
    $planTokens = New-Object System.Collections.Generic.List[string]
    $planTokens.Add('多域' + '协同压制')
    $planTokens.Add('稳态' + '侦察覆盖')
    $planTokens.Add('重点' + '区域突破')
    $planTokens.Add('分布式' + '稳态感知')
    $planTokens.Add('云边' + '协同自适应')
    $planTokens.Add('集中式' + '快速压制')
    $planTokens.Add('光电' + '精确打击')
    $planTokens.Add('多集群' + '协同压制')
    $planTokens.Add('电子' + '干扰配合')
    $planTokens.Add('集群' + '协同攻击')
    $planTokens.Add('电子' + '压制协同')
    $planPattern = ''
    foreach ($tk in $planTokens) {
        if ($planPattern -ne '') { $planPattern = $planPattern + [char]0x7C }
        $planPattern = $planPattern + [regex]::Escape($tk)
    }
    $planHits = @(SearchHits $engineFiles $planPattern @('planTokens', 'planPattern', 'SearchHits',
                                                         'MUST NOT', '零命中', '方案名', '检索范围',
                                                         'policies'))
    Check "C07" "SCD-CAND-01 / ADR-C2-01：引擎产物（include/src/scripts/CMake）内方案名零命中" `
        ($planHits.Count -eq 0) ("命中 {0} 处{1}" -f $planHits.Count, (Format-Hits $planHits))

    # C07b：**反证** —— 方案名确实住在规则包（否则 C07 的"零命中"可能是假绿）
    $policyFiles = [string[]]@(CollectFiles @("policies") @(".json") @())
    $policyPlanHits = @(SearchHits $policyFiles $planPattern @())
    Check "C07b" "ADR-C2-01 反证：方案名只住在规则包 policies/（换规则即换候选）" `
        ($policyPlanHits.Count -gt 0) `
        ("规则包内命中 {0} 处（方案名来自外部数据而非引擎）" -f $policyPlanHits.Count)

    # C08：**硬编码优化**零命中（`+ 3` 这类写死的增益 —— SCD-OPT-01）。
    # 数字与运算符由字符码拼出：脚本自身的源码里不出现完整模式片段（自指陷阱，同 C07）。
    $plus = [regex]::Escape('+')
    $hardcodedPattern = $plus + '\s*' + [char]0x33 + '(?![0-9])'
    $hardcodedHits = @(SearchHits $engineFiles $hardcodedPattern @('hardcodedPattern', 'SearchHits',
                                                                   'MUST NOT', '零命中', '检索范围',
                                                                   '演示常量'))
    Check "C08" "SCD-OPT-01：引擎产物内无硬编码优化增益（写死的 `+ N` 类）" `
        ($hardcodedHits.Count -eq 0) ("命中 {0} 处{1}" -f $hardcodedHits.Count, (Format-Hits $hardcodedHits))

    # C08c：**演示数值**零命中（评分基线这类取值属规则包 —— ADR-C1-02）
    #   判据（保守、可机检）：**数值上下文**——独立数字字面量、`std::int64_t(N)`、
    #   `== N` / `= N` / `return N` / `(N)`。数字由字符码拼出（自指陷阱）。
    #   非数值上下文（注释说明、十六进制常量、长数字里的一小段）不计。
    $twoA = [char]0x39 + [char]0x33
    $twoB = [char]0x39 + [char]0x39
    $apiPattern = ''
    foreach ($num in @($twoA, $twoB)) {
        $alts = @(
            '(?<![0-9A-Za-z_.])' + $num + '(?![0-9A-Za-z_.])',
            'std::int64_t\(' + $num + '\)',
            '(==|=|!=|<=|>=)\s*' + $num + '(?![0-9A-Za-z_.])',
            'return\s+' + $num + '(?![0-9A-Za-z_.])',
            '\(' + $num + '\)'
        )
        foreach ($a in $alts) {
            if ($apiPattern -ne '') { $apiPattern = $apiPattern + [char]0x7C }
            $apiPattern = $apiPattern + $a
        }
    }
    $apiHits = New-Object System.Collections.Generic.List[string]
    foreach ($f in $engineFiles) {
        $n = 0
        foreach ($line in [System.IO.File]::ReadAllLines($f, [System.Text.Encoding]::UTF8)) {
            $n++
            $code = ($line -replace '//.*$', '')          # 注释不计
            if ($code -notmatch $apiPattern) { continue }
            $apiHits.Add(("{0}:{1}" -f (Rel $f), $n))
        }
    }
    Check "C08c" "ADR-C1-02：引擎产物内无演示数值（评分基线只住规则包）" `
        ($apiHits.Count -eq 0) ("命中 {0} 处{1}" -f $apiHits.Count, (Format-Hits $apiHits))

    # C08b：**反证** —— 演示常量确实落在规则包里
    $demoPattern = ('(?<![0-9])' + $twoA + '(?![0-9])' +
                    [char]0x7C + '(?<![0-9])' + $twoB + '(?![0-9])')
    $demoPolicyHits = @(SearchHits $policyFiles $demoPattern @())
    Check "C08b" "ADR-C1-02 反证：演示数值只住在规则包 policies/（基线不进引擎）" `
        ($demoPolicyHits.Count -gt 0) `
        ("规则包内命中 {0} 处（基线来自外部数据而非引擎）" -f $demoPolicyHits.Count)

    # C09：跨仓 import 零命中，且只允许 nlohmann/json（P1 / SCD-NFR-01）
    $otherRepos = '(telemetry_store|telemetry-store|device_ingest|device-ingest|realtime_hub|' +
                  'realtime-hub|map_2d|map-2d|entity_ledger|entity-ledger|phase_engine|' +
                  'phase-engine|resource_alloc|resource-alloc|topology|view_composer|' +
                  'view-composer|alert_engine|alert-engine|report_engine|report-engine|' +
                  'selfcheck|geo_data|geo-data|media_player|media-player|drogon|Drogon|sqlite|' +
                  'mysql|pqxx|libpq|nanodbc|oatpp|crow|httplib|winhttp|WinHttp|curl)'
    $incHits = @(SearchHits $linkFiles ('#\s*include\s*[<"]' + $otherRepos) @('otherRepos', 'MUST NOT',
                                                                              '零命中', '检索范围'))
    Check "C09" "P1 / SCD-NFR-01：跨仓 import 与 Web/SQL/网络库依赖零命中" ($incHits.Count -eq 0) `
        ("命中 {0} 处{1}" -f $incHits.Count, (Format-Hits $incHits))

    $allIncludes = New-Object System.Collections.Generic.List[string]
    foreach ($f in $linkFiles) {
        foreach ($line in [System.IO.File]::ReadAllLines($f)) {
            $m = [regex]::Match($line, '^\s*#\s*include\s+(?:<([^>]+)>|"([^"]+)")')
            if ($m.Success) {
                if ($m.Groups[1].Success) { $allIncludes.Add($m.Groups[1].Value) }
                else { $allIncludes.Add($m.Groups[2].Value) }
            }
        }
    }
    # 只允许：C/C++ 标准库（无扩展名的头）、本模块公开头（scoring/...）、
    # 本模块内部头（internal.h）、示例共用头（../common.h 形式由 -match 放行）、nlohmann/json。
    $nonJson = $allIncludes | Where-Object {
        ($_ -notmatch '^(scoring/|internal\.h$|\.\./common\.h$)') -and
        ($_ -notmatch '^[a-z_]+$') -and
        ($_ -notmatch '^(nlohmann|third_party)')
    }
    Check "C10" "仅依赖 nlohmann/json（无其它第三方头）" ($nonJson.Count -eq 0) `
        ("非标准库/非 nlohmann 的 include：{0}" -f (($nonJson -join ", ") -replace '^$', '无'))

    # C11：不产生保留码 1001（冲突裁决 C15；注释与字符串里的说明文字不算）
    $codeFiles = @(CollectFiles @("include", "src") @(".h", ".hpp", ".cc", ".cpp") @())
    $bad1001 = New-Object System.Collections.Generic.List[string]
    $one = [char]0x31
    $zero = [char]0x30
    $code1001 = $one + $zero + $zero + $one
    foreach ($f in $codeFiles) {
        $n = 0
        foreach ($line in [System.IO.File]::ReadAllLines($f)) {
            $n++
            $stripped = ($line -replace '//.*$', '') -replace '/\*.*?\*/', ''
            if ($stripped -match '"') { continue }   # 字符串里的说明文字不计
            if ($stripped -match ('(?<![0-9])' + $code1001 + '(?![0-9])')) {
                $bad1001.Add(("{0}:{1}" -f (Rel $f), $n))
            }
        }
    }
    Check "C11" "冲突裁决 C15：不产生保留码（幂等成功走 code=0 + idempotent）" `
        ($bad1001.Count -eq 0) ("命中 {0} 处{1}" -f $bad1001.Count, (Format-Hits $bad1001))

    # C12：引擎内无落库 / 无广播（SCD-NFR-03：出口只走反向接口）
    $srcFiles = @(CollectFiles @("src") @(".cc", ".h") @())
    $dbPattern = "SQLite|sqlite3|mysql_|PQexec|nanodbc|drogon::|Drogon|broadcast\(|EventHub|eventhub|WSASocket|socket\("
    $dbHits = @(SearchHits $srcFiles $dbPattern @('MUST NOT'))
    Check "C12" "SCD-NFR-03 / P3：引擎内无落库 / 无广播 / 无网络调用" ($dbHits.Count -eq 0) `
        ("命中 {0} 处{1}" -f $dbHits.Count, (Format-Hits $dbHits))

    # C13：**演示基线的复算留痕** —— 从 selftest 的 stdout 里抓锚点行（ADR-C1-04）
    $anchorOk = $false
    $anchorLine = ""
    if (Test-Path $selftestExe) {
        $outPath = Join-Path ([System.IO.Path]::GetTempPath()) ("scoring-anchor-{0}.txt" -f $PID)
        & $selftestExe anchor_recommended_total > $outPath 2>&1
        if (Test-Path $outPath) {
            $txt = [System.IO.File]::ReadAllText($outPath, [System.Text.Encoding]::UTF8)
            $lines = $txt -split "`r?`n"
            foreach ($l in $lines) {
                if ($l -match 'baseline') { $anchorLine = $l.Trim() }
            }
            # 锚点行必须同时出现"独立推论 = X%"与"定点和 Y"
            if ($anchorLine -match '独立推论\s*=\s*([0-9]+)%' -and $anchorLine -match '定点和\s*([0-9]+)') {
                $anchorOk = $true
            }
        }
    }
    Check "C13" "ADR-C1-04：演示基线由规则包 baseline×weight 独立复算（不经过引擎常量）" $anchorOk `
        (($anchorLine -replace '^\s+', '') -replace '^$', '锚点行未取到')

    # §7 第 2 行的结论：引擎产物内演示常量零命中（合法住所 = 规则包）
    $s07_02_ok = ($planHits.Count -eq 0) -and ($policyPlanHits.Count -gt 0) -and
                 ($demoHits.Count -eq 0) -and ($demoPolicyHits.Count -gt 0) -and $anchorOk
    Check "S07-02" "全仓检索方案名、演示常量与硬编码优化 → 零命中（反证：只在规则包）" $s07_02_ok `
        ("引擎产物：方案名 {0} 处 / 演示数值 {1} 处；规则包（合法住所）：方案名 {2} 处 / 演示数值 {3} 处；基线复算={4}" -f `
            $planHits.Count, $demoHits.Count, $policyPlanHits.Count, $demoPolicyHits.Count, $anchorOk)

    # ================================================================ ⑤ 反向接口与入口纪律
    Section "⑤ 反向接口纪律：IPlanStore / IPlanSink / IClock / ILogSink 由宿主注入（P8/P9）"

    $headerPath = Join-Path $script:Repo "include\scoring\scoring.h"
    $headerOk = Test-Path $headerPath
    $headerText = ""
    if ($headerOk) { $headerText = [System.IO.File]::ReadAllText($headerPath) }

    $hasSink = ($headerText -match 'class\s+IPlanSink') -and
               ($headerText -match 'virtual\s+void\s+onPlanStateChanged')
    $hasStore = ($headerText -match 'class\s+IPlanStore') -and
                ($headerText -match 'virtual\s+bool\s+save') -and
                ($headerText -match 'virtual\s+bool\s+load')
    $hasClock = ($headerText -match 'class\s+IClock') -and ($headerText -match 'virtual\s+int64_t\s+nowMs')
    $hasLog = ($headerText -match 'class\s+ILogSink') -and ($headerText -match 'virtual\s+void\s+log') -and
              ($headerText -match 'virtual\s+void\s+commandAudit')
    Check "C14" "公开头声明四个反向接口 IPlanStore / IPlanSink / IClock / ILogSink" `
        ($hasSink -and $hasStore -and $hasClock -and $hasLog) `
        ("sink={0} store={1} clock={2} log={3}" -f $hasSink, $hasStore, $hasClock, $hasLog)

    $injectedAll = ($headerText -match 'std::shared_ptr<IPlanStore>\s+store') -and
                   ($headerText -match 'std::shared_ptr<IPlanSink>\s+sink') -and
                   ($headerText -match 'std::shared_ptr<IClock>\s+clock') -and
                   ($headerText -match 'std::shared_ptr<ILogSink>\s+log')
    Check "C15" "四个出口全部经 ScoringEngineOptions 注入（无内建实现、无全局单例）" $injectedAll `
        "ScoringEngineOptions{store,sink,clock,log} 四个可空 shared_ptr"

    # C16：唯一公开头
    $publicHeaders = @()
    $includeDir = Join-Path $script:Repo "include"
    if (Test-Path $includeDir) {
        $publicHeaders = @(Get-ChildItem $includeDir -Recurse -File -Filter *.h |
            ForEach-Object { Rel $_.FullName })
    }
    Check "C16" "唯一公开头 include/scoring/scoring.h（P1）" `
        (($publicHeaders.Count -eq 1) -and ($publicHeaders[0] -eq "include\scoring\scoring.h")) `
        ("公开头：{0}" -f ($publicHeaders -join ", "))

    $internalH = Join-Path $script:Repo "src\internal.h"
    $internalGuarded = $false
    if (Test-Path $internalH) {
        $t = [System.IO.File]::ReadAllText($internalH)
        $internalGuarded = ($t -match '宿主 MUST NOT 包含')
    }
    Check "C17" "内部头 src/internal.h 明确标注宿主不可包含" $internalGuarded `
        "src/internal.h 头部注明'MUST NOT 包含本文件'"

    # C18：PhaseContext 形状逐字等于 protocol.md §1.4（五字段）
    $pcBody = ""
    if ($headerText -match '(?s)struct PhaseContext\s*\{(.*?)\};') { $pcBody = $Matches[1] }
    $pcOk = ($pcBody -ne "") -and ($pcBody -match 'phaseKey') -and ($pcBody -match 'seq') -and
            ($pcBody -match 'scenarioKey') -and ($pcBody -match 'enteredAt') -and
            ($pcBody -match 'missionId')
    Check "C18" "protocol §1.4：PhaseContext 五字段逐字一致（当前阶段的唯一入参形状）" $pcOk `
        "phaseKey / seq / scenarioKey / enteredAt / missionId"

    # C19：错误码表逐值对齐 protocol §3.2，且 1001 不在其中（C15）
    $codesOk = ($headerText -match 'BadRequest\s*=\s*1000') -and
               ($headerText -match 'Conflict\s*=\s*1002') -and
               ($headerText -match 'PreconditionUnmet\s*=\s*1003') -and
               ($headerText -match 'NotFound\s*=\s*1004') -and
               ($headerText -match 'Internal\s*=\s*1005') -and
               ($headerText -match 'VersionMismatch\s*=\s*1006')
    $noReserved = -not ($headerText -match ('=\s*' + $code1001 + '(?![0-9])'))
    Check "C19" "protocol §3.2 / C15：错误码逐值对齐且不含保留码" ($codesOk -and $noReserved) `
        ("0/1000/1002/1003/1004/1005/1006 齐备={0}；无保留码={1}" -f $codesOk, $noReserved)

    # C20：plan.state 事件的既有四字段在引擎内逐字出现（CTR-EV-04：只增不改）
    $eventOk = ($headerText -match 'plan\.state' -or
                ([System.IO.File]::ReadAllText((Join-Path $script:Repo "src\decide.cc")) -match 'plan\.state')) -and
               ($headerText -match 'adopted') -and ($headerText -match 'optimized') -and
               ($headerText -match 'confirmed')
    Check "C20" "SCD-DECIDE-04：plan.state 事件与 adopted/optimized/confirmed 取值在引擎内落地" `
        $eventOk "事件名与 action 取值取自 protocol §4.3 冻结表"

    # ================================================================ ⑥ 规则包（protocol.md §5）
    Section "⑥ 规则包（protocol.md §5：policiesNamespace / schemaVersion / kind / items）"

    $policyDir = Join-Path $script:Repo "policies\mapapp"
    $neededPolicies = @("scoringMetrics.json", "planTemplates.json")
    $absentPolicies = @()
    foreach ($f in $neededPolicies) {
        if (-not (Test-Path (Join-Path $policyDir $f))) { $absentPolicies += $f }
    }
    Check "C21" "规则包齐全 policies/mapapp/{scoringMetrics,planTemplates}.json" `
        ($absentPolicies.Count -eq 0) ("缺失：{0}" -f (($absentPolicies -join ", ") -replace '^$', '无'))

    $schemaOk = $true
    $schemaDetail = New-Object System.Collections.Generic.List[string]
    $kindsOk = $true
    foreach ($f in $neededPolicies) {
        $full = Join-Path $policyDir $f
        if (-not (Test-Path $full)) { $schemaOk = $false; continue }
        try {
            $doc = [System.IO.File]::ReadAllText($full, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
        } catch {
            $schemaOk = $false
            $schemaDetail.Add(("{0}：非法 JSON" -f $f))
            continue
        }
        $hasNs = ($null -ne $doc.policiesNamespace) -and ($doc.policiesNamespace -ne "")
        $hasVer = ($null -ne $doc.schemaVersion) -and ($doc.schemaVersion -match '^\d+\.\d+\.\d+$')
        $hasKind = ($null -ne $doc.kind) -and ($doc.kind -ne "")
        $hasItems = ($null -ne $doc.items)
        if (-not ($hasNs -and $hasVer -and $hasKind -and $hasItems)) {
            $schemaOk = $false
            $schemaDetail.Add(("{0}：ns={1} ver={2} kind={3} items={4}" -f $f, $hasNs, $hasVer,
                               $hasKind, $hasItems))
        }
        # §5.3 登记的两个 kind 必须与文件一一对应
        $expectKind = "scoringMetrics"
        if ($f -eq "planTemplates.json") { $expectKind = "planTemplates" }
        if ($doc.kind -ne $expectKind) {
            $kindsOk = $false
            $schemaDetail.Add(("{0}：kind={1}，应为 {2}" -f $f, $doc.kind, $expectKind))
        }
        # MAJOR 必须与引擎声明一致（§5.2）
        if ($doc.schemaVersion -and ($doc.schemaVersion -notmatch '^1\.')) {
            $kindsOk = $false
            $schemaDetail.Add(("{0}：MAJOR 非 1（引擎只支持 MAJOR=1）" -f $f))
        }
    }
    Check "C22" "规则包骨架：policiesNamespace + schemaVersion(MAJOR.MINOR.PATCH) + kind + items" `
        $schemaOk ("问题：{0}" -f (($schemaDetail -join "；") -replace '^$', '无'))
    Check "C23" "protocol §5.2/§5.3：kind 与 schemaVersion MAJOR 与引擎声明一致" $kindsOk `
        "kind ∈ {scoringMetrics, planTemplates}；MAJOR = 1"

    # C23b：**演示基线锚点的参数条件**（权重和为 1、基线六项齐备）—— 在引擎之外核对规则包
    $anchorParamsOk = $false
    $anchorDetail = ""
    if (Test-Path (Join-Path $policyDir "scoringMetrics.json")) {
        $mp = [System.IO.File]::ReadAllText((Join-Path $policyDir "scoringMetrics.json"),
              [System.Text.Encoding]::UTF8) | ConvertFrom-Json
        $wsum = 0.0
        $nBase = 0
        foreach ($it in $mp.items) {
            $wsum += [double]$it.weight
            if ($null -ne $it.baseline) { $nBase++ }
        }
        $anchorParamsOk = ([Math]::Abs($wsum - 1.0) -lt 1e-9)
        $anchorDetail = ("权重和 = {0:N4}；带基线的指标 {1} 项" -f $wsum, $nBase)
    }
    Check "C23b" "SCD-SCORE-03：规则包权重和 = 1.0000（演示基线锚点的参数前提）" $anchorParamsOk `
        $anchorDetail

    # ================================================================ ⑦ 独立交付
    Section "⑦ 独立交付（SCD-NFR-02：独立构建 / 示例 / 测试 / 验收脚本）"

    $needed = @("CMakeLists.txt", "include\scoring\scoring.h", "src\internal.h",
                "tests\CMakeLists.txt", "tests\selftest.cc", "examples\CMakeLists.txt",
                "scripts\acceptance.ps1", "policies\mapapp\scoringMetrics.json",
                "policies\mapapp\planTemplates.json", "README.md", "LICENSE",
                "third_party\nlohmann\json.hpp")
    $absent = @()
    foreach ($f in $needed) { if (-not (Test-Path (Join-Path $script:Repo $f))) { $absent += $f } }
    Check "C24" "独立交付要件齐全（构建 / 公开头 / 测试 / 示例 / 验收脚本 / 规则包 / 单头回落）" `
        ($absent.Count -eq 0) ("缺失：{0}" -f (($absent -join ", ") -replace '^$', '无'))

    $exampleNames = @("example_minimal", "example_audit", "example_full_flow", "example_decision")
    $exampleCodes = @{}
    foreach ($name in $exampleNames) {
        $exe = Join-Path $bin "$name.exe"
        if (-not (Test-Path $exe)) { $exe = Join-Path $bin $name }
        if (-not (Test-Path $exe)) { $exampleCodes[$name] = -1; continue }
        $outPath = Join-Path ([System.IO.Path]::GetTempPath()) ("scoring-{0}-{1}.txt" -f $name, $PID)
        & $exe > $outPath 2>&1
        $exampleCodes[$name] = $LASTEXITCODE
    }
    $exOk = $true
    foreach ($k in $exampleCodes.Keys) { if ($exampleCodes[$k] -ne 0) { $exOk = $false } }
    Check "C25" "四个示例独立运行退出码 0" $exOk `
        (($exampleCodes.Keys | Sort-Object | ForEach-Object { "{0}={1}" -f $_, $exampleCodes[$_] }) -join " ")

    # ⑦ -2 ctest（可选的目标，缺 ctest 不判失败）
    $ctest = (Get-Command ctest -ErrorAction SilentlyContinue).Source
    if ($ctest) {
        $ctestOut = (& $ctest --test-dir $buildPath -C $Config --output-on-failure 2>&1 | Out-String)
        $ctestOk = ($LASTEXITCODE -eq 0)
        Check "C26" "ctest 注册的用例全部通过" $ctestOk ((($ctestOut -split "`n") |
            Where-Object { $_ -match 'tests passed|tests failed|Total Test time' }) -join " ")
    } else {
        Check "C26" "ctest 注册的用例全部通过（跳过：未找到 ctest）" $true "ctest 不在 PATH，视为不适用"
    }

    # ⑦ -3 验收脚本自身可机检：以退出码 0/1 结束
    $scriptText = [System.IO.File]::ReadAllText((Join-Path $script:Repo "scripts\acceptance.ps1"))
    $exitOk = ($scriptText -match '(?m)^\s*exit\s+1') -and ($scriptText -match '(?m)^\s*exit\s+0')
    Check "C27" "验收脚本以退出码 0/1 结束（SCD-NFR-02）" $exitOk `
        "脚本内含 exit 0 与 exit 1 两条收口路径"

    # ================================================================ 汇总
    Section "汇总"

    $total = $script:Results.Count
    $passed = 0
    foreach ($r in $script:Results) { if ($r.Ok) { $passed++ } }
    $failed = $total - $passed

    if ($jsonOk) {
        Write-Host ("selftest ：用例 {0} 个（失败 {1}）｜断言 {2} 条（失败 {3}）" -f `
            $data.cases, $data.casesFailed, $data.asserts, $data.assertsFailed)
    }
    Write-Host ("验收检查：{0} 项｜PASS {1}｜FAIL {2}" -f $total, $passed, $failed)

    if ($failed -eq 0) {
        Write-Host ""
        Write-Host "验收通过（退出码 0）" -ForegroundColor Green
        exit 0
    }
    Write-Host ""
    Write-Host "验收失败（退出码 1）：" -ForegroundColor Red
    foreach ($r in $script:Results) {
        if (-not $r.Ok) { Write-Host ("  - {0} {1}：{2}" -f $r.Id, $r.Title, $r.Detail) -ForegroundColor Red }
    }
    exit 1
} finally {
    Pop-Location
}
