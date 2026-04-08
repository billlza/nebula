#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

#include "codegen/cpp_backend.hpp"
#include "frontend/errors.hpp"
#include "frontend/diagnostic.hpp"
#include "frontend/lexer.hpp"
#include "frontend/parser.hpp"
#include "frontend/typecheck.hpp"
#include "nir/cfg.hpp"
#include "nir/cfg_ir.hpp"
#include "nir/lower.hpp"
#include "passes/escape_analysis.hpp"
#include "passes/borrow_xstmt.hpp"
#include "passes/call_target_resolver.hpp"
#include "passes/epistemic_lint.hpp"
#include "passes/rep_owner_infer.hpp"
#include "cli_shared.hpp"
#include "project.hpp"

static bool parse_bench_output(const std::string& s,
                               int& warmup_iterations,
                               int& measure_iterations,
                               int& samples,
                               double& p50_ms,
                               double& p90_ms,
                               double& p99_ms,
                               double& mean_ms,
                               double& stddev_ms,
                               double& throughput_ops_s,
                               std::string& clock_source,
                               std::string& platform,
                               std::string& perf_capability,
                               std::string& perf_counters,
                               std::string& perf_reason);

static std::string quote_for_log(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (char c : s) {
    if (c == '\\' || c == '"') out.push_back('\\');
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

static std::string cmd_for_log(const std::vector<std::string>& args) {
  std::string out;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (i) out.push_back(' ');
    const std::string& arg = args[i];
    const bool is_flag = !arg.empty() && arg[0] == '-';
    if (is_flag && arg.find_first_of(" \t\"\\") == std::string::npos) {
      out += arg;
    } else {
      out += quote_for_log(arg);
    }
  }
  return out;
}

static int normalize_wait_status(int status) {
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return 1;
}

int run_command(const std::vector<std::string>& args) {
  if (args.empty()) return 1;
  std::cerr << "[cmd] " << cmd_for_log(args) << "\n";

  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
  argv.push_back(nullptr);

  const pid_t pid = fork();
  if (pid < 0) {
    std::cerr << "[cmd] fork failed: " << std::strerror(errno) << "\n";
    return 1;
  }

  if (pid == 0) {
    execvp(argv[0], argv.data());
    std::perror("execvp");
    _exit(127);
  }

  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno == EINTR) continue;
    std::cerr << "[cmd] waitpid failed: " << std::strerror(errno) << "\n";
    return 1;
  }

  const int rc = normalize_wait_status(status);
  if (rc != 0) std::cerr << "[cmd] exit=" << rc << "\n";
  return rc;
}

static std::string run_capture(const std::vector<std::string>& args, int* out_rc) {
  std::string out;
  if (args.empty()) {
    if (out_rc) *out_rc = 1;
    return "";
  }

  std::cerr << "[cmd] " << cmd_for_log(args) << "\n";

  int pipefd[2];
  if (pipe(pipefd) != 0) {
    if (out_rc) *out_rc = 1;
    return "";
  }

  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
  argv.push_back(nullptr);

  const pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    if (out_rc) *out_rc = 1;
    return "";
  }

  if (pid == 0) {
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);
    execvp(argv[0], argv.data());
    std::perror("execvp");
    _exit(127);
  }

  close(pipefd[1]);
  char buf[4096];
  while (true) {
    const ssize_t n = read(pipefd[0], buf, sizeof(buf));
    if (n > 0) {
      out.append(buf, buf + n);
      continue;
    }
    if (n == 0) break;
    if (errno == EINTR) continue;
    break;
  }
  close(pipefd[0]);

  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno == EINTR) continue;
    if (out_rc) *out_rc = 1;
    return out;
  }

  const int rc = normalize_wait_status(status);
  if (out_rc) *out_rc = rc;
  if (rc != 0) std::cerr << "[cmd] exit=" << rc << "\n";
  return out;
}

AnalysisProfile resolve_profile(BuildMode mode, AnalysisProfile requested) {
  if (requested != AnalysisProfile::Auto) return requested;
  return (mode == BuildMode::Release) ? AnalysisProfile::Deep : AnalysisProfile::Fast;
}

AnalysisTier resolve_analysis_tier(BuildMode mode, AnalysisTier requested) {
  if (requested != AnalysisTier::Auto) return requested;
  return (mode == BuildMode::Release) ? AnalysisTier::Smart : AnalysisTier::Basic;
}

bool should_include_lint_in_build_stage(const CliOptions& opt) {
  const AnalysisTier tier = resolve_analysis_tier(opt.mode, opt.analysis_tier);
  if (tier != AnalysisTier::Basic) return true;
  if (opt.mode == BuildMode::Release) return true;
  if (opt.profile_explicit) return true;
  return false;
}

static const char* analysis_tier_name(AnalysisTier tier) {
  switch (tier) {
  case AnalysisTier::Auto: return "auto";
  case AnalysisTier::Basic: return "basic";
  case AnalysisTier::Smart: return "smart";
  case AnalysisTier::Deep: return "deep";
  }
  return "basic";
}

static const char* warn_policy_name(WarnPolicy policy) {
  switch (policy) {
  case WarnPolicy::Strict: return "strict";
  case WarnPolicy::Balanced: return "balanced";
  case WarnPolicy::Lenient: return "lenient";
  }
  return "balanced";
}

static const char* gate_profile_name(GateProfile profile) {
  switch (profile) {
  case GateProfile::Strict: return "strict";
  case GateProfile::Balanced: return "balanced";
  case GateProfile::Lenient: return "lenient";
  }
  return "balanced";
}

static bool root_cause_v2_enabled(const CliOptions& opt) {
  switch (opt.root_cause_v2) {
  case RootCauseV2Mode::On: return true;
  case RootCauseV2Mode::Off: return false;
  case RootCauseV2Mode::Auto: return opt.root_cause_v2_default_on;
  }
  return false;
}

static const char* warning_class_for_code(const std::string& code) {
  if (code.rfind("NBL-A", 0) == 0) return "api-deprecation";
  if (code.rfind("NBL-P", 0) == 0 || code.rfind("NBL-X", 0) == 0) return "performance-risk";
  if (code.rfind("NBL-C", 0) == 0) return "best-practice-drift";
  if (code.rfind("NBL-U", 0) == 0 || code.rfind("NBL-R", 0) == 0 || code.rfind("NBL-S", 0) == 0) {
    return "safety-risk";
  }
  return "general-warning";
}

static const char* warning_dimension_for_code(const std::string& code) {
  if (code.rfind("NBL-A", 0) == 0) return "api-lifecycle";
  if (code.rfind("NBL-P", 0) == 0 || code.rfind("NBL-X", 0) == 0 || code.rfind("NBL-PR", 0) == 0) {
    return "perf-runtime";
  }
  if (code.rfind("NBL-C", 0) == 0) return "best-practice-drift";
  if (code.rfind("NBL-U", 0) == 0 || code.rfind("NBL-R", 0) == 0 || code.rfind("NBL-S", 0) == 0) {
    return "safety-contract";
  }
  return "general";
}

static const char* warning_reason_for_diag(const nebula::frontend::Diagnostic& d) {
  if (d.code == "NBL-A001") return "deprecated-api";
  if (d.code.rfind("NBL-A", 0) == 0) return "api-lifecycle-general";
  if (d.code == "NBL-P001") return "hot-loop-alloc";
  if (d.code == "NBL-P010") return "shared-hot-path";
  if (d.code == "NBL-X001") return "predictive-regression";
  if (d.code == "NBL-X002") return "predictive-complexity-pressure";
  if (d.code == "NBL-X003") return "predictive-inferred-shared-hot-followup";
  if (d.code == "NBL-PR001") return "preflight-latency-budget";
  if (d.code == "NBL-PR002") return "analysis-budget-skip-lint";
  if (d.code == "NBL-C001") return "style-drift";
  if (d.code == "NBL-C010") return "explicit-style-drift-marker";
  if (d.code.rfind("NBL-C", 0) == 0) return "best-practice-general";
  if (d.code == "NBL-R001") return "region-escape-risk";
  if (d.code == "NBL-U001") return "unsafe-call-boundary";
  if (d.code == "NBL-U002") return "unsafe-annotation-misuse";
  if (d.code.rfind("NBL-U", 0) == 0 || d.code.rfind("NBL-R", 0) == 0 || d.code.rfind("NBL-S", 0) == 0) {
    return "safety-contract-general";
  }
  if (d.code.rfind("NBL-P", 0) == 0 || d.code.rfind("NBL-X", 0) == 0) return "perf-runtime-general";
  return "general-warning";
}

static std::uint32_t warning_class_bit_for_name(std::string_view cls) {
  if (cls == "api-deprecation") return kWarnClassApi;
  if (cls == "performance-risk") return kWarnClassPerformance;
  if (cls == "best-practice-drift") return kWarnClassBestPractice;
  if (cls == "safety-risk") return kWarnClassSafety;
  return kWarnClassGeneral;
}

static std::uint32_t warning_class_bit_for_code(const std::string& code) {
  return warning_class_bit_for_name(warning_class_for_code(code));
}

static std::uint32_t gate_dimension_bit_for_name(std::string_view dim) {
  if (dim == "api-lifecycle") return kGateDimApiLifecycle;
  if (dim == "perf-runtime") return kGateDimPerfRuntime;
  if (dim == "best-practice-drift") return kGateDimBestPracticeDrift;
  if (dim == "safety-contract") return kGateDimSafetyContract;
  return kGateDimGeneral;
}

static std::uint32_t gate_dimension_bit_for_code(const std::string& code) {
  return gate_dimension_bit_for_name(warning_dimension_for_code(code));
}

static std::uint32_t base_warn_class_mask_for_policy(WarnPolicy policy) {
  switch (policy) {
  case WarnPolicy::Strict: return kWarnClassAll;
  case WarnPolicy::Balanced: return kWarnClassAll;
  case WarnPolicy::Lenient: return kWarnClassAll & ~kWarnClassBestPractice;
  }
  return kWarnClassAll;
}

static std::uint32_t base_gate_dimension_mask_for_profile(GateProfile profile) {
  switch (profile) {
  case GateProfile::Strict: return kGateDimAll;
  case GateProfile::Balanced: return kGateDimAll;
  case GateProfile::Lenient:
    return kGateDimApiLifecycle | kGateDimPerfRuntime | kGateDimSafetyContract;
  }
  return kGateDimAll;
}

static std::uint32_t effective_gate_dimension_mask(const CliOptions& opt) {
  std::uint32_t mask = base_gate_dimension_mask_for_profile(opt.gate_profile);
  mask &= ~opt.gate_dimension_force_off_mask;
  mask |= opt.gate_dimension_force_on_mask;
  return mask;
}

static int gate_threshold_for_profile(GateProfile profile) {
  switch (profile) {
  case GateProfile::Strict: return 50;
  case GateProfile::Balanced: return 70;
  case GateProfile::Lenient: return 90;
  }
  return 70;
}

static int gate_weight_for_warning(const nebula::frontend::Diagnostic& d) {
  if (!d.is_warning()) return 0;

  int weight = 0;
  const std::string_view dim = warning_dimension_for_code(d.code);
  if (dim == "api-lifecycle") {
    weight = 75;
  } else if (dim == "perf-runtime") {
    weight = 70;
  } else if (dim == "best-practice-drift") {
    weight = 45;
  } else if (dim == "safety-contract") {
    weight = 85;
  } else {
    weight = 60;
  }

  using nebula::frontend::DiagnosticRisk;
  switch (d.risk) {
  case DiagnosticRisk::Unknown: break;
  case DiagnosticRisk::Low: break;
  case DiagnosticRisk::Medium: weight += 10; break;
  case DiagnosticRisk::High: weight += 20; break;
  case DiagnosticRisk::Critical: weight += 30; break;
  }

  if (d.predictive) {
    const double conf = d.confidence.value_or(0.0);
    weight += (conf >= 0.80) ? 10 : 5;
  }
  if (d.code == "NBL-P010") weight += 10;
  if (d.code == "NBL-P001") weight += 5;
  if (d.code == "NBL-A001") weight += 5;

  if (weight < 0) weight = 0;
  if (weight > 100) weight = 100;
  return weight;
}

static std::string root_group_key(const nebula::frontend::Diagnostic& d) {
  const std::string grouping_code =
      (!d.caused_by_code.empty() && d.code == "NBL-X003") ? d.caused_by_code : d.code;
  std::string family = "general";
  if (grouping_code.rfind("NBL-PAR", 0) == 0) family = "parser";
  else if (grouping_code.rfind("NBL-T09", 0) == 0) family = "ref-borrow-window";
  else if (grouping_code.rfind("NBL-T08", 0) == 0) family = "typing-surface";
  else if (grouping_code.rfind("NBL-U", 0) == 0) family = "unsafe-boundary";
  else if (grouping_code.rfind("NBL-R", 0) == 0) family = "region-escape";
  else if (grouping_code.rfind("NBL-A", 0) == 0) family = "api-lifecycle";
  else if (grouping_code.rfind("NBL-P", 0) == 0 || grouping_code.rfind("NBL-X", 0) == 0) {
    family = "performance";
  } else if (grouping_code.rfind("NBL-C", 0) == 0) {
    family = "best-practice";
  } else if (grouping_code.rfind("NBL-CLI", 0) == 0) {
    family = "cli";
  }

  std::ostringstream os;
  os << family << "|" << nebula::frontend::stage_name(d.stage);
  if (!grouping_code.empty()) os << "|" << grouping_code;
  if (d.span.start.line > 0 || d.span.start.col > 0) {
    os << "|" << d.span.start.line << ":" << d.span.start.col;
  }
  return os.str();
}

static nebula::passes::EpistemicLintProfile to_lint_profile(AnalysisProfile p) {
  return (p == AnalysisProfile::Deep) ? nebula::passes::EpistemicLintProfile::Deep
                                      : nebula::passes::EpistemicLintProfile::Fast;
}

static nebula::passes::EscapeAnalysisProfile to_escape_profile(AnalysisProfile p) {
  return (p == AnalysisProfile::Deep) ? nebula::passes::EscapeAnalysisProfile::Deep
                                      : nebula::passes::EscapeAnalysisProfile::Fast;
}

static std::vector<fs::path> list_nb_files(const fs::path& dir) {
  std::vector<fs::path> out;
  if (!fs::exists(dir)) return out;
  for (const auto& ent : fs::directory_iterator(dir)) {
    if (!ent.is_regular_file()) continue;
    const fs::path p = ent.path();
    if (p.extension() == ".nb") out.push_back(p);
  }
  std::sort(out.begin(), out.end());
  return out;
}

static bool is_explicit_project_target(const fs::path& input) {
  const fs::path abs_input = fs::absolute(input);
  if (abs_input.filename() == "nebula.toml") return fs::exists(abs_input);
  if (fs::is_regular_file(abs_input)) return true;
  return fs::is_directory(abs_input) && fs::exists(abs_input / "nebula.toml");
}

static bool has_annotation(const std::vector<std::string>& ann, const std::string& x) {
  return std::find(ann.begin(), ann.end(), x) != ann.end();
}

static bool nir_has_fn_with_ann(const nebula::nir::Program& p, const std::string& ann) {
  for (const auto& it : p.items) {
    if (!std::holds_alternative<nebula::nir::Function>(it.node)) continue;
    const auto& fn = std::get<nebula::nir::Function>(it.node);
    if (has_annotation(fn.annotations, ann)) return true;
  }
  return false;
}

static bool analyze_loaded_project(const LoadedCompileInput& loaded,
                                   const CliOptions& opt,
                                   CompilePipelineResult& analysis) {
  CompilePipelineOptions popt;
  const AnalysisProfile requested =
      opt.profile_explicit ? opt.analysis_profile : AnalysisProfile::Fast;
  popt.mode = opt.mode;
  popt.profile = resolve_profile(opt.mode, requested);
  popt.analysis_tier = resolve_analysis_tier(opt.mode, opt.analysis_tier);
  popt.strict_region = opt.strict_region;
  popt.warnings_as_errors = opt.warnings_as_errors;
  popt.include_lint = should_include_lint_in_build_stage(opt);
  popt.budget_ms = opt.diag_budget_ms;
  popt.source_path = loaded.entry_file.string();
  popt.cache_key_source = loaded.cache_key_source;
  popt.stage = nebula::frontend::DiagnosticStage::Build;

  analysis = run_compile_pipeline(loaded.compile_sources, popt);
  emit_diagnostics(analysis.diags, opt, std::cerr);
  return !analysis.has_error && analysis.nir_prog && analysis.rep_owner;
}

[[maybe_unused]] static int cmd_test_project_target(const CliOptions& opt, const LoadedCompileInput& loaded) {
  CompilePipelineResult analysis;
  if (!analyze_loaded_project(loaded, opt, analysis)) return 1;
  if (!nir_has_fn_with_ann(*analysis.nir_prog, "test")) return 0;

  const fs::path out_cpp = cpp_output_path(loaded.entry_file, opt, "_test");
  const fs::path out_bin = opt.out_dir / (loaded.entry_file.stem().string() + "_test.out");

  nebula::codegen::EmitOptions eopt;
  eopt.main_mode = nebula::codegen::MainMode::RunTests;
  eopt.strict_region = opt.strict_region;
  const std::string cpp = nebula::codegen::emit_cpp23(*analysis.nir_prog, *analysis.rep_owner, eopt);

  if (!write_text_file(out_cpp, cpp)) {
    auto d = make_cli_diag(
        nebula::frontend::Severity::Error, "NBL-CLI-IO002",
        "failed to write generated C++: " + out_cpp.string(),
        nebula::frontend::DiagnosticStage::Build, nebula::frontend::DiagnosticRisk::High,
        "output directory is not writable", "test build cannot proceed");
    emit_diagnostics({d}, opt, std::cerr);
    return 1;
  }

  std::cerr << "wrote: " << out_cpp.string() << "\n";
  if (compile_cpp(opt, out_cpp, out_bin, CompileFlavor::Test, loaded.host_cxx_sources) != 0) {
    return 1;
  }
  return run_command({out_bin.string()}) == 0 ? 0 : 1;
}

[[maybe_unused]] static int cmd_bench_project_target(const CliOptions& opt, const LoadedCompileInput& loaded) {
  fs::create_directories("benchmark_results");
  const fs::path csv_path = fs::path("benchmark_results") / "latency.csv";
  std::ofstream csv(csv_path);
  csv << "file,warmup_iterations,measure_iterations,samples,p50_ms,p90_ms,p99_ms,mean_ms,stddev_ms,throughput_ops_s,clock,platform,perf_capability,perf_counters,perf_reason\n";

  CompilePipelineResult analysis;
  if (!analyze_loaded_project(loaded, opt, analysis)) return 1;
  if (!nir_has_fn_with_ann(*analysis.nir_prog, "bench")) {
    std::cerr << "wrote: " << csv_path.string() << "\n";
    return 0;
  }

  const fs::path out_cpp = cpp_output_path(loaded.entry_file, opt, "_bench");
  const fs::path out_bin = opt.out_dir / (loaded.entry_file.stem().string() + "_bench.out");

  nebula::codegen::EmitOptions eopt;
  eopt.main_mode = nebula::codegen::MainMode::RunBench;
  eopt.strict_region = opt.strict_region;
  const std::string cpp = nebula::codegen::emit_cpp23(*analysis.nir_prog, *analysis.rep_owner, eopt);

  if (!write_text_file(out_cpp, cpp)) {
    auto d = make_cli_diag(
        nebula::frontend::Severity::Error, "NBL-CLI-IO002",
        "failed to write generated C++: " + out_cpp.string(),
        nebula::frontend::DiagnosticStage::Build, nebula::frontend::DiagnosticRisk::High,
        "output directory is not writable", "bench build cannot proceed");
    emit_diagnostics({d}, opt, std::cerr);
    return 1;
  }

  std::cerr << "wrote: " << out_cpp.string() << "\n";
  if (compile_cpp(opt, out_cpp, out_bin, CompileFlavor::Bench, loaded.host_cxx_sources) != 0) {
    return 1;
  }

  int rc = 0;
  const std::string out_text = run_capture({out_bin.string()}, &rc);
  if (rc != 0) {
    std::cerr << out_text;
    return 1;
  }

  int warmup = 0, measure = 0, samples = 0;
  double p50 = 0, p90 = 0, p99 = 0, mean = 0, stddev = 0, thr = 0;
  std::string clock_source;
  std::string platform;
  std::string perf_capability;
  std::string perf_counters;
  std::string perf_reason;
  if (!parse_bench_output(out_text, warmup, measure, samples, p50, p90, p99, mean, stddev, thr,
                          clock_source, platform, perf_capability, perf_counters, perf_reason)) {
    auto d = make_cli_diag(
        nebula::frontend::Severity::Error, "NBL-CLI-BENCH-PARSE",
        "failed to parse benchmark output for project: " + loaded.project_name,
        nebula::frontend::DiagnosticStage::Build, nebula::frontend::DiagnosticRisk::High,
        "benchmark runner output does not match expected format",
        "latency CSV row cannot be generated");
    emit_diagnostics({d}, opt, std::cerr);
    std::cerr << out_text;
    return 1;
  }

  csv << loaded.project_name << "," << warmup << "," << measure << "," << samples << "," << p50
      << "," << p90 << "," << p99 << "," << mean << "," << stddev << "," << thr << ","
      << clock_source << "," << platform << "," << perf_capability << "," << perf_counters << ","
      << perf_reason << "\n";
  std::cerr << out_text;
  std::cerr << "wrote: " << csv_path.string() << "\n";
  return 0;
}

