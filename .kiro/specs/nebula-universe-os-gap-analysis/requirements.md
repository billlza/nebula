# Requirements Document

## Introduction

本特性定义一份基于仓库证据的 Nebula 语言与 “Universe OS” 愿景差距评估。评估对象是当前工作树可证明的能力，而不是愿景叙述、未来 RFC 或相邻的托管应用能力。交付物需要回答三个问题：Nebula 当前能做什么、成为可独立运行的 Universe OS 需要什么、每个能力域距离目标还缺哪些语言、实现、验证与生态工作。

本阶段只规定分析方法和交付内容，不实现语言、编译器、运行时、内核、驱动或产品代码。评估采用逐能力域的序数成熟度、证据置信度、硬门禁和依赖路径；评估不把不同能力域相加为单一完成百分比。

## Repository Evidence Baseline

以下基线是需求形成时对当前工作树的观察，后续评估必须重新绑定到明确修订或工作树指纹：

- `VERSION` 与 `RELEASE_NOTES_v1.0.0.md` 将 1.0.0 定义为编译器、CLI、项目/包工作流、bundled `std`、运行时头文件和发布资产的 GA 基线；Linux backend SDK 只有 `nebula-service` 与 `nebula-observe` 属于 GA，其余大多为 installed-preview 或 repo-local preview。
- `README.md`、`AGENTS.md`、`spec/compiler_pipeline.md` 与源码目录共同证明生产编译链为 `Source -> AST -> Typed AST -> NIR/CFG -> analyses -> C++23 -> host compiler`；不存在已支持的独立原生后端。
- `frontend/`、`nir/`、`passes/` 与语言规范证明已实现基础类型、struct/enum、单态化泛型、Result/`?`、async 基础、控制流、显式 region、Rep × Owner 推断、unsafe 边界以及保守的 borrow/exclusivity 辅助。
- 语言规范与 trait/protocol RFC 明确记录 traits/protocols、受约束泛型、closures、lifetime parameters、完整集合层级、深层模式、原始指针/volatile/atomics/intrinsics/inline assembly 等缺口。
- `runtime/` 与 `std/` 证明当前运行时和库以宿主 OS、C++ 标准库及运行时头文件为基础；async 是单线程协作式托管运行时，不是内核调度器。
- `docs/system_profile.md` 与 UniverseOS gate registry 证明 system/no-std CLI 拒绝 hosted `std` 并强制 strict-region；该能力是实验性契约门槛，不是 freestanding runtime。
- `codegen/freestanding_cpp_emitter.*`、`cli/freestanding_*`、`boot/` 与 `BLD-017` 至 `BLD-020` 证明存在 macOS/Linux 主机上的实验性 `Int/Bool/Void` ELF64 relocatable object 路径；该路径仍经生成 C++ 与显式 clang 工具链，不包含链接、启动、运行时或内核能力。
- `docs/universeos/readiness_assessment.md` 将语言/工具链最高行评为 2/5，安全、托管 ABI、生态与托管相邻资产评为 1/5，而独立后端、freestanding runtime、可启动链、内核子系统及 UniverseOS userspace 为 0/5；没有能力行达到候选级 3。
- `docs/universeos/kernel_boundary.md` 明确不存在 kernel entry/runtime、interrupt、MMU、scheduler、syscall ABI、driver model、process isolation、storage stack 或 networking stack。
- `tests/README.md`、`CMakeLists.txt` 与 CI workflow 证明存在四平台严格构建、C++ tests、大型 contract suite、sanitizer lane 与发布资产验证；这些证据主要覆盖托管编译器/工具链和窄实验门槛，不能替代 OS 子系统验证。
- 当前工作树包含大量预先存在的未提交修改；发布标签证据、源码冻结指纹证据与当前工作树证据必须分别标注，不能相互替代。

## Glossary

