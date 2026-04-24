#pragma once

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "frontend/diagnostic.hpp"
#include "frontend/runtime_profile.hpp"
#include "frontend/typed_ast.hpp"
#include "nir/lower.hpp"
#include "passes/async_explain.hpp"
#include "passes/rep_owner_infer.hpp"

namespace fs = std::filesystem;

enum class BuildMode : std::uint8_t { Debug, Release };
enum class AnalysisProfile : std::uint8_t { Auto, Fast, Deep };
enum class AnalysisTier : std::uint8_t { Auto, Basic, Smart, Deep };
enum class DiagFormat : std::uint8_t { Text, Json };
enum class DiagView : std::uint8_t { Auto, Raw, Grouped };
enum class WarnPolicy : std::uint8_t { Strict, Balanced, Lenient };
enum class GateProfile : std::uint8_t { Strict, Balanced, Lenient };
enum class RootCauseV2Mode : std::uint8_t { Auto, On, Off };
enum class RunGate : std::uint8_t { High, All, None };
enum class PreflightMode : std::uint8_t { Fast, Off };
enum class CrossStageReuseMode : std::uint8_t { Off, Safe };
enum class DiskCacheMode : std::uint8_t { Off, On };
enum class CompileFlavor : std::uint8_t { Normal, Test, Bench };
enum class CacheReportFormat : std::uint8_t { Text, Json };
enum class BuildArtifactKind : std::uint8_t { Executable, StaticLib, SharedLib };
enum class NativeSourceLanguage : std::uint8_t { C, Cxx, Asm };

using nebula::frontend::PanicPolicy;
using nebula::frontend::RuntimeProfile;
using nebula::frontend::effective_no_std;
using nebula::frontend::effective_strict_region;
using nebula::frontend::is_system_profile;
using nebula::frontend::panic_policy_requires_host_unwind;

struct NativeSourceSpec {
  fs::path path;
  NativeSourceLanguage language = NativeSourceLanguage::Cxx;
  std::vector<fs::path> include_dirs;
  std::vector<std::string> defines;
  std::vector<std::string> arch;
  std::vector<std::string> cpu_features;
};

struct NativeGeneratedHeaderConfig {
  fs::path out;
  fs::path template_path;
  std::vector<std::pair<std::string, std::string>> values;
};

struct NativeBuildConfig {
  std::vector<fs::path> c_sources;
  std::vector<fs::path> cxx_sources;
  std::vector<fs::path> include_dirs;
  std::vector<std::string> defines;
  std::vector<NativeSourceSpec> sources;
  std::vector<NativeGeneratedHeaderConfig> generated_headers;

  [[nodiscard]] bool empty() const {
    return c_sources.empty() && cxx_sources.empty() && include_dirs.empty() && defines.empty() &&
           sources.empty() && generated_headers.empty();
  }
};

struct NativeSourceInput {
  fs::path path;
  NativeSourceLanguage language = NativeSourceLanguage::Cxx;
  std::vector<fs::path> include_dirs;
  std::vector<std::string> defines;
  std::vector<std::string> extra_flags;
  std::string package_name;
};

struct NativeBuildInputs {
  std::vector<NativeSourceInput> sources;

  [[nodiscard]] bool empty() const { return sources.empty(); }
};

inline constexpr std::uint32_t kWarnClassApi = 1u << 0;
inline constexpr std::uint32_t kWarnClassPerformance = 1u << 1;
inline constexpr std::uint32_t kWarnClassBestPractice = 1u << 2;
inline constexpr std::uint32_t kWarnClassSafety = 1u << 3;
inline constexpr std::uint32_t kWarnClassGeneral = 1u << 4;
inline constexpr std::uint32_t kWarnClassAll =
    kWarnClassApi | kWarnClassPerformance | kWarnClassBestPractice | kWarnClassSafety |
    kWarnClassGeneral;

inline constexpr std::uint32_t kGateDimApiLifecycle = 1u << 0;
inline constexpr std::uint32_t kGateDimPerfRuntime = 1u << 1;
inline constexpr std::uint32_t kGateDimBestPracticeDrift = 1u << 2;
inline constexpr std::uint32_t kGateDimSafetyContract = 1u << 3;
inline constexpr std::uint32_t kGateDimGeneral = 1u << 4;
inline constexpr std::uint32_t kGateDimAll =
    kGateDimApiLifecycle | kGateDimPerfRuntime | kGateDimBestPracticeDrift |
    kGateDimSafetyContract | kGateDimGeneral;

struct CliOptions {
  bool warnings_as_errors = false;
  bool strict_region = false;
  bool dump_ownership = false;
  bool dump_cfg_ir = false;
  bool emit_cpp = false;
  bool reuse = false;
  bool no_build = false;
  bool profile_explicit = false;
  bool analysis_tier_explicit = false;
  bool warn_policy_explicit = false;
  bool cache_report = false;
  bool disk_cache_prune = false;
  bool no_std = false;
  bool runtime_profile_explicit = false;
  bool target_explicit = false;
  bool panic_policy_explicit = false;