static bool parse_bench_output(const std::string& s,
                               int& warmup_iterations,
                               int& measure_iterations,
                               int& samples,
                               double& p50_ms,
                               double& p90_ms,
                               double& p99_ms,
                               double& mean_ms,
                               double& stddev_ms,
                               double& throughput_ops_s,
                               std::string& clock_source,
                               std::string& platform,
                               std::string& perf_capability,
                               std::string& perf_counters,
                               std::string& perf_reason) {
  warmup_iterations = 0;
  measure_iterations = 0;
  samples = 0;
  p50_ms = p90_ms = p99_ms = mean_ms = stddev_ms = throughput_ops_s = 0.0;
  clock_source.clear();
  platform.clear();
  perf_capability.clear();
  perf_counters.clear();
  perf_reason.clear();
  std::size_t pos = s.find("NEBULA_BENCH");
  if (pos == std::string::npos) return false;
  std::size_t end = s.find('\n', pos);
  std::string line = (end == std::string::npos) ? s.substr(pos) : s.substr(pos, end - pos);

  auto take = [&](const std::string& key, double& out) -> bool {
    std::size_t k = line.find(key + "=");
    if (k == std::string::npos) return false;
    k += key.size() + 1;
    std::size_t j = k;
    while (j < line.size() &&
           (std::isdigit((unsigned char)line[j]) || line[j] == '.' || line[j] == 'e' ||
            line[j] == 'E' || line[j] == '+' || line[j] == '-')) {
      j++;
    }
    try {
      out = std::stod(line.substr(k, j - k));
      return true;
    } catch (...) {
      out = 0.0;
      return false;
    }
  };

  auto take_int = [&](const std::string& key, int& out) -> bool {
    std::size_t k = line.find(key + "=");
    if (k == std::string::npos) return false;
    k += key.size() + 1;
    std::size_t j = k;
    while (j < line.size() &&
           (std::isdigit(static_cast<unsigned char>(line[j])) || line[j] == '-')) {
      j++;
    }
    try {
      out = std::stoi(line.substr(k, j - k));
      return true;
    } catch (...) {
      out = 0;
      return false;
    }
  };

  auto take_string = [&](const std::string& key, std::string& out) -> bool {
    std::size_t k = line.find(key + "=");
    if (k == std::string::npos) return false;
    k += key.size() + 1;
    std::size_t j = k;
    while (j < line.size() && !std::isspace(static_cast<unsigned char>(line[j]))) j++;
    out = line.substr(k, j - k);
    return !out.empty();
  };

  const bool ok = take_int("warmup_iterations", warmup_iterations) &&
                  take_int("measure_iterations", measure_iterations) &&
                  take_int("samples", samples) &&
                  take("p50_ms", p50_ms) &&
                  take("p90_ms", p90_ms) &&
                  take("p99_ms", p99_ms) &&
                  take("mean_ms", mean_ms) &&
                  take("stddev_ms", stddev_ms) &&
                  take("throughput_ops_s", throughput_ops_s) &&
                  take_string("clock", clock_source) &&
                  take_string("platform", platform) &&
                  take_string("perf_capability", perf_capability) &&
                  take_string("perf_counters", perf_counters) &&
                  take_string("perf_reason", perf_reason);
  return ok;
}

static const char* rep_name(nebula::passes::RepKind r) {
  switch (r) {
  case nebula::passes::RepKind::Stack: return "Stack";
  case nebula::passes::RepKind::Region: return "Region";
  case nebula::passes::RepKind::Heap: return "Heap";
  }
  return "Stack";
}

static const char* owner_name(nebula::passes::OwnerKind o) {
  switch (o) {
  case nebula::passes::OwnerKind::None: return "None";
  case nebula::passes::OwnerKind::Unique: return "Unique";
  case nebula::passes::OwnerKind::Shared: return "Shared";
  }
  return "None";
}

static std::unordered_map<nebula::nir::VarId, std::string> collect_var_names(const nebula::nir::Function& fn) {
  std::unordered_map<nebula::nir::VarId, std::string> out;
  for (const auto& p : fn.params) out.insert({p.var, p.name});

  std::function<void(const nebula::nir::Block&)> walk_block = [&](const nebula::nir::Block& b) {
    for (const auto& s : b.stmts) {
      std::visit(
          [&](auto&& st) {
            using S = std::decay_t<decltype(st)>;
            if constexpr (std::is_same_v<S, nebula::nir::Stmt::Let>) {
              out.insert({st.var, st.name});
            } else if constexpr (std::is_same_v<S, nebula::nir::Stmt::For>) {
              out.insert({st.var, st.var_name});
              walk_block(st.body);
            } else if constexpr (std::is_same_v<S, nebula::nir::Stmt::If>) {
              walk_block(st.then_body);
              if (st.else_body.has_value()) walk_block(*st.else_body);
            } else if constexpr (std::is_same_v<S, nebula::nir::Stmt::Region>) {
              walk_block(st.body);
            } else if constexpr (std::is_same_v<S, nebula::nir::Stmt::Unsafe>) {
              walk_block(st.body);
            }
          },
          s.node);
    }
  };

  if (fn.body.has_value()) walk_block(*fn.body);
  return out;
}

static std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
    case '\\': out += "\\\\"; break;
    case '"': out += "\\\""; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default: out.push_back(c); break;
    }
  }
  return out;
}

static int risk_rank(nebula::frontend::DiagnosticRisk r) {
  using nebula::frontend::DiagnosticRisk;
  switch (r) {
  case DiagnosticRisk::Unknown: return 0;
  case DiagnosticRisk::Low: return 1;
  case DiagnosticRisk::Medium: return 2;
  case DiagnosticRisk::High: return 3;
  case DiagnosticRisk::Critical: return 4;
  }
  return 0;
}

static std::string category_from_code(const std::string& code) {
  if (code.rfind("NBL-T", 0) == 0) return "typecheck";
  if (code.rfind("NBL-PAR", 0) == 0) return "parser";
  if (code.rfind("NBL-R", 0) == 0) return "memory-safety";
  if (code.rfind("NBL-U", 0) == 0) return "unsafe-boundary";
  if (code.rfind("NBL-S", 0) == 0) return "safety";
  if (code.rfind("NBL-P", 0) == 0) return "performance";
  if (code.rfind("NBL-C", 0) == 0) return "complexity";
  if (code.rfind("NBL-A", 0) == 0) return "api-lifecycle";
  if (code.rfind("NBL-X", 0) == 0) return "predictive";
  if (code.rfind("NBL-CLI", 0) == 0) return "cli";
  return "general";
}

static nebula::frontend::DiagnosticRisk default_risk_for_diag(const nebula::frontend::Diagnostic& d) {
  using nebula::frontend::DiagnosticRisk;
  if (d.code == "NBL-R001") return DiagnosticRisk::High;
  if (d.code == "NBL-P010") return DiagnosticRisk::High;
  if (d.code.rfind("NBL-X", 0) == 0) return DiagnosticRisk::High;
  if (d.code.rfind("NBL-U", 0) == 0) return DiagnosticRisk::High;
  if (d.code.rfind("NBL-P", 0) == 0) return DiagnosticRisk::Medium;
  if (d.code.rfind("NBL-A", 0) == 0) return DiagnosticRisk::Medium;
  if (d.code.rfind("NBL-C", 0) == 0) return DiagnosticRisk::Medium;
  if (d.code.rfind("NBL-T", 0) == 0 || d.code.rfind("NBL-PAR", 0) == 0) return DiagnosticRisk::High;
  if (d.code.rfind("NBL-S", 0) == 0) return DiagnosticRisk::High;
  if (d.is_error()) return DiagnosticRisk::High;
  if (d.is_warning()) return DiagnosticRisk::Medium;
  if (d.is_note()) return DiagnosticRisk::Low;
  return DiagnosticRisk::Unknown;
}

static void enrich_diagnostic(nebula::frontend::Diagnostic& d,
                              nebula::frontend::DiagnosticStage stage) {
  using nebula::frontend::DiagnosticRisk;
  using nebula::frontend::DiagnosticStage;

  if (d.stage == DiagnosticStage::Unspecified) d.stage = stage;
  if (d.category.empty()) d.category = category_from_code(d.code);
  if (d.risk == DiagnosticRisk::Unknown) d.risk = default_risk_for_diag(d);

  if (d.code == "NBL-R001") {
    if (d.cause.empty()) d.cause = "region allocation escapes lexical lifetime";
    if (d.impact.empty()) d.impact = "allocation is promoted to heap or rejected in strict mode";
    if (d.suggestions.empty()) {
      d.suggestions.push_back("keep region-allocated values within region scope");
      d.suggestions.push_back("use explicit heap/promote when cross-scope ownership is intended");
    }
  } else if (d.code == "NBL-U001") {
    if (d.cause.empty()) d.cause = "safe context attempted to call an @unsafe callable";
    if (d.impact.empty()) {
      d.impact = "unsafe operation boundary is crossed without explicit audit context";
    }
    if (d.suggestions.empty()) {
      d.suggestions.push_back("wrap call in unsafe { ... }");
      d.suggestions.push_back("or annotate enclosing function with @unsafe after review");
    }
  } else if (d.code == "NBL-U002") {
    if (d.cause.empty()) d.cause = "@unsafe annotation target is not a function";
    if (d.impact.empty()) {
      d.impact = "unsafe contract boundary is undefined and cannot be enforced";
    }
    if (d.suggestions.empty()) {
      d.suggestions.push_back("remove @unsafe from non-function item");
      d.suggestions.push_back("apply @unsafe to a function declaration instead");
    }
  } else if (d.code == "NBL-T090") {
    if (d.cause.empty()) {
      d.cause = "multiple ref parameters overlap on the same mutable alias location in one call";
    }
    if (d.impact.empty()) {
      d.impact = "exclusive mutable borrow is violated and may permit aliasing writes";
    }
    if (d.suggestions.empty()) {
      d.suggestions.push_back("pass distinct storage roots to each ref parameter");
      d.suggestions.push_back("split the operation into separate calls");
    }
  } else if (d.code == "NBL-T091") {
    if (d.cause.empty()) {
      d.cause = "ref parameter and non-ref argument overlap on one mutable alias location";
    }
    if (d.impact.empty()) {
      d.impact = "mutable borrow may overlap read/write access in the same call";
    }
    if (d.suggestions.empty()) {
      d.suggestions.push_back("avoid using the same base value across ref and non-ref args");
      d.suggestions.push_back("compute non-ref values before passing ref arguments");
    }
  } else if (d.code == "NBL-T092") {
    if (d.cause.empty()) {
      d.cause = "same-statement access overlaps an earlier active ref borrow on the same alias location";
    }
    if (d.impact.empty()) {
      d.impact = "exclusive mutable borrow is violated within one statement";
    }
    if (d.suggestions.empty()) {
      d.suggestions.push_back("split conflicting accesses into separate statements");
      d.suggestions.push_back("reorder expression so reads/writes happen before ref borrow");
    }
  } else if (d.code == "NBL-T093") {
    if (d.cause.empty()) {
      d.cause =
          "later-statement read overlaps an active ref borrow from an earlier statement on the same alias location";
    }
    if (d.impact.empty()) {
      d.impact = "exclusive mutable borrow is violated across statement boundaries";
    }
    if (d.suggestions.empty()) {
      d.suggestions.push_back("move the read before the ref-taking call");
      d.suggestions.push_back("or isolate the ref-taking operation in a separate block");
    }
  } else if (d.code == "NBL-T094") {
    if (d.cause.empty()) {
      d.cause =
          "later-statement write overlaps an active ref borrow from an earlier statement on the same alias location";
    }
    if (d.impact.empty()) {
      d.impact = "exclusive mutable borrow is violated by overlapping writes";
    }
    if (d.suggestions.empty()) {
      d.suggestions.push_back("perform this write before the ref-taking call");
      d.suggestions.push_back("or end the borrow by splitting into separate blocks");
    }
  } else if (d.code == "NBL-T095") {
    if (d.cause.empty()) {
      d.cause =
          "later-statement ref borrow overlaps an active ref borrow from an earlier statement on the same alias location";
    }
    if (d.impact.empty()) {
      d.impact = "exclusive mutable borrow is violated by overlapping borrow windows";
    }
    if (d.suggestions.empty()) {
      d.suggestions.push_back("end the earlier borrow before taking another ref borrow");
      d.suggestions.push_back("or borrow disjoint storage roots");
    }
  } else if (d.code == "NBL-P001") {
    if (d.cause.empty()) d.cause = "loop body performs heap allocation";
    if (d.impact.empty()) d.impact = "may increase latency variance and allocator pressure";
    if (d.suggestions.empty()) {
      d.suggestions.push_back("hoist allocation outside loop or pre-allocate");
      d.suggestions.push_back("prefer stack/region allocation for non-escaping values");
    }
  } else if (d.code == "NBL-P010") {
    if (d.cause.empty()) d.cause = "shared ownership used in hot path";
    if (d.impact.empty()) d.impact = "reference counting overhead can reduce throughput";
    if (d.suggestions.empty()) {
      d.suggestions.push_back("prefer unique ownership in hot path");
      d.suggestions.push_back("move shared ownership boundaries to cold path");
    }
  } else if (d.code == "NBL-X001" || d.code == "NBL-X002" || d.code == "NBL-X003") {
    d.predictive = true;
    if (!d.confidence.has_value()) {
      if (d.code == "NBL-X001") {
        d.confidence = 0.85;
      } else if (d.code == "NBL-X002") {
        d.confidence = 0.60;
      } else {
        d.confidence = 0.72;
      }
    }
  }
}

static void append_diag(std::vector<nebula::frontend::Diagnostic>& out,
                        nebula::frontend::Diagnostic d,
                        nebula::frontend::DiagnosticStage stage) {
  enrich_diagnostic(d, stage);
  out.push_back(std::move(d));
}

static void append_diags(std::vector<nebula::frontend::Diagnostic>& out,
                         const std::vector<nebula::frontend::Diagnostic>& src,
                         nebula::frontend::DiagnosticStage stage) {
  for (auto d : src) append_diag(out, std::move(d), stage);
}

static void sort_diagnostics_stable(std::vector<nebula::frontend::Diagnostic>& diags) {
  std::stable_sort(diags.begin(), diags.end(),
                   [](const nebula::frontend::Diagnostic& lhs,
                      const nebula::frontend::Diagnostic& rhs) {
                     const std::string_view lhs_path = lhs.span.source_path;
                     const std::string_view rhs_path = rhs.span.source_path;
                     if (lhs_path != rhs_path) return lhs_path < rhs_path;
                     if (lhs.span.start.line != rhs.span.start.line) {
                       return lhs.span.start.line < rhs.span.start.line;
                     }
                     if (lhs.span.start.col != rhs.span.start.col) {
                       return lhs.span.start.col < rhs.span.start.col;
                     }
                     if (lhs.code != rhs.code) return lhs.code < rhs.code;
                     if (lhs.message != rhs.message) return lhs.message < rhs.message;
                     if (lhs.stage != rhs.stage) {
                       return static_cast<int>(lhs.stage) < static_cast<int>(rhs.stage);
                     }
                     return static_cast<int>(lhs.severity) < static_cast<int>(rhs.severity);
                   });
}

