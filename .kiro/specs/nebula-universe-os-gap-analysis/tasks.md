# Implementation Plan: Nebula Universe OS Gap Analysis

## Overview

以 Python 实现只读、证据优先、fail-closed 的仓库评估流水线。任务按 Revision Binder → Source Inventory → Evidence/Claim Guard → capability evaluators → Hard-Gate/maturity → canonical model/validation → renderers → 仓库采集与最终报告的依赖顺序推进；只创建分析工具、自动化测试与版本化评估产物，不修改 Nebula 编译器、运行时、内核、驱动或其他产品代码。

## Tasks

- [x] 1. 建立评估工具骨架与规范化类型系统 [核心实现]
  - [x] 1.1 创建独立 Python 包、只读 CLI 入口和测试目录
    - 在 `tools/universe_os_gap_analysis/` 建立模块边界，在 `tests/universe_os_gap_analysis/` 建立测试结构，并固定 Python/Hypothesis/JSON Schema 测试依赖版本。
    - CLI 默认禁止网络和外部命令，只允许读取仓库并向显式 assessment 输出目录写入；不得导入或修改产品实现路径。
    - _Requirements: 1.1, 1.3, 14.1_
  - [x] 1.2 实现枚举、typed model、ID/reference 类型和稳定序列化基础
    - 定义设计中的 `AssessmentRevision`、`SourceInventoryEntry`、`EvidenceRecord`、`EvidenceConflict`、`CapabilityDomain`、`CapabilityAssessment`、`GapEntry`、`HardGate`、`AssessmentModel` 及全部闭集枚举。
    - 保证稳定 ID、排序、唯一 Evidence_Status、repository-relative path 和 typed observed/recommendation 分区可被后续组件共用。
    - _Requirements: 1.4, 3.1, 4.1, 12.1, 12.2, 12.3, 14.2, 14.3, 14.7_
  - [x] 1.3 实现六级目标、成熟度 rubric、capability/checklist 与初始结论目录
    - 编码且固定 T0–T5 的顺序和定义、0–5 序数含义、Hosted Adjacency/OS Substrate 边界、mandatory domain 元数据及 non-additive 声明。
    - 编码当前结论契约和最短证据路径模板，但只允许在仓库证据未推翻时采用。
    - _Requirements: 2.1–2.8, 3.2, 3.7, 15.1–15.7_

- [x] 2. 绑定 revision 并生成可复现 worktree fingerprint [核心实现]
  - [x] 2.1 实现 Revision Binder
    - 采集 commit、branch、VERSION、describe/tags、UTC assessment timestamp、cleanliness 与 repository root identity，并明确区分 tagged release、committed revision、current worktree 三条证据轴。
    - Git/version/文件漂移读取失败时返回 `REV-*` 错误并终止，不混用部分 snapshot。
    - _Requirements: 1.1, 1.2_
  - [x] 2.2 实现 length-prefixed SHA-256 工作树指纹
    - 按设计对 tracked 与 non-ignored untracked 路径排序，哈希 path、file kind、mode、content；符号链接不跟随，另存 tracked diff hash 与 untracked path-set hash。
    - 排除仅限显式输出目录，记录规则版本、路径与理由；读取失败或采集前后 fingerprint 漂移必须 fail closed。
    - _Requirements: 1.1, 1.2, 9.7_
  - [x]* 2.3 编写 Revision Binder 与 fingerprint 集成测试 [测试 · 可选]
    - 在临时 Git 仓库覆盖 clean/dirty/tagged、tracked/untracked、符号链接、mode、读取失败、输出排除、自引用防护和前后漂移；验证 timestamp 不影响内容指纹。
    - _Requirements: 1.1, 1.2, 9.7_
  - [x]* 2.4 编写 Hypothesis Property 1 测试 [测试 · 可选]
    - **Property 1: Revision-origin isolation**
    - 对 dirty/clean 状态和 evidence origin 组合验证 Current_Worktree 永远可区分且不能渲染成 tagged-release 证据；至少运行 100 个样例并保留最小反例/seed。
    - **Validates: Requirements 1.2**