- **Nebula_Repository**: 仓库根目录及其中受版本控制的源码、规范、文档、测试、构建和发布配置。
- **Assessment_Revision**: 一次评估绑定的提交、分支、版本、工作树状态及可选源码指纹。
- **Universe_OS**: 由 Nebula 可验证地拥有的独立系统平台，包括启动链、freestanding runtime、内核资源管理、硬件/驱动抽象、隔离用户态、系统服务、应用生命周期、安全、可观测性、更新与恢复。
- **Gap_Analysis**: 生成当前能力基线、目标能力模型、差距分类、成熟度矩阵和依赖路线的分析系统。
- **Evidence_Collector**: 从 Nebula_Repository 采集、去重并标注证据的 Gap_Analysis 组件。
- **Maturity_Assessor**: 对能力域分配成熟度、置信度和下一门禁的 Gap_Analysis 组件。
- **Claim_Guard**: 防止将愿景、计划、预览或相邻能力表述为已实现事实的 Gap_Analysis 组件。
- **Assessment_Report**: Gap_Analysis 的版本化、可审阅交付物。
- **Evidence_Record**: 含 claim、status、source path、location、revision、evidence kind、confidence 和 limitations 的证据条目。
- **Evidence_Status**: `Compiler_Tooling_GA`、`Backend_SDK_GA`、`Installed_Preview`、`Repo_Preview`、`Experimental`、`Planned`、`Unsupported` 或 `Unknown` 之一。
- **Capability_Domain**: 被独立评估的技术或产品能力域。
- **Gap_Category**: `Language_Gap`、`Implementation_Gap`、`Verification_Gap` 或 `Ecosystem_Gap` 之一。
- **Language_Gap**: 缺少规范化语言语义、类型规则、内存规则、并发规则或兼容性契约。
- **Implementation_Gap**: 规范或目标已存在，但编译器、运行时、库、平台或 OS 组件尚未实现。
- **Verification_Gap**: 实现或声明存在，但缺少目标平台、负向路径、互操作、安全、性能或发布级证据。
- **Ecosystem_Gap**: 缺少稳定库、包分发、工具集成、运维流程、社区采用或持续兼容性证据。
- **Hosted_Adjacency**: 在现有宿主 OS 上运行的工具、服务、控制平面和 thin-host app core 能力。
- **OS_Substrate**: freestanding runtime、系统 ABI、链接/启动链、内核、驱动和隔离用户态组成的基础层。
- **Target_Level**: `T0_Hosted_Adjacency`、`T1_Independent_Language_Platform`、`T2_Freestanding_Substrate`、`T3_Boot_And_Kernel_Foundation`、`T4_Isolated_Userspace_Platform` 或 `T5_Operable_Universe_OS`。
- **Maturity_Score**: 0 至 5 的序数值；0 无实现证据，1 窄实验，2 可重复仓库内实现，3 跨支持主机的候选契约，4 受支持生产能力，5 成熟独立生态能力。
- **Confidence_Rating**: `High`、`Medium` 或 `Low`，表示证据对结论的支撑强度。
- **Hard_Gate**: 后续能力开始或升级成熟度前必须满足的不可替代依赖。
- **Current_Worktree**: 包含已跟踪和未跟踪本地修改的当前文件状态。

## Requirements

### Requirement 1: Bind the Assessment to Repository Evidence

**User Story:** As a maintainer, I want every capability claim bound to repository evidence, so that the assessment can be audited and reproduced.

#### Acceptance Criteria

1. THE Evidence_Collector SHALL record the Assessment_Revision with commit identifier, version, branch, worktree cleanliness, and assessment timestamp.
2. WHEN the Current_Worktree contains uncommitted changes, THE Evidence_Collector SHALL label evidence from the Current_Worktree separately from tagged-release evidence.
3. THE Evidence_Collector SHALL inspect source code, README, ROADMAP, changelog, all release notes, specifications, RFCs, tests, build configuration, CI workflows, release workflows, standard-library modules, official packages, examples, and UniverseOS gate documents.
4. THE Evidence_Collector SHALL store each accepted claim as an Evidence_Record with all glossary-defined fields.
5. IF two Evidence_Records make incompatible claims, THEN THE Evidence_Collector SHALL report an evidence conflict with both source locations and no inferred winner.
6. IF a capability has only prose plans or RFC text, THEN THE Evidence_Collector SHALL assign Evidence_Status `Planned` rather than an implemented status.