void emit_diagnostics(const std::vector<nebula::frontend::Diagnostic>& diags, const CliOptions& opt,
                      std::ostream& os) {
  const std::uint32_t base_warn_mask = base_warn_class_mask_for_policy(opt.warn_policy);
  std::uint32_t effective_warn_mask = base_warn_mask;
  effective_warn_mask &= ~opt.warn_class_force_off_mask;
  effective_warn_mask |= opt.warn_class_force_on_mask;
  const std::uint32_t effective_gate_mask = effective_gate_dimension_mask(opt);
  const int gate_threshold = gate_threshold_for_profile(opt.gate_profile);

  auto is_suppressed_warning = [&](const nebula::frontend::Diagnostic& d) {
    if (!d.is_warning()) return false;
    return (effective_warn_mask & warning_class_bit_for_code(d.code)) == 0;
  };

  auto emit_suppressed_note = [&](std::ostream& out,
                                  std::size_t suppressed_api,
                                  std::size_t suppressed_perf,
                                  std::size_t suppressed_best,
                                  std::size_t suppressed_safe,
                                  std::size_t suppressed_other) {
    const std::size_t suppressed_total =
        suppressed_api + suppressed_perf + suppressed_best + suppressed_safe + suppressed_other;
    if (suppressed_total == 0) return;

    const bool legacy_lenient_only =
        (opt.warn_policy == WarnPolicy::Lenient) && opt.warn_class_force_off_mask == 0 &&
        opt.warn_class_force_on_mask == 0 &&
        suppressed_total == suppressed_best && suppressed_best > 0;
    if (legacy_lenient_only) {
      out << "note: " << suppressed_total
          << " warning(s) hidden by --warn-policy lenient (best-practice-drift)\n";
      return;
    }

    out << "note: " << suppressed_total
        << " warning(s) hidden by warning class policy"
        << " (api-deprecation=" << suppressed_api
        << ", performance-risk=" << suppressed_perf
        << ", best-practice-drift=" << suppressed_best
        << ", safety-risk=" << suppressed_safe
        << ", general-warning=" << suppressed_other << ")\n";
  };

  const AnalysisTier tier = resolve_analysis_tier(opt.mode, opt.analysis_tier);
  DiagView view = opt.diag_view;
  if (view == DiagView::Auto) {
    view = (tier == AnalysisTier::Basic) ? DiagView::Raw : DiagView::Grouped;
  }
  const bool grouped_view = (view == DiagView::Grouped);
  const bool rc_v2_enabled = grouped_view && root_cause_v2_enabled(opt);

  auto emit_json_diag = [&](std::ostream& out,
                            const nebula::frontend::Diagnostic& d,
                            bool annotate_group,
                            bool is_root,
                            std::string_view diag_id,
                            std::string_view caused_by,
                            std::string_view derivation_reason,
                            std::size_t rank_score,
                            std::size_t derived_count,
                            std::string_view root_group_override) {
    out << "{";
    out << "\"severity\":\"" << nebula::frontend::severity_name(d.severity) << "\",";
    out << "\"stage\":\"" << nebula::frontend::stage_name(d.stage) << "\",";
    out << "\"risk\":\"" << nebula::frontend::risk_name(d.risk) << "\",";
    out << "\"category\":\"" << json_escape(d.category) << "\",";
    out << "\"code\":\"" << json_escape(d.code) << "\",";
    out << "\"message\":\"" << json_escape(d.message) << "\",";
    out << "\"path\":\"" << json_escape(d.span.source_path) << "\",";
    out << "\"line\":" << d.span.start.line << ",";
    out << "\"col\":" << d.span.start.col << ",";
    out << "\"end_line\":" << d.span.end.line << ",";
    out << "\"end_col\":" << d.span.end.col << ",";
    out << "\"cause\":\"" << json_escape(d.cause) << "\",";
    out << "\"impact\":\"" << json_escape(d.impact) << "\",";
    out << "\"machine_reason\":\"" << json_escape(d.machine_reason) << "\",";
    out << "\"machine_subreason\":\"" << json_escape(d.machine_subreason) << "\",";
    out << "\"machine_detail\":\"" << json_escape(d.machine_detail) << "\",";
    out << "\"machine_trigger_family\":\"" << json_escape(d.machine_trigger_family) << "\",";
    out << "\"machine_trigger_family_detail\":\""
        << json_escape(d.machine_trigger_family_detail) << "\",";
    out << "\"machine_trigger_subreason\":\"" << json_escape(d.machine_trigger_subreason)
        << "\",";
    out << "\"machine_owner\":\"" << json_escape(d.machine_owner) << "\",";
    out << "\"machine_owner_reason\":\"" << json_escape(d.machine_owner_reason) << "\",";
    out << "\"machine_owner_reason_detail\":\"" << json_escape(d.machine_owner_reason_detail)
        << "\",";
    out << "\"caused_by_code\":\"" << json_escape(d.caused_by_code) << "\",";
    out << "\"predictive\":" << (d.predictive ? "true" : "false") << ",";
    if (d.confidence.has_value()) {
      out << "\"confidence\":" << *d.confidence << ",";
    } else {
      out << "\"confidence\":null,";
    }
    out << "\"warning_dimension\":\""
        << json_escape(d.is_warning() ? warning_dimension_for_code(d.code) : "none")
        << "\",";
    out << "\"warning_reason\":\""
        << json_escape(d.is_warning() ? warning_reason_for_diag(d) : "none")
        << "\",";
    out << "\"gate_weight\":" << (d.is_warning() ? gate_weight_for_warning(d) : 0) << ",";
    out << "\"suggestions\":[";
    for (std::size_t j = 0; j < d.suggestions.size(); ++j) {
      if (j) out << ",";
      out << "\"" << json_escape(d.suggestions[j]) << "\"";
    }
    out << "],";
    out << "\"related_spans\":[";
    for (std::size_t j = 0; j < d.related_spans.size(); ++j) {
      if (j) out << ",";
      const auto& rs = d.related_spans[j];
      out << "{"
          << "\"path\":\"" << json_escape(rs.source_path) << "\","
          << "\"line\":" << rs.start.line << ","
          << "\"col\":" << rs.start.col << ","
          << "\"end_line\":" << rs.end.line << ","
          << "\"end_col\":" << rs.end.col << "}";
    }
    out << "]";
    if (!diag_id.empty()) out << ",\"diag_id\":\"" << json_escape(std::string(diag_id)) << "\"";
    if (annotate_group) {
      const std::string root_group =
          root_group_override.empty() ? root_group_key(d) : std::string(root_group_override);
      out << ",\"root_group\":\"" << json_escape(root_group) << "\"";
      out << ",\"is_root\":" << (is_root ? "true" : "false");
      if (d.is_warning()) {
        out << ",\"warning_class\":\"" << warning_class_for_code(d.code) << "\"";
      }
    }
    if (is_root) {
      out << ",\"rank_score\":" << rank_score;
      out << ",\"derived_count\":" << derived_count;
    }
    if (!caused_by.empty()) out << ",\"caused_by\":\"" << json_escape(std::string(caused_by)) << "\"";
    if (!derivation_reason.empty()) {
      out << ",\"derivation_reason\":\"" << json_escape(std::string(derivation_reason)) << "\"";
    }
    out << "}";
  };

  auto emit_text_raw = [&](std::ostream& out) {
    std::size_t suppressed_api = 0;
    std::size_t suppressed_perf = 0;
    std::size_t suppressed_best = 0;
    std::size_t suppressed_safe = 0;
    std::size_t suppressed_other = 0;
    for (const auto& d : diags) {
      if (is_suppressed_warning(d)) {
        const std::string_view cls = warning_class_for_code(d.code);
        if (cls == "api-deprecation") {
          ++suppressed_api;
        } else if (cls == "performance-risk") {
          ++suppressed_perf;
        } else if (cls == "best-practice-drift") {
          ++suppressed_best;
        } else if (cls == "safety-risk") {
          ++suppressed_safe;
        } else {
          ++suppressed_other;
        }
        continue;
      }
      nebula::frontend::print_diagnostic(out, d);
    }
    emit_suppressed_note(out, suppressed_api, suppressed_perf, suppressed_best, suppressed_safe,
                         suppressed_other);
  };

  if (view == DiagView::Raw || diags.empty()) {
    if (opt.diag_format == DiagFormat::Json) {
      os << "[\n";
      bool first = true;
      for (const auto& d : diags) {
        if (is_suppressed_warning(d)) continue;
        if (!first) os << ",\n";
        os << "  ";
        emit_json_diag(os, d, false, false, "", "", "", 0, 0, "");
        first = false;
      }
      os << "\n]\n";
      return;
    }
    emit_text_raw(os);
    return;
  }

  struct RootBucket {
    const nebula::frontend::Diagnostic* root = nullptr;
    std::string group_key;
    std::vector<const nebula::frontend::Diagnostic*> same_group_derived;
    std::size_t first_seen = 0;
    std::size_t rank_score = 0;
    std::size_t derived_count = 0;
    std::string id;
  };
  struct DerivedEntry {
    const nebula::frontend::Diagnostic* diag = nullptr;
    std::optional<std::size_t> parent_root_index;
    std::string reason;
    std::string id;
    std::string group_key;
  };
  struct TopRootCauseEntry {
    struct PriorityFixPlanStep {
      std::string category;
      std::string reason;
      int boost = 0;
      std::size_t count = 0;
      std::string action;
    };
    struct OwnerPriorityPlanStep {
      std::string reason;
      int boost = 0;
      std::size_t count = 0;
      std::string action;
    };
    std::size_t root_index = 0;
    std::string id;
    std::size_t score = 0;
    int risk_rank_value = 0;
    std::size_t covered_count = 0;
    std::size_t stage_preflight = 0;
    std::size_t stage_build = 0;
    std::size_t stage_other = 0;
    std::size_t dim_api = 0;
    std::size_t dim_perf = 0;
    std::size_t dim_best = 0;
    std::size_t dim_safe = 0;
    std::size_t dim_general = 0;
    std::string why_selected;
    std::string first_fix_hint;
    std::vector<std::string> followup_codes;
    std::vector<std::pair<std::string, std::string>> followup_fix_hints;
    std::size_t followup_count = 0;
    std::string priority_chain_summary;
    std::string priority_machine_reason;
    int priority_machine_boost = 0;
    std::string priority_machine_action;
    std::string priority_epistemic_reason;
    int priority_epistemic_boost = 0;
    std::string priority_epistemic_action;
    std::string priority_owner_reason;
    int priority_owner_boost = 0;
    int priority_owner_weight = 0;
    std::string priority_owner_action;
    std::vector<OwnerPriorityPlanStep> priority_owner_plan;
    std::vector<PriorityFixPlanStep> priority_fix_plan;
    std::size_t r001_reason_return = 0;
    std::size_t r001_reason_call = 0;
    std::size_t r001_reason_field = 0;
    std::vector<std::pair<std::string, std::size_t>> r001_subreason_counts;
    std::vector<std::pair<std::string, std::size_t>> r001_detail_counts;
    std::vector<std::pair<std::string, std::size_t>> owner_reason_counts;
    std::string stable_key;
  };

  using GroupingClock = std::chrono::steady_clock;
  auto elapsed_ms = [](GroupingClock::time_point begin, GroupingClock::time_point end) -> std::size_t {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();
    if (ms <= 0) return 0;
    return static_cast<std::size_t>(ms);
  };
  auto replace_marker = [](std::string& text, const std::string& marker, std::size_t value) {
    const std::string value_text = std::to_string(value);
    std::size_t pos = 0;
    while (true) {
      pos = text.find(marker, pos);
      if (pos == std::string::npos) break;
      text.replace(pos, marker.size(), value_text);
      pos += value_text.size();
    }
  };

  const std::string kGroupingEmitMsMarker = "__GROUPING_EMIT_MS__";
  const std::string kGroupingTotalMsMarker = "__GROUPING_TOTAL_MS__";
  const auto grouping_started = GroupingClock::now();
  const auto index_started = grouping_started;
  std::size_t grouping_index_ms = 0;
  std::size_t grouping_rank_ms = 0;
  std::size_t grouping_root_cause_v2_ms = 0;

  std::unordered_map<std::string, std::size_t> bucket_by_key;
  bucket_by_key.reserve(diags.size() + 1);
  std::vector<RootBucket> buckets;
  buckets.reserve(diags.size());
  std::size_t suppressed = 0;
  std::size_t suppressed_api = 0;
  std::size_t suppressed_perf = 0;
  std::size_t suppressed_best = 0;
  std::size_t suppressed_safe = 0;
  std::size_t suppressed_other = 0;
  std::size_t seen_order = 0;

  bool budget_exceeded = false;
  for (const auto& d : diags) {
    if (opt.diag_grouping_delay_ms > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(opt.diag_grouping_delay_ms));
    }
    if (is_suppressed_warning(d)) {
      ++suppressed;
      const std::string_view cls = warning_class_for_code(d.code);
      if (cls == "api-deprecation") {
        ++suppressed_api;
      } else if (cls == "performance-risk") {
        ++suppressed_perf;
      } else if (cls == "best-practice-drift") {
        ++suppressed_best;
      } else if (cls == "safety-risk") {
        ++suppressed_safe;
      } else {
        ++suppressed_other;
      }
      continue;
    }

    const std::string key = root_group_key(d);
    auto it = bucket_by_key.find(key);
    if (it == bucket_by_key.end()) {
      RootBucket b;
      b.root = &d;
      b.group_key = key;
      b.first_seen = seen_order++;
      bucket_by_key.emplace(key, buckets.size());
      buckets.push_back(std::move(b));
    } else {
      buckets[it->second].same_group_derived.push_back(&d);
    }

    if (opt.diag_budget_ms > 0) {
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          GroupingClock::now() - index_started);
      if (elapsed.count() > opt.diag_budget_ms) {
        budget_exceeded = true;
        break;
      }
    }
  }
  grouping_index_ms = elapsed_ms(index_started, GroupingClock::now());

  if (budget_exceeded) {
    if (opt.diag_format == DiagFormat::Json) {
      const auto emit_started = GroupingClock::now();
      std::ostringstream out_json;
      out_json << "{\n";
      out_json << "  \"summary\": {\"budget_exceeded\": true, \"budget_ms\": " << opt.diag_budget_ms
               << ",\"grouping_total_ms\":" << kGroupingTotalMsMarker
               << ",\"grouping_index_ms\":" << grouping_index_ms
               << ",\"grouping_rank_ms\":" << grouping_rank_ms
               << ",\"grouping_root_cause_v2_ms\":" << grouping_root_cause_v2_ms
               << ",\"grouping_emit_ms\":" << kGroupingEmitMsMarker
               << ",\"grouping_budget_fallback\":true},\n";
      out_json << "  \"all\": [\n";
      bool first = true;
      for (const auto& d : diags) {
        if (is_suppressed_warning(d)) continue;
        if (!first) out_json << ",\n";
        out_json << "    ";
        emit_json_diag(out_json, d, false, false, "", "", "", 0, 0, "");
        first = false;
      }
      out_json << "\n  ]\n";
      out_json << "}\n";

      std::string rendered = out_json.str();
      const std::size_t grouping_emit_ms = elapsed_ms(emit_started, GroupingClock::now());
      const std::size_t grouping_total_ms = elapsed_ms(grouping_started, GroupingClock::now());
      replace_marker(rendered, kGroupingEmitMsMarker, grouping_emit_ms);
      replace_marker(rendered, kGroupingTotalMsMarker, grouping_total_ms);
      os << rendered;
    } else {
      const auto emit_started = GroupingClock::now();
      std::ostringstream out_text;
      out_text << "note: diagnostic grouping budget exceeded (" << opt.diag_budget_ms
               << "ms); falling back to raw diagnostics\n";
      emit_text_raw(out_text);
      out_text << "grouping-perf: total-ms=" << kGroupingTotalMsMarker
               << " index-ms=" << grouping_index_ms
               << " rank-ms=" << grouping_rank_ms
               << " root-cause-v2-ms=" << grouping_root_cause_v2_ms
               << " emit-ms=" << kGroupingEmitMsMarker
               << " budget-fallback=on\n";

      std::string rendered = out_text.str();
      const std::size_t grouping_emit_ms = elapsed_ms(emit_started, GroupingClock::now());
      const std::size_t grouping_total_ms = elapsed_ms(grouping_started, GroupingClock::now());
      replace_marker(rendered, kGroupingEmitMsMarker, grouping_emit_ms);
      replace_marker(rendered, kGroupingTotalMsMarker, grouping_total_ms);
      os << rendered;
    }
    return;
  }

  const auto rank_started = GroupingClock::now();

  auto severity_rank = [](const nebula::frontend::Diagnostic& d) -> int {
    if (d.is_error()) return 3;
    if (d.is_warning()) return 2;
    if (d.is_note()) return 1;
    return 0;
  };
  for (auto& b : buckets) {
    int score = 0;
    score += severity_rank(*b.root) * 100;
    score += risk_rank(b.root->risk) * 20;
    score += static_cast<int>(b.same_group_derived.size()) * 10;
    if (b.root->predictive) score += 5;
    if (b.root->confidence.has_value()) {
      score += static_cast<int>((*b.root->confidence) * 10.0);
    }
    if (score < 0) score = 0;
    b.rank_score = static_cast<std::size_t>(score);
  }
  std::sort(buckets.begin(), buckets.end(), [](const RootBucket& lhs, const RootBucket& rhs) {
    if (lhs.rank_score != rhs.rank_score) return lhs.rank_score > rhs.rank_score;
    if (lhs.first_seen != rhs.first_seen) return lhs.first_seen < rhs.first_seen;
    return lhs.group_key < rhs.group_key;
  });

  std::size_t truncated_roots = 0;
  std::vector<RootBucket> overflow_buckets;
  if (opt.max_root_causes > 0) {
    const std::size_t max_roots = static_cast<std::size_t>(opt.max_root_causes);
    if (buckets.size() > max_roots) {
      truncated_roots = buckets.size() - max_roots;
      overflow_buckets.assign(
          std::make_move_iterator(buckets.begin() + static_cast<std::ptrdiff_t>(max_roots)),
          std::make_move_iterator(buckets.end()));
      buckets.resize(max_roots);
    }
  }

  for (std::size_t i = 0; i < buckets.size(); ++i) {
    buckets[i].id = "r" + std::to_string(i + 1);
  }

  std::vector<DerivedEntry> derived_entries;
  derived_entries.reserve(diags.size());
  for (std::size_t i = 0; i < buckets.size(); ++i) {
    for (const auto* d : buckets[i].same_group_derived) {
      DerivedEntry e;
      e.diag = d;
      e.parent_root_index = i;
      if (!d->caused_by_code.empty() && d->caused_by_code == buckets[i].root->code) {
        e.reason = "caused-by-code";
      } else {
        e.reason = "same-root-group";
      }
      e.group_key = buckets[i].group_key;
      derived_entries.push_back(std::move(e));
    }
  }
  for (const auto& b : overflow_buckets) {
    if (b.root) {
      DerivedEntry e;
      e.diag = b.root;
      if (!buckets.empty()) e.parent_root_index = 0;
      e.reason = "truncated-root-overflow";
      e.group_key = b.group_key;
      derived_entries.push_back(std::move(e));
    }
    for (const auto* d : b.same_group_derived) {
      DerivedEntry e;
      e.diag = d;
      if (!buckets.empty()) e.parent_root_index = 0;
      e.reason = "truncated-root-overflow";
      e.group_key = b.group_key;
      derived_entries.push_back(std::move(e));
    }
  }
  for (std::size_t i = 0; i < derived_entries.size(); ++i) {
    derived_entries[i].id = "d" + std::to_string(i + 1);
    if (derived_entries[i].parent_root_index.has_value()) {
      buckets[*derived_entries[i].parent_root_index].derived_count += 1;
    }
  }

  std::size_t causal_edges = 0;
  for (const auto& e : derived_entries) {
    if (e.parent_root_index.has_value()) causal_edges += 1;
  }

  std::vector<std::vector<std::string>> root_followup_codes(buckets.size());
  for (const auto& e : derived_entries) {
    if (!e.parent_root_index.has_value()) continue;
    if (e.reason != "caused-by-code") continue;
    if (e.diag == nullptr) continue;
    auto& codes = root_followup_codes[*e.parent_root_index];
    if (std::find(codes.begin(), codes.end(), e.diag->code) == codes.end()) {
      codes.push_back(e.diag->code);
    }
  }
  auto span_close = [](const nebula::frontend::Diagnostic& lhs,
                       const nebula::frontend::Diagnostic& rhs) -> bool {
    const int line_diff = (lhs.span.start.line >= rhs.span.start.line)
                              ? (lhs.span.start.line - rhs.span.start.line)
                              : (rhs.span.start.line - lhs.span.start.line);
    const int col_diff = (lhs.span.start.col >= rhs.span.start.col)
                             ? (lhs.span.start.col - rhs.span.start.col)
                             : (rhs.span.start.col - lhs.span.start.col);
    return line_diff <= 1 && col_diff <= 8;
  };
  auto near_duplicate = [&](const nebula::frontend::Diagnostic& lhs,
                            const nebula::frontend::Diagnostic& rhs) -> bool {
    if (lhs.code != rhs.code) return false;
    return span_close(lhs, rhs);
  };

  std::unordered_map<std::string, std::vector<const nebula::frontend::Diagnostic*>>
      caused_by_index;
  caused_by_index.reserve(diags.size());
  for (const auto& d : diags) {
    if (d.caused_by_code.empty()) continue;
    if (d.caused_by_code == d.code) continue;
    caused_by_index[d.caused_by_code].push_back(&d);
  }
  auto causal_chain_diags_for = [&](const nebula::frontend::Diagnostic& root_diag)
      -> std::vector<const nebula::frontend::Diagnostic*> {
    std::vector<const nebula::frontend::Diagnostic*> ordered;
    if (root_diag.code.empty()) return ordered;

    std::deque<const nebula::frontend::Diagnostic*> pending;
    std::unordered_set<const nebula::frontend::Diagnostic*> seen_diags;
    std::unordered_set<std::string> seen_codes;

    auto enqueue_children = [&](const nebula::frontend::Diagnostic& parent,
                                bool span_scoped,
                                std::size_t max_codes) {
      auto it = caused_by_index.find(parent.code);
      if (it == caused_by_index.end()) return;
      for (const auto* child : it->second) {
        if (child == nullptr) continue;
        if (span_scoped && !span_close(*child, parent)) continue;
        if (!seen_diags.insert(child).second) continue;
        if (seen_codes.insert(child->code).second && ordered.size() < max_codes) {
          ordered.push_back(child);
        }
        pending.push_back(child);
      }
    };
    constexpr std::size_t kMaxChainCodes = 8;
    enqueue_children(root_diag, true, kMaxChainCodes);
    while (!pending.empty() && ordered.size() < kMaxChainCodes) {
      const auto* cur = pending.front();
      pending.pop_front();
      if (cur == nullptr) continue;
      enqueue_children(*cur, true, kMaxChainCodes);
    }
    return ordered;
  };
  auto risk_base_score = [](nebula::frontend::DiagnosticRisk r) -> int {
    using nebula::frontend::DiagnosticRisk;
    switch (r) {
    case DiagnosticRisk::Critical: return 45;
    case DiagnosticRisk::High: return 35;
    case DiagnosticRisk::Medium: return 22;
    case DiagnosticRisk::Low: return 10;
    case DiagnosticRisk::Unknown: return 8;
    }
    return 8;
  };
  auto dimension_boost = [](std::string_view dim) -> int {
    if (dim == "safety-contract") return 10;
    if (dim == "perf-runtime") return 7;
    if (dim == "api-lifecycle") return 5;
    if (dim == "best-practice-drift") return 2;
    return 1;
  };
  auto dim_count_add = [](const nebula::frontend::Diagnostic& d,
                          std::size_t& api,
                          std::size_t& perf,
                          std::size_t& best,
                          std::size_t& safe,
                          std::size_t& general) {
    if (!d.is_warning()) return;
    const std::string_view dim = warning_dimension_for_code(d.code);
    if (dim == "api-lifecycle") {
      ++api;
    } else if (dim == "perf-runtime") {
      ++perf;
    } else if (dim == "best-practice-drift") {
      ++best;
    } else if (dim == "safety-contract") {
      ++safe;
    } else {
      ++general;
    }
  };
  auto r001_reason_count_add = [](const nebula::frontend::Diagnostic& d,
                                  std::size_t& reason_return,
                                  std::size_t& reason_call,
                                  std::size_t& reason_field,
                                  std::unordered_map<std::string, std::size_t>& subreasons,
                                  std::unordered_map<std::string, std::size_t>& details,
                                  std::unordered_set<std::string>& seen_keys) {
    if (d.code != "NBL-R001") return;
    const std::string span_key =
        std::to_string(d.span.start.line) + ":" + std::to_string(d.span.start.col);
    if (d.machine_reason == "return") {
      if (seen_keys.insert("reason:return@" + span_key).second) ++reason_return;
    } else if (d.machine_reason == "call") {
      if (seen_keys.insert("reason:call@" + span_key).second) ++reason_call;
    } else if (d.machine_reason == "field") {
      if (seen_keys.insert("reason:field@" + span_key).second) ++reason_field;
    }
    if (!d.machine_subreason.empty() &&
        seen_keys.insert("subreason:" + d.machine_subreason + "@" + span_key).second) {
      ++subreasons[d.machine_subreason];
    }
    std::string machine_detail = d.machine_detail;
    if (machine_detail.empty() && !d.machine_reason.empty() && !d.machine_subreason.empty()) {
      machine_detail = d.machine_reason + "/" + d.machine_subreason;
    }
    if (!machine_detail.empty() &&
        seen_keys.insert("detail:" + machine_detail + "@" + span_key).second) {
      ++details[machine_detail];
    }
  };
  auto normalize_owner_reason = [](std::string_view reason) -> std::string_view {
    if (reason == "cross-function-return-path-unknown") {
      return "cross-function-return-path-unknown-no-summary";
    }
    return reason;
  };
  auto owner_reason_priority_signal_for = [](std::string_view reason) -> std::pair<int, std::string> {
    if (reason == "cross-function-return-path-unknown") {
      reason = "cross-function-return-path-unknown-no-summary";
    }
    if (reason == "alias-fanout") return {11, "alias-fanout"};
    if (reason == "cross-function-return-path-alias-fanout") {
      return {10, "cross-function-return-path-alias-fanout"};
    }
    if (reason == "cross-function-return-path-alias-fanout-mixed") {
      return {9, "cross-function-return-path-alias-fanout-mixed"};
    }
    if (reason == "cross-function-return-path-unknown-indirect-unresolved") {
      return {8, "cross-function-return-path-unknown-indirect-unresolved"};
    }
    if (reason == "cross-function-return-path-unknown-external-opaque") {
      return {7, "cross-function-return-path-unknown-external-opaque"};
    }
    if (reason == "cross-function-return-path-unknown-no-summary") {
      return {6, "cross-function-return-path-unknown-no-summary"};
    }
    if (reason == "cross-function-return-path-fanin") {
      return {3, "cross-function-return-path-fanin"};
    }
    return {0, ""};
  };
  auto owner_reason_count_add = [&](const nebula::frontend::Diagnostic& d,
                                    std::unordered_map<std::string, std::size_t>& owner_reasons,
                                    std::unordered_set<std::string>& seen_keys) {
    auto make_seen_key = [&](std::string_view reason) -> std::string {
      std::string key;
      key.reserve(reason.size() + 32);
      key.append(reason);
      key.push_back('#');
      key.append(std::to_string(d.span.start.line));
      key.push_back(':');
      key.append(std::to_string(d.span.start.col));
      return key;
    };
    auto add_reason_once = [&](std::string_view reason,
                               std::unordered_set<std::string>& seen) {
      if (reason.empty()) return;
      const auto it = seen.insert(std::string(reason));
      if (!it.second) return;
      const auto seen_key = make_seen_key(*it.first);
      if (!seen_keys.insert(seen_key).second) return;
      ++owner_reasons[*it.first];
    };
    std::unordered_set<std::string> seen;
    add_reason_once(normalize_owner_reason(d.machine_owner_reason), seen);
    if (!d.machine_owner_reason_detail.empty()) {
      std::size_t begin = 0;
      while (begin <= d.machine_owner_reason_detail.size()) {
        const std::size_t sep = d.machine_owner_reason_detail.find('|', begin);
        const std::size_t end =
            (sep == std::string::npos) ? d.machine_owner_reason_detail.size() : sep;
        const std::string_view reason(
            d.machine_owner_reason_detail.data() + begin, end - begin);
        add_reason_once(normalize_owner_reason(reason), seen);
        if (sep == std::string::npos) break;
        begin = sep + 1;
      }
    }
    const std::string_view unknown_subreason = normalize_owner_reason(d.machine_subreason);
    if (unknown_subreason == "cross-function-return-path-unknown-indirect-unresolved" ||
        unknown_subreason == "cross-function-return-path-unknown-external-opaque" ||
        unknown_subreason == "cross-function-return-path-unknown-no-summary") {
      add_reason_once(unknown_subreason, seen);
    }
  };
  auto owner_reason_priority_action = [](std::string_view reason) -> std::string {
    if (reason == "cross-function-return-path-unknown") {
      reason = "cross-function-return-path-unknown-no-summary";
    }
    if (reason == "alias-fanout") {
      return "collapse escaping aliases to one owner path before tuning unknown paths";
    }
    if (reason == "cross-function-return-path-alias-fanout") {
      return "collapse return-path alias fanout before resolving unknown return paths";
    }
    if (reason == "cross-function-return-path-alias-fanout-mixed") {
      return "collapse mixed return-path/non-return fanout before resolving unknown return paths";
    }
    if (reason == "cross-function-return-path-unknown-indirect-unresolved") {
      return "resolve indirect call targets for return paths next";
    }
    if (reason == "cross-function-return-path-unknown-external-opaque") {
      return "add escape summary stubs for opaque external callees next";
    }
    if (reason == "cross-function-return-path-unknown-no-summary") {
      return "add missing function escape summaries next";
    }
    if (reason == "cross-function-return-path-fanin") {
      return "reduce multi-root return fanin after fanout and unknown paths";
    }
    return "";
  };
  auto machine_reason_priority_signal_for =
      [](std::size_t reason_return, std::size_t reason_call, std::size_t reason_field)
      -> std::pair<int, std::string> {
    if (reason_return > 0) return {5, "return"};
    if (reason_call > 0) return {3, "call"};
    if (reason_field > 0) return {2, "field"};
    return {0, ""};
  };
  auto machine_reason_priority_action = [](std::string_view reason) -> std::string {
    if (reason == "return") {
      return "harden return escape paths first; keep region roots from crossing function boundaries";
    }
    if (reason == "call") {
      return "tighten call-boundary ownership summaries before field-level tuning";
    }
    if (reason == "field") {
      return "prevent escaping field writes after return/call escape paths are stabilized";
    }
    return "";
  };
  auto epistemic_priority_signal_for = [](std::string_view root_code,
                                          bool has_followup_p010,
                                          bool has_followup_x003) -> std::pair<int, std::string> {
    const bool has_owner_loop = (root_code == "NBL-P010") || has_followup_p010;
    if (has_followup_x003 && has_owner_loop) {
      return {4, "owner-inference-min-loop"};
    }
    if (has_followup_x003) {
      return {2, "predictive-shared-hot-path-followup"};
    }
    return {0, ""};
  };
  auto epistemic_priority_action = [](std::string_view reason) -> std::string {
    if (reason == "owner-inference-min-loop") {
      return "close owner root cause first, then validate P010/X003 hot-path evidence in order";
    }
    if (reason == "predictive-shared-hot-path-followup") {
      return "treat X003 as a follow-up check after primary ownership fixes land";
    }
    return "";
  };

  struct RootCoverageStats {
    std::size_t stable_covered_count = 0;
    std::size_t stage_preflight = 0;
    std::size_t stage_build = 0;
    std::size_t stage_other = 0;
    std::size_t dim_api = 0;
    std::size_t dim_perf = 0;
    std::size_t dim_best = 0;
    std::size_t dim_safe = 0;
    std::size_t dim_general = 0;
    std::size_t r001_reason_return = 0;
    std::size_t r001_reason_call = 0;
    std::size_t r001_reason_field = 0;
    std::unordered_map<std::string, std::size_t> r001_subreason_counts;
    std::unordered_map<std::string, std::size_t> r001_detail_counts;
    std::unordered_set<std::string> r001_seen_keys;
    std::unordered_map<std::string, std::size_t> owner_reason_counts;
    std::unordered_set<std::string> owner_reason_seen_keys;
  };
  std::vector<RootCoverageStats> root_coverage_stats(buckets.size());
  for (std::size_t i = 0; i < buckets.size(); ++i) {
    const auto* root = buckets[i].root;
    if (root == nullptr) continue;
    auto& stats = root_coverage_stats[i];
    if (root->stage == nebula::frontend::DiagnosticStage::Preflight) {
      ++stats.stage_preflight;
    } else if (root->stage == nebula::frontend::DiagnosticStage::Build) {
      ++stats.stage_build;
    } else {
      ++stats.stage_other;
    }
    dim_count_add(*root, stats.dim_api, stats.dim_perf, stats.dim_best, stats.dim_safe,
                  stats.dim_general);
    r001_reason_count_add(*root, stats.r001_reason_return, stats.r001_reason_call,
                          stats.r001_reason_field, stats.r001_subreason_counts,
                          stats.r001_detail_counts, stats.r001_seen_keys);
    owner_reason_count_add(*root, stats.owner_reason_counts, stats.owner_reason_seen_keys);
  }
  for (const auto& d : derived_entries) {
    if (!d.parent_root_index.has_value()) continue;
    if (d.reason == "truncated-root-overflow") continue;
    auto& stats = root_coverage_stats[*d.parent_root_index];
    ++stats.stable_covered_count;
    if (d.diag->stage == nebula::frontend::DiagnosticStage::Preflight) {
      ++stats.stage_preflight;
    } else if (d.diag->stage == nebula::frontend::DiagnosticStage::Build) {
      ++stats.stage_build;
    } else {
      ++stats.stage_other;
    }
    dim_count_add(*d.diag, stats.dim_api, stats.dim_perf, stats.dim_best, stats.dim_safe,
                  stats.dim_general);
    r001_reason_count_add(*d.diag, stats.r001_reason_return, stats.r001_reason_call,
                          stats.r001_reason_field, stats.r001_subreason_counts,
                          stats.r001_detail_counts, stats.r001_seen_keys);
    owner_reason_count_add(*d.diag, stats.owner_reason_counts, stats.owner_reason_seen_keys);
  }

  std::vector<TopRootCauseEntry> top_root_causes;
  std::size_t covered_derived_total = 0;
  std::size_t top_root_candidate_count = 0;
  const auto rc_v2_started = GroupingClock::now();
  if (rc_v2_enabled) {
    struct Candidate {
      std::size_t root_index = 0;
      int base_score = 0;
      int risk_rank_value = 0;
      std::size_t covered_count = 0;
      std::size_t stage_preflight = 0;
      std::size_t stage_build = 0;
      std::size_t stage_other = 0;
      std::size_t dim_api = 0;
      std::size_t dim_perf = 0;
      std::size_t dim_best = 0;
      std::size_t dim_safe = 0;
      std::size_t dim_general = 0;
      std::string first_fix_hint;
      std::string priority_machine_reason;
      int priority_machine_boost = 0;
      std::string priority_epistemic_reason;
      int priority_epistemic_boost = 0;
      std::string priority_owner_reason;
      int priority_owner_boost = 0;
      int priority_owner_weight = 0;
      int machine_priority_tiebreak = 0;
      int epistemic_priority_tiebreak = 0;
      int owner_priority_tiebreak = 0;
      std::vector<TopRootCauseEntry::OwnerPriorityPlanStep> priority_owner_plan;
      std::vector<TopRootCauseEntry::PriorityFixPlanStep> priority_fix_plan;
      std::size_t r001_reason_return = 0;
      std::size_t r001_reason_call = 0;
      std::size_t r001_reason_field = 0;
      std::unordered_map<std::string, std::size_t> r001_subreason_counts;
      std::unordered_map<std::string, std::size_t> r001_detail_counts;
      std::unordered_map<std::string, std::size_t> owner_reason_counts;
      std::string stable_key;
    };

    std::vector<Candidate> candidates;
    candidates.reserve(buckets.size());
    for (std::size_t i = 0; i < buckets.size(); ++i) {
      const auto* root = buckets[i].root;
      if (root == nullptr) continue;

      Candidate c;
      c.root_index = i;
      c.risk_rank_value = risk_rank(root->risk);
      const auto& stats = root_coverage_stats[i];
      c.covered_count = stats.stable_covered_count;
      c.stage_preflight = stats.stage_preflight;
      c.stage_build = stats.stage_build;
      c.stage_other = stats.stage_other;
      c.dim_api = stats.dim_api;
      c.dim_perf = stats.dim_perf;
      c.dim_best = stats.dim_best;
      c.dim_safe = stats.dim_safe;
      c.dim_general = stats.dim_general;
      c.r001_reason_return = stats.r001_reason_return;
      c.r001_reason_call = stats.r001_reason_call;
      c.r001_reason_field = stats.r001_reason_field;
      c.r001_subreason_counts = stats.r001_subreason_counts;
      c.r001_detail_counts = stats.r001_detail_counts;
      c.owner_reason_counts = stats.owner_reason_counts;

      if (!root->suggestions.empty()) c.first_fix_hint = root->suggestions.front();

      std::ostringstream stable_key;
      stable_key << root->code << "|" << root->span.start.line << ":" << root->span.start.col
                 << "|" << root->message;
      c.stable_key = stable_key.str();

      int score = 0;
      score += risk_base_score(root->risk);
      score += std::min(25, static_cast<int>(c.covered_count) * 3);
      if (root->stage == nebula::frontend::DiagnosticStage::Preflight) score += 10;
      const std::string_view dim = root->is_warning() ? warning_dimension_for_code(root->code) : "general";
      score += dimension_boost(dim);
      std::pair<int, std::string> owner_signal = {0, ""};
      const auto machine_signal = machine_reason_priority_signal_for(
          c.r001_reason_return, c.r001_reason_call, c.r001_reason_field);
      bool has_followup_p010 = false;
      bool has_followup_x003 = false;
      if (root != nullptr) {
        const auto chain_diags = causal_chain_diags_for(*root);
        for (const auto* chain_diag : chain_diags) {
          if (chain_diag == nullptr) continue;
          if (chain_diag->code == "NBL-P010") has_followup_p010 = true;
          if (chain_diag->code == "NBL-X003") has_followup_x003 = true;
        }
      }
      const auto epistemic_signal =
          epistemic_priority_signal_for(root->code, has_followup_p010, has_followup_x003);
      std::vector<TopRootCauseEntry::OwnerPriorityPlanStep> owner_plan;
      owner_plan.reserve(c.owner_reason_counts.size());
      for (const auto& owner_reason : c.owner_reason_counts) {
        const auto cur = owner_reason_priority_signal_for(owner_reason.first);
        if (cur.first > 0 && !cur.second.empty()) {
          TopRootCauseEntry::OwnerPriorityPlanStep step;
          step.reason = cur.second;
          step.boost = cur.first;
          step.count = owner_reason.second;
          step.action = owner_reason_priority_action(cur.second);
          owner_plan.push_back(std::move(step));
        }
        if (cur.first > owner_signal.first ||
            (cur.first == owner_signal.first && !cur.second.empty() &&
             (owner_signal.second.empty() || cur.second < owner_signal.second))) {
          owner_signal = cur;
        }
      }
      std::sort(owner_plan.begin(), owner_plan.end(),
                [](const auto& lhs, const auto& rhs) {
                  if (lhs.boost != rhs.boost) return lhs.boost > rhs.boost;
                  if (lhs.count != rhs.count) return lhs.count > rhs.count;
                  return lhs.reason < rhs.reason;
                });
      c.priority_owner_plan = std::move(owner_plan);
      c.priority_machine_boost = machine_signal.first;
      c.priority_machine_reason = machine_signal.second;
      c.priority_epistemic_boost = epistemic_signal.first;
      c.priority_epistemic_reason = epistemic_signal.second;
      c.priority_owner_boost = owner_signal.first;
      c.priority_owner_reason = owner_signal.second;
      c.machine_priority_tiebreak = c.priority_machine_boost;
      c.epistemic_priority_tiebreak = c.priority_epistemic_boost;
      int owner_tiebreak = 0;
      for (std::size_t plan_i = 0; plan_i < c.priority_owner_plan.size(); ++plan_i) {
        const auto& step = c.priority_owner_plan[plan_i];
        const int base = (plan_i == 0) ? step.boost : (step.boost / 3);
        owner_tiebreak += std::max(0, base) * static_cast<int>(std::min<std::size_t>(step.count, 3));
      }
      c.owner_priority_tiebreak = owner_tiebreak;
      c.priority_owner_weight = std::min(7, owner_tiebreak / 6);
      auto reason_count_for = [&](std::string_view machine_reason) -> std::size_t {
        if (machine_reason == "return") return c.r001_reason_return;
        if (machine_reason == "call") return c.r001_reason_call;
        if (machine_reason == "field") return c.r001_reason_field;
        return 0;
      };
      std::vector<TopRootCauseEntry::PriorityFixPlanStep> fix_plan;
      fix_plan.reserve(c.priority_owner_plan.size() + 2);
      for (const auto& step : c.priority_owner_plan) {
        TopRootCauseEntry::PriorityFixPlanStep plan_step;
        plan_step.category = "owner";
        plan_step.reason = step.reason;
        plan_step.boost = step.boost;
        plan_step.count = step.count;
        plan_step.action = step.action;
        fix_plan.push_back(std::move(plan_step));
      }
      if (!c.priority_machine_reason.empty() && c.priority_machine_boost > 0) {
        TopRootCauseEntry::PriorityFixPlanStep plan_step;
        plan_step.category = "machine";
        plan_step.reason = c.priority_machine_reason;
        plan_step.boost = c.priority_machine_boost;
        plan_step.count = reason_count_for(c.priority_machine_reason);
        plan_step.action = machine_reason_priority_action(c.priority_machine_reason);
        fix_plan.push_back(std::move(plan_step));
      }
      if (!c.priority_epistemic_reason.empty() && c.priority_epistemic_boost > 0) {
        TopRootCauseEntry::PriorityFixPlanStep plan_step;
        plan_step.category = "epistemic";
        plan_step.reason = c.priority_epistemic_reason;
        plan_step.boost = c.priority_epistemic_boost;
        std::size_t epistemic_count = 0;
        if (has_followup_p010) ++epistemic_count;
        if (has_followup_x003) ++epistemic_count;
        if (epistemic_count == 0) epistemic_count = 1;
        plan_step.count = epistemic_count;
        plan_step.action = epistemic_priority_action(c.priority_epistemic_reason);
        fix_plan.push_back(std::move(plan_step));
      }
      std::sort(fix_plan.begin(), fix_plan.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.boost != rhs.boost) return lhs.boost > rhs.boost;
        if (lhs.count != rhs.count) return lhs.count > rhs.count;
        if (lhs.category != rhs.category) return lhs.category < rhs.category;
        return lhs.reason < rhs.reason;
      });
      c.priority_fix_plan = std::move(fix_plan);
      score += c.priority_machine_boost;
      score += c.priority_epistemic_boost;
      score += c.priority_owner_boost;
      score += c.priority_owner_weight;
      if (root->predictive) {
        const double conf = root->confidence.value_or(0.0);
        score += (conf >= 0.80) ? 4 : 1;
      }
      if (score < 0) score = 0;
      if (score > 100) score = 100;
      c.base_score = score;
      candidates.push_back(std::move(c));
    }

    std::vector<std::size_t> filtered;
    filtered.reserve(candidates.size());
    for (std::size_t i = 0; i < candidates.size(); ++i) {
      if (candidates[i].covered_count < static_cast<std::size_t>(opt.root_cause_min_covered)) continue;
      filtered.push_back(i);
    }
    top_root_candidate_count = filtered.size();

    const std::size_t top_k = static_cast<std::size_t>(opt.root_cause_top_k);
    struct SelectedCandidate {
      std::size_t idx = 0;
      int final_score = 0;
    };
    std::vector<SelectedCandidate> selected;
    selected.reserve(top_k);
    while (!filtered.empty() && selected.size() < top_k) {
      std::size_t best_pos = 0;
      bool have_best = false;
      int best_score = 0;
      int best_risk = 0;
      std::size_t best_covered = 0;
      int best_machine_tiebreak = 0;
      int best_epistemic_tiebreak = 0;
      int best_owner_tiebreak = 0;
      std::string best_key;
      for (std::size_t pos = 0; pos < filtered.size(); ++pos) {
        const auto& cand = candidates[filtered[pos]];
        int adjusted_score = cand.base_score;
        for (const auto& sel : selected) {
          const auto& selected_cand = candidates[sel.idx];
          const auto* lhs = buckets[cand.root_index].root;
          const auto* rhs = buckets[selected_cand.root_index].root;
          if (lhs && rhs && near_duplicate(*lhs, *rhs)) {
            adjusted_score -= 12;
            break;
          }
        }
        if (adjusted_score < 0) adjusted_score = 0;
        if (!have_best ||
            adjusted_score > best_score ||
            (adjusted_score == best_score && cand.risk_rank_value > best_risk) ||
            (adjusted_score == best_score && cand.risk_rank_value == best_risk &&
             cand.covered_count > best_covered) ||
            (adjusted_score == best_score && cand.risk_rank_value == best_risk &&
             cand.covered_count == best_covered &&
             cand.machine_priority_tiebreak > best_machine_tiebreak) ||
            (adjusted_score == best_score && cand.risk_rank_value == best_risk &&
             cand.covered_count == best_covered &&
             cand.machine_priority_tiebreak == best_machine_tiebreak &&
             cand.epistemic_priority_tiebreak > best_epistemic_tiebreak) ||
            (adjusted_score == best_score && cand.risk_rank_value == best_risk &&
             cand.covered_count == best_covered &&
             cand.machine_priority_tiebreak == best_machine_tiebreak &&
             cand.epistemic_priority_tiebreak == best_epistemic_tiebreak &&
             cand.owner_priority_tiebreak > best_owner_tiebreak) ||
            (adjusted_score == best_score && cand.risk_rank_value == best_risk &&
             cand.covered_count == best_covered &&
             cand.machine_priority_tiebreak == best_machine_tiebreak &&
             cand.epistemic_priority_tiebreak == best_epistemic_tiebreak &&
             cand.owner_priority_tiebreak == best_owner_tiebreak && cand.stable_key < best_key)) {
          have_best = true;
          best_pos = pos;
          best_score = adjusted_score;
          best_risk = cand.risk_rank_value;
          best_covered = cand.covered_count;
          best_machine_tiebreak = cand.machine_priority_tiebreak;
          best_epistemic_tiebreak = cand.epistemic_priority_tiebreak;
          best_owner_tiebreak = cand.owner_priority_tiebreak;
          best_key = cand.stable_key;
        }
      }
      if (!have_best) break;
      const std::size_t picked = filtered[best_pos];
      filtered.erase(filtered.begin() + static_cast<std::ptrdiff_t>(best_pos));
      SelectedCandidate sel;
      sel.idx = picked;
      sel.final_score = best_score;
      selected.push_back(sel);
    }

    top_root_causes.reserve(selected.size());
    for (std::size_t i = 0; i < selected.size(); ++i) {
      const auto& cand = candidates[selected[i].idx];
      TopRootCauseEntry out;
      out.root_index = cand.root_index;
      out.id = "rc" + std::to_string(i + 1);
      out.score = static_cast<std::size_t>(selected[i].final_score);
      out.risk_rank_value = cand.risk_rank_value;
      out.covered_count = cand.covered_count;
      out.stage_preflight = cand.stage_preflight;
      out.stage_build = cand.stage_build;
      out.stage_other = cand.stage_other;
      out.dim_api = cand.dim_api;
      out.dim_perf = cand.dim_perf;
      out.dim_best = cand.dim_best;
      out.dim_safe = cand.dim_safe;
      out.dim_general = cand.dim_general;
      out.first_fix_hint = cand.first_fix_hint;
      out.why_selected = "risk-priority-and-causal-coverage";
      out.followup_codes = root_followup_codes[cand.root_index];
      if (const auto* root = buckets[cand.root_index].root; root != nullptr) {
        const auto chain_diags = causal_chain_diags_for(*root);
        std::unordered_set<std::string> hinted_codes;
        for (const auto* chain_diag : chain_diags) {
          if (chain_diag == nullptr) continue;
          const auto& code = chain_diag->code;
          if (std::find(out.followup_codes.begin(), out.followup_codes.end(), code) !=
              out.followup_codes.end()) {
            // Keep existing order from direct caused_by links.
          } else {
            out.followup_codes.push_back(code);
          }
          if (hinted_codes.insert(code).second && !chain_diag->suggestions.empty()) {
            out.followup_fix_hints.push_back({code, chain_diag->suggestions.front()});
          }
        }
      }
      out.followup_count = out.followup_codes.size();
      out.priority_machine_reason = cand.priority_machine_reason;
      out.priority_machine_boost = cand.priority_machine_boost;
      out.priority_machine_action = machine_reason_priority_action(cand.priority_machine_reason);
      out.priority_epistemic_reason = cand.priority_epistemic_reason;
      out.priority_epistemic_boost = cand.priority_epistemic_boost;
      out.priority_epistemic_action = epistemic_priority_action(cand.priority_epistemic_reason);
      out.priority_owner_reason = cand.priority_owner_reason;
      out.priority_owner_boost = cand.priority_owner_boost;
      out.priority_owner_weight = cand.priority_owner_weight;
      out.priority_owner_action = owner_reason_priority_action(cand.priority_owner_reason);
      out.priority_owner_plan = cand.priority_owner_plan;
      out.priority_fix_plan = cand.priority_fix_plan;
      out.r001_reason_return = cand.r001_reason_return;
      out.r001_reason_call = cand.r001_reason_call;
      out.r001_reason_field = cand.r001_reason_field;
      out.r001_subreason_counts.reserve(cand.r001_subreason_counts.size());
      for (const auto& subreason : cand.r001_subreason_counts) {
        out.r001_subreason_counts.push_back(subreason);
      }
      std::sort(out.r001_subreason_counts.begin(), out.r001_subreason_counts.end(),
                [](const auto& lhs, const auto& rhs) {
                  if (lhs.second != rhs.second) return lhs.second > rhs.second;
                  return lhs.first < rhs.first;
                });
      out.r001_detail_counts.reserve(cand.r001_detail_counts.size());
      for (const auto& detail : cand.r001_detail_counts) {
        out.r001_detail_counts.push_back(detail);
      }
      std::sort(out.r001_detail_counts.begin(), out.r001_detail_counts.end(),
                [](const auto& lhs, const auto& rhs) {
                  if (lhs.second != rhs.second) return lhs.second > rhs.second;
                  return lhs.first < rhs.first;
                });
      out.owner_reason_counts.reserve(cand.owner_reason_counts.size());
      for (const auto& owner_reason : cand.owner_reason_counts) {
        out.owner_reason_counts.push_back(owner_reason);
      }
      std::sort(out.owner_reason_counts.begin(), out.owner_reason_counts.end(),
                [](const auto& lhs, const auto& rhs) {
                  if (lhs.second != rhs.second) return lhs.second > rhs.second;
                  return lhs.first < rhs.first;
                });
      if (!out.followup_codes.empty()) {
        const auto* root = buckets[cand.root_index].root;
        if (root) {
          std::ostringstream chain;
          chain << root->code;
          for (const auto& code : out.followup_codes) chain << "->" << code;
          out.priority_chain_summary = chain.str();
        }
      }
      out.stable_key = cand.stable_key;
      top_root_causes.push_back(std::move(out));
      covered_derived_total += cand.covered_count;
    }
  }
  grouping_root_cause_v2_ms = elapsed_ms(rc_v2_started, GroupingClock::now());

  std::size_t warn_api = 0;
  std::size_t warn_perf = 0;
  std::size_t warn_best = 0;
  std::size_t warn_safe = 0;
  std::size_t warn_other = 0;
  std::size_t dim_api = 0;
  std::size_t dim_perf = 0;
  std::size_t dim_best = 0;
  std::size_t dim_safe = 0;
  std::size_t dim_general = 0;
  std::size_t gate_candidates_total = 0;
  std::size_t gate_blocked_total = 0;
  std::size_t gate_block_api = 0;
  std::size_t gate_block_perf = 0;
  std::size_t gate_block_best = 0;
  std::size_t gate_block_safe = 0;
  std::size_t gate_block_general = 0;
  for (const auto& d : diags) {
    if (!d.is_warning()) continue;
    const std::string_view cls = warning_class_for_code(d.code);
    const std::string_view dim = warning_dimension_for_code(d.code);
    if (cls == "api-deprecation") {
      ++warn_api;
    } else if (cls == "performance-risk") {
      ++warn_perf;
    } else if (cls == "best-practice-drift") {
      ++warn_best;
    } else if (cls == "safety-risk") {
      ++warn_safe;
    } else {
      ++warn_other;
    }
    if (dim == "api-lifecycle") {
      ++dim_api;
    } else if (dim == "perf-runtime") {
      ++dim_perf;
    } else if (dim == "best-practice-drift") {
      ++dim_best;
    } else if (dim == "safety-contract") {
      ++dim_safe;
    } else {
      ++dim_general;
    }

    const std::uint32_t dim_bit = gate_dimension_bit_for_code(d.code);
    const bool dim_enabled = (effective_gate_mask & dim_bit) != 0u;
    if (!dim_enabled) continue;
    ++gate_candidates_total;
    const int gate_weight = gate_weight_for_warning(d);
    if (gate_weight < gate_threshold) continue;
    ++gate_blocked_total;
    if (dim == "api-lifecycle") {
      ++gate_block_api;
    } else if (dim == "perf-runtime") {
      ++gate_block_perf;
    } else if (dim == "best-practice-drift") {
      ++gate_block_best;
    } else if (dim == "safety-contract") {
      ++gate_block_safe;
    } else {
      ++gate_block_general;
    }
  }
  grouping_rank_ms = elapsed_ms(rank_started, GroupingClock::now());
  if (grouping_rank_ms >= grouping_root_cause_v2_ms) {
    grouping_rank_ms -= grouping_root_cause_v2_ms;
  } else {
    grouping_rank_ms = 0;
  }

  if (opt.diag_format == DiagFormat::Json) {
    const auto emit_started = GroupingClock::now();
    std::ostringstream out_json;
    out_json << "{\n";
    out_json << "  \"summary\": {"
             << "\"roots\":" << buckets.size()
             << ",\"derived\":" << derived_entries.size()
             << ",\"causal_edges\":" << causal_edges
             << ",\"tier\":\"" << analysis_tier_name(tier) << "\""
             << ",\"warn_policy\":\"" << warn_policy_name(opt.warn_policy) << "\""
             << ",\"gate_profile\":\"" << gate_profile_name(opt.gate_profile) << "\""
             << ",\"warn_class_policy\":{"
             << "\"api_deprecation\":\"" << ((effective_warn_mask & kWarnClassApi) ? "on" : "off") << "\""
             << ",\"performance_risk\":\"" << ((effective_warn_mask & kWarnClassPerformance) ? "on" : "off")
             << "\""
             << ",\"best_practice_drift\":\"" << ((effective_warn_mask & kWarnClassBestPractice) ? "on" : "off")
             << "\""
             << ",\"safety_risk\":\"" << ((effective_warn_mask & kWarnClassSafety) ? "on" : "off") << "\""
             << ",\"general_warning\":\"" << ((effective_warn_mask & kWarnClassGeneral) ? "on" : "off")
             << "\""
             << "}"
             << ",\"gate_dimension_policy\":{"
             << "\"api_lifecycle\":\"" << ((effective_gate_mask & kGateDimApiLifecycle) ? "on" : "off")
             << "\""
             << ",\"perf_runtime\":\"" << ((effective_gate_mask & kGateDimPerfRuntime) ? "on" : "off") << "\""
             << ",\"best_practice_drift\":\""
             << ((effective_gate_mask & kGateDimBestPracticeDrift) ? "on" : "off") << "\""
             << ",\"safety_contract\":\"" << ((effective_gate_mask & kGateDimSafetyContract) ? "on" : "off")
             << "\""
             << ",\"general\":\"" << ((effective_gate_mask & kGateDimGeneral) ? "on" : "off") << "\""
             << "}"
             << ",\"suppressed_by_class\":{"
             << "\"api_deprecation\":" << suppressed_api
             << ",\"performance_risk\":" << suppressed_perf
             << ",\"best_practice_drift\":" << suppressed_best
             << ",\"safety_risk\":" << suppressed_safe
             << ",\"general_warning\":" << suppressed_other
             << "}"
             << ",\"max_root_causes\":" << opt.max_root_causes
             << ",\"truncated_roots\":" << truncated_roots
             << ",\"suppressed\":" << suppressed
             << ",\"warning_summary\":{"
             << "\"api_deprecation\":" << warn_api
             << ",\"performance_risk\":" << warn_perf
             << ",\"best_practice_drift\":" << warn_best
             << ",\"safety_risk\":" << warn_safe
             << ",\"other\":" << warn_other
             << "}"
             << ",\"warning_dimension_counts\":{"
             << "\"api_lifecycle\":" << dim_api
             << ",\"perf_runtime\":" << dim_perf
             << ",\"best_practice_drift\":" << dim_best
             << ",\"safety_contract\":" << dim_safe
             << ",\"general\":" << dim_general
             << "}"
             << ",\"gating_warning_counts\":{"
             << "\"threshold\":" << gate_threshold
             << ",\"candidates\":" << gate_candidates_total
             << ",\"blocked\":" << gate_blocked_total
             << ",\"api_lifecycle\":" << gate_block_api
             << ",\"perf_runtime\":" << gate_block_perf
             << ",\"best_practice_drift\":" << gate_block_best
             << ",\"safety_contract\":" << gate_block_safe
             << ",\"general\":" << gate_block_general
             << "}"
             << ",\"root_cause_v2_enabled\":" << (rc_v2_enabled ? "true" : "false")
             << ",\"top_root_cause_count\":" << top_root_causes.size()
             << ",\"top_root_candidate_count\":" << top_root_candidate_count
             << ",\"covered_derived_total\":" << covered_derived_total
             << ",\"grouping_total_ms\":" << kGroupingTotalMsMarker
             << ",\"grouping_index_ms\":" << grouping_index_ms
             << ",\"grouping_rank_ms\":" << grouping_rank_ms
             << ",\"grouping_root_cause_v2_ms\":" << grouping_root_cause_v2_ms
             << ",\"grouping_emit_ms\":" << kGroupingEmitMsMarker
             << ",\"grouping_budget_fallback\":false"
             << "},\n";

    out_json << "  \"roots\": [\n";
    for (std::size_t i = 0; i < buckets.size(); ++i) {
      out_json << "    ";
      emit_json_diag(out_json, *buckets[i].root, true, true, buckets[i].id, "", "",
                     buckets[i].rank_score, buckets[i].derived_count, buckets[i].group_key);
      if (i + 1 < buckets.size()) out_json << ",";
      out_json << "\n";
    }
    out_json << "  ],\n";

    out_json << "  \"derived\": [\n";
    for (std::size_t i = 0; i < derived_entries.size(); ++i) {
      out_json << "    ";
      std::string caused_by;
      if (derived_entries[i].parent_root_index.has_value()) {
        caused_by = buckets[*derived_entries[i].parent_root_index].id;
      }
      emit_json_diag(out_json, *derived_entries[i].diag, true, false, derived_entries[i].id,
                     caused_by, derived_entries[i].reason, 0, 0, derived_entries[i].group_key);
      if (i + 1 < derived_entries.size()) out_json << ",";
      out_json << "\n";
    }
    out_json << "  ],\n";

    out_json << "  \"causal_edges\": [\n";
    bool first = true;
    for (const auto& e : derived_entries) {
      if (!e.parent_root_index.has_value()) continue;
      if (!first) out_json << ",\n";
      first = false;
      out_json << "    {\"from\":\"" << buckets[*e.parent_root_index].id
               << "\",\"to\":\"" << e.id
               << "\",\"reason\":\"" << json_escape(e.reason) << "\"}";
    }
    out_json << "\n  ],\n";

    out_json << "  \"top_root_causes\": [\n";
    for (std::size_t i = 0; i < top_root_causes.size(); ++i) {
      const auto& rc = top_root_causes[i];
      const auto* d = buckets[rc.root_index].root;
      out_json << "    {"
               << "\"id\":\"" << rc.id << "\""
               << ",\"primary_diag\":{"
               << "\"diag_id\":\"" << buckets[rc.root_index].id << "\""
               << ",\"code\":\"" << json_escape(d->code) << "\""
               << ",\"line\":" << d->span.start.line
               << ",\"col\":" << d->span.start.col
               << "}"
               << ",\"score\":" << rc.score
               << ",\"risk_rank\":\"" << nebula::frontend::risk_name(d->risk) << "\""
               << ",\"covered_count\":" << rc.covered_count
               << ",\"stage_mix\":{"
               << "\"preflight\":" << rc.stage_preflight
               << ",\"build\":" << rc.stage_build
               << ",\"other\":" << rc.stage_other
               << "}"
               << ",\"dimensions\":{"
               << "\"api_lifecycle\":" << rc.dim_api
               << ",\"perf_runtime\":" << rc.dim_perf
               << ",\"best_practice_drift\":" << rc.dim_best
               << ",\"safety_contract\":" << rc.dim_safe
               << ",\"general\":" << rc.dim_general
               << "}"
               << ",\"why_selected\":\"" << rc.why_selected << "\""
               << ",\"first_fix_hint\":\"" << json_escape(rc.first_fix_hint) << "\""
               << ",\"followup_count\":" << rc.followup_count
               << ",\"followup_codes\":[";
      for (std::size_t j = 0; j < rc.followup_codes.size(); ++j) {
        if (j) out_json << ",";
        out_json << "\"" << json_escape(rc.followup_codes[j]) << "\"";
      }
      out_json << "]"
               << ",\"followup_fix_hints\":[";
      for (std::size_t j = 0; j < rc.followup_fix_hints.size(); ++j) {
        if (j) out_json << ",";
        out_json << "{\"code\":\"" << json_escape(rc.followup_fix_hints[j].first)
                 << "\",\"hint\":\"" << json_escape(rc.followup_fix_hints[j].second) << "\"}";
      }
      out_json << "]"
               << ",\"priority_machine_reason\":\"" << json_escape(rc.priority_machine_reason) << "\""
               << ",\"priority_machine_boost\":" << rc.priority_machine_boost
               << ",\"priority_machine_action\":\"" << json_escape(rc.priority_machine_action) << "\""
               << ",\"priority_epistemic_reason\":\"" << json_escape(rc.priority_epistemic_reason)
               << "\""
               << ",\"priority_epistemic_boost\":" << rc.priority_epistemic_boost
               << ",\"priority_epistemic_action\":\"" << json_escape(rc.priority_epistemic_action)
               << "\""
               << ",\"priority_owner_reason\":\"" << json_escape(rc.priority_owner_reason) << "\""
               << ",\"priority_owner_boost\":" << rc.priority_owner_boost
               << ",\"priority_owner_weight\":" << rc.priority_owner_weight
               << ",\"priority_owner_action\":\"" << json_escape(rc.priority_owner_action) << "\""
               << ",\"priority_owner_plan\":[";
      for (std::size_t j = 0; j < rc.priority_owner_plan.size(); ++j) {
        if (j) out_json << ",";
        out_json << "{\"reason\":\"" << json_escape(rc.priority_owner_plan[j].reason)
                 << "\",\"boost\":" << rc.priority_owner_plan[j].boost
                 << ",\"count\":" << rc.priority_owner_plan[j].count
                 << ",\"action\":\"" << json_escape(rc.priority_owner_plan[j].action) << "\"}";
      }
      out_json << "]"
               << ",\"priority_fix_plan\":[";
      for (std::size_t j = 0; j < rc.priority_fix_plan.size(); ++j) {
        if (j) out_json << ",";
        out_json << "{\"category\":\"" << json_escape(rc.priority_fix_plan[j].category)
                 << "\",\"reason\":\"" << json_escape(rc.priority_fix_plan[j].reason)
                 << "\",\"boost\":" << rc.priority_fix_plan[j].boost
                 << ",\"count\":" << rc.priority_fix_plan[j].count
                 << ",\"action\":\"" << json_escape(rc.priority_fix_plan[j].action) << "\"}";
      }
      out_json << "]"
               << ",\"machine_owner_reason_breakdown\":[";
      for (std::size_t j = 0; j < rc.owner_reason_counts.size(); ++j) {
        if (j) out_json << ",";
        out_json << "{\"name\":\"" << json_escape(rc.owner_reason_counts[j].first)
                 << "\",\"count\":" << rc.owner_reason_counts[j].second << "}";
      }
      out_json << "]"
               << ",\"machine_reason_breakdown\":{"
               << "\"return\":" << rc.r001_reason_return
               << ",\"call\":" << rc.r001_reason_call
               << ",\"field\":" << rc.r001_reason_field
               << "}"
               << ",\"machine_subreason_breakdown\":[";
      for (std::size_t j = 0; j < rc.r001_subreason_counts.size(); ++j) {
        if (j) out_json << ",";
        out_json << "{\"name\":\"" << json_escape(rc.r001_subreason_counts[j].first)
                 << "\",\"count\":" << rc.r001_subreason_counts[j].second << "}";
      }
      out_json << "]"
               << ",\"machine_detail_breakdown\":[";
      for (std::size_t j = 0; j < rc.r001_detail_counts.size(); ++j) {
        if (j) out_json << ",";
        out_json << "{\"name\":\"" << json_escape(rc.r001_detail_counts[j].first)
                 << "\",\"count\":" << rc.r001_detail_counts[j].second << "}";
      }
      out_json << "]"
               << ",\"priority_chain_summary\":\"" << json_escape(rc.priority_chain_summary) << "\""
               << "}";
      if (i + 1 < top_root_causes.size()) out_json << ",";
      out_json << "\n";
    }
    out_json << "  ],\n";

    out_json << "  \"fix_order\": [";
    for (std::size_t i = 0; i < top_root_causes.size(); ++i) {
      if (i) out_json << ",";
      out_json << "\"" << top_root_causes[i].id << "\"";
    }
    out_json << "]\n";
    out_json << "}\n";

    std::string rendered = out_json.str();
    const std::size_t grouping_emit_ms = elapsed_ms(emit_started, GroupingClock::now());
    const std::size_t grouping_total_ms = elapsed_ms(grouping_started, GroupingClock::now());
    replace_marker(rendered, kGroupingEmitMsMarker, grouping_emit_ms);
    replace_marker(rendered, kGroupingTotalMsMarker, grouping_total_ms);
    os << rendered;
    return;
  }

  const auto emit_started = GroupingClock::now();
  std::ostringstream out_text;
  out_text << "diag-summary: roots=" << buckets.size() << " derived=" << derived_entries.size()
           << " tier=" << analysis_tier_name(tier)
           << " warn-policy=" << warn_policy_name(opt.warn_policy)
           << " gate-profile=" << gate_profile_name(opt.gate_profile)
           << " warn-class=(api=" << ((effective_warn_mask & kWarnClassApi) ? "on" : "off")
           << ",perf=" << ((effective_warn_mask & kWarnClassPerformance) ? "on" : "off")
           << ",best=" << ((effective_warn_mask & kWarnClassBestPractice) ? "on" : "off")
           << ",safety=" << ((effective_warn_mask & kWarnClassSafety) ? "on" : "off")
           << ",general=" << ((effective_warn_mask & kWarnClassGeneral) ? "on" : "off") << ")"
           << " gate-dimension=(api=" << ((effective_gate_mask & kGateDimApiLifecycle) ? "on" : "off")
           << ",perf=" << ((effective_gate_mask & kGateDimPerfRuntime) ? "on" : "off")
           << ",best=" << ((effective_gate_mask & kGateDimBestPracticeDrift) ? "on" : "off")
           << ",safety=" << ((effective_gate_mask & kGateDimSafetyContract) ? "on" : "off")
           << ",general=" << ((effective_gate_mask & kGateDimGeneral) ? "on" : "off") << ")"
           << " max-root-causes=" << opt.max_root_causes
           << " truncated-roots=" << truncated_roots
           << " causal-edges=" << causal_edges
           << " root-cause-v2=" << (rc_v2_enabled ? "on" : "off") << "\n";
  out_text << "grouping-perf: total-ms=" << kGroupingTotalMsMarker
           << " index-ms=" << grouping_index_ms
           << " rank-ms=" << grouping_rank_ms
           << " root-cause-v2-ms=" << grouping_root_cause_v2_ms
           << " emit-ms=" << kGroupingEmitMsMarker
           << " budget-fallback=off\n";
  if (warn_api + warn_perf + warn_best + warn_safe + warn_other > 0) {
    out_text << "warning-summary: api-deprecation=" << warn_api << " performance-risk=" << warn_perf
             << " best-practice-drift=" << warn_best << " safety-risk=" << warn_safe
             << " other=" << warn_other << "\n";
    out_text << "warning-dimension-summary: api-lifecycle=" << dim_api
             << " perf-runtime=" << dim_perf
             << " best-practice-drift=" << dim_best
             << " safety-contract=" << dim_safe
             << " general=" << dim_general << "\n";
    out_text << "gate-warning-summary: threshold=" << gate_threshold
             << " candidates=" << gate_candidates_total
             << " blocked=" << gate_blocked_total
             << " (api-lifecycle=" << gate_block_api
             << " perf-runtime=" << gate_block_perf
             << " best-practice-drift=" << gate_block_best
             << " safety-contract=" << gate_block_safe
             << " general=" << gate_block_general << ")\n";
  }
  if (rc_v2_enabled) {
    out_text << "top-root-causes: count=" << top_root_candidate_count
             << " selected=" << top_root_causes.size()
             << " strategy=risk+coverage\n";
    if (top_root_causes.empty()) {
      out_text << "fix-order: (none)\n";
    } else {
      out_text << "fix-order: ";
      for (std::size_t i = 0; i < top_root_causes.size(); ++i) {
        if (i) out_text << " -> ";
        out_text << top_root_causes[i].id;
      }
      out_text << "\n";
      for (const auto& rc : top_root_causes) {
        const auto* d = buckets[rc.root_index].root;
        out_text << "top-root: " << rc.id
                 << " score=" << rc.score
                 << " risk=" << nebula::frontend::risk_name(d->risk)
                 << " covered=" << rc.covered_count
                 << " followups=" << rc.followup_count
                 << " code=" << d->code
                 << " reason=" << rc.why_selected << "\n";
        if (!rc.first_fix_hint.empty()) {
          out_text << "first-fix-hint: " << rc.first_fix_hint << "\n";
        }
        if (!rc.priority_chain_summary.empty()) {
          out_text << "fix-priority-chain: " << rc.priority_chain_summary << "\n";
        }
        if (!rc.followup_fix_hints.empty()) {
          out_text << "followup-fix-hints: ";
          for (std::size_t i = 0; i < rc.followup_fix_hints.size(); ++i) {
            if (i) out_text << " | ";
            out_text << rc.followup_fix_hints[i].first << ": " << rc.followup_fix_hints[i].second;
          }
          out_text << "\n";
        }
        if (!rc.priority_machine_reason.empty() && rc.priority_machine_boost > 0) {
          out_text << "priority-machine-signal: " << rc.priority_machine_reason
                   << " (boost=" << rc.priority_machine_boost << ")\n";
          if (!rc.priority_machine_action.empty()) {
            out_text << "priority-machine-action: " << rc.priority_machine_action << "\n";
          }
        }
        if (!rc.priority_epistemic_reason.empty() && rc.priority_epistemic_boost > 0) {
          out_text << "priority-epistemic-signal: " << rc.priority_epistemic_reason
                   << " (boost=" << rc.priority_epistemic_boost << ")\n";
          if (!rc.priority_epistemic_action.empty()) {
            out_text << "priority-epistemic-action: " << rc.priority_epistemic_action << "\n";
          }
        }
        if (!rc.priority_owner_reason.empty() && rc.priority_owner_boost > 0) {
          out_text << "priority-owner-signal: " << rc.priority_owner_reason
                   << " (boost=" << rc.priority_owner_boost
                   << ", weight=" << rc.priority_owner_weight << ")\n";
          if (!rc.priority_owner_action.empty()) {
            out_text << "priority-owner-action: " << rc.priority_owner_action << "\n";
          }
        }
        if (!rc.priority_owner_plan.empty()) {
          out_text << "priority-owner-plan: ";
          for (std::size_t i = 0; i < rc.priority_owner_plan.size(); ++i) {
            if (i) out_text << " -> ";
            out_text << rc.priority_owner_plan[i].reason << " (boost="
                     << rc.priority_owner_plan[i].boost << ", count="
                     << rc.priority_owner_plan[i].count << ")";
          }
          out_text << "\n";
        }
        if (!rc.priority_fix_plan.empty()) {
          out_text << "priority-fix-plan: ";
          for (std::size_t i = 0; i < rc.priority_fix_plan.size(); ++i) {
            if (i) out_text << " -> ";
            out_text << rc.priority_fix_plan[i].category << ":" << rc.priority_fix_plan[i].reason
                     << " (boost=" << rc.priority_fix_plan[i].boost
                     << ", count=" << rc.priority_fix_plan[i].count << ")";
          }
          out_text << "\n";
        }
        if (!rc.owner_reason_counts.empty()) {
          out_text << "machine-owner-reason-breakdown: ";
          for (std::size_t i = 0; i < rc.owner_reason_counts.size(); ++i) {
            if (i) out_text << ", ";
            out_text << rc.owner_reason_counts[i].first << "=" << rc.owner_reason_counts[i].second;
          }
          out_text << "\n";
        }
        if (rc.r001_reason_return + rc.r001_reason_call + rc.r001_reason_field > 0) {
          out_text << "machine-reason-breakdown: return=" << rc.r001_reason_return
                   << " call=" << rc.r001_reason_call
                   << " field=" << rc.r001_reason_field << "\n";
          if (!rc.r001_subreason_counts.empty()) {
            out_text << "machine-subreason-breakdown: ";
            for (std::size_t i = 0; i < rc.r001_subreason_counts.size(); ++i) {
              if (i) out_text << ", ";
              out_text << rc.r001_subreason_counts[i].first << "="
                       << rc.r001_subreason_counts[i].second;
            }
            out_text << "\n";
          }
          if (!rc.r001_detail_counts.empty()) {
            out_text << "machine-detail-breakdown: ";
            for (std::size_t i = 0; i < rc.r001_detail_counts.size(); ++i) {
              if (i) out_text << ", ";
              out_text << rc.r001_detail_counts[i].first << "="
                       << rc.r001_detail_counts[i].second;
            }
            out_text << "\n";
          }
        }
      }
    }
  }
  emit_suppressed_note(out_text, suppressed_api, suppressed_perf, suppressed_best, suppressed_safe,
                       suppressed_other);
  if (truncated_roots > 0) {
    out_text << "note: root-cause list truncated by --max-root-causes=" << opt.max_root_causes
             << " (" << truncated_roots << " root(s) moved to cascaded section)\n";
  }

  for (const auto& b : buckets) {
    out_text << "root-rank: " << b.id
             << " score=" << b.rank_score
             << " derived=" << b.derived_count << "\n";
    const auto* d = b.root;
    if (d->is_warning()) {
      out_text << "hint: " << d->code
               << " warning-class=" << warning_class_for_code(d->code)
               << " warning-dimension=" << warning_dimension_for_code(d->code)
               << " warning-reason=" << warning_reason_for_diag(*d)
               << " gate-weight=" << gate_weight_for_warning(*d) << "\n";
    }
    nebula::frontend::print_diagnostic(out_text, *d);
  }

  if (!derived_entries.empty()) {
    out_text << "note: cascaded diagnostics follow (" << derived_entries.size() << ")\n";
    for (const auto& e : derived_entries) {
      if (e.parent_root_index.has_value()) {
        out_text << "cause-link: " << e.id
                 << " caused-by=" << buckets[*e.parent_root_index].id
                 << " reason=" << e.reason << "\n";
      } else {
        out_text << "cause-link: " << e.id
                 << " caused-by=none"
                 << " reason=" << e.reason << "\n";
      }
      nebula::frontend::print_diagnostic(out_text, *e.diag);
    }
  }

  std::string rendered = out_text.str();
  const std::size_t grouping_emit_ms = elapsed_ms(emit_started, GroupingClock::now());
  const std::size_t grouping_total_ms = elapsed_ms(grouping_started, GroupingClock::now());
  replace_marker(rendered, kGroupingEmitMsMarker, grouping_emit_ms);
  replace_marker(rendered, kGroupingTotalMsMarker, grouping_total_ms);
  os << rendered;
}