- [x] 3. 构建完整 source inventory 与受控执行证据模型 [核心实现]
  - [x] 3.1 实现必查来源发现、分类和稳定锚点适配器
    - 枚举 source、README、ROADMAP、CHANGELOG、全部 release notes、spec/RFC、tests/cases、CMake/build、CI/release workflow、runtime/std/official、examples 与 UniverseOS gate/docs。
    - 为 Markdown heading、symbol、manifest key、case ID、workflow job 和 artifact metadata 生成 content hash、stable anchor、revision origin 与 inspected/validated/skipped 状态；必查类别缺失触发 `INV-*`。
    - _Requirements: 1.3, 14.4, 14.5, 14.6_
  - [x] 3.2 实现 gate registry、case manifest、workflow、release 与 artifact adapter
    - 解析 gate registry 内嵌 JSON、`case.toml`、CI/release job 和资产元数据，验证 gate/case/dependency/non-claim 引用而不把定义或历史结果冒充本次执行。
    - 对无法执行或无法取得的来源记录 `NotRun`/`Unavailable` 及原因。
    - _Requirements: 1.3, 7.7, 11.5, 14.4, 14.6_
  - [x] 3.3 实现 execution policy 与执行证据快照
    - 默认禁用命令；仅支持配置化本地 allowlist，并记录 command ID、环境/平台、退出码、stdout/stderr artifact、执行 fingerprint 和 validation state。
    - timeout、缺工具、平台不可用及 fingerprint drift 分别 fail/标注，不得推断 gate 通过。
    - _Requirements: 1.4, 9.5, 13.1, 14.6_
  - [x] 3.4 编写 inventory 与 adapter 集成测试 [测试 · 可选]
    - 使用 fixture 和本仓库 dry run 验证所有 Requirement 1.3 类别、最小稳定锚点、gate/case 交叉引用、未执行披露和 `INV-*` fail-closed 行为。
    - _Requirements: 1.3, 7.7, 14.4–14.6_

- [x] 4. 规范化证据并实施 Claim Guard [核心实现]
  - [x] 4.1 实现 Evidence Collector、normalizer 和 deduplicator
    - 将 source/test/release/workflow/artifact/example/plan/non-claim 转换成字段完整的 Evidence_Record；按稳定 claim key 去重，同时保留多来源和 revision origin。
    - 实现状态决策顺序；仅计划文本固定为 `Planned`，无 verifiable path 为 `Unknown`，`Unsupported` 只接受显式 non-claim/negative gate 或完整 inventory 审核。
    - _Requirements: 1.4, 1.6, 4.1, 4.2, 13.5_
  - [x] 4.2 实现 lossless evidence conflict detection
    - 检测不兼容 claim，生成包含全部 record/location 的对称冲突，`winner = null`、confidence 强制 Low；不得按输入顺序或“更有利”来源选赢家。
    - _Requirements: 1.5, 13.4_
  - [x] 4.3 实现 Claim Guard 状态、措辞、scope 与 non-claim 规则
    - 仅当前 revision 直接实现证据允许当前时态；保留 GA/preview/experimental/planned/unsupported/unknown，不允许 summary 升级。
    - 固定 primitive object 为 clang-backed ELF64 relocatable-object emission；保留 kernel/driver/interrupt/MMU/scheduler/syscall ABI/freestanding runtime/bootability/backend independence non-claims，并声明 gate 只证明命名 scope。
    - _Requirements: 4.3–4.6, 7.6, 8.6, 11.6, 13.1–13.3, 13.6, 13.7_
  - [x] 4.4 实现 exclusions 与 trust-assumption 审核
    - 检测 opaque/dynamic/FFI/unsafe exclusions 及 trusted tool、cooperative descendant、caller-controlled directory、host security service 等假设，要求完整写入 limitations。
    - 任一检测集合与记录集合不一致时立即产生带 record/requirement refs 的 `CLM-*` 并终止。
    - _Requirements: 6.6, 9.5, 9.6_
  - [x] 4.5 编写 Hypothesis Property 2 测试 [测试 · 可选]
    - **Property 2: Accepted evidence is complete and singly classified**
    - 生成合法/非法 record，验证字段完整、唯一允许状态、引用与 stable location；至少 100 examples。
    - **Validates: Requirements 1.4, 4.1, 4.2**
  - [x] 4.6 编写 Hypothesis Property 3 测试 [测试 · 可选]
    - **Property 3: Conflicts are symmetric, lossless, and winner-free**
    - 对冲突集合全部排列验证无损、无 winner、Low confidence 且结果与输入顺序无关。
    - **Validates: Requirements 1.5, 13.4**
  - [x] 4.7 编写 Hypothesis Property 4 测试 [测试 · 可选]
    - **Property 4: Plans never become implementation**
    - 生成 roadmap/RFC/proposed-test/planned-gate-only claims，验证 Planned、未来时态和零成熟度 credit。
    - **Validates: Requirements 1.6, 13.2**
  - [x] 4.8 编写 Hypothesis Property 15 测试 [测试 · 可选]
    - **Property 15: Trust assumptions are complete or validation fails**
    - 生成 detected/recorded assumption 集合及差集，验证缺漏时引用受影响 records 并 fail closed。
    - **Validates: Requirements 9.5, 9.6**
  - [x] 4.9 编写 Hypothesis Property 21 测试 [测试 · 可选]
    - **Property 21: Present-tense claims require direct current evidence**
    - 验证 docs/examples scope 上限、pathless Unknown、current-evidence 时态门禁及 non-claim 持续性。
    - **Validates: Requirements 13.1, 13.3, 13.5, 13.6**