  BuildMode mode = BuildMode::Debug;
  AnalysisProfile analysis_profile = AnalysisProfile::Auto;
  AnalysisTier analysis_tier = AnalysisTier::Auto;
  DiagFormat diag_format = DiagFormat::Text;
  DiagView diag_view = DiagView::Auto;
  WarnPolicy warn_policy = WarnPolicy::Balanced;
  GateProfile gate_profile = GateProfile::Balanced;
  RootCauseV2Mode root_cause_v2 = RootCauseV2Mode::Auto;
  RunGate run_gate = RunGate::High;
  PreflightMode preflight = PreflightMode::Fast;
  CrossStageReuseMode cross_stage_reuse = CrossStageReuseMode::Off;
  DiskCacheMode disk_cache = DiskCacheMode::Off;
  CacheReportFormat cache_report_format = CacheReportFormat::Text;
  RuntimeProfile runtime_profile = RuntimeProfile::Hosted;
  PanicPolicy panic_policy = PanicPolicy::Abort;
  int diag_budget_ms = 0;
  int diag_grouping_delay_ms = 0;
  int max_root_causes = 0;
  int root_cause_top_k = 8;
  int root_cause_min_covered = 0;
  int disk_cache_ttl_sec = 3600;
  int disk_cache_max_entries = 256;
  std::uint32_t warn_class_force_on_mask = 0;
  std::uint32_t warn_class_force_off_mask = 0;
  std::uint32_t gate_dimension_force_on_mask = 0;
  std::uint32_t gate_dimension_force_off_mask = 0;

  fs::path disk_cache_dir = ".nebula-cache";
  fs::path out_dir = "generated_cpp";
  fs::path dir = "examples";
  fs::path repo_root = ".";
  fs::path self_executable;
  fs::path include_root;
  fs::path std_root;
  fs::path backend_sdk_root;
  std::string include_root_error;
  std::string std_root_error;
  std::string backend_sdk_root_error;
  std::string target = "host";
  std::optional<fs::path> out_path;
  std::vector<std::string> run_args;
  bool root_cause_v2_default_on = false;
  BuildArtifactKind artifact_kind = BuildArtifactKind::Executable;
};

struct CompilePipelineOptions {
  BuildMode mode = BuildMode::Debug;
  AnalysisProfile profile = AnalysisProfile::Fast;
  AnalysisTier analysis_tier = AnalysisTier::Basic;
  bool strict_region = false;
  bool warnings_as_errors = false;
  bool include_lint = true;
  bool allow_cross_stage_reuse = false;
  bool disk_cache_enabled = false;
  bool disk_cache_prune = false;
  bool dump_ownership = false;
  bool dump_cfg_ir = false;
  bool no_std = false;
  RuntimeProfile runtime_profile = RuntimeProfile::Hosted;
  PanicPolicy panic_policy = PanicPolicy::Abort;
  int budget_ms = 0;
  int disk_cache_ttl_sec = 3600;
  int disk_cache_max_entries = 256;
  fs::path disk_cache_dir = ".nebula-cache";
  std::string source_path;
  std::string cache_key_source;
  std::string target = "host";
  nebula::frontend::DiagnosticStage stage = nebula::frontend::DiagnosticStage::Build;
};

struct LoadedProjectInput {
  fs::path project_root;
  fs::path manifest_path;
  fs::path entry_path;
  std::string project_name;
  std::string cache_key_source;
  std::vector<nebula::frontend::SourceFile> sources;
  std::vector<fs::path> host_cxx_sources;
  NativeBuildInputs native_inputs;
};

struct CompilePipelineResult {
  bool has_error = false;
  bool has_warning = false;
  bool has_note = false;
  bool cache_hit = false;
  bool cross_stage_reused = false;
  bool has_cached_cpp = false;
  std::size_t analysis_elapsed_ms = 0;
  std::size_t fn_count = 0;
  std::size_t cfg_nodes = 0;
  std::string cached_cpp;
  std::vector<nebula::frontend::Diagnostic> diags;
  std::shared_ptr<std::vector<nebula::frontend::TProgram>> typed_programs;
  std::shared_ptr<nebula::nir::Program> nir_prog;
  std::shared_ptr<nebula::passes::AsyncExplainResult> async_explain;
  std::shared_ptr<nebula::passes::RepOwnerResult> rep_owner;
};

struct CompilePipelineCacheStats {
  std::size_t hits = 0;
  std::size_t misses = 0;
  std::size_t evictions = 0;
  std::size_t cross_stage_candidates = 0;
  std::size_t cross_stage_preflight_to_build = 0;
  std::size_t cross_stage_build_to_preflight = 0;
  std::size_t cross_stage_other = 0;
  std::size_t cross_stage_reused = 0;
  std::size_t cross_stage_reused_preflight_to_build = 0;
  std::size_t cross_stage_reused_build_to_preflight = 0;
  std::size_t cross_stage_reused_other = 0;
  std::size_t cross_stage_saved_ms_estimate = 0;
  std::size_t disk_hits = 0;
  std::size_t disk_misses = 0;
  std::size_t disk_writes = 0;
  std::size_t disk_expired = 0;
  std::size_t disk_evictions = 0;
  std::size_t disk_entries = 0;
  std::size_t entries = 0;
};