nebula::frontend::Diagnostic make_cli_diag(nebula::frontend::Severity severity,
                                                  std::string code,
                                                  std::string message,
                                                  nebula::frontend::DiagnosticStage stage,
                                                  nebula::frontend::DiagnosticRisk risk,
                                                  std::string cause,
                                                  std::string impact,
                                                  std::vector<std::string> suggestions) {
  nebula::frontend::Diagnostic d;
  d.severity = severity;
  d.code = std::move(code);
  d.message = std::move(message);
  d.stage = stage;
  d.risk = risk;
  d.category = "cli";
  d.cause = std::move(cause);
  d.impact = std::move(impact);
  d.suggestions = std::move(suggestions);
  return d;
}

static void summarize_diag_levels(CompilePipelineResult& res) {
  sort_diagnostics_stable(res.diags);
  res.has_error = false;
  res.has_warning = false;
  res.has_note = false;
  for (const auto& d : res.diags) {
    if (d.is_error()) res.has_error = true;
    if (d.is_warning()) res.has_warning = true;
    if (d.is_note()) res.has_note = true;
  }
}

constexpr int kCompilePipelineCompilerSchemaVersion = 1;
constexpr int kCompilePipelineCacheSchemaVersion = 2;
constexpr const char* kDiskCacheMagic = "NBLDC8";
constexpr int kArtifactMetaVersion = 2;

