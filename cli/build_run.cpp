#include "cli_shared.hpp"
#include "project.hpp"

#include <iostream>

#include "codegen/cpp_backend.hpp"

namespace {
struct CacheReportScope {
  const CliOptions& opt;
  const CompilePipelineCacheStats before;

  explicit CacheReportScope(const CliOptions& options)
      : opt(options), before(get_compile_pipeline_cache_stats()) {}

  ~CacheReportScope() { emit_cache_report(opt, before, std::cerr); }
};
} // namespace

int cmd_build(const fs::path& file, const CliOptions& opt) {
  CacheReportScope cache_scope(opt);
  auto loaded = load_compile_input(file, nebula::frontend::DiagnosticStage::Build);
  if (!loaded.diags.empty()) emit_diagnostics(loaded.diags, opt, std::cerr);
  if (!loaded.ok) {
    return 1;
  }
  const fs::path effective_file = loaded.entry_file.empty() ? file : loaded.entry_file;
  AnalysisProfile requested = opt.profile_explicit ? opt.analysis_profile : AnalysisProfile::Auto;
  const AnalysisProfile resolved = resolve_profile(opt.mode, requested);
  const AnalysisTier tier = resolve_analysis_tier(opt.mode, opt.analysis_tier);

  CompilePipelineOptions popt;
  popt.mode = opt.mode;
  popt.profile = resolved;
  popt.analysis_tier = tier;
  popt.strict_region = opt.strict_region;
  popt.warnings_as_errors = opt.warnings_as_errors;
  popt.include_lint = should_include_lint_in_build_stage(opt);
  popt.allow_cross_stage_reuse = false;
  popt.budget_ms = opt.diag_budget_ms;
  popt.source_path = effective_file.string();
  popt.cache_key_source = loaded.cache_key_source;
  popt.stage = nebula::frontend::DiagnosticStage::Build;

  auto analysis = run_compile_pipeline(loaded.compile_sources, popt);
  emit_diagnostics(analysis.diags, opt, std::cerr);
  if (analysis.has_error || !analysis.nir_prog || !analysis.rep_owner) return 1;

  const fs::path out_cpp = cpp_output_path(effective_file, opt, "");
  const fs::path out_bin = chosen_artifact_path(effective_file, opt);

  nebula::codegen::EmitOptions eopt;
  eopt.main_mode = nebula::codegen::MainMode::CallMainIfPresent;
  eopt.strict_region = opt.strict_region;
  const std::string cpp = nebula::codegen::emit_cpp23(*analysis.nir_prog, *analysis.rep_owner, eopt);

  if (!write_text_file(out_cpp, cpp)) {
    auto d = make_cli_diag(
        nebula::frontend::Severity::Error, "NBL-CLI-IO002",
        "failed to write generated C++: " + out_cpp.string(),
        nebula::frontend::DiagnosticStage::Build, nebula::frontend::DiagnosticRisk::High,
        "output directory is not writable", "build cannot proceed without generated C++",
        {"check output directory permissions"});
    emit_diagnostics({d}, opt, std::cerr);
    return 1;
  }

  std::cerr << "wrote: " << out_cpp.string() << "\n";
  if (compile_cpp(opt, out_cpp, out_bin, CompileFlavor::Normal, loaded.host_cxx_sources) != 0) {
    return 1;
  }

  const ArtifactMeta meta = expected_meta_for(opt, resolved, hash_source(loaded.cache_key_source));
  (void)write_artifact_meta(out_bin, meta);

  std::cerr << "wrote artifact: " << out_bin.string() << "\n";
  return 0;
}

int cmd_run(const fs::path& file, const CliOptions& opt) {
  CacheReportScope cache_scope(opt);
  if (opt.no_build && opt.reuse) {
    auto d = make_cli_diag(
        nebula::frontend::Severity::Error, "NBL-CLI-CONFLICT",
        "--no-build cannot be used with --reuse", nebula::frontend::DiagnosticStage::Build,
        nebula::frontend::DiagnosticRisk::High,
        "mutually exclusive run artifact policies",
        "command cannot decide whether compilation is allowed",
        {"remove either --no-build or --reuse"});
    emit_diagnostics({d}, opt, std::cerr);
    return 1;
  }

  auto loaded = load_compile_input(file, nebula::frontend::DiagnosticStage::Build);
  if (!loaded.diags.empty()) emit_diagnostics(loaded.diags, opt, std::cerr);
  if (!loaded.ok) {
    return 1;
  }
  const fs::path effective_file = loaded.entry_file.empty() ? file : loaded.entry_file;
  if (run_preflight_if_enabled(effective_file, loaded.compile_sources, loaded.cache_key_source, opt) != 0) {
    return 1;
  }

  if (opt.no_build) {
    auto lookup = resolve_no_build_artifact(effective_file, opt);
    emit_diagnostics(lookup.diags, opt, std::cerr);
    if (!lookup.artifact.has_value()) return 1;
    return run_command({lookup.artifact->string()});
  }

  const fs::path out_bin = chosen_artifact_path(effective_file, opt);
  const AnalysisProfile requested = opt.profile_explicit ? opt.analysis_profile : AnalysisProfile::Auto;
  const AnalysisProfile resolved_profile = resolve_profile(opt.mode, requested);
  const AnalysisTier tier = resolve_analysis_tier(opt.mode, opt.analysis_tier);
  const ArtifactMeta expected =
      expected_meta_for(opt, resolved_profile, hash_source(loaded.cache_key_source));

  bool reused = false;
  if (opt.reuse && fs::exists(out_bin)) {
    auto meta = read_artifact_meta(out_bin);
    reused = meta.has_value() && artifact_meta_matches(*meta, expected);
  }

  if (!reused) {
    CompilePipelineOptions popt;
    popt.mode = opt.mode;
    popt.profile = resolved_profile;
    popt.analysis_tier = tier;
    popt.strict_region = opt.strict_region;
    popt.warnings_as_errors = opt.warnings_as_errors;
    popt.include_lint = should_include_lint_in_build_stage(opt);
    popt.allow_cross_stage_reuse = (opt.cross_stage_reuse == CrossStageReuseMode::Safe);
    popt.disk_cache_enabled = (opt.disk_cache == DiskCacheMode::On);
    popt.disk_cache_ttl_sec = opt.disk_cache_ttl_sec;
    popt.disk_cache_max_entries = opt.disk_cache_max_entries;
    popt.disk_cache_dir = opt.disk_cache_dir;
    popt.disk_cache_prune = opt.disk_cache_prune;
    popt.budget_ms = opt.diag_budget_ms;
    popt.source_path = effective_file.string();
    popt.cache_key_source = loaded.cache_key_source;
    popt.stage = nebula::frontend::DiagnosticStage::Build;

    auto analysis = run_compile_pipeline(loaded.compile_sources, popt);
    emit_diagnostics(analysis.diags, opt, std::cerr);
    if (analysis.has_error) return 1;

    const fs::path out_cpp = cpp_output_path(effective_file, opt, "");
    std::string cpp;
    if (analysis.has_cached_cpp) {
      cpp = analysis.cached_cpp;
    } else if (analysis.nir_prog && analysis.rep_owner) {
      nebula::codegen::EmitOptions eopt;
      eopt.main_mode = nebula::codegen::MainMode::CallMainIfPresent;
      eopt.strict_region = opt.strict_region;
      cpp = nebula::codegen::emit_cpp23(*analysis.nir_prog, *analysis.rep_owner, eopt);
    } else {
      auto d = make_cli_diag(
          nebula::frontend::Severity::Error, "NBL-CLI-CACHE-STATE",
          "build analysis result missing codegen payload",
          nebula::frontend::DiagnosticStage::Build, nebula::frontend::DiagnosticRisk::High,
          "analysis cache state is incomplete for code generation",
          "run cannot proceed without generated C++",
          {"retry without --disk-cache", "or rebuild cache with --disk-cache-prune"});
      emit_diagnostics({d}, opt, std::cerr);
      return 1;
    }

    if (!write_text_file(out_cpp, cpp)) {
      auto d = make_cli_diag(
          nebula::frontend::Severity::Error, "NBL-CLI-IO002",
          "failed to write generated C++: " + out_cpp.string(),
          nebula::frontend::DiagnosticStage::Build, nebula::frontend::DiagnosticRisk::High,
          "output directory is not writable", "build cannot proceed without generated C++",
          {"check output directory permissions"});
      emit_diagnostics({d}, opt, std::cerr);
      return 1;
    }
    std::cerr << "wrote: " << out_cpp.string() << "\n";

    if (compile_cpp(opt, out_cpp, out_bin, CompileFlavor::Normal, loaded.host_cxx_sources) != 0) {
      return 1;
    }
    (void)write_artifact_meta(out_bin, expected);
  } else {
    auto d = make_cli_diag(
        nebula::frontend::Severity::Note, "NBL-CLI-REUSE",
        "reusing cached artifact: " + out_bin.string(),
        nebula::frontend::DiagnosticStage::Build, nebula::frontend::DiagnosticRisk::Low,
        "artifact metadata matches current source/mode/profile",
        "skipped rebuild to keep run latency low");
    emit_diagnostics({d}, opt, std::cerr);
  }

  return run_command({out_bin.string()});
}