- [x] 5. 实现 Language、Type System、Memory、Concurrency 与 Safety evaluators [核心实现]
  - [x] 5.1 实现语言语义与类型系统声明式 evaluator
    - 覆盖 lexical/control flow/function/method/module/visibility/generic/trait/closure/pattern/error/reflection/macro/metaprogramming，以及 width/pointer/reference/slice/array/collection/null/aggregate/enum/callable/variance/lifetime/constrained generic/dynamic dispatch。
    - 将规范、parser/typechecker、兼容性政策分开；记录 authoritative path、实现入口、测试 gate、Language_Gap 与 semantic-stability Verification_Gap。
    - _Requirements: 5.1–5.5_
  - [x] 5.2 实现 memory/ownership/concurrency/unsafe evaluator
    - 覆盖 storage、promotion、init/destruction、allocation failure、raw memory/resource lifetime，区分 Rep × Owner/borrow assistance 与 normative move/borrow/lifetime/alias model。
    - 覆盖 thread/task/actor/structured concurrency/interruption/atomic/order/race/interrupt safety/synchronization，以及 unsafe/FFI/raw pointer/volatile/MMIO/intrinsic/assembly/privilege transition；hosted cooperative async 生成 scheduler-independent Implementation_Gap。
    - _Requirements: 6.1–6.6_
  - [x] 5.3 编写 Hypothesis Property 10 测试 [测试 · 可选]
    - **Property 10: Semantic evidence creates the correct gap kind**
    - 随机组合 specification、parser/typechecker 和 compatibility-policy 证据，验证 authoritative references 与 one-primary-category 规则。
    - **Validates: Requirements 5.3, 5.4**
  - [x] 5.4 编写 Hypothesis Property 11 测试 [测试 · 可选]
    - **Property 11: Safety assistance and hosted async stay bounded**
    - 验证辅助分析不满足 normative safety、hosted async 产生实现 gap，且所有 unsafe/opaque/dynamic/FFI exclusion 被保留。
    - **Validates: Requirements 6.2, 6.4, 6.6**

- [x] 6. 实现 ABI、backend、runtime、library、package 与 boot evaluators [核心实现]
  - [x] 6.1 实现 ABI 与 compiler/backend evaluator
    - 分别评估 extern/export C ABI、calling convention、symbol/layout/alignment/versioning/fixtures，以及 compiler/runtime/boot/syscall/driver/package ABI。
    - 覆盖 frontend、NIR/CFG、analysis、optimization、incremental、debug info、native codegen、assembler/linker、bootstrap 和 production dependency inventory；generated C++、external clang 或 inventory/independent bootstrap 缺口阻断 T1。
    - _Requirements: 7.1–7.6_
  - [x] 6.2 实现 hosted/freestanding runtime、library-layer 与 package evaluator
    - 覆盖 startup/static init/panic/allocation/termination/unwind/exception/runtime ABI，按 API/host dependency/allocation/platform/stability/verification 评估 std。
    - 将 future `core`、hosted `std`、future `system` 分域；resolver 或 implementation 任一缺失时 import 为 Planned；覆盖 manifest/workspace/lock/registry/git/native/reproducibility/signing/vulnerability/compatibility/offline。
    - _Requirements: 8.1–8.6_
  - [x] 6.3 实现 boot evaluator 与 pre-kernel gate candidate 生成
    - 将 target spec、protocol、entry、linker script/input、relocation、startup object、deterministic linked ELF、boot media、QEMU execution 分成独立证据和 gate。
    - 建立 low-level soundness → system ABI → backend/bootstrap 与 runtime/boot-toolchain 并行分支 → linked ELF join → media → QEMU 的候选依赖，不允许 primitive ET_REL 证明后续阶段。
    - _Requirements: 7.7, 10.1, 12.5, 12.7, 15.7_
  - [x] 6.4 编写 Hypothesis Property 12 测试 [测试 · 可选]
    - **Property 12: ABI evidence is scope-isolated and production dependencies block T1**
    - 生成 ABI scopes 与 production dependency 组合，验证 hosted C ABI 不跨域，任何独立性 blocker 均使 T1 未达成。
    - **Validates: Requirements 7.2, 7.4, 7.5**
  - [x] 6.5 编写 Hypothesis Property 13 测试 [测试 · 可选]
    - **Property 13: Primitive-object proof and boot gates remain decomposed**
    - 验证 primitive object 措辞上限，以及 target/link/startup/linked image/media/execution gate 的独立有序性。
    - **Validates: Requirements 7.6, 7.7**
  - [x] 6.6 编写 Hypothesis Property 14 测试 [测试 · 可选]
    - **Property 14: Library layers and preview statuses do not collapse**
    - 验证 core/std/system 分离、缺 resolver/implementation 时 Planned，以及 Installed/Repo Preview 在 summary/target 中不升级。
    - **Validates: Requirements 8.3, 8.4, 8.6**