static std::string source_identity_seed(std::string_view path, std::string_view text) {
  std::ostringstream os;
  os << "path=" << path << "\n";
  os << text;
  return os.str();
}

static std::string compile_pipeline_cache_key(const std::string& src,
                                              const CompilePipelineOptions& opt) {
  std::ostringstream os;
  os << "compiler_schema=" << kCompilePipelineCompilerSchemaVersion
     << "|cache_schema=" << kCompilePipelineCacheSchemaVersion
     << "|h=" << hash_source(src)
     << "|path_h=" << hash_source(opt.source_path)
     << "|mode=" << static_cast<int>(opt.mode)
     << "|profile=" << static_cast<int>(opt.profile)
     << "|tier=" << static_cast<int>(opt.analysis_tier)
     << "|strict=" << (opt.strict_region ? 1 : 0)
     << "|werr=" << (opt.warnings_as_errors ? 1 : 0)
     << "|lint=" << (opt.include_lint ? 1 : 0)
     << "|budget=" << opt.budget_ms;
  return os.str();
}

static std::string compile_pipeline_cache_key(const std::vector<nebula::frontend::SourceFile>& sources,
                                              const CompilePipelineOptions& opt) {
  if (!opt.cache_key_source.empty()) return compile_pipeline_cache_key(opt.cache_key_source, opt);
  std::ostringstream joined;
  for (const auto& source : sources) {
    joined << source.path << "\n";
    joined << "pkg=" << source.package_name << "\n";
    joined << "mod=" << source.module_name << "\n";
    for (const auto& import_name : source.resolved_imports) {
      joined << "import=" << import_name << "\n";
    }
    joined << source.text << "\n---\n";
  }
  return compile_pipeline_cache_key(joined.str(), opt);
}