struct ArtifactMeta {
  std::string source_hash;
  std::string mode;
  std::string profile;
  std::string artifact_kind = "executable";
  int compiler_schema_version = 1;
  int cache_schema_version = 2;
  bool strict_region = false;
  bool warnings_as_errors = false;
  bool no_std = false;
  std::string runtime_profile = "hosted";
  std::string target = "host";
  std::string panic_policy = "abort";
};

struct ArtifactLookupResult {
  std::optional<fs::path> artifact;
  std::vector<nebula::frontend::Diagnostic> diags;
};

void print_version(std::ostream& os);
void print_usage();
bool parse_cli_options(const std::vector<std::string>& args,
                       const std::string& cmd,
                       CliOptions& opt,
                       std::string& err);

std::optional<fs::path> find_executable_on_path(std::string_view command);
int run_command(const std::vector<std::string>& args);
AnalysisProfile resolve_profile(BuildMode mode, AnalysisProfile requested);
AnalysisTier resolve_analysis_tier(BuildMode mode, AnalysisTier requested);
bool should_include_lint_in_build_stage(const CliOptions& opt);
CompilePipelineResult run_compile_pipeline(const std::string& src, const CompilePipelineOptions& opt);
CompilePipelineResult run_compile_pipeline(const std::vector<nebula::frontend::SourceFile>& sources,
                                           const CompilePipelineOptions& opt);
CompilePipelineCacheStats get_compile_pipeline_cache_stats();
void emit_cache_report(const CliOptions& opt, const CompilePipelineCacheStats& before, std::ostream& os);
void emit_diagnostics(const std::vector<nebula::frontend::Diagnostic>& diags, const CliOptions& opt,
                      std::ostream& os);
nebula::frontend::Diagnostic make_cli_diag(nebula::frontend::Severity severity,
                                           std::string code,
                                           std::string message,
                                           nebula::frontend::DiagnosticStage stage,
                                           nebula::frontend::DiagnosticRisk risk,
                                           std::string cause = {},
                                           std::string impact = {},
                                           std::vector<std::string> suggestions = {});
int compile_cpp(const CliOptions& opt,
                const fs::path& cpp_path,
                const fs::path& out_bin,
                CompileFlavor flavor,
                const std::vector<fs::path>& extra_sources = {},
                const NativeBuildInputs& native_inputs = {},
                BuildArtifactKind artifact_kind = BuildArtifactKind::Executable);
bool write_text_file(const fs::path& path, const std::string& text);
fs::path cpp_output_path(const fs::path& source, const CliOptions& opt, const std::string& suffix);
fs::path chosen_artifact_path(const fs::path& source, const CliOptions& opt);
std::string hash_source(const std::string& src);
ArtifactMeta expected_meta_for(const CliOptions& opt,
                               AnalysisProfile resolved_profile,
                               const std::string& source_hash);
bool write_artifact_meta(const fs::path& artifact, const ArtifactMeta& m);
std::optional<ArtifactMeta> read_artifact_meta(const fs::path& artifact);
bool artifact_meta_matches(const ArtifactMeta& lhs, const ArtifactMeta& rhs);
ArtifactLookupResult resolve_no_build_artifact(const fs::path& source_file, const CliOptions& opt);
bool read_source(const fs::path& file,
                 std::string& out,
                 std::vector<nebula::frontend::Diagnostic>& diags,
                 nebula::frontend::DiagnosticStage stage);
int run_preflight_if_enabled(const fs::path& file,
                             const std::vector<nebula::frontend::SourceFile>& sources,
                             const std::string& cache_key_source,
                             const CliOptions& opt);

int cmd_check(const fs::path& file, const CliOptions& opt);
int cmd_build(const fs::path& file, const CliOptions& opt);
int cmd_run(const fs::path& file, const CliOptions& opt);
int cmd_test(const CliOptions& opt);
int cmd_bench(const CliOptions& opt);
int cmd_new(const std::vector<std::string>& args, const CliOptions& opt);
int cmd_add(const std::vector<std::string>& args, const CliOptions& opt);
int cmd_publish(const std::vector<std::string>& args, const CliOptions& opt);
int cmd_fetch(const std::vector<std::string>& args, const CliOptions& opt);
int cmd_update(const std::vector<std::string>& args, const CliOptions& opt);
int cmd_fmt(const std::vector<std::string>& args, const CliOptions& opt);
int cmd_explain(const std::vector<std::string>& args, const CliOptions& opt);
int cmd_lsp(const std::vector<std::string>& args, const CliOptions& opt);
