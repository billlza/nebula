#include "cli_shared.hpp"
#include "project.hpp"

#include <cctype>
#include <iostream>
#include <unordered_set>

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

namespace {

static std::string sanitize_artifact_stem(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char ch : text) {
    if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-') {
      out.push_back(ch);
    } else {
      out.push_back('_');
    }
  }
  return out.empty() ? std::string("nebula_artifact") : out;
}

static std::string default_header_stem(const LoadedCompileInput& loaded, const fs::path& effective_file) {
  const std::string base = !loaded.project_name.empty() ? loaded.project_name : effective_file.stem().string();
  return sanitize_artifact_stem(base);
}

static fs::path default_build_artifact_path(const LoadedCompileInput& loaded,
                                            const fs::path& effective_file,
                                            const CliOptions& opt) {
  if (opt.out_path.has_value()) return *opt.out_path;
  const std::string stem = default_header_stem(loaded, effective_file);
  switch (opt.artifact_kind) {
  case BuildArtifactKind::Executable:
    return opt.out_dir / (effective_file.stem().string() + ".out");
  case BuildArtifactKind::StaticLib:
    return opt.out_dir / ("lib" + stem + ".a");
  case BuildArtifactKind::SharedLib:
#if defined(_WIN32)
    return opt.out_dir / (stem + ".dll");
#elif defined(__APPLE__)
    return opt.out_dir / ("lib" + stem + ".dylib");
#else
    return opt.out_dir / ("lib" + stem + ".so");
#endif
  }
  return opt.out_dir / (effective_file.stem().string() + ".out");
}

static fs::path header_output_path_for(const LoadedCompileInput& loaded,
                                       const fs::path& effective_file,
                                       const fs::path& artifact_path) {
  return artifact_path.parent_path() / (default_header_stem(loaded, effective_file) + ".h");
}

} // namespace