static std::string compile_pipeline_cache_key_with_stage(
    const std::vector<nebula::frontend::SourceFile>& sources,
    const CompilePipelineOptions& opt) {
  std::ostringstream os;
  os << compile_pipeline_cache_key(sources, opt) << "|stage=" << static_cast<int>(opt.stage);
  return os.str();
}

struct CompilePipelineCacheCounters {
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
};

static std::unordered_map<std::string, CompilePipelineResult> g_pipeline_cache;
static std::unordered_map<std::string, std::uint8_t> g_pipeline_cache_cross_stage_index;
static CompilePipelineCacheCounters g_pipeline_cache_counters;
static std::unordered_set<std::string> g_disk_cache_prune_once;

static fs::path disk_cache_entries_dir(const CompilePipelineOptions& opt) {
  return opt.disk_cache_dir / ("v" + std::to_string(kCompilePipelineCacheSchemaVersion)) / "entries";
}

static std::string disk_cache_entry_id(const std::string& stage_key) {
  return hash_source(stage_key);
}

static fs::path disk_cache_entry_path(const CompilePipelineOptions& opt, const std::string& stage_key) {
  return disk_cache_entries_dir(opt) / (disk_cache_entry_id(stage_key) + ".bin");
}

static bool ensure_disk_cache_dirs(const CompilePipelineOptions& opt) {
  std::error_code ec;
  fs::create_directories(disk_cache_entries_dir(opt), ec);
  return !ec;
}

static std::size_t count_disk_cache_entries(const CompilePipelineOptions& opt) {
  std::error_code ec;
  const fs::path dir = disk_cache_entries_dir(opt);
  if (!fs::exists(dir, ec) || ec) return 0;
  std::size_t n = 0;
  for (const auto& ent : fs::directory_iterator(dir, ec)) {
    if (ec) break;
    if (!ent.is_regular_file()) continue;
    if (ent.path().extension() == ".bin") ++n;
  }
  return n;
}

static bool write_u8(std::ostream& os, std::uint8_t v) {
  os.write(reinterpret_cast<const char*>(&v), sizeof(v));
  return static_cast<bool>(os);
}

static bool write_u32(std::ostream& os, std::uint32_t v) {
  os.write(reinterpret_cast<const char*>(&v), sizeof(v));
  return static_cast<bool>(os);
}

static bool write_u64(std::ostream& os, std::uint64_t v) {
  os.write(reinterpret_cast<const char*>(&v), sizeof(v));
  return static_cast<bool>(os);
}

static bool write_i32(std::ostream& os, std::int32_t v) {
  os.write(reinterpret_cast<const char*>(&v), sizeof(v));
  return static_cast<bool>(os);
}

static bool write_f64(std::ostream& os, double v) {
  os.write(reinterpret_cast<const char*>(&v), sizeof(v));
  return static_cast<bool>(os);
}

static bool write_string_bin(std::ostream& os, const std::string& s) {
  if (!write_u32(os, static_cast<std::uint32_t>(s.size()))) return false;
  os.write(s.data(), static_cast<std::streamsize>(s.size()));
  return static_cast<bool>(os);
}

static bool write_span_bin(std::ostream& os, const nebula::frontend::Span& sp) {
  if (!write_u64(os, static_cast<std::uint64_t>(sp.start.offset))) return false;
  if (!write_i32(os, static_cast<std::int32_t>(sp.start.line))) return false;
  if (!write_i32(os, static_cast<std::int32_t>(sp.start.col))) return false;
  if (!write_u64(os, static_cast<std::uint64_t>(sp.end.offset))) return false;
  if (!write_i32(os, static_cast<std::int32_t>(sp.end.line))) return false;
  if (!write_i32(os, static_cast<std::int32_t>(sp.end.col))) return false;
  if (!write_string_bin(os, sp.source_path)) return false;
  return true;
}

static bool write_diag_bin(std::ostream& os, const nebula::frontend::Diagnostic& d) {
  if (!write_u8(os, static_cast<std::uint8_t>(d.severity))) return false;
  if (!write_u8(os, static_cast<std::uint8_t>(d.stage))) return false;
  if (!write_u8(os, static_cast<std::uint8_t>(d.risk))) return false;
  if (!write_u8(os, d.predictive ? 1u : 0u)) return false;
  if (!write_u8(os, d.confidence.has_value() ? 1u : 0u)) return false;
  if (d.confidence.has_value() && !write_f64(os, *d.confidence)) return false;
  if (!write_span_bin(os, d.span)) return false;
  if (!write_string_bin(os, d.code)) return false;
  if (!write_string_bin(os, d.message)) return false;
  if (!write_string_bin(os, d.category)) return false;
  if (!write_string_bin(os, d.cause)) return false;
  if (!write_string_bin(os, d.impact)) return false;
  if (!write_string_bin(os, d.machine_reason)) return false;
  if (!write_string_bin(os, d.machine_subreason)) return false;
  if (!write_string_bin(os, d.machine_detail)) return false;
  if (!write_string_bin(os, d.machine_trigger_family)) return false;
  if (!write_string_bin(os, d.machine_trigger_family_detail)) return false;
  if (!write_string_bin(os, d.machine_trigger_subreason)) return false;
  if (!write_string_bin(os, d.machine_owner)) return false;
  if (!write_string_bin(os, d.machine_owner_reason)) return false;
  if (!write_string_bin(os, d.machine_owner_reason_detail)) return false;
  if (!write_string_bin(os, d.caused_by_code)) return false;
  if (!write_u32(os, static_cast<std::uint32_t>(d.suggestions.size()))) return false;
  for (const auto& s : d.suggestions) {
    if (!write_string_bin(os, s)) return false;
  }
  if (!write_u32(os, static_cast<std::uint32_t>(d.related_spans.size()))) return false;
  for (const auto& s : d.related_spans) {
    if (!write_span_bin(os, s)) return false;
  }
  return true;
}

static bool read_u8(std::istream& is, std::uint8_t& v) {
  is.read(reinterpret_cast<char*>(&v), sizeof(v));
  return static_cast<bool>(is);
}

static bool read_u32(std::istream& is, std::uint32_t& v) {
  is.read(reinterpret_cast<char*>(&v), sizeof(v));
  return static_cast<bool>(is);
}

static bool read_u64(std::istream& is, std::uint64_t& v) {
  is.read(reinterpret_cast<char*>(&v), sizeof(v));
  return static_cast<bool>(is);
}

static bool read_i32(std::istream& is, std::int32_t& v) {
  is.read(reinterpret_cast<char*>(&v), sizeof(v));
  return static_cast<bool>(is);
}

static bool read_f64(std::istream& is, double& v) {
  is.read(reinterpret_cast<char*>(&v), sizeof(v));
  return static_cast<bool>(is);
}

static bool read_string_bin(std::istream& is, std::string& s) {
  std::uint32_t n = 0;
  if (!read_u32(is, n)) return false;
  s.assign(n, '\0');
  if (n == 0) return true;
  is.read(s.data(), static_cast<std::streamsize>(n));
  return static_cast<bool>(is);
}

static bool read_span_bin(std::istream& is, nebula::frontend::Span& sp) {
  std::uint64_t start_offset = 0;
  std::uint64_t end_offset = 0;
  std::int32_t start_line = 0;
  std::int32_t start_col = 0;
  std::int32_t end_line = 0;
  std::int32_t end_col = 0;
  if (!read_u64(is, start_offset)) return false;
  if (!read_i32(is, start_line)) return false;
  if (!read_i32(is, start_col)) return false;
  if (!read_u64(is, end_offset)) return false;
  if (!read_i32(is, end_line)) return false;
  if (!read_i32(is, end_col)) return false;
  sp.start.offset = static_cast<std::size_t>(start_offset);
  sp.start.line = static_cast<int>(start_line);
  sp.start.col = static_cast<int>(start_col);
  sp.end.offset = static_cast<std::size_t>(end_offset);
  sp.end.line = static_cast<int>(end_line);
  sp.end.col = static_cast<int>(end_col);
  if (!read_string_bin(is, sp.source_path)) return false;
  return true;
}

static bool read_diag_bin(std::istream& is, nebula::frontend::Diagnostic& d) {
  std::uint8_t sev = 0;
  std::uint8_t stage = 0;
  std::uint8_t risk = 0;
  std::uint8_t predictive = 0;
  std::uint8_t has_confidence = 0;
  if (!read_u8(is, sev)) return false;
  if (!read_u8(is, stage)) return false;
  if (!read_u8(is, risk)) return false;
  if (!read_u8(is, predictive)) return false;
  if (!read_u8(is, has_confidence)) return false;
  d.severity = static_cast<nebula::frontend::Severity>(sev);
  d.stage = static_cast<nebula::frontend::DiagnosticStage>(stage);
  d.risk = static_cast<nebula::frontend::DiagnosticRisk>(risk);
  d.predictive = (predictive != 0);
  d.confidence.reset();
  if (has_confidence != 0) {
    double conf = 0.0;
    if (!read_f64(is, conf)) return false;
    d.confidence = conf;
  }
  if (!read_span_bin(is, d.span)) return false;
  if (!read_string_bin(is, d.code)) return false;
  if (!read_string_bin(is, d.message)) return false;
  if (!read_string_bin(is, d.category)) return false;
  if (!read_string_bin(is, d.cause)) return false;
  if (!read_string_bin(is, d.impact)) return false;
  if (!read_string_bin(is, d.machine_reason)) return false;
  if (!read_string_bin(is, d.machine_subreason)) return false;
  if (!read_string_bin(is, d.machine_detail)) return false;
  if (!read_string_bin(is, d.machine_trigger_family)) return false;
  if (!read_string_bin(is, d.machine_trigger_family_detail)) return false;
  if (!read_string_bin(is, d.machine_trigger_subreason)) return false;
  if (!read_string_bin(is, d.machine_owner)) return false;
  if (!read_string_bin(is, d.machine_owner_reason)) return false;
  if (!read_string_bin(is, d.machine_owner_reason_detail)) return false;
  if (!read_string_bin(is, d.caused_by_code)) return false;
  std::uint32_t sugg_n = 0;
  if (!read_u32(is, sugg_n)) return false;
  d.suggestions.clear();
  d.suggestions.reserve(sugg_n);
  for (std::uint32_t i = 0; i < sugg_n; ++i) {
    std::string s;
    if (!read_string_bin(is, s)) return false;
    d.suggestions.push_back(std::move(s));
  }
  std::uint32_t rel_n = 0;
  if (!read_u32(is, rel_n)) return false;
  d.related_spans.clear();
  d.related_spans.reserve(rel_n);
  for (std::uint32_t i = 0; i < rel_n; ++i) {
    nebula::frontend::Span sp;
    if (!read_span_bin(is, sp)) return false;
    d.related_spans.push_back(std::move(sp));
  }
  return true;
}

static bool write_disk_cache_entry(const fs::path& path,
                                   const std::string& stage_key,
                                   const CompilePipelineResult& result) {
  const fs::path tmp = fs::path(path.string() + ".tmp");
  std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
  if (!out) return false;

  const std::string magic = kDiskCacheMagic;
  if (!write_string_bin(out, magic)) return false;
  if (!write_string_bin(out, stage_key)) return false;
  if (!write_u64(out, static_cast<std::uint64_t>(result.analysis_elapsed_ms))) return false;
  if (!write_u64(out, static_cast<std::uint64_t>(result.fn_count))) return false;
  if (!write_u64(out, static_cast<std::uint64_t>(result.cfg_nodes))) return false;
  if (!write_u8(out, result.has_error ? 1u : 0u)) return false;
  if (!write_u8(out, result.has_warning ? 1u : 0u)) return false;
  if (!write_u8(out, result.has_note ? 1u : 0u)) return false;
  if (!write_u8(out, result.has_cached_cpp ? 1u : 0u)) return false;
  if (!write_string_bin(out, result.has_cached_cpp ? result.cached_cpp : std::string())) return false;
  if (!write_u32(out, static_cast<std::uint32_t>(result.diags.size()))) return false;
  for (const auto& d : result.diags) {
    if (!write_diag_bin(out, d)) return false;
  }
  out.flush();
  if (!out) return false;
  out.close();
  std::error_code ec;
  fs::rename(tmp, path, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return false;
  }
  return true;
}

static bool read_disk_cache_entry(const fs::path& path,
                                  const std::string& stage_key,
                                  CompilePipelineResult& result) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;

  std::string magic;
  if (!read_string_bin(in, magic) || magic != kDiskCacheMagic) return false;
  std::string key;
  if (!read_string_bin(in, key) || key != stage_key) return false;

  std::uint64_t elapsed = 0;
  std::uint64_t fn_count = 0;
  std::uint64_t cfg_nodes = 0;
  std::uint8_t has_error = 0;
  std::uint8_t has_warning = 0;
  std::uint8_t has_note = 0;
  std::uint8_t has_cached_cpp = 0;
  if (!read_u64(in, elapsed)) return false;
  if (!read_u64(in, fn_count)) return false;
  if (!read_u64(in, cfg_nodes)) return false;
  if (!read_u8(in, has_error)) return false;
  if (!read_u8(in, has_warning)) return false;
  if (!read_u8(in, has_note)) return false;
  if (!read_u8(in, has_cached_cpp)) return false;

  std::string cached_cpp;
  if (!read_string_bin(in, cached_cpp)) return false;
  std::uint32_t diag_count = 0;
  if (!read_u32(in, diag_count)) return false;

  std::vector<nebula::frontend::Diagnostic> diags;
  diags.reserve(diag_count);
  for (std::uint32_t i = 0; i < diag_count; ++i) {
    nebula::frontend::Diagnostic d;
    if (!read_diag_bin(in, d)) return false;
    diags.push_back(std::move(d));
  }

  result = CompilePipelineResult{};
  result.analysis_elapsed_ms = static_cast<std::size_t>(elapsed);
  result.fn_count = static_cast<std::size_t>(fn_count);
  result.cfg_nodes = static_cast<std::size_t>(cfg_nodes);
  result.has_error = (has_error != 0);
  result.has_warning = (has_warning != 0);
  result.has_note = (has_note != 0);
  result.has_cached_cpp = (has_cached_cpp != 0);
  result.cached_cpp = result.has_cached_cpp ? std::move(cached_cpp) : std::string();
  result.diags = std::move(diags);
  return true;
}

static void refresh_disk_cache_entry_count(const CompilePipelineOptions& opt) {
  g_pipeline_cache_counters.disk_entries = count_disk_cache_entries(opt);
}

static void prune_disk_cache_entries(const CompilePipelineOptions& opt, bool include_ttl_expired) {
  if (!opt.disk_cache_enabled) return;
  if (!ensure_disk_cache_dirs(opt)) return;

  const fs::path dir = disk_cache_entries_dir(opt);
  std::error_code ec;
  std::vector<std::pair<fs::path, fs::file_time_type>> files;
  for (const auto& ent : fs::directory_iterator(dir, ec)) {
    if (ec) break;
    if (!ent.is_regular_file()) continue;
    if (ent.path().extension() != ".bin") continue;
    auto mtime = fs::last_write_time(ent.path(), ec);
    if (ec) continue;
    files.push_back({ent.path(), mtime});
  }

  const auto now = fs::file_time_type::clock::now();
  if (include_ttl_expired && opt.disk_cache_ttl_sec > 0) {
    const auto ttl = std::chrono::seconds(opt.disk_cache_ttl_sec);
    for (const auto& [path, mtime] : files) {
      if (now - mtime <= ttl) continue;
      std::error_code rm_ec;
      fs::remove(path, rm_ec);
      if (!rm_ec) ++g_pipeline_cache_counters.disk_expired;
    }
    files.clear();
    ec.clear();
    for (const auto& ent : fs::directory_iterator(dir, ec)) {
      if (ec) break;
      if (!ent.is_regular_file()) continue;
      if (ent.path().extension() != ".bin") continue;
      auto mtime = fs::last_write_time(ent.path(), ec);
      if (ec) continue;
      files.push_back({ent.path(), mtime});
    }
  }

  if (opt.disk_cache_max_entries > 0 && files.size() > static_cast<std::size_t>(opt.disk_cache_max_entries)) {
    std::sort(files.begin(), files.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });
    std::size_t need_remove = files.size() - static_cast<std::size_t>(opt.disk_cache_max_entries);
    for (std::size_t i = 0; i < need_remove; ++i) {
      std::error_code rm_ec;
      fs::remove(files[i].first, rm_ec);
      if (!rm_ec) ++g_pipeline_cache_counters.disk_evictions;
    }
  }

  refresh_disk_cache_entry_count(opt);
}

static std::uint8_t stage_bit(nebula::frontend::DiagnosticStage stage) {
  using nebula::frontend::DiagnosticStage;
  switch (stage) {
  case DiagnosticStage::Preflight: return 1u << 0;
  case DiagnosticStage::Build: return 1u << 1;
  default: return 1u << 2;
  }
}

static std::string stage_key_from_blind(const std::string& blind_key,
                                        nebula::frontend::DiagnosticStage stage) {
  std::ostringstream os;
  os << blind_key << "|stage=" << static_cast<int>(stage);
  return os.str();
}

static void retag_reused_diagnostics_for_stage(CompilePipelineResult& result,
                                               nebula::frontend::DiagnosticStage stage) {
  std::vector<nebula::frontend::Diagnostic> rewritten;
  rewritten.reserve(result.diags.size());
  for (auto d : result.diags) {
    if (stage != nebula::frontend::DiagnosticStage::Preflight && d.code == "NBL-PR001") {
      continue;
    }
    d.stage = stage;
    rewritten.push_back(std::move(d));
  }
  result.diags = std::move(rewritten);
}

CompilePipelineCacheStats get_compile_pipeline_cache_stats() {
  CompilePipelineCacheStats out;
  out.hits = g_pipeline_cache_counters.hits;
  out.misses = g_pipeline_cache_counters.misses;
  out.evictions = g_pipeline_cache_counters.evictions;
  out.cross_stage_candidates = g_pipeline_cache_counters.cross_stage_candidates;
  out.cross_stage_preflight_to_build = g_pipeline_cache_counters.cross_stage_preflight_to_build;
  out.cross_stage_build_to_preflight = g_pipeline_cache_counters.cross_stage_build_to_preflight;
  out.cross_stage_other = g_pipeline_cache_counters.cross_stage_other;
  out.cross_stage_reused = g_pipeline_cache_counters.cross_stage_reused;
  out.cross_stage_reused_preflight_to_build =
      g_pipeline_cache_counters.cross_stage_reused_preflight_to_build;
  out.cross_stage_reused_build_to_preflight =
      g_pipeline_cache_counters.cross_stage_reused_build_to_preflight;
  out.cross_stage_reused_other = g_pipeline_cache_counters.cross_stage_reused_other;
  out.cross_stage_saved_ms_estimate = g_pipeline_cache_counters.cross_stage_saved_ms_estimate;
  out.disk_hits = g_pipeline_cache_counters.disk_hits;
  out.disk_misses = g_pipeline_cache_counters.disk_misses;
  out.disk_writes = g_pipeline_cache_counters.disk_writes;
  out.disk_expired = g_pipeline_cache_counters.disk_expired;
  out.disk_evictions = g_pipeline_cache_counters.disk_evictions;
  out.disk_entries = g_pipeline_cache_counters.disk_entries;
  out.entries = g_pipeline_cache.size();
  return out;
}

static std::size_t counter_delta(std::size_t after, std::size_t before) {
  return (after >= before) ? (after - before) : 0;
}