### Requirement 2: Define the Universe OS Target and Scope

**User Story:** As a product owner, I want an operational Universe OS definition, so that distance is measured against a stable target rather than a slogan.

#### Acceptance Criteria

1. THE Gap_Analysis SHALL define Universe_OS as the complete Nebula-owned capability set in the Glossary.
2. THE Gap_Analysis SHALL divide the target into the six ordered Target_Level values defined in the Glossary.
3. THE Gap_Analysis SHALL assign Hosted_Adjacency capabilities to `T0_Hosted_Adjacency` without counting Hosted_Adjacency as OS_Substrate completion.
4. THE Gap_Analysis SHALL define `T1_Independent_Language_Platform` as language, compiler, package, debugger, compatibility, and reproducible-backend independence sufficient for sustained non-host-language development.
5. THE Gap_Analysis SHALL define `T2_Freestanding_Substrate` as system ABI, freestanding core/runtime, target model, linker inputs, panic/allocation policy, and hardware-safe primitives without hosted runtime dependency.
6. THE Gap_Analysis SHALL define `T3_Boot_And_Kernel_Foundation` as reproducible boot, memory management, interrupts, scheduling, syscalls, capabilities, drivers, storage, and networking foundations.
7. THE Gap_Analysis SHALL define `T4_Isolated_Userspace_Platform` as process isolation, user runtime, system services, IPC, install/update, recovery, application APIs, and platform shell foundations.
8. THE Gap_Analysis SHALL define `T5_Operable_Universe_OS` as supported hardware, security operations, observability, packaging, upgrades, recovery, application distribution, compatibility, and sustained ecosystem evidence.

### Requirement 3: Measure Maturity and Remaining Distance

**User Story:** As a decision maker, I want a transparent maturity method, so that “how far” has a reproducible meaning.

#### Acceptance Criteria

1. THE Maturity_Assessor SHALL assign one Maturity_Score, one Confidence_Rating, current evidence, limitations, next Hard_Gate, and blocking dependencies to every Capability_Domain.
2. THE Maturity_Assessor SHALL apply the six Maturity_Score meanings defined in the Glossary without converting ordinal values into percentages.
3. THE Maturity_Assessor SHALL present remaining distance as a capability matrix plus a dependency-ordered Hard_Gate path.
4. WHEN a Capability_Domain depends on a strictly lower-maturity Hard_Gate, THE Maturity_Assessor SHALL cap the Capability_Domain at the maturity supported by that dependency.
5. IF any dependency has a missing or out-of-range Maturity_Score, THEN THE Gap_Analysis SHALL fail assessment validation before calculating dependent maturity.
6. IF no implementation evidence exists for a Capability_Domain, THEN THE Maturity_Assessor SHALL assign Maturity_Score 0 even when adjacent plans or prerequisites exist.
7. THE Assessment_Report SHALL state that capability scores are non-additive and do not represent schedule estimates.

### Requirement 4: Establish the Current Capability Baseline

**User Story:** As a language adopter, I want a current capability baseline, so that I can distinguish usable Nebula features from experiments and plans.

#### Acceptance Criteria