- [x] 7. 实现 Kernel、Hardware、Drivers、Userspace、Operations 与 Ecosystem evaluators [核心实现]
  - [x] 7.1 实现 kernel、hardware 与 driver evaluators
    - 独立覆盖 firmware/boot protocol/console/timer/interrupt/trap/CPU/MMU/page/physical+virtual memory/DMA/IOMMU/power，以及 kernel entry/panic/sync/scheduler/context switch/syscall/capability/IPC/process/thread/isolation/accounting。
    - 独立覆盖 discovery/bus/lifecycle/isolation/IRQ/DMA safety、storage/network/input/display/audio/qualification；无 direct implementation evidence 的 domain 输出 maturity 0。
    - _Requirements: 10.1–10.3, 10.6, 12.6_
  - [x] 7.2 实现 userspace、system service 与 product-shell evaluator
    - 覆盖 filesystem/storage/network/service manager/identity/policy/time/config/install/update/rollback/backup/recovery、user runtime/command/app model/GUI/accessibility/sandbox/distribution/SDK。
    - 在没有 Nebula-owned process/syscall boundary 时保持 T4 domain 为 0，并保留 isolation、userspace、update/recovery、shell 的独立 gate。
    - _Requirements: 10.4–10.6, 12.6_
  - [x] 7.3 实现 debugging、observability、security 与 reliability evaluator
    - 区分 compiler/hosted service 与 boot/kernel/driver/userspace observability，覆盖 diagnostics/LSP/debug/stack/symbol/crash/profile/trace/metric/log correlation。
    - 覆盖 supply chain/integrity/package trust/unsafe audit/capability/isolation/privilege/secure boot/secret/crypto/update rollback/incident response，以及 bounded execution/containment/transactionality/crash consistency/power-loss/deterministic rebuild/recovery。
    - _Requirements: 9.1–9.5_
  - [x] 7.4 实现 application ownership、ecosystem 与 release evaluator
    - 对 CLI/backend/control plane/data/auth/jobs/TLS/crypto/UI/thin-host/native adapters 及 renderer/accessibility/signing/notarization/install/update/distribution/crash reporting 强制唯一 `NebulaOwned | HostOwned | OperationsOwned`。
    - 覆盖 package/docs/starter/compatibility/contributor/adoption/security/LTS 与 strict matrix/contracts/sanitizer/release smoke/SBOM/provenance/attestation/installers/rollback/platform qualification；scoped release 不得提升 OS substrate。
    - _Requirements: 11.1–11.6_
  - [x] 7.5 实现 preview-security ecosystem obligation 生成
    - 对 security-sensitive preview package 生成 maintenance、certification、deployment、vulnerability-response Ecosystem_Gap，除非每项均有独立直接关闭证据。
    - _Requirements: 9.8_
  - [x] 7.6 编写 Hypothesis Property 9 测试 [测试 · 可选]
    - **Property 9: Hosted and scoped-release evidence cannot propagate into OS substrate**
    - 验证 hosted examples/observability、compiler/tooling GA 与 Linux backend SDK release 只影响声明 scope。
    - **Validates: Requirements 4.6, 9.2, 11.6**
  - [x] 7.7 编写 Hypothesis Property 17 测试 [测试 · 可选]
    - **Property 17: Preview security packages create ecosystem obligations**
    - 生成 preview security packages 与独立 closing evidence，验证四类生态义务完整性。
    - **Validates: Requirements 9.8**
  - [x] 7.8 编写 Hypothesis Property 18 测试 [测试 · 可选]
    - **Property 18: A boot hello does not imply an operating system**
    - 单独加入 QEMU serial hello 后，验证 driver/interrupt/MMU/scheduler/syscall/isolation/storage/network/userspace/operations 不变。
    - **Validates: Requirements 10.7**
  - [x] 7.9 编写 Hypothesis Property 19 测试 [测试 · 可选]
    - **Property 19: Application responsibility ownership is exclusive**
    - 验证每个 application responsibility 只有一个 owner，且 ownership 不传播 maturity。
    - **Validates: Requirements 11.2**