void emit_cache_report(const CliOptions& opt, const CompilePipelineCacheStats& before, std::ostream& os) {
  if (!opt.cache_report) return;
  const auto after = get_compile_pipeline_cache_stats();
  const std::size_t hits = counter_delta(after.hits, before.hits);
  const std::size_t misses = counter_delta(after.misses, before.misses);
  const std::size_t evictions = counter_delta(after.evictions, before.evictions);
  const std::size_t cross_stage = counter_delta(after.cross_stage_candidates, before.cross_stage_candidates);
  const std::size_t cross_stage_p2b =
      counter_delta(after.cross_stage_preflight_to_build, before.cross_stage_preflight_to_build);
  const std::size_t cross_stage_b2p =
      counter_delta(after.cross_stage_build_to_preflight, before.cross_stage_build_to_preflight);
  const std::size_t cross_stage_other = counter_delta(after.cross_stage_other, before.cross_stage_other);
  const std::size_t cross_stage_reused =
      counter_delta(after.cross_stage_reused, before.cross_stage_reused);
  const std::size_t cross_stage_reused_p2b = counter_delta(
      after.cross_stage_reused_preflight_to_build, before.cross_stage_reused_preflight_to_build);
  const std::size_t cross_stage_reused_b2p = counter_delta(
      after.cross_stage_reused_build_to_preflight, before.cross_stage_reused_build_to_preflight);
  const std::size_t cross_stage_reused_other =
      counter_delta(after.cross_stage_reused_other, before.cross_stage_reused_other);
  const std::size_t cross_stage_saved_ms =
      counter_delta(after.cross_stage_saved_ms_estimate, before.cross_stage_saved_ms_estimate);
  const std::size_t disk_hits = counter_delta(after.disk_hits, before.disk_hits);
  const std::size_t disk_misses = counter_delta(after.disk_misses, before.disk_misses);
  const std::size_t disk_writes = counter_delta(after.disk_writes, before.disk_writes);
  const std::size_t disk_expired = counter_delta(after.disk_expired, before.disk_expired);
  const std::size_t disk_evictions = counter_delta(after.disk_evictions, before.disk_evictions);

  if (opt.cache_report_format == CacheReportFormat::Json) {
    os << "{\"cache_report\":{"
       << "\"hits\":" << hits
       << ",\"misses\":" << misses
       << ",\"evictions\":" << evictions
       << ",\"entries\":" << after.entries
       << ",\"cross_stage_candidates\":" << cross_stage
       << ",\"cross_stage_breakdown\":{"
       << "\"preflight_to_build\":" << cross_stage_p2b
       << ",\"build_to_preflight\":" << cross_stage_b2p
       << ",\"other\":" << cross_stage_other
       << "}"
       << ",\"cross_stage_reused\":" << cross_stage_reused
       << ",\"cross_stage_reused_breakdown\":{"
       << "\"preflight_to_build\":" << cross_stage_reused_p2b
       << ",\"build_to_preflight\":" << cross_stage_reused_b2p
       << ",\"other\":" << cross_stage_reused_other
       << "}"
       << ",\"cross_stage_saved_ms_estimate\":" << cross_stage_saved_ms
       << ",\"disk_hits\":" << disk_hits
       << ",\"disk_misses\":" << disk_misses
       << ",\"disk_writes\":" << disk_writes
       << ",\"disk_expired\":" << disk_expired
       << ",\"disk_evictions\":" << disk_evictions
       << ",\"disk_entries\":" << after.disk_entries
       << "}"
       << "}\n";
    return;
  }

  os << "cache-report: hits=" << hits
     << " misses=" << misses
     << " evictions=" << evictions
     << " entries=" << after.entries
     << " cross-stage-candidates=" << cross_stage
     << " (preflight->build=" << cross_stage_p2b
     << " build->preflight=" << cross_stage_b2p
     << " other=" << cross_stage_other << ")"
     << " cross-stage-reused=" << cross_stage_reused
     << " (preflight->build=" << cross_stage_reused_p2b
     << " build->preflight=" << cross_stage_reused_b2p
     << " other=" << cross_stage_reused_other << ")"
     << " cross-stage-saved-ms-estimate=" << cross_stage_saved_ms
     << " disk-hits=" << disk_hits
     << " disk-misses=" << disk_misses
     << " disk-writes=" << disk_writes
     << " disk-expired=" << disk_expired
     << " disk-evictions=" << disk_evictions
     << " disk-entries=" << after.disk_entries << "\n";
}

CompilePipelineResult run_compile_pipeline(const std::string& src,
                                           const CompilePipelineOptions& opt) {
  CompilePipelineOptions single_opt = opt;
  if (single_opt.cache_key_source.empty()) {
    single_opt.cache_key_source = source_identity_seed(single_opt.source_path, src);
  }
  return run_compile_pipeline(
      std::vector<nebula::frontend::SourceFile>{{single_opt.source_path, src, {}, {}, {}}},
      single_opt);
}

CompilePipelineResult run_compile_pipeline(const std::vector<nebula::frontend::SourceFile>& sources,
                                           const CompilePipelineOptions& opt) {
  const std::string blind_key = compile_pipeline_cache_key(sources, opt);
  const std::string key = compile_pipeline_cache_key_with_stage(sources, opt);
  auto it = g_pipeline_cache.find(key);
  if (it != g_pipeline_cache.end()) {
    ++g_pipeline_cache_counters.hits;
    CompilePipelineResult cached = it->second;
    if (opt.stage == nebula::frontend::DiagnosticStage::Build &&
        !cached.has_cached_cpp && (!cached.nir_prog || !cached.rep_owner)) {
      // Stale/incomplete in-memory payload cannot satisfy build codegen; force recompute.
    } else {
      cached.cache_hit = true;
      cached.cross_stage_reused = false;
      return cached;
    }
  }

  std::vector<nebula::frontend::Diagnostic> disk_cache_notes;
  auto add_disk_cache_note = [&](std::string code, std::string message, std::string cause) {
    disk_cache_notes.push_back(make_cli_diag(
        nebula::frontend::Severity::Note, std::move(code), std::move(message), opt.stage,
        nebula::frontend::DiagnosticRisk::Low, std::move(cause),
        "analysis cache fell back to local pipeline execution"));
  };

  if (opt.disk_cache_enabled) {
    if (opt.disk_cache_prune) {
      std::error_code canon_ec;
      const fs::path canon = fs::weakly_canonical(opt.disk_cache_dir, canon_ec);
      const std::string prune_key = canon_ec ? opt.disk_cache_dir.lexically_normal().string()
                                             : canon.lexically_normal().string();
      if (g_disk_cache_prune_once.insert(prune_key).second) {
        prune_disk_cache_entries(opt, true);
      }
    }

    if (!ensure_disk_cache_dirs(opt)) {
      add_disk_cache_note("NBL-CLI-CACHE-IO",
                          "disk cache directory is not writable: " + opt.disk_cache_dir.string(),
                          "failed to create cache directory");
    } else {
      const fs::path path = disk_cache_entry_path(opt, key);
      std::error_code ec;
      if (!fs::exists(path, ec) || ec) {
        ++g_pipeline_cache_counters.disk_misses;
      } else {
        bool expired = false;
        if (opt.disk_cache_ttl_sec > 0) {
          const auto mtime = fs::last_write_time(path, ec);
          if (!ec) {
            const auto ttl = std::chrono::seconds(opt.disk_cache_ttl_sec);
            const auto age = fs::file_time_type::clock::now() - mtime;
            if (age > ttl) expired = true;
          }
        }

        if (expired) {
          std::error_code rm_ec;
          fs::remove(path, rm_ec);
          if (!rm_ec) ++g_pipeline_cache_counters.disk_expired;
          ++g_pipeline_cache_counters.disk_misses;
        } else {
          CompilePipelineResult from_disk;
          if (read_disk_cache_entry(path, key, from_disk)) {
            const bool missing_build_payload =
                (opt.stage == nebula::frontend::DiagnosticStage::Build && !from_disk.has_error &&
                 !from_disk.has_cached_cpp);
            if (missing_build_payload) {
              std::error_code rm_ec;
              fs::remove(path, rm_ec);
              ++g_pipeline_cache_counters.disk_misses;
              add_disk_cache_note("NBL-CLI-CACHE-READ",
                                  "disk cache entry missing codegen payload and has been dropped",
                                  "stale build-stage cache entry without cached C++");
            } else {
              ++g_pipeline_cache_counters.disk_hits;
              from_disk.cache_hit = true;
              from_disk.cross_stage_reused = false;
              summarize_diag_levels(from_disk);
              std::error_code touch_ec;
              fs::last_write_time(path, fs::file_time_type::clock::now(), touch_ec);
              refresh_disk_cache_entry_count(opt);

              if (g_pipeline_cache.size() >= 256) {
                g_pipeline_cache.clear();
                g_pipeline_cache_cross_stage_index.clear();
                ++g_pipeline_cache_counters.evictions;
              }
              g_pipeline_cache.insert_or_assign(key, from_disk);
              g_pipeline_cache_cross_stage_index[blind_key] |= stage_bit(opt.stage);
              return from_disk;
            }
          } else {
            std::error_code rm_ec;
            fs::remove(path, rm_ec);
            ++g_pipeline_cache_counters.disk_misses;
            add_disk_cache_note("NBL-CLI-CACHE-READ",
                                "disk cache entry was invalid and has been dropped",
                                "cache entry parse failed for key: " + key);
          }
        }
      }
      refresh_disk_cache_entry_count(opt);
    }
  }

  ++g_pipeline_cache_counters.misses;
  auto csi = g_pipeline_cache_cross_stage_index.find(blind_key);
  if (csi != g_pipeline_cache_cross_stage_index.end()) {
    const std::uint8_t seen = csi->second;
    const std::uint8_t current = stage_bit(opt.stage);
    if ((seen & current) == 0u && seen != 0u) {
      ++g_pipeline_cache_counters.cross_stage_candidates;
      const std::uint8_t preflight = stage_bit(nebula::frontend::DiagnosticStage::Preflight);
      const std::uint8_t build = stage_bit(nebula::frontend::DiagnosticStage::Build);
      if (current == build && (seen & preflight) != 0u) {
        ++g_pipeline_cache_counters.cross_stage_preflight_to_build;
      } else if (current == preflight && (seen & build) != 0u) {
        ++g_pipeline_cache_counters.cross_stage_build_to_preflight;
      } else {
        ++g_pipeline_cache_counters.cross_stage_other;
      }
    }
  }

  if (opt.allow_cross_stage_reuse && csi != g_pipeline_cache_cross_stage_index.end()) {
    const std::uint8_t seen = csi->second;
    const std::uint8_t preflight = stage_bit(nebula::frontend::DiagnosticStage::Preflight);
    const std::uint8_t build = stage_bit(nebula::frontend::DiagnosticStage::Build);

    auto try_reuse_from = [&](nebula::frontend::DiagnosticStage from_stage)
        -> std::optional<CompilePipelineResult> {
      if ((seen & stage_bit(from_stage)) == 0u) return std::nullopt;
      const std::string from_key = stage_key_from_blind(blind_key, from_stage);
      auto fit = g_pipeline_cache.find(from_key);
      if (fit == g_pipeline_cache.end()) return std::nullopt;

      if (opt.stage == nebula::frontend::DiagnosticStage::Build &&
          from_stage == nebula::frontend::DiagnosticStage::Preflight &&
          (!fit->second.nir_prog || !fit->second.rep_owner)) {
        return std::nullopt;
      }

      CompilePipelineResult reused = fit->second;
      reused.cache_hit = true;
      reused.cross_stage_reused = true;
      ++g_pipeline_cache_counters.cross_stage_reused;
      g_pipeline_cache_counters.cross_stage_saved_ms_estimate += reused.analysis_elapsed_ms;

      const std::uint8_t current = stage_bit(opt.stage);
      if (current == build && from_stage == nebula::frontend::DiagnosticStage::Preflight) {
        ++g_pipeline_cache_counters.cross_stage_reused_preflight_to_build;
      } else if (current == preflight && from_stage == nebula::frontend::DiagnosticStage::Build) {
        ++g_pipeline_cache_counters.cross_stage_reused_build_to_preflight;
      } else {
        ++g_pipeline_cache_counters.cross_stage_reused_other;
      }

      retag_reused_diagnostics_for_stage(reused, opt.stage);
      summarize_diag_levels(reused);
      if (g_pipeline_cache.size() >= 256) {
        g_pipeline_cache.clear();
        g_pipeline_cache_cross_stage_index.clear();
        ++g_pipeline_cache_counters.evictions;
      }
      g_pipeline_cache.insert_or_assign(key, reused);
      g_pipeline_cache_cross_stage_index[blind_key] |= stage_bit(opt.stage);
      return reused;
    };

    if (opt.stage == nebula::frontend::DiagnosticStage::Build) {
      if (auto reused = try_reuse_from(nebula::frontend::DiagnosticStage::Preflight); reused.has_value()) {
        return *reused;
      }
    } else if (opt.stage == nebula::frontend::DiagnosticStage::Preflight) {
      if (auto reused = try_reuse_from(nebula::frontend::DiagnosticStage::Build); reused.has_value()) {
        return *reused;
      }
    }
  }

  auto finalize_and_store = [&](CompilePipelineResult& out) {
    if (!disk_cache_notes.empty()) {
      append_diags(out.diags, disk_cache_notes, opt.stage);
      summarize_diag_levels(out);
    }
    sort_diagnostics_stable(out.diags);
    summarize_diag_levels(out);
    if (opt.disk_cache_enabled && opt.stage == nebula::frontend::DiagnosticStage::Build &&
        !out.has_error && !out.has_cached_cpp && out.nir_prog && out.rep_owner) {
      nebula::codegen::EmitOptions eopt;
      eopt.main_mode = nebula::codegen::MainMode::CallMainIfPresent;
      eopt.strict_region = opt.strict_region;
      out.cached_cpp = nebula::codegen::emit_cpp23(*out.nir_prog, *out.rep_owner, eopt);
      out.has_cached_cpp = true;
    }

    if (g_pipeline_cache.size() >= 256) {
      g_pipeline_cache.clear();
      g_pipeline_cache_cross_stage_index.clear();
      ++g_pipeline_cache_counters.evictions;
    }
    g_pipeline_cache.insert_or_assign(key, out);
    g_pipeline_cache_cross_stage_index[blind_key] |= stage_bit(opt.stage);

    if (opt.disk_cache_enabled) {
      if (!ensure_disk_cache_dirs(opt)) {
        append_diag(out.diags,
                    make_cli_diag(
                        nebula::frontend::Severity::Note, "NBL-CLI-CACHE-IO",
                        "disk cache write skipped: unable to create cache directory",
                        opt.stage, nebula::frontend::DiagnosticRisk::Low,
                        "cache directory initialization failed",
                        "analysis result is still available for this run"),
                    opt.stage);
        summarize_diag_levels(out);
      } else {
        const fs::path path = disk_cache_entry_path(opt, key);
        if (write_disk_cache_entry(path, key, out)) {
          ++g_pipeline_cache_counters.disk_writes;
        } else {
          append_diag(out.diags,
                      make_cli_diag(
                          nebula::frontend::Severity::Note, "NBL-CLI-CACHE-WRITE",
                          "disk cache write failed for key: " + key,
                          opt.stage, nebula::frontend::DiagnosticRisk::Low,
                          "cache file write/rename failed",
                          "subsequent runs may miss disk cache"),
                      opt.stage);
          summarize_diag_levels(out);
        }
        prune_disk_cache_entries(opt, false);
      }
      refresh_disk_cache_entry_count(opt);
    }
  };

  CompilePipelineResult result;
  const auto started = std::chrono::steady_clock::now();
  auto elapsed_ms = [&]() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
  };
  auto maybe_emit_preflight_latency_note = [&](CompilePipelineResult& out) {
    if (opt.stage != nebula::frontend::DiagnosticStage::Preflight) return;
    if (elapsed_ms().count() > 200) {
      append_diag(out.diags,
                  make_cli_diag(
                      nebula::frontend::Severity::Note, "NBL-PR001",
                      "preflight exceeded 200ms/file budget; diagnostics may be reduced in future passes",
                      opt.stage, nebula::frontend::DiagnosticRisk::Low,
                      "preflight path exceeded internal latency target",
                      "future versions may skip non-essential checks to preserve interactive speed"),
                  opt.stage);
      summarize_diag_levels(out);
    }
  };

  try {
    std::vector<nebula::frontend::Program> parsed_programs;
    parsed_programs.reserve(sources.size());
    for (const auto& source : sources) {
      nebula::frontend::Lexer lex(source.text, source.path);
      auto toks = lex.lex_all();
      nebula::frontend::Parser parser(std::move(toks));
      auto program = parser.parse_program();
      if (!source.package_name.empty()) program.package_name = source.package_name;
      if (!source.module_name.empty() && !program.module_name.has_value()) {
        program.module_name = source.module_name;
      }
      parsed_programs.push_back(std::move(program));
    }

    std::vector<nebula::frontend::CompilationUnit> units;
    units.reserve(parsed_programs.size());
    for (std::size_t i = 0; i < parsed_programs.size(); ++i) {
      nebula::frontend::CompilationUnit unit;
      unit.program = &parsed_programs[i];
      unit.package_name = sources[i].package_name;
      unit.source_path = sources[i].path;
      unit.resolved_imports = sources[i].resolved_imports;
      units.push_back(std::move(unit));
    }

    nebula::frontend::TypecheckOptions tc_opt;
    tc_opt.warnings_as_errors = opt.warnings_as_errors;
    auto tc = nebula::frontend::typecheck(units, tc_opt);
    append_diags(result.diags, tc.diags, opt.stage);

    summarize_diag_levels(result);
    if (tc.programs.empty() || result.has_error) {
      maybe_emit_preflight_latency_note(result);
      result.analysis_elapsed_ms = static_cast<std::size_t>(elapsed_ms().count());
      finalize_and_store(result);
      return result;
    }

    auto nir_prog = nebula::nir::lower_to_nir(tc.programs);
    for (const auto& it : nir_prog.items) {
      if (!std::holds_alternative<nebula::nir::Function>(it.node)) continue;
      const auto& fn = std::get<nebula::nir::Function>(it.node);
      auto cfg = nebula::nir::build_cfg(fn);
      result.fn_count += 1;
      result.cfg_nodes += cfg.nodes.size();
    }

    auto call_resolution = nebula::passes::run_call_target_resolver(nir_prog);
    nebula::passes::EscapeAnalysisOptions eopt;
    eopt.profile = to_escape_profile(opt.profile);
    auto escape = nebula::passes::run_escape_analysis(nir_prog, call_resolution, eopt);
    nebula::passes::RepOwnerOptions ropt;
    ropt.strict_region = opt.strict_region;
    ropt.warnings_as_errors = opt.warnings_as_errors;
    auto rep_owner = nebula::passes::run_rep_owner_infer(nir_prog, escape, ropt, &call_resolution);
    append_diags(result.diags, rep_owner.diags, opt.stage);
    auto borrow_xstmt = nebula::passes::run_borrow_xstmt(nir_prog, escape, call_resolution);
    append_diags(result.diags, borrow_xstmt.diags, opt.stage);

    summarize_diag_levels(result);
    if (!result.has_error && opt.include_lint) {
      // Millisecond budgets are coarse; for tiny budgets we proactively trim optional lint.
      const bool aggressive_tiny_budget = (opt.budget_ms > 0 && opt.budget_ms <= 2);
      const bool budget_hit_before_lint =
          aggressive_tiny_budget || (opt.budget_ms > 0 && elapsed_ms().count() >= opt.budget_ms);
      if (budget_hit_before_lint) {
        append_diag(result.diags,
                    make_cli_diag(
                        nebula::frontend::Severity::Note, "NBL-PR002",
                        "analysis budget exceeded; skipped optional lint pass",
                        opt.stage, nebula::frontend::DiagnosticRisk::Low,
                        "elapsed analysis time exceeded --diag-budget-ms",
                        "performance/style diagnostics may be incomplete in this run",
                        {"increase --diag-budget-ms", "or use release/smart mode in CI"}), opt.stage);
        summarize_diag_levels(result);
      } else {
        nebula::passes::EpistemicLintOptions lopt;
        lopt.warnings_as_errors = opt.warnings_as_errors;
        lopt.profile = to_lint_profile(opt.profile);
        auto lint_diags = nebula::passes::run_epistemic_lint(nir_prog, rep_owner, lopt);
        append_diags(result.diags, lint_diags, opt.stage);
        summarize_diag_levels(result);
      }
    }

    result.nir_prog = std::make_shared<nebula::nir::Program>(std::move(nir_prog));
    result.rep_owner = std::make_shared<nebula::passes::RepOwnerResult>(std::move(rep_owner));

    maybe_emit_preflight_latency_note(result);
    result.analysis_elapsed_ms = static_cast<std::size_t>(elapsed_ms().count());
  } catch (const nebula::frontend::FrontendError& e) {
    nebula::frontend::Diagnostic d;
    d.severity = nebula::frontend::Severity::Error;
    d.code = "NBL-PAR900";
    d.message = e.what();
    d.span = e.span;
    if (d.span.source_path.empty()) {
      d.span.source_path = opt.source_path.empty() ? (sources.empty() ? std::string{} : sources.front().path)
                                                   : opt.source_path;
    }
    d.category = "parser";
    d.risk = nebula::frontend::DiagnosticRisk::High;
    append_diag(result.diags, std::move(d), opt.stage);
    summarize_diag_levels(result);
    result.analysis_elapsed_ms = static_cast<std::size_t>(elapsed_ms().count());
  }

  finalize_and_store(result);
  return result;
}

static void maybe_dump_ownership(const CompilePipelineResult& result) {
  if (!result.nir_prog || !result.rep_owner) return;
  for (const auto& item : result.nir_prog->items) {
    if (!std::holds_alternative<nebula::nir::Function>(item.node)) continue;
    const auto& fn = std::get<nebula::nir::Function>(item.node);
    auto itf = result.rep_owner->by_function.find(nebula::nir::function_identity(fn));
    if (itf == result.rep_owner->by_function.end()) continue;

    const auto names = collect_var_names(fn);
    std::vector<nebula::nir::VarId> ids;
    ids.reserve(itf->second.vars.size());
    for (const auto& [id, _] : itf->second.vars) ids.push_back(id);
    std::sort(ids.begin(), ids.end());

    std::cerr << "ownership: fn " << nebula::nir::function_identity(fn) << "\n";
    for (nebula::nir::VarId id : ids) {
      const auto dit = itf->second.vars.find(id);
      if (dit == itf->second.vars.end()) continue;
      const auto nit = names.find(id);
      const std::string var_name = (nit == names.end()) ? "?" : nit->second;

      const auto& dec = dit->second;
      const std::string region = (dec.rep == nebula::passes::RepKind::Region) ? dec.region : "-";
      std::cerr << "  v" << id << " " << var_name << " -> rep=" << rep_name(dec.rep)
                << " owner=" << owner_name(dec.owner) << " region=" << region << "\n";
    }
  }
}

static void maybe_dump_cfg_ir(const CompilePipelineResult& result) {
  if (!result.nir_prog) return;
  for (const auto& item : result.nir_prog->items) {
    if (!std::holds_alternative<nebula::nir::Function>(item.node)) continue;
    const auto& fn = std::get<nebula::nir::Function>(item.node);
    auto cfg_ir = nebula::nir::cfgir::lower_to_cfg_ir(fn);
    std::cerr << nebula::nir::cfgir::dump_cfg_ir(cfg_ir);
  }
}

static std::string default_cxx() {
  const char* cxx = std::getenv("CXX");
  if (cxx && *cxx) return cxx;
  return "clang++";
}

static std::vector<std::string> compile_flags_for(const CliOptions& opt, CompileFlavor flavor) {
  if (flavor == CompileFlavor::Test) {
    return {"-O1", "-g", "-fno-omit-frame-pointer", "-fsanitize=address,undefined"};
  }
  if (flavor == CompileFlavor::Bench) {
    return {"-O2", "-DNDEBUG"};
  }
  if (opt.mode == BuildMode::Release) {
    return {"-O3", "-DNDEBUG"};
  }
  return {"-O0", "-g3"};
}