1. THE Gap_Analysis SHALL classify each current capability with exactly one Evidence_Status.
2. THE Gap_Analysis SHALL distinguish Compiler_Tooling_GA, Backend_SDK_GA, Installed_Preview packages, Repo_Preview packages, Experimental gates, Planned work, Unsupported capabilities, and Unknown capabilities.
3. THE Gap_Analysis SHALL classify both the hosted C++23 backend and the required host compiler with Evidence_Status `Compiler_Tooling_GA` while recording the host compiler as an external production dependency.
4. THE Gap_Analysis SHALL identify the primitive freestanding object path as an Experimental clang-backed relocatable-object gate.
5. THE Gap_Analysis SHALL identify absent freestanding runtime, linked boot artifact, kernel subsystems, drivers, and Universe_OS userspace as Unsupported current capabilities.
6. WHEN an example demonstrates a hosted workflow, THE Gap_Analysis SHALL classify the example as Hosted_Adjacency evidence.

### Requirement 5: Analyze Language Semantics and Type-System Gaps

**User Story:** As a language designer, I want semantic and type-system gaps enumerated, so that system-level implementation does not depend on unspecified behavior.

#### Acceptance Criteria

1. THE Gap_Analysis SHALL assess lexical rules, control flow, functions, methods, modules, visibility, generics, traits or protocols, closures, patterns, error effects, reflection, macros, and metaprogramming.
2. THE Gap_Analysis SHALL assess primitive widths, pointers, references, slices, arrays, collections, nullable values, aggregates, enums, callable types, variance, lifetimes, constrained generics, and dynamic dispatch.
3. WHEN a language feature is documented, THE Gap_Analysis SHALL create a Language_Gap entry with the authoritative source path and any direct implementation evidence.
4. WHEN a language feature has parser or typechecker support but no compatibility policy, THE Gap_Analysis SHALL create a Verification_Gap for semantic stability.
5. THE Gap_Analysis SHALL identify low-level semantic prerequisites for target layout, initialization, destruction, aliasing, and system-call boundaries.

### Requirement 6: Analyze Memory, Ownership, Concurrency, and Safety Gaps

**User Story:** As a systems engineer, I want memory and concurrency contracts evaluated, so that kernel and runtime code has explicit safety boundaries.

#### Acceptance Criteria

1. THE Gap_Analysis SHALL assess stack, region, heap, static storage, promotion, initialization, destruction, allocator failure, raw memory, and resource-lifetime semantics.
2. THE Gap_Analysis SHALL distinguish current Rep × Owner inference and borrow assistance from a normative move, borrow, lifetime, and aliasing model.
3. THE Gap_Analysis SHALL assess threads, tasks, actors, structured concurrency, interruption, atomics, memory ordering, data-race prevention, interrupt safety, and synchronization primitives.
4. WHEN current async behavior depends on the hosted cooperative runtime, THE Gap_Analysis SHALL classify scheduler-independent concurrency as an Implementation_Gap.
5. THE Gap_Analysis SHALL assess unsafe blocks, unsafe functions, FFI boundaries, raw pointers, volatile access, MMIO, intrinsics, inline assembly, and privilege transitions.
6. IF a safety guarantee excludes opaque, dynamic, FFI, or unsafe boundaries, THEN THE Claim_Guard SHALL include the exclusion in the related Evidence_Record.

### Requirement 7: Analyze FFI, ABI, Compilation, Linking, and Backend Gaps

**User Story:** As a compiler engineer, I want toolchain and ABI gaps separated, so that a hosted transpiler milestone is not mistaken for an OS toolchain.

#### Acceptance Criteria