- [x] 8. 构建 Hard-Gate DAG、成熟度降帽与 target achievement [核心实现]
  - [x] 8.1 实现 Hard-Gate graph builder 与预计算验证
    - 验证 unknown node、重复/自边、缺失/越界 gate score、cycle 和非法 branch/join；仅 blocking edge 参与 cap，每条边必须带上限理由。
    - 输出明确并行 branch、join gate、frontier 和 dependency-ordered path；任何 `GRF-*` 在成熟度计算前终止。
    - _Requirements: 3.3–3.5, 12.5–12.7_
  - [x] 8.2 实现 raw/effective maturity assessor
    - 仅从直接证据计算 raw 0–5；按拓扑序以所有 blocking dependency/gate 分数取最小值得 effective score。
    - 无实现证据固定 raw/effective 0；language/tooling 无跨主机 candidate+迁移/回滚+release-review 证据时最高 2。
    - _Requirements: 3.1–3.6, 10.6, 15.4, 15.5_
  - [x] 8.3 实现 target-level achievement 和 scoped blockers
    - 仅当 mandatory domains、hard gates、conflicts 和 validation 全部满足时标记 level achieved；T0 adjacency 不能影响 T1–T5，生产 C++/host tooling blocker 保持 T1 unachieved。
    - 不生成总分、平均值、百分比或工期；报告 next gate、blocking dependencies 和 limitations。
    - _Requirements: 2.2, 2.3, 3.1–3.3, 7.4, 7.5, 15.2, 15.3, 15.6_
  - [x] 8.4 编写 Hypothesis Property 5 测试 [测试 · 可选]
    - **Property 5: Target hierarchy and hosted-adjacency isolation**
    - 验证恰好六个有序 target，添加 hosted evidence 不改变 T1–T5 achievement/critical path。
    - **Validates: Requirements 2.2, 2.3, 15.6**
  - [x] 8.5 编写 Hypothesis Property 6 测试 [测试 · 可选]
    - **Property 6: Domain assessments are complete, ordinal, and non-additive**
    - 随机生成 domain sets，验证一域一 assessment、字段完整、整数 0..5 且无 aggregate/schedule 输出。
    - **Validates: Requirements 3.1, 3.2, 3.7**
  - [x] 8.6 编写 Hypothesis Property 7 测试 [测试 · 可选]
    - **Property 7: Dependency validation precedes maturity capping**
    - 覆盖非法 score/node/edge/cycle 与合法 DAG/parallel join，验证先失败后计算及所有 cap 不变量；复杂图至少 300 examples。
    - **Validates: Requirements 3.4, 3.5, 12.7**
  - [x] 8.7 编写 Hypothesis Property 8 测试 [测试 · 可选]
    - **Property 8: No direct implementation evidence means zero**
    - 对 plans/prerequisites/examples/adjacency 任意组合验证所有无实现 domain，尤其 OS substrate，始终为 0。
    - **Validates: Requirements 3.6, 10.6, 15.5**
  - [x] 8.8 编写 Hypothesis Property 23 测试 [测试 · 可选]
    - **Property 23: Candidate evidence is required to exceed repository-local maturity 2**
    - 组合 cross-host candidate contract、migration/rollback 和 release-review evidence，验证三者齐备前 cap=2。
    - **Validates: Requirements 15.4**