int compile_cpp(const CliOptions& opt,
                const fs::path& cpp_path,
                const fs::path& out_bin,
                CompileFlavor flavor,
                const std::vector<fs::path>& extra_sources) {
  if (out_bin.has_parent_path()) fs::create_directories(out_bin.parent_path());
  std::vector<std::string> cmd;
  cmd.push_back(default_cxx());
  cmd.push_back("-std=c++23");
  for (const auto& flag : compile_flags_for(opt, flavor)) cmd.push_back(flag);
  fs::path include_root = opt.repo_root;
  const fs::path generated_include =
      opt.repo_root / "build" / "generated" / "include" / "runtime" / "nebula_runtime.hpp";
  if (fs::exists(generated_include)) {
    include_root = opt.repo_root / "build" / "generated" / "include";
  }
  cmd.push_back("-I" + include_root.string());
  cmd.push_back(cpp_path.string());
  for (const auto& extra : extra_sources) cmd.push_back(extra.string());
  cmd.push_back("-o");
  cmd.push_back(out_bin.string());
  return run_command(cmd);
}

bool write_text_file(const fs::path& path, const std::string& text) {
  if (path.has_parent_path()) fs::create_directories(path.parent_path());
  std::ofstream out(path);
  if (!out) return false;
  out << text;
  return true;
}

fs::path cpp_output_path(const fs::path& source, const CliOptions& opt, const std::string& suffix) {
  return opt.out_dir / (source.stem().string() + suffix + ".cpp");
}

static fs::path default_cache_artifact_path(const fs::path& source, const CliOptions& opt) {
  return opt.out_dir / (source.stem().string() + ".out");
}

static fs::path default_legacy_artifact_path(const fs::path& source) {
  return fs::current_path() / (source.stem().string() + ".out");
}

fs::path chosen_artifact_path(const fs::path& source, const CliOptions& opt) {
  if (opt.out_path.has_value()) return *opt.out_path;
  return default_cache_artifact_path(source, opt);
}

static fs::path artifact_meta_path(const fs::path& artifact) {
  return fs::path(artifact.string() + ".nebmeta");
}

std::string hash_source(const std::string& src) {
  std::uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : src) {
    h ^= static_cast<std::uint64_t>(c);
    h *= 1099511628211ULL;
  }
  std::ostringstream os;
  os << std::hex << h;
  return os.str();
}

ArtifactMeta expected_meta_for(const CliOptions& opt,
                                      AnalysisProfile resolved_profile,
                                      const std::string& source_hash) {
  ArtifactMeta m;
  m.source_hash = source_hash;
  m.mode = (opt.mode == BuildMode::Release) ? "release" : "debug";
  switch (resolved_profile) {
  case AnalysisProfile::Auto: m.profile = "auto"; break;
  case AnalysisProfile::Fast: m.profile = "fast"; break;
  case AnalysisProfile::Deep: m.profile = "deep"; break;
  }
  m.compiler_schema_version = kCompilePipelineCompilerSchemaVersion;
  m.cache_schema_version = kCompilePipelineCacheSchemaVersion;
  m.strict_region = opt.strict_region;
  m.warnings_as_errors = opt.warnings_as_errors;
  return m;
}

bool write_artifact_meta(const fs::path& artifact, const ArtifactMeta& m) {
  std::ofstream out(artifact_meta_path(artifact));
  if (!out) return false;
  out << "version=" << kArtifactMetaVersion << "\n";
  out << "source_hash=" << m.source_hash << "\n";
  out << "mode=" << m.mode << "\n";
  out << "profile=" << m.profile << "\n";
  out << "compiler_schema_version=" << m.compiler_schema_version << "\n";
  out << "cache_schema_version=" << m.cache_schema_version << "\n";
  out << "strict_region=" << (m.strict_region ? "1" : "0") << "\n";
  out << "warnings_as_errors=" << (m.warnings_as_errors ? "1" : "0") << "\n";
  return true;
}

std::optional<ArtifactMeta> read_artifact_meta(const fs::path& artifact) {
  std::ifstream in(artifact_meta_path(artifact));
  if (!in) return std::nullopt;

  ArtifactMeta m;
  int version = 0;
  std::string line;
  while (std::getline(in, line)) {
    const std::size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = line.substr(0, eq);
    const std::string value = line.substr(eq + 1);
    if (key == "version") version = std::atoi(value.c_str());
    if (key == "source_hash") m.source_hash = value;
    if (key == "mode") m.mode = value;
    if (key == "profile") m.profile = value;
    if (key == "compiler_schema_version") m.compiler_schema_version = std::atoi(value.c_str());
    if (key == "cache_schema_version") m.cache_schema_version = std::atoi(value.c_str());
    if (key == "strict_region") m.strict_region = (value == "1");
    if (key == "warnings_as_errors") m.warnings_as_errors = (value == "1");
  }
  if (version != kArtifactMetaVersion) return std::nullopt;
  if (m.source_hash.empty() || m.mode.empty() || m.profile.empty()) return std::nullopt;
  return m;
}

bool artifact_meta_matches(const ArtifactMeta& lhs, const ArtifactMeta& rhs) {
  return lhs.source_hash == rhs.source_hash && lhs.mode == rhs.mode && lhs.profile == rhs.profile &&
         lhs.compiler_schema_version == rhs.compiler_schema_version &&
         lhs.cache_schema_version == rhs.cache_schema_version &&
         lhs.strict_region == rhs.strict_region && lhs.warnings_as_errors == rhs.warnings_as_errors;
}

static bool preflight_gate_blocks(const nebula::frontend::Diagnostic& d, const CliOptions& opt) {
  using nebula::frontend::DiagnosticStage;

  if (d.stage != DiagnosticStage::Preflight) return false;
  if (d.is_error()) return true;
  if (!d.is_warning()) return false;

  if (opt.run_gate == RunGate::None) return false;
  if (opt.run_gate == RunGate::All) return true; // note does not gate

  const std::uint32_t gate_mask = effective_gate_dimension_mask(opt);
  const std::uint32_t dim_bit = gate_dimension_bit_for_code(d.code);
  if ((gate_mask & dim_bit) == 0u) return false;

  const int threshold = gate_threshold_for_profile(opt.gate_profile);
  const int weight = gate_weight_for_warning(d);
  return weight >= threshold;
}

static bool has_preflight_blockers(const std::vector<nebula::frontend::Diagnostic>& diags,
                                   const CliOptions& opt) {
  for (const auto& d : diags) {
    if (preflight_gate_blocks(d, opt)) return true;
  }
  return false;
}

ArtifactLookupResult resolve_no_build_artifact(const fs::path& source_file,
                                                      const CliOptions& opt) {
  using nebula::frontend::DiagnosticRisk;
  using nebula::frontend::DiagnosticStage;
  using nebula::frontend::Severity;

  ArtifactLookupResult out;

  if (opt.out_path.has_value()) {
    if (fs::exists(*opt.out_path)) {
      out.artifact = *opt.out_path;
      return out;
    }
    out.diags.push_back(make_cli_diag(
        Severity::Error, "NBL-CLI-NOBUILD-MISSING",
        "--no-build requires an existing artifact at: " + opt.out_path->string(),
        DiagnosticStage::Build, DiagnosticRisk::High,
        "explicit output path does not exist",
        "run cannot proceed without a compiled binary",
        {"build once without --no-build to create the artifact",
         "check -o/--out path spelling"}));
    return out;
  }

  std::vector<fs::path> candidates;
  const fs::path cache = default_cache_artifact_path(source_file, opt);
  const fs::path legacy = default_legacy_artifact_path(source_file);
  if (fs::exists(cache)) candidates.push_back(cache);
  if (fs::exists(legacy)) candidates.push_back(legacy);

  std::set<std::string> seen;
  std::vector<fs::path> dedup;
  for (const auto& p : candidates) {
    std::error_code ec;
    const fs::path norm = fs::weakly_canonical(p, ec);
    const std::string key = ec ? p.lexically_normal().string() : norm.string();
    if (!seen.insert(key).second) continue;
    dedup.push_back(p);
  }

  if (dedup.empty()) {
    out.diags.push_back(make_cli_diag(
        Severity::Error, "NBL-CLI-NOBUILD-MISSING",
        "--no-build requested but no artifact was found", DiagnosticStage::Build,
        DiagnosticRisk::High,
        "neither cache nor legacy artifact path exists",
        "run cannot execute without a binary",
        {"build once to generate an artifact", "or pass -o/--out to point to an existing binary"}));
    return out;
  }

  if (dedup.size() > 1) {
    out.diags.push_back(make_cli_diag(
        Severity::Error, "NBL-CLI-AMBIGUOUS-ARTIFACT",
        "multiple artifacts found; pass -o/--out to select one", DiagnosticStage::Build,
        DiagnosticRisk::High,
        "cache and legacy locations both contain binaries",
        "running the wrong artifact can execute stale code",
        {"provide explicit -o/--out path", "delete stale artifact not intended for execution"}));
    return out;
  }

  out.artifact = dedup.front();
  return out;
}

bool read_source(const fs::path& file,
                        std::string& out,
                        std::vector<nebula::frontend::Diagnostic>& diags,
                        nebula::frontend::DiagnosticStage stage) {
  std::ifstream in(file);
  if (!in) {
    append_diag(diags,
                make_cli_diag(
                    nebula::frontend::Severity::Error, "NBL-CLI-IO001",
                    "cannot open file: " + file.string(), stage,
                    nebula::frontend::DiagnosticRisk::High,
                    "source file path is not readable",
                    "compilation cannot start", {"verify file path and permissions"}),
                stage);
    return false;
  }

  out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return true;
}

int run_preflight_if_enabled(const fs::path& file,
                             const std::vector<nebula::frontend::SourceFile>& sources,
                             const std::string& cache_key_source,
                             const CliOptions& opt) {
  if (opt.preflight == PreflightMode::Off) return 0;

  CompilePipelineOptions popt;
  popt.mode = BuildMode::Debug;
  popt.profile = AnalysisProfile::Fast;
  popt.analysis_tier = AnalysisTier::Basic;
  popt.strict_region = opt.strict_region;
  popt.warnings_as_errors = opt.warnings_as_errors;
  popt.include_lint = true;
  popt.allow_cross_stage_reuse = (opt.cross_stage_reuse == CrossStageReuseMode::Safe);
  popt.disk_cache_enabled = (opt.disk_cache == DiskCacheMode::On);
  popt.disk_cache_ttl_sec = opt.disk_cache_ttl_sec;
  popt.disk_cache_max_entries = opt.disk_cache_max_entries;
  popt.disk_cache_dir = opt.disk_cache_dir;
  popt.disk_cache_prune = opt.disk_cache_prune;
  popt.budget_ms = opt.diag_budget_ms;
  popt.source_path = file.string();
  popt.cache_key_source = cache_key_source;
  popt.stage = nebula::frontend::DiagnosticStage::Preflight;

  auto preflight = run_compile_pipeline(sources, popt);
  emit_diagnostics(preflight.diags, opt, std::cerr);

  if (has_preflight_blockers(preflight.diags, opt)) {
    std::cerr << "error: run blocked by preflight gate\n";
    return 1;
  }

  return 0;
}

namespace {
struct CacheReportScope {
  const CliOptions& opt;
  const CompilePipelineCacheStats before;

  explicit CacheReportScope(const CliOptions& options)
      : opt(options), before(get_compile_pipeline_cache_stats()) {}

  ~CacheReportScope() { emit_cache_report(opt, before, std::cerr); }
};
} // namespace

int cmd_check(const fs::path& file, const CliOptions& opt) {
  CacheReportScope cache_scope(opt);
  auto loaded = load_compile_input(file, nebula::frontend::DiagnosticStage::Build);
  if (!loaded.diags.empty()) emit_diagnostics(loaded.diags, opt, std::cerr);
  if (!loaded.ok) {
    return 1;
  }
  const fs::path effective_file = loaded.entry_file.empty() ? file : loaded.entry_file;
  AnalysisProfile requested = opt.profile_explicit ? opt.analysis_profile : AnalysisProfile::Deep;
  const AnalysisProfile resolved = resolve_profile(opt.mode, requested);
  const AnalysisTier tier = resolve_analysis_tier(opt.mode, opt.analysis_tier);

  CompilePipelineOptions popt;
  popt.mode = opt.mode;
  popt.profile = resolved;
  popt.analysis_tier = tier;
  popt.strict_region = opt.strict_region;
  popt.warnings_as_errors = opt.warnings_as_errors;
  popt.include_lint = true;
  popt.dump_ownership = opt.dump_ownership;
  popt.dump_cfg_ir = opt.dump_cfg_ir;
  popt.budget_ms = opt.diag_budget_ms;
  popt.source_path = effective_file.string();
  popt.cache_key_source = loaded.cache_key_source;
  popt.stage = nebula::frontend::DiagnosticStage::Build;

  auto result = run_compile_pipeline(loaded.compile_sources, popt);
  emit_diagnostics(result.diags, opt, std::cerr);

  if (!result.has_error) {
    if (opt.dump_ownership) maybe_dump_ownership(result);
    if (opt.dump_cfg_ir) maybe_dump_cfg_ir(result);
    std::cerr << "info: lowered to NIR (" << result.fn_count
              << " function(s)), built CFG (" << result.cfg_nodes << " node(s) total)\n";
    std::cerr << "ok: check passed";
    if (result.has_warning) std::cerr << " (with warnings)";
    std::cerr << "\n";
  }

  return result.has_error ? 1 : 0;
}

static int cmd_test_project_target(const fs::path& target, const CliOptions& opt) {
  auto loaded = load_compile_input(target, nebula::frontend::DiagnosticStage::Build);
  if (!loaded.diags.empty()) emit_diagnostics(loaded.diags, opt, std::cerr);
  if (!loaded.ok) return 1;
  return cmd_test_project_target(opt, loaded);
}

static int cmd_bench_project_target(const fs::path& target, const CliOptions& opt) {
  auto loaded = load_compile_input(target, nebula::frontend::DiagnosticStage::Build);
  if (!loaded.diags.empty()) emit_diagnostics(loaded.diags, opt, std::cerr);
  if (!loaded.ok) return 1;
  return cmd_bench_project_target(opt, loaded);
}


int cmd_test(const CliOptions& opt) {
  CacheReportScope cache_scope(opt);
  if (is_explicit_project_target(opt.dir)) return cmd_test_project_target(opt.dir, opt);
  const auto files = list_nb_files(opt.dir);
  if (files.empty()) {
    auto d = make_cli_diag(
        nebula::frontend::Severity::Error, "NBL-CLI-TEST-EMPTY",
        "no .nb files found in: " + opt.dir.string(),
        nebula::frontend::DiagnosticStage::Build, nebula::frontend::DiagnosticRisk::High,
        "test directory has no Nebula sources", "test run cannot start",
        {"create .nb files under --dir", "or pass --dir to a directory with tests"});
    emit_diagnostics({d}, opt, std::cerr);
    return 1;
  }

  int failed = 0;
  const AnalysisProfile requested = opt.profile_explicit ? opt.analysis_profile : AnalysisProfile::Fast;
  const AnalysisProfile resolved_profile = resolve_profile(opt.mode, requested);
  const AnalysisTier tier = resolve_analysis_tier(opt.mode, opt.analysis_tier);

  for (const auto& file : files) {
    std::string src;
    std::vector<nebula::frontend::Diagnostic> io_diags;
    if (!read_source(file, src, io_diags, nebula::frontend::DiagnosticStage::Build)) {
      emit_diagnostics(io_diags, opt, std::cerr);
      failed = 1;
      continue;
    }

    CompilePipelineOptions popt;
    popt.mode = opt.mode;
    popt.profile = resolved_profile;
    popt.analysis_tier = tier;
    popt.strict_region = opt.strict_region;
    popt.warnings_as_errors = opt.warnings_as_errors;
    popt.include_lint = should_include_lint_in_build_stage(opt);
    popt.budget_ms = opt.diag_budget_ms;
    popt.source_path = file.string();
    popt.cache_key_source = source_identity_seed(file.string(), src);
    popt.stage = nebula::frontend::DiagnosticStage::Build;

    auto analysis = run_compile_pipeline(
        std::vector<nebula::frontend::SourceFile>{{file.string(), src, {}, {}, {}}}, popt);
    emit_diagnostics(analysis.diags, opt, std::cerr);
    if (analysis.has_error || !analysis.nir_prog || !analysis.rep_owner) {
      failed = 1;
      continue;
    }

    if (!nir_has_fn_with_ann(*analysis.nir_prog, "test")) continue;

    const fs::path out_cpp = cpp_output_path(file, opt, "_test");
    const fs::path out_bin = opt.out_dir / (file.stem().string() + "_test.out");

    nebula::codegen::EmitOptions eopt;
    eopt.main_mode = nebula::codegen::MainMode::RunTests;
    eopt.strict_region = opt.strict_region;
    const std::string cpp = nebula::codegen::emit_cpp23(*analysis.nir_prog, *analysis.rep_owner, eopt);

    if (!write_text_file(out_cpp, cpp)) {
      auto d = make_cli_diag(
          nebula::frontend::Severity::Error, "NBL-CLI-IO002",
          "failed to write generated C++: " + out_cpp.string(),
          nebula::frontend::DiagnosticStage::Build, nebula::frontend::DiagnosticRisk::High,
          "output directory is not writable", "test build cannot proceed");
      emit_diagnostics({d}, opt, std::cerr);
      failed = 1;
      continue;
    }

    std::cerr << "wrote: " << out_cpp.string() << "\n";
    if (compile_cpp(opt, out_cpp, out_bin, CompileFlavor::Test) != 0) {
      failed = 1;
      continue;
    }

    if (run_command({out_bin.string()}) != 0) {
      failed = 1;
      continue;
    }
  }

  return failed ? 1 : 0;
}

int cmd_bench(const CliOptions& opt) {
  CacheReportScope cache_scope(opt);
  if (is_explicit_project_target(opt.dir)) return cmd_bench_project_target(opt.dir, opt);
  const auto files = list_nb_files(opt.dir);
  if (files.empty()) {
    auto d = make_cli_diag(
        nebula::frontend::Severity::Error, "NBL-CLI-BENCH-EMPTY",
        "no .nb files found in: " + opt.dir.string(),
        nebula::frontend::DiagnosticStage::Build, nebula::frontend::DiagnosticRisk::High,
        "bench directory has no Nebula sources", "bench run cannot start");
    emit_diagnostics({d}, opt, std::cerr);
    return 1;
  }

  fs::create_directories("benchmark_results");
  const fs::path csv_path = fs::path("benchmark_results") / "latency.csv";
  std::ofstream csv(csv_path);
  csv << "file,warmup_iterations,measure_iterations,samples,p50_ms,p90_ms,p99_ms,mean_ms,stddev_ms,throughput_ops_s,clock,platform,perf_capability,perf_counters,perf_reason\n";

  int failed = 0;
  const AnalysisProfile requested = opt.profile_explicit ? opt.analysis_profile : AnalysisProfile::Fast;
  const AnalysisProfile resolved_profile = resolve_profile(opt.mode, requested);
  const AnalysisTier tier = resolve_analysis_tier(opt.mode, opt.analysis_tier);

  for (const auto& file : files) {
    std::string src;
    std::vector<nebula::frontend::Diagnostic> io_diags;
    if (!read_source(file, src, io_diags, nebula::frontend::DiagnosticStage::Build)) {
      emit_diagnostics(io_diags, opt, std::cerr);
      failed = 1;
      continue;
    }

    CompilePipelineOptions popt;
    popt.mode = opt.mode;
    popt.profile = resolved_profile;
    popt.analysis_tier = tier;
    popt.strict_region = opt.strict_region;
    popt.warnings_as_errors = opt.warnings_as_errors;
    popt.include_lint = should_include_lint_in_build_stage(opt);
    popt.budget_ms = opt.diag_budget_ms;
    popt.source_path = file.string();
    popt.cache_key_source = source_identity_seed(file.string(), src);
    popt.stage = nebula::frontend::DiagnosticStage::Build;

    auto analysis = run_compile_pipeline(
        std::vector<nebula::frontend::SourceFile>{{file.string(), src, {}, {}, {}}}, popt);
    emit_diagnostics(analysis.diags, opt, std::cerr);
    if (analysis.has_error || !analysis.nir_prog || !analysis.rep_owner) {
      failed = 1;
      continue;
    }

    if (!nir_has_fn_with_ann(*analysis.nir_prog, "bench")) continue;

    const fs::path out_cpp = cpp_output_path(file, opt, "_bench");
    const fs::path out_bin = opt.out_dir / (file.stem().string() + "_bench.out");

    nebula::codegen::EmitOptions eopt;
    eopt.main_mode = nebula::codegen::MainMode::RunBench;
    eopt.strict_region = opt.strict_region;
    const std::string cpp = nebula::codegen::emit_cpp23(*analysis.nir_prog, *analysis.rep_owner, eopt);

    if (!write_text_file(out_cpp, cpp)) {
      auto d = make_cli_diag(
          nebula::frontend::Severity::Error, "NBL-CLI-IO002",
          "failed to write generated C++: " + out_cpp.string(),
          nebula::frontend::DiagnosticStage::Build, nebula::frontend::DiagnosticRisk::High,
          "output directory is not writable", "bench build cannot proceed");
      emit_diagnostics({d}, opt, std::cerr);
      failed = 1;
      continue;
    }

    std::cerr << "wrote: " << out_cpp.string() << "\n";
    if (compile_cpp(opt, out_cpp, out_bin, CompileFlavor::Bench) != 0) {
      failed = 1;
      continue;
    }

    int rc = 0;
    const std::string out_text = run_capture({out_bin.string()}, &rc);
    if (rc != 0) {
      std::cerr << out_text;
      failed = 1;
      continue;
    }

    int warmup = 0, measure = 0, samples = 0;
    double p50 = 0, p90 = 0, p99 = 0, mean = 0, stddev = 0, thr = 0;
    std::string clock_source;
    std::string platform;
    std::string perf_capability;
    std::string perf_counters;
    std::string perf_reason;
    if (!parse_bench_output(out_text, warmup, measure, samples, p50, p90, p99, mean, stddev, thr,
                            clock_source, platform, perf_capability, perf_counters, perf_reason)) {
      auto d = make_cli_diag(
          nebula::frontend::Severity::Error, "NBL-CLI-BENCH-PARSE",
          "failed to parse benchmark output for: " + file.string(),
          nebula::frontend::DiagnosticStage::Build, nebula::frontend::DiagnosticRisk::High,
          "benchmark runner output does not match expected format",
          "latency CSV row cannot be generated");
      emit_diagnostics({d}, opt, std::cerr);
      std::cerr << out_text;
      failed = 1;
      continue;
    }

    csv << file.filename().string() << "," << warmup << "," << measure << "," << samples << "," << p50
        << "," << p90 << "," << p99 << "," << mean << "," << stddev << "," << thr << ","
        << clock_source << "," << platform << "," << perf_capability << "," << perf_counters
        << "," << perf_reason << "\n";
    std::cerr << out_text;
  }

  std::cerr << "wrote: " << csv_path.string() << "\n";
  return failed ? 1 : 0;
}