1. THE Gap_Analysis SHALL assess imported extern contracts, exported C ABI types, calling conventions, symbol rules, aggregate layout, enum layout, alignment, versioning, and cross-language fixtures.
2. THE Gap_Analysis SHALL distinguish hosted C ABI evidence from freestanding compiler ABI, runtime ABI, boot ABI, syscall ABI, driver ABI, and package ABI.
3. THE Gap_Analysis SHALL assess frontend completeness, NIR/CFG, analyses, optimization, incremental compilation, debug information, native code generation, assembler integration, linker integration, and bootstrap reproducibility.
4. WHILE the production compiler dependency inventory is incomplete, THE Maturity_Assessor SHALL classify `T1_Independent_Language_Platform` as unachieved.
5. WHEN generated C++ or external clang remains a production dependency without an accepted independent bootstrap path, THE Maturity_Assessor SHALL classify `T1_Independent_Language_Platform` as unachieved.
6. WHEN the primitive object gate passes, THE Claim_Guard SHALL describe the evidence as ELF relocatable-object emission rather than direct backend, linked image, runtime, or boot evidence.
7. THE Gap_Analysis SHALL map target specification, linker scripts, relocation support, startup objects, deterministic linking, boot-media assembly, and boot execution to separate Hard_Gate entries.

### Requirement 8: Analyze Runtime, Standard Library, and Package-System Gaps

**User Story:** As a platform developer, I want runtime and library layers assessed, so that hosted services are not confused with freestanding capabilities.

#### Acceptance Criteria

1. THE Gap_Analysis SHALL assess hosted runtime services, freestanding startup, static initialization, panic behavior, allocation hooks, termination, unwinding, exception policy, and runtime ABI.
2. THE Gap_Analysis SHALL assess current `std` modules by API coverage, host dependency, allocation behavior, platform coverage, stability, and verification.
3. THE Gap_Analysis SHALL assess future `core`, hosted `std`, and future `system` layering as separate Capability_Domain entries.
4. IF either resolver support or implementation support is absent for a `core::` or `system::` import, THEN THE Gap_Analysis SHALL classify the import as Planned.
5. THE Gap_Analysis SHALL assess package manifests, workspaces, locks, local registry, hosted registry helpers, git dependencies, native dependencies, reproducibility, signing, vulnerability response, compatibility, and offline operation.
6. WHEN a package is Installed_Preview or Repo_Preview, THE Claim_Guard SHALL preserve that Evidence_Status in summaries and target-level calculations.

### Requirement 9: Analyze Debugging, Observability, Security, and Reliability Gaps

**User Story:** As an operator, I want diagnosability and security maturity evaluated, so that development tooling is not treated as production OS operability.

#### Acceptance Criteria

1. THE Gap_Analysis SHALL assess source diagnostics, LSP, formatter, explain data, debugger integration, stack traces, symbols, crash dumps, profiling, tracing, metrics, logs, and kernel/user correlation.
2. THE Gap_Analysis SHALL distinguish compiler diagnostics and hosted service observability from boot, kernel, driver, and userspace observability.
3. THE Gap_Analysis SHALL assess compiler supply chain, artifact integrity, package trust, unsafe audit, capability security, process isolation, privilege separation, secure boot, secret lifecycle, crypto lifecycle, update rollback, and incident response.
4. THE Gap_Analysis SHALL assess bounded execution, containment, transactionality, crash consistency, power-loss durability, deterministic rebuilds, and recovery behavior.
5. IF evidence assumes trusted tools, cooperative descendants, caller-controlled directories, or host security services, THEN THE Claim_Guard SHALL record each trust assumption as a limitation.
6. IF the Claim_Guard detects an unrecorded trust assumption, THEN THE Gap_Analysis SHALL fail assessment validation immediately with the affected Evidence_Record references.
7. IF any required Evidence_Record or Capability_Domain assessment is missing or invalid, THEN THE Gap_Analysis SHALL fail assessment validation with the affected requirement references.
8. WHEN security-sensitive packages remain preview, THE Gap_Analysis SHALL create Ecosystem_Gap entries for maintenance, certification, deployment, and vulnerability-response maturity.

### Requirement 10: Analyze Hardware, Drivers, Kernel, and Userspace Gaps

**User Story:** As an OS architect, I want every OS substrate layer evaluated independently, so that missing foundations remain visible.

#### Acceptance Criteria