- [x] 9. 生成、分类并确定性排序 gap register 与 roadmap [核心实现]
  - [x] 9.1 实现 GapEntry 生成和 one-primary-category 校验
    - 每个 gap 恰有一个 primary，可有去重且不重复 primary 的 secondary labels；记录 domains、status、target、severity、dependencies、acceptance evidence、owner area、observed fact 和 recommendation。
    - 按 language/implementation/verification/ecosystem 规则覆盖所有 evaluator 产出的缺口。
    - _Requirements: 5.3, 5.4, 9.8, 12.1–12.3_
  - [x] 9.2 实现 gap priority、parallel roadmap 与 gate frontier
    - 使用 `(dependencyCriticality, safetyImpact, claimRisk, targetUnblockValue, stableId)` 词典序排序，禁止异质值求和。
    - 生成 pre-kernel 与 post-boot 独立 gate 路线，标注可并行 workstream 和显式 join，保持 observed facts 与 recommendations 分离。
    - _Requirements: 12.4–12.7, 14.7, 15.7_
  - [x] 9.3 编写 Hypothesis Property 20 测试 [测试 · 可选]
    - **Property 20: Gap register classification and ranking are deterministic**
    - 对 gap 输入排列、label 重复和 priority ties 验证字段、唯一 primary、secondary 约束与稳定排序。
    - **Validates: Requirements 12.1, 12.2, 12.3, 12.4**

- [x] 10. 组装 canonical AssessmentModel 并实施唯一发布验证闸门 [核心实现]
  - [x] 10.1 实现 cross-reference、schema、coverage 与 fail-closed Validator
    - 检查 schema/enum/range、Evidence_Record、每域 assessment、gap primary、全部 object/path/anchor refs、六级 target、需求覆盖、DAG、trust assumptions、status/wording 和初始结论约束。
    - 输出 `REV/INV/EVD/CLM/CNF/GRF/MAT/RPT-*` findings，包含 object IDs 和 requirement refs；required object 缺失/损坏时拒绝有效报告。
    - _Requirements: 3.5, 9.6, 9.7, 13.4, 14.1–14.7_
  - [x] 10.2 实现 canonical model builder 与全产物发布事务
    - 将 revision、inventory、evidence、conflicts、targets、domains、assessments、gaps、gates、assumptions、non-claims、observed conclusions、recommendations 汇入唯一 AssessmentModel。
    - 仅 valid model 可进入 renderer；任何验证或 renderer parity 错误拒绝发布全部产物，避免留下部分“有效”报告。
    - _Requirements: 9.7, 14.1–14.7_
  - [x] 10.3 编写 Hypothesis Property 16 测试 [测试 · 可选]
    - **Property 16: Required assessment objects fail closed**
    - 从合法模型随机删除/破坏 required Evidence_Record 或 CapabilityDomain，验证失败包含对象和 requirement refs 且无部分有效输出。
    - **Validates: Requirements 9.7**

- [x] 11. 实现 JSON、machine-readable table 与 Markdown renderers [核心实现]
  - [x] 11.1 实现稳定 `assessment.json` renderer 与 JSON Schema
    - 对 canonical model 做 deterministic serialization，输出完整 reference graph、validation state 和 schema version；禁止 renderer 引入模型外事实。
    - _Requirements: 14.1, 14.3, 14.4, 14.7_
  - [x] 11.2 实现 capability matrix 与 gap-register table renderer
    - 每个 domain 恰一行，包含 score/confidence/status/evidence refs/next gate/target；每个 gap 恰一行且包含 Requirement 12.3 字段。
    - 产出稳定 machine-readable CSV/JSON table，并支持与 canonical model 双向 ID/reference parity 校验。
    - _Requirements: 3.3, 12.3, 14.2, 14.3_
  - [x] 11.3 实现叙事 Markdown renderer
    - 生成 executive conclusion、revision、inventory、baseline、target、rubric、matrix、gap register、Mermaid DAG、prioritized roadmap、conflicts、assumptions、non-claims 与 unvalidated execution 清单。
    - 每项 material conclusion 引用 repo-relative path 与最小稳定 anchor/case/gate ID，明确 capability 非加和/非进度/非工期，并分隔 observed facts/recommendations。
    - _Requirements: 3.7, 13.7, 14.1, 14.4–14.7_
  - [x] 11.4 编写 Hypothesis Property 22 测试 [测试 · 可选]
    - **Property 22: Structured and narrative outputs are lossless projections**
    - 生成合法 canonical models，验证 domain/gap 行一一对应、material anchors、unexecuted disclosure、facts/recommendations 分区和跨格式 parity。
    - **Validates: Requirements 14.2, 14.3, 14.4, 14.5, 14.6, 14.7**
  - [x] 11.5 编写 renderer golden 与 fail-closed unit tests [测试 · 可选]
    - 固定 executive conclusion、所有必需章节、T0–T5/rubric、Mermaid、stable anchors、non-additive 声明、Unknown/冲突和 invalid-model 拒绝路径。
    - _Requirements: 2.1–2.8, 3.7, 13.7, 14.1–14.7, 15.1–15.7_

- [x] 12. 连接端到端只读评估流水线 [核心实现]
  - [x] 12.1 实现 CLI orchestration 与 deterministic pipeline
    - 按 Binder → Inventory → Collector/Normalizer → Claim Guard → Evaluators → Maturity → Validator → Renderer 顺序连接模块，支持 `--repo-root`、`--output-dir`、`--dry-run` 与明确 execution policy。
    - 默认只读、无网络、无命令；对仓库漂移、validation failure 或 rendering parity failure 返回非零且不发布结果。
    - _Requirements: 1.1–1.6, 9.6, 9.7, 14.1–14.7_
  - [x] 12.2 实现 assessment artifact manifest 与原子发布
    - 在输出目录生成 `assessment.json`、`assessment.md`、capability matrix、gap register 及 artifact manifest，绑定 revision fingerprint 与每个文件 digest。
    - 先在私有 staging 完成全量验证，再一次性发布；失败清理 staging 且不覆盖上一份有效 assessment。
    - _Requirements: 1.1, 9.7, 14.1–14.7_
  - [x] 12.3 编写端到端 fail-closed integration tests [测试 · 可选]
    - 覆盖合法 fixture、invalid score、missing record、unknown dependency、cycle、漏 trust assumption、fingerprint drift、renderer mismatch、无网络/无外部命令默认策略与原子发布。
    - _Requirements: 3.5, 9.6, 9.7, 14.1–14.7_

- [x] 13. Checkpoint - Ensure all tests pass
  - Ensure all tests pass, ask the user if questions arise.

- [x] 14. 对 Nebula 仓库执行证据采集与 baseline 组装 [核心实现]
  - [x] 14.1 运行只读 inventory dry run 并固化 repository evidence manifest
    - 对当前绑定 revision 采集 Requirement 1.3 全部类别，记录 current worktree 与 release/commit origin、stable anchors、inspected/validated/not-run/unavailable 状态。
    - 解析 UniverseOS registry 与 `case.toml`，确认 gate/case/dependency/non-claim 引用；不修改产品代码或仓库证据文件。
    - _Requirements: 1.1–1.3, 14.4–14.6_
  - [x] 14.2 建立 curated baseline evidence 与 capability assessments
    - 以 README、AGENTS、compiler pipeline、ABI、system profile、support matrix、tiering、release notes、runtime/std/source/tests/workflows 为证据，评估 hosted C++23+host compiler、primitive object、library/package tiers 和外部依赖限制。
    - 完整运行所有 domain evaluators；无直接证据的 freestanding runtime、linked/bootable chain、kernel、drivers、UniverseOS userspace 保持 0/Unsupported 或审计后的 Unknown，不从计划推断。
    - _Requirements: 4.1–4.6, 5.1–11.6, 13.1–13.7, 15.1–15.6_
  - [x] 14.3 执行 allowlisted 快速 docs/gate contracts [可选增强]
    - 仅在用户显式开启执行证据时运行 `TST-280`、`TST-282`、`TST-329`、`TST-331` 等适用快速 gate，并绑定 command/environment/fingerprint/artifacts。
    - 未执行的 `BLD-017`–`BLD-020` 只保留 test-definition evidence；失败、超时或 unavailable 不得表述为通过。
    - _Requirements: 1.4, 7.6, 9.5, 13.1, 14.6_
  - [x] 14.4 编写真实仓库 baseline integration tests [测试 · 可选]
    - 验证 Compiler_Tooling_GA/Backend_SDK_GA/preview/experimental/planned/unsupported/unknown 边界、external host compiler limitation、scoped releases 和 initial conclusion contract。
    - _Requirements: 4.1–4.6, 7.4–7.6, 8.4, 8.6, 11.6, 15.1–15.7_

- [x] 15. 生成并验证最终 assessment report 产物 [核心实现]
  - [x] 15.1 从绑定 revision 生成最终 canonical assessment 与所有 renderings
    - 生成版本化 `assessment.json`、`assessment.md`、capability matrix、gap register 和 manifest；报告冲突、假设、non-claims、未执行证据及 dependency-ordered Hard-Gate roadmap。
    - 仅在直接新证据满足门槛时偏离初始结论；否则明确 T1 未达成、T2–T5 未达成、language/tooling ≤2、freestanding/boot/kernel/userspace=0 和 Hosted Adjacency 隔离。
    - _Requirements: 3.3, 3.7, 12.3–12.7, 14.1–14.7, 15.1–15.7_
  - [x] 15.2 对最终报告执行 schema、reference、parity 与 claim validation
    - Schema-validate JSON，验证 capability/gap 行数与 IDs、Markdown 引用、artifact digests、revision/fingerprint、status/wording、trust assumptions 和全部 15 项需求覆盖。
    - 任一 finding 时拒绝标记报告有效并返回 requirement/object refs；通过时记录 validator 结果，不把 prerequisite gate 扩写为 OS claim。
    - _Requirements: 9.6, 9.7, 13.7, 14.1–14.7_
  - [x] 15.3 编写最终产物 smoke/parity regression test [测试 · 可选]
    - 自动加载版本化产物，验证 canonical model、JSON、tables、Markdown 与 manifest 的双向引用和初始结论/非声明边界。
    - _Requirements: 13.6, 13.7, 14.1–14.7, 15.1–15.7_

- [x] 16. Final checkpoint - Ensure all tests pass
  - Ensure all tests pass, ask the user if questions arise.

## Notes

- `[核心实现]` 是默认必须执行的分析工具、证据采集和报告生成工作；不得修改 Nebula 产品代码。
- 标有 `*` 的子任务均为可跳过项：`[测试 · 可选]` 覆盖 Hypothesis、unit、integration、golden/smoke；`[可选增强]` 仅增加 allowlisted 本地执行证据，不改变默认只读/禁命令策略。
- 每条 Correctness Property 都有独立 Hypothesis 子任务；默认至少 `max_examples=100`，复杂 DAG 测试至少 300，并保留最小反例与 seed。
- 测试是对应实现任务的子任务而非独立产品工作；capability score 始终为非加和序数，绝不转换为百分比、平均值或工期。
- 所有产物必须由同一个 canonical AssessmentModel 无损投影；任何 validation failure 都 fail closed，不发布部分有效报告。

## Task Dependency Graph

```json
{
  "waves": [
    { "id": 0, "tasks": ["1.1"] },
    { "id": 1, "tasks": ["1.2", "1.3"] },
    { "id": 2, "tasks": ["2.1", "3.1"] },
    { "id": 3, "tasks": ["2.2", "3.2"] },
    { "id": 4, "tasks": ["2.3", "2.4", "3.3"] },
    { "id": 5, "tasks": ["3.4", "4.1"] },
    { "id": 6, "tasks": ["4.2", "4.5", "4.7"] },
    { "id": 7, "tasks": ["4.3", "4.6"] },
    { "id": 8, "tasks": ["4.4", "4.9"] },
    { "id": 9, "tasks": ["4.8", "5.1", "5.2", "6.1", "6.2", "6.3", "7.1", "7.2", "7.3", "7.4"] },
    { "id": 10, "tasks": ["5.3", "5.4", "6.4", "6.5", "6.6", "7.5", "7.6", "7.8", "7.9"] },
    { "id": 11, "tasks": ["7.7", "8.1"] },
    { "id": 12, "tasks": ["8.2"] },
    { "id": 13, "tasks": ["8.3", "8.6"] },
    { "id": 14, "tasks": ["8.4", "8.5", "8.7", "8.8", "9.1"] },
    { "id": 15, "tasks": ["9.2"] },
    { "id": 16, "tasks": ["9.3", "10.1"] },
    { "id": 17, "tasks": ["10.2"] },
    { "id": 18, "tasks": ["10.3", "11.1", "11.2", "11.3"] },
    { "id": 19, "tasks": ["11.4", "11.5", "12.1"] },
    { "id": 20, "tasks": ["12.2"] },
    { "id": 21, "tasks": ["12.3", "14.1"] },
    { "id": 22, "tasks": ["14.2"] },
    { "id": 23, "tasks": ["14.3", "14.4"] },
    { "id": 24, "tasks": ["15.1"] },
    { "id": 25, "tasks": ["15.2"] },
    { "id": 26, "tasks": ["15.3"] }
  ]
}
```