int cmd_build(const fs::path& file, const CliOptions& opt) {
  CacheReportScope cache_scope(opt);
  auto loaded = load_compile_input(file,
                                   nebula::frontend::DiagnosticStage::Build,
                                   load_compile_options_from_cli(opt));
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
  popt.strict_region = effective_strict_region(opt.runtime_profile, opt.strict_region);
  popt.warnings_as_errors = opt.warnings_as_errors;
  popt.no_std = effective_no_std(opt.runtime_profile, opt.no_std);
  popt.runtime_profile = opt.runtime_profile;
  popt.panic_policy = opt.panic_policy;
  popt.target = opt.target;
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
  const fs::path out_bin = default_build_artifact_path(loaded, effective_file, opt);
  const bool library_mode = opt.artifact_kind != BuildArtifactKind::Executable;
  if (library_mode && !loaded.host_cxx_sources.empty()) {
    auto d = make_cli_diag(
        nebula::frontend::Severity::Error, "NBL-CLI-CABI-HOSTCXX",
        "C ABI library build does not support host_cxx sources yet",
        nebula::frontend::DiagnosticStage::Build, nebula::frontend::DiagnosticRisk::High,
        "host_cxx files currently model executable-side host integration, not stable library ABI surface",
        "generated library could accidentally expose or link unintended host-side symbols",
        {"remove host_cxx from the package for library builds", "or keep the host bridge in a separate consumer project"});
    emit_diagnostics({d}, opt, std::cerr);
    return 1;
  }
  if (library_mode && !loaded.native_inputs.empty()) {
    auto d = make_cli_diag(
        nebula::frontend::Severity::Error, "NBL-CLI-CABI-NATIVE",
        "C ABI library build does not support [native] package sources yet",
        nebula::frontend::DiagnosticStage::Build, nebula::frontend::DiagnosticRisk::High,
        "package-native sources currently model executable-side integration, not stable library ABI surface",
        "generated library could accidentally depend on non-portable native build inputs",
        {"remove [native] from the reachable package graph for library builds",
         "or keep the native bridge in a separate executable consumer"});
    emit_diagnostics({d}, opt, std::cerr);
    return 1;
  }

  nebula::codegen::EmitOptions eopt;
  eopt.main_mode = library_mode ? nebula::codegen::MainMode::None
                                : nebula::codegen::MainMode::CallMainIfPresent;
  eopt.strict_region = effective_strict_region(opt.runtime_profile, opt.strict_region);
  eopt.runtime_profile = opt.runtime_profile;
  eopt.target = opt.target;
  eopt.panic_policy = opt.panic_policy;
  eopt.emit_c_abi_wrappers = library_mode;
  if (library_mode && !loaded.project_name.empty()) {
    eopt.c_abi_export_package = loaded.project_name;
  }
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
  if (library_mode) {
    const auto exports =
        nebula::codegen::collect_c_abi_functions(*analysis.nir_prog, loaded.project_name);
    if (exports.empty()) {
      auto d = make_cli_diag(
          nebula::frontend::Severity::Error, "NBL-CLI-CABI-NOEXPORT",
          "library build requested but no @export @abi_c functions were found",
          nebula::frontend::DiagnosticStage::Build, nebula::frontend::DiagnosticRisk::High,
          "C ABI library output needs an explicit exported function surface",
          "generated library would not expose a supported public ABI",
          {"mark at least one function with @export and @abi_c", "or build the project as an executable"});
      emit_diagnostics({d}, opt, std::cerr);
      return 1;
    }
    std::unordered_set<std::string> seen_exports;
    for (const auto& fn : exports) {
      if (!seen_exports.insert(fn.export_name).second) {
        auto d = make_cli_diag(
            nebula::frontend::Severity::Error, "NBL-CLI-CABI-CONFLICT",
            "duplicate exported C ABI symbol: " + fn.export_name,
            nebula::frontend::DiagnosticStage::Build, nebula::frontend::DiagnosticRisk::High,
            "sanitized package/module/function names collided in the public C ABI surface",
            "generated library would expose ambiguous symbols",
            {"rename one of the exported functions or modules", "or narrow the export set"});
        emit_diagnostics({d}, opt, std::cerr);
        return 1;
      }
    }
    const fs::path header_path = header_output_path_for(loaded, effective_file, out_bin);
    const std::string header = nebula::codegen::emit_c_abi_header(
        *analysis.nir_prog, exports, default_header_stem(loaded, effective_file));
    if (!write_text_file(header_path, header)) {
      auto d = make_cli_diag(
          nebula::frontend::Severity::Error, "NBL-CLI-IO002",
          "failed to write generated C header: " + header_path.string(),
          nebula::frontend::DiagnosticStage::Build, nebula::frontend::DiagnosticRisk::High,
          "output directory is not writable", "C ABI build cannot proceed without the generated header");
      emit_diagnostics({d}, opt, std::cerr);
      return 1;
    }
    std::cerr << "wrote: " << header_path.string() << "\n";
  }

  if (compile_cpp(opt, out_cpp, out_bin, CompileFlavor::Normal, loaded.host_cxx_sources,
                  loaded.native_inputs, opt.artifact_kind) != 0) {
    return 1;
  }

  const ArtifactMeta meta = expected_meta_for(opt, resolved, hash_source(loaded.cache_key_source));
  (void)write_artifact_meta(out_bin, meta);

  std::cerr << "wrote artifact: " << out_bin.string() << "\n";
  return 0;
}

int cmd_run(const fs::path& file, const CliOptions& opt) {
  CacheReportScope cache_scope(opt);
  auto run_args = [&](const fs::path& artifact) {
    std::vector<std::string> cmd;
    cmd.reserve(1 + opt.run_args.size());
    cmd.push_back(artifact.string());
    cmd.insert(cmd.end(), opt.run_args.begin(), opt.run_args.end());
    return cmd;
  };
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

  auto loaded = load_compile_input(file,
                                   nebula::frontend::DiagnosticStage::Build,
                                   load_compile_options_from_cli(opt));
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
    return run_command(run_args(*lookup.artifact));
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
    popt.strict_region = effective_strict_region(opt.runtime_profile, opt.strict_region);
    popt.warnings_as_errors = opt.warnings_as_errors;
    popt.no_std = effective_no_std(opt.runtime_profile, opt.no_std);
    popt.runtime_profile = opt.runtime_profile;
    popt.panic_policy = opt.panic_policy;
    popt.target = opt.target;
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
      eopt.strict_region = effective_strict_region(opt.runtime_profile, opt.strict_region);
      eopt.runtime_profile = opt.runtime_profile;
      eopt.target = opt.target;
      eopt.panic_policy = opt.panic_policy;
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

    if (compile_cpp(opt, out_cpp, out_bin, CompileFlavor::Normal, loaded.host_cxx_sources,
                    loaded.native_inputs) != 0) {
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

  return run_command(run_args(out_bin));
}