1. THE Gap_Analysis SHALL assess target discovery, firmware, boot protocol, early console, timers, interrupts, traps, CPU state, MMU, page tables, physical memory, virtual memory, DMA, IOMMU, and power management.
2. THE Gap_Analysis SHALL assess device discovery, bus abstractions, driver lifecycle, driver isolation, interrupt routing, DMA safety, storage devices, network devices, input, display, audio, and hardware qualification.
3. THE Gap_Analysis SHALL assess kernel entry, panic, synchronization, scheduler, context switching, syscall dispatch, capability enforcement, IPC, process model, thread model, address-space isolation, and resource accounting.
4. THE Gap_Analysis SHALL assess filesystems, storage stack, network stack, service manager, identity, policy, time, configuration, package installation, updates, rollback, backup, and recovery.
5. THE Gap_Analysis SHALL assess user runtime, command environment, application model, GUI/accessibility shell, sandboxing, application distribution, and developer SDK.
6. IF a listed OS_Substrate capability has no implementation evidence, THEN THE Maturity_Assessor SHALL assign Maturity_Score 0 to that Capability_Domain.
7. WHEN a QEMU serial hello eventually exists, THE Claim_Guard SHALL retain separate gaps for drivers, interrupts, MMU, scheduler, syscalls, isolation, storage, networking, userspace, and operations.

### Requirement 11: Analyze Application Platform, Ecosystem, and Release Engineering

**User Story:** As a product planner, I want application and ecosystem maturity assessed, so that thin-host and backend assets receive accurate product positioning.

#### Acceptance Criteria

1. THE Gap_Analysis SHALL assess CLI tools, backend services, control-plane applications, embedded data, authentication, jobs, TLS, crypto, UI semantics, thin-host bridges, and native host adapters.
2. THE Gap_Analysis SHALL partition application evidence into Nebula-owned, host-owned, and operations-owned responsibilities.
3. THE Gap_Analysis SHALL assess renderer, widget, layout, accessibility, device integration, signing, notarization, install, update, app distribution, and crash-reporting ownership.
4. THE Gap_Analysis SHALL assess package breadth, documentation, starter projects, compatibility governance, contributor workflow, third-party adoption, security maintenance, and long-term support.
5. THE Gap_Analysis SHALL assess strict build matrices, contract suites, sanitizer lanes, release smoke, SBOM, provenance, attestations, installers, rollback, and cross-platform qualification.
6. WHEN release evidence is scoped to compiler/tooling or Linux backend SDK scope, THE Claim_Guard SHALL block every proposed OS_Substrate maturity increase derived from that evidence.

### Requirement 12: Classify and Prioritize Gaps

**User Story:** As a roadmap owner, I want gaps classified and dependency-ordered, so that implementation starts with prerequisites rather than visible demos.

#### Acceptance Criteria

1. THE Gap_Analysis SHALL assign every identified gap exactly one primary Gap_Category.
2. THE Gap_Analysis SHALL allow secondary Gap_Category labels while preserving exactly one primary Gap_Category.
3. THE Gap_Analysis SHALL record affected Capability_Domain, current Evidence_Status, target Target_Level, severity, dependency, acceptance evidence, and recommended owner area for every gap.
4. THE Gap_Analysis SHALL rank gaps by dependency criticality, safety impact, claim risk, and target-level unblock value.
5. THE Gap_Analysis SHALL identify the low-level language soundness, freestanding ABI, independent backend/bootstrap, freestanding core/runtime, complete boot toolchain, linked ELF, boot media, and QEMU execution sequence as pre-kernel Hard_Gate work.
6. THE Gap_Analysis SHALL place memory management, interrupts, scheduling, syscalls/capabilities, drivers/DMA, storage, networking, process isolation, userspace, update/recovery, and product shell behind separate post-boot gates.
7. WHEN two workstreams can progress independently, THE Gap_Analysis SHALL mark the workstreams as parallel branches with an explicit join gate.

### Requirement 13: Control Claims, Uncertainty, and Conflicts

**User Story:** As a reviewer, I want unsupported claims blocked, so that the assessment remains trustworthy as the repository evolves.

#### Acceptance Criteria

1. THE Claim_Guard SHALL use present-tense implementation claims only for Evidence_Records with implementation evidence from the Assessment_Revision.
2. THE Claim_Guard SHALL label roadmap items, RFC syntax, proposed tests, and planned gate names as future work.
3. THE Claim_Guard SHALL label examples and documentation-only contracts according to the strongest directly supported Evidence_Status.
4. IF source, test, and documentation evidence disagree, THEN THE Claim_Guard SHALL set Confidence_Rating `Low` and create an evidence-conflict finding.
5. IF a claim lacks a verifiable source path, THEN THE Claim_Guard SHALL classify the claim as `Unknown`.
6. THE Claim_Guard SHALL preserve explicit non-claims for kernel, driver, interrupt, MMU, scheduler, syscall ABI, freestanding runtime, bootability, and backend independence until corresponding accepted gates exist.
7. THE Assessment_Report SHALL state that a passing prerequisite gate proves only the named gate scope.

### Requirement 14: Produce Traceable Assessment Outputs

**User Story:** As a maintainer, I want structured and narrative outputs, so that humans and automation can review the same assessment.

#### Acceptance Criteria

1. THE Assessment_Report SHALL contain an executive conclusion, Assessment_Revision, source inventory, current baseline, target model, maturity rubric, capability matrix, gap register, dependency graph, prioritized roadmap, evidence conflicts, assumptions, and non-claims.
2. THE Assessment_Report SHALL include a machine-readable table with one row per Capability_Domain and columns for Maturity_Score, Confidence_Rating, Evidence_Status, Evidence_Record references, next Hard_Gate, and target Target_Level.
3. THE Assessment_Report SHALL include a machine-readable gap register with the fields required by Requirement 12.3.
4. THE Assessment_Report SHALL cite repository-relative paths and stable test or gate identifiers for each material conclusion.
5. WHEN a source location has no stable line reference, THE Assessment_Report SHALL cite the smallest stable heading, symbol, case ID, or manifest key.
6. THE Assessment_Report SHALL identify evidence that was inspected but could not be validated by execution.
7. THE Assessment_Report SHALL separate observed current facts from recommendations.

### Requirement 15: State the Initial Evidence-Backed Distance Conclusion

**User Story:** As a stakeholder, I want an honest initial conclusion, so that planning begins from the repository's demonstrated state.

#### Acceptance Criteria

1. THE Gap_Analysis SHALL characterize the current repository as a promising hosted language, compiler/tooling, backend-service, and thin-host-app foundation.
2. THE Gap_Analysis SHALL characterize the current repository as materially short of `T1_Independent_Language_Platform` because production compilation remains dependent on generated C++ and external host tooling.
3. THE Gap_Analysis SHALL characterize `T2_Freestanding_Substrate`, `T3_Boot_And_Kernel_Foundation`, `T4_Isolated_Userspace_Platform`, and `T5_Operable_Universe_OS` as unachieved target levels under current evidence.
4. THE Gap_Analysis SHALL report the current strongest repository-local language/tooling capabilities at no higher than Maturity_Score 2 unless newer cross-host candidate evidence is discovered.
5. THE Gap_Analysis SHALL report freestanding runtime, linked/bootable chain, kernel subsystems, and Universe_OS userspace at Maturity_Score 0 unless direct implementation evidence is discovered.
6. THE Gap_Analysis SHALL state that Hosted_Adjacency assets reduce future application-porting effort while remaining separate from every OS_Substrate critical-path dependency and Hard_Gate.
7. THE Gap_Analysis SHALL present the shortest evidence-backed path as language soundness to system ABI, independent backend and freestanding runtime, complete boot toolchain to linked/bootable proof, then separately gated kernel and userspace subsystems.