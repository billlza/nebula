#include "cli_shared.hpp"

#include <charconv>
#include <iostream>

#ifndef NEBULA_VERSION
#error "NEBULA_VERSION must be provided by the build system"
#endif

static constexpr const char* kNebulaVersion = "v" NEBULA_VERSION;

void print_version(std::ostream& os) {
  os << "nebula " << kNebulaVersion << "\n";
}

void print_usage() {
  std::cerr << "nebula " << kNebulaVersion << R"(

USAGE:
  nebula check <path> [--mode debug|release] [--profile auto|fast|deep] [--diag-format text|json]
                     [--analysis-tier basic|smart|deep] [--diag-view raw|grouped]
                     [--warn-policy strict|balanced|lenient] [--diag-budget-ms MS]
                     [--diag-grouping-delay-ms MS]
                     [--warn-class <class>=on|off]...
                     [--max-root-causes N] [--root-cause-v2 auto|on|off]
                     [--root-cause-top-k N] [--root-cause-min-covered N]
                     [--cache-report on|off] [--cache-report-format text|json]
                     [--warnings-as-errors] [--strict-region] [--dump-ownership] [--dump-cfg-ir]
  nebula build <path> [--mode debug|release] [--profile auto|fast|deep] [--diag-format text|json]
                     [--analysis-tier basic|smart|deep] [--diag-view raw|grouped]
                     [--warn-policy strict|balanced|lenient] [--diag-budget-ms MS]
                     [--diag-grouping-delay-ms MS]
                     [--warn-class <class>=on|off]...
                     [--max-root-causes N] [--root-cause-v2 auto|on|off]
                     [--root-cause-top-k N] [--root-cause-min-covered N]
                     [--cache-report on|off] [--cache-report-format text|json]
                     [-o|--out PATH] [--out-dir DIR] [--emit-cpp]
  nebula run <path> [--mode debug|release] [--profile auto|fast|deep] [--diag-format text|json]
                   [--analysis-tier basic|smart|deep] [--smart on|off]
                   [--diag-view raw|grouped] [--warn-policy strict|balanced|lenient]
                   [--warn-class <class>=on|off]...
                   [--gate-profile strict|balanced|lenient]
                   [--gate-dimension <name>=on|off]...
                   [--cross-stage-reuse off|safe]
                   [--disk-cache on|off] [--disk-cache-ttl-sec N] [--disk-cache-max-entries N]
                   [--disk-cache-dir DIR] [--disk-cache-prune]
                   [--diag-budget-ms MS] [--max-root-causes N]
                   [--diag-grouping-delay-ms MS]
                   [--root-cause-v2 auto|on|off] [--root-cause-top-k N]
                   [--root-cause-min-covered N]
                   [--cache-report on|off] [--cache-report-format text|json]
                   [--preflight fast|off] [--run-gate high|all|none] [--reuse] [--no-build]
                   [-o|--out PATH] [--out-dir DIR]
  nebula test [--dir PATH] [--mode debug|release] [--profile auto|fast|deep] [--diag-format text|json]
              [--analysis-tier basic|smart|deep] [--diag-view raw|grouped]
              [--warn-policy strict|balanced|lenient] [--diag-budget-ms MS]
              [--diag-grouping-delay-ms MS]
              [--warn-class <class>=on|off]...
              [--max-root-causes N] [--root-cause-v2 auto|on|off]
              [--root-cause-top-k N] [--root-cause-min-covered N]
              [--cache-report on|off] [--cache-report-format text|json]
  nebula bench [--dir PATH] [--mode debug|release] [--profile auto|fast|deep] [--diag-format text|json]
               [--analysis-tier basic|smart|deep] [--diag-view raw|grouped]
               [--warn-policy strict|balanced|lenient] [--diag-budget-ms MS]
               [--diag-grouping-delay-ms MS]
               [--warn-class <class>=on|off]...
               [--max-root-causes N] [--root-cause-v2 auto|on|off]
               [--root-cause-top-k N] [--root-cause-min-covered N]
               [--cache-report on|off] [--cache-report-format text|json]
  nebula new <path>
  nebula add <project-or-manifest> <name> <version>
  nebula add <project-or-manifest> <name> --path <dir>
  nebula add <project-or-manifest> <name> --git <url> --rev <rev>
  nebula publish <project-or-manifest> [--force]
  nebula fetch <project-or-manifest>
  nebula update <project-or-manifest>
  nebula fmt <file-or-dir>
  nebula explain <path> [--file PATH] [--line N] [--col N] [--format text|json]
  nebula lsp
)";
}

static bool parse_mode(std::string_view s, BuildMode& out) {
  if (s == "debug") {
    out = BuildMode::Debug;
    return true;
  }
  if (s == "release") {
    out = BuildMode::Release;
    return true;
  }
  return false;
}

static bool parse_profile(std::string_view s, AnalysisProfile& out) {
  if (s == "auto") {
    out = AnalysisProfile::Auto;
    return true;
  }
  if (s == "fast") {
    out = AnalysisProfile::Fast;
    return true;
  }
  if (s == "deep") {
    out = AnalysisProfile::Deep;
    return true;
  }
  return false;
}

static bool parse_diag_format(std::string_view s, DiagFormat& out) {
  if (s == "text") {
    out = DiagFormat::Text;
    return true;
  }
  if (s == "json") {
    out = DiagFormat::Json;
    return true;
  }
  return false;
}

static bool parse_analysis_tier(std::string_view s, AnalysisTier& out) {
  if (s == "basic") {
    out = AnalysisTier::Basic;
    return true;
  }
  if (s == "smart") {
    out = AnalysisTier::Smart;
    return true;
  }
  if (s == "deep") {
    out = AnalysisTier::Deep;
    return true;
  }
  return false;
}

static bool parse_diag_view(std::string_view s, DiagView& out) {
  if (s == "raw") {
    out = DiagView::Raw;
    return true;
  }
  if (s == "grouped") {
    out = DiagView::Grouped;
    return true;
  }
  return false;
}

static bool parse_warn_policy(std::string_view s, WarnPolicy& out) {
  if (s == "strict") {
    out = WarnPolicy::Strict;
    return true;
  }
  if (s == "balanced") {
    out = WarnPolicy::Balanced;
    return true;
  }
  if (s == "lenient") {
    out = WarnPolicy::Lenient;
    return true;
  }
  return false;
}

static bool parse_gate_profile(std::string_view s, GateProfile& out) {
  if (s == "strict") {
    out = GateProfile::Strict;
    return true;
  }
  if (s == "balanced") {
    out = GateProfile::Balanced;
    return true;
  }
  if (s == "lenient") {
    out = GateProfile::Lenient;
    return true;
  }
  return false;
}

static bool parse_root_cause_v2_mode(std::string_view s, RootCauseV2Mode& out) {
  if (s == "auto") {
    out = RootCauseV2Mode::Auto;
    return true;
  }
  if (s == "on") {
    out = RootCauseV2Mode::On;
    return true;
  }
  if (s == "off") {
    out = RootCauseV2Mode::Off;
    return true;
  }
  return false;
}

static bool parse_positive_int(std::string_view s, int& out) {
  if (s.empty()) return false;
  int v = 0;
  const auto* begin = s.data();
  const auto* end = s.data() + s.size();
  auto [ptr, ec] = std::from_chars(begin, end, v);
  if (ec != std::errc() || ptr != end || v < 0) return false;
  out = v;
  return true;
}

static bool parse_run_gate(std::string_view s, RunGate& out) {
  if (s == "high") {
    out = RunGate::High;
    return true;
  }
  if (s == "all") {
    out = RunGate::All;
    return true;
  }
  if (s == "none") {
    out = RunGate::None;
    return true;
  }
  return false;
}

static bool parse_preflight_mode(std::string_view s, PreflightMode& out) {
  if (s == "fast") {
    out = PreflightMode::Fast;
    return true;
  }
  if (s == "off") {
    out = PreflightMode::Off;
    return true;
  }
  return false;
}

static bool parse_cross_stage_reuse_mode(std::string_view s, CrossStageReuseMode& out) {
  if (s == "off") {
    out = CrossStageReuseMode::Off;
    return true;
  }
  if (s == "safe") {
    out = CrossStageReuseMode::Safe;
    return true;
  }
  return false;
}

static bool parse_disk_cache_mode(std::string_view s, DiskCacheMode& out) {
  if (s == "off") {
    out = DiskCacheMode::Off;
    return true;
  }
  if (s == "on") {
    out = DiskCacheMode::On;
    return true;
  }
  return false;
}

static bool parse_cache_report_format(std::string_view s, CacheReportFormat& out) {
  if (s == "text") {
    out = CacheReportFormat::Text;
    return true;
  }
  if (s == "json") {
    out = CacheReportFormat::Json;
    return true;
  }
  return false;
}

static bool parse_warn_class(std::string_view s, std::uint32_t& out_mask) {
  if (s == "api" || s == "api-deprecation") {
    out_mask = kWarnClassApi;
    return true;
  }
  if (s == "perf" || s == "performance" || s == "performance-risk") {
    out_mask = kWarnClassPerformance;
    return true;
  }
  if (s == "best" || s == "best-practice" || s == "best-practice-drift") {
    out_mask = kWarnClassBestPractice;
    return true;
  }
  if (s == "safety" || s == "safety-risk") {
    out_mask = kWarnClassSafety;
    return true;
  }
  if (s == "general" || s == "general-warning" || s == "other") {
    out_mask = kWarnClassGeneral;
    return true;
  }
  if (s == "all") {
    out_mask = kWarnClassAll;
    return true;
  }
  return false;
}

static bool parse_gate_dimension(std::string_view s, std::uint32_t& out_mask) {
  if (s == "api-lifecycle" || s == "api") {
    out_mask = kGateDimApiLifecycle;
    return true;
  }
  if (s == "perf-runtime" || s == "performance" || s == "perf") {
    out_mask = kGateDimPerfRuntime;
    return true;
  }
  if (s == "best-practice-drift" || s == "best-practice" || s == "best") {
    out_mask = kGateDimBestPracticeDrift;
    return true;
  }
  if (s == "safety-contract" || s == "safety") {
    out_mask = kGateDimSafetyContract;
    return true;
  }
  if (s == "general") {
    out_mask = kGateDimGeneral;
    return true;
  }
  if (s == "all") {
    out_mask = kGateDimAll;
    return true;
  }
  return false;
}

bool parse_cli_options(const std::vector<std::string>& args,
                       const std::string& cmd,
                       CliOptions& opt,
                       std::string& err) {
  std::size_t start = 2;
  if (cmd == "check" || cmd == "build" || cmd == "run") start = 3;
  const bool run_only_options_allowed = (cmd == "run");

  for (std::size_t i = start; i < args.size(); ++i) {
    const std::string& tok = args[i];

    auto next_value = [&](std::string_view name) -> std::optional<std::string> {
      if (i + 1 >= args.size()) {
        err = "missing value for " + std::string(name);
        return std::nullopt;
      }
      ++i;
      return args[i];
    };

    if (tok == "--") {
      if (i + 1 < args.size()) {
        err = "unexpected positional argument: " + args[i + 1];
        return false;
      }
      break;
    } else if (tok == "--warnings-as-errors") {
      opt.warnings_as_errors = true;
    } else if (tok == "--strict-region") {
      opt.strict_region = true;
    } else if (tok == "--dump-ownership") {
      opt.dump_ownership = true;
    } else if (tok == "--dump-cfg-ir") {
      opt.dump_cfg_ir = true;
    } else if (tok == "--emit-cpp") {
      opt.emit_cpp = true;
    } else if (tok == "--reuse") {
      if (!run_only_options_allowed) {
        err = "unknown option: " + tok;
        return false;
      }
      opt.reuse = true;
    } else if (tok == "--no-build") {
      if (!run_only_options_allowed) {
        err = "unknown option: " + tok;
        return false;
      }
      opt.no_build = true;
    } else if (tok == "--mode") {
      auto value = next_value("--mode");
      if (!value.has_value()) return false;
      if (!parse_mode(*value, opt.mode)) {
        err = "invalid --mode value: " + *value;
        return false;
      }
    } else if (tok == "--profile") {
      auto value = next_value("--profile");
      if (!value.has_value()) return false;
      if (!parse_profile(*value, opt.analysis_profile)) {
        err = "invalid --profile value: " + *value;
        return false;
      }
      opt.profile_explicit = true;
    } else if (tok == "--diag-format") {
      auto value = next_value("--diag-format");
      if (!value.has_value()) return false;
      if (!parse_diag_format(*value, opt.diag_format)) {
        err = "invalid --diag-format value: " + *value;
        return false;
      }
    } else if (tok == "--analysis-tier") {
      auto value = next_value("--analysis-tier");
      if (!value.has_value()) return false;
      if (!parse_analysis_tier(*value, opt.analysis_tier)) {
        err = "invalid --analysis-tier value: " + *value;
        return false;
      }
      opt.analysis_tier_explicit = true;
    } else if (tok == "--smart") {
      auto value = next_value("--smart");
      if (!value.has_value()) return false;
      if (*value == "on") {
        opt.analysis_tier = AnalysisTier::Smart;
      } else if (*value == "off") {
        opt.analysis_tier = AnalysisTier::Basic;
      } else {
        err = "invalid --smart value: " + *value;
        return false;
      }
      opt.analysis_tier_explicit = true;
    } else if (tok == "--diag-view") {
      auto value = next_value("--diag-view");
      if (!value.has_value()) return false;
      if (!parse_diag_view(*value, opt.diag_view)) {
        err = "invalid --diag-view value: " + *value;
        return false;
      }
    } else if (tok == "--warn-policy") {
      auto value = next_value("--warn-policy");
      if (!value.has_value()) return false;
      if (!parse_warn_policy(*value, opt.warn_policy)) {
        err = "invalid --warn-policy value: " + *value;
        return false;
      }
      opt.warn_policy_explicit = true;
    } else if (tok == "--gate-profile") {
      if (!run_only_options_allowed) {
        err = "unknown option: " + tok;
        return false;
      }
      auto value = next_value("--gate-profile");
      if (!value.has_value()) return false;
      if (!parse_gate_profile(*value, opt.gate_profile)) {
        err = "invalid --gate-profile value: " + *value;
        return false;
      }
    } else if (tok == "--warn-class") {
      auto value = next_value("--warn-class");
      if (!value.has_value()) return false;
      const std::size_t eq = value->find('=');
      if (eq == std::string::npos || eq == 0 || eq + 1 >= value->size()) {
        err = "invalid --warn-class value: " + *value;
        return false;
      }
      const std::string cls = value->substr(0, eq);
      const std::string state = value->substr(eq + 1);
      std::uint32_t cls_mask = 0;
      if (!parse_warn_class(cls, cls_mask)) {
        err = "invalid --warn-class value: " + *value;
        return false;
      }
      if (state == "on") {
        opt.warn_class_force_on_mask |= cls_mask;
        opt.warn_class_force_off_mask &= ~cls_mask;
      } else if (state == "off") {
        opt.warn_class_force_off_mask |= cls_mask;
        opt.warn_class_force_on_mask &= ~cls_mask;
      } else {
        err = "invalid --warn-class value: " + *value;
        return false;
      }
    } else if (tok == "--gate-dimension") {
      if (!run_only_options_allowed) {
        err = "unknown option: " + tok;
        return false;
      }
      auto value = next_value("--gate-dimension");
      if (!value.has_value()) return false;
      const std::size_t eq = value->find('=');
      if (eq == std::string::npos || eq == 0 || eq + 1 >= value->size()) {
        err = "invalid --gate-dimension value: " + *value;
        return false;
      }
      const std::string dim = value->substr(0, eq);
      const std::string state = value->substr(eq + 1);
      std::uint32_t dim_mask = 0;
      if (!parse_gate_dimension(dim, dim_mask)) {
        err = "invalid --gate-dimension value: " + *value;
        return false;
      }
      if (state == "on") {
        opt.gate_dimension_force_on_mask |= dim_mask;
        opt.gate_dimension_force_off_mask &= ~dim_mask;
      } else if (state == "off") {
        opt.gate_dimension_force_off_mask |= dim_mask;
        opt.gate_dimension_force_on_mask &= ~dim_mask;
      } else {
        err = "invalid --gate-dimension value: " + *value;
        return false;
      }
    } else if (tok == "--diag-budget-ms") {
      auto value = next_value("--diag-budget-ms");
      if (!value.has_value()) return false;
      if (!parse_positive_int(*value, opt.diag_budget_ms)) {
        err = "invalid --diag-budget-ms value: " + *value;
        return false;
      }
    } else if (tok == "--diag-grouping-delay-ms") {
      auto value = next_value("--diag-grouping-delay-ms");
      if (!value.has_value()) return false;
      if (!parse_positive_int(*value, opt.diag_grouping_delay_ms)) {
        err = "invalid --diag-grouping-delay-ms value: " + *value;
        return false;
      }
    } else if (tok == "--max-root-causes") {
      auto value = next_value("--max-root-causes");
      if (!value.has_value()) return false;
      if (!parse_positive_int(*value, opt.max_root_causes)) {
        err = "invalid --max-root-causes value: " + *value;
        return false;
      }
    } else if (tok == "--root-cause-v2") {
      auto value = next_value("--root-cause-v2");
      if (!value.has_value()) return false;
      if (!parse_root_cause_v2_mode(*value, opt.root_cause_v2)) {
        err = "invalid --root-cause-v2 value: " + *value;
        return false;
      }
    } else if (tok == "--root-cause-top-k") {
      auto value = next_value("--root-cause-top-k");
      if (!value.has_value()) return false;
      if (!parse_positive_int(*value, opt.root_cause_top_k) || opt.root_cause_top_k <= 0 ||
          opt.root_cause_top_k > 50) {
        err = "invalid --root-cause-top-k value: " + *value;
        return false;
      }
    } else if (tok == "--root-cause-min-covered") {
      auto value = next_value("--root-cause-min-covered");
      if (!value.has_value()) return false;
      if (!parse_positive_int(*value, opt.root_cause_min_covered)) {
        err = "invalid --root-cause-min-covered value: " + *value;
        return false;
      }
    } else if (tok == "--cache-report") {
      auto value = next_value("--cache-report");
      if (!value.has_value()) return false;
      if (*value == "on") {
        opt.cache_report = true;
      } else if (*value == "off") {
        opt.cache_report = false;
      } else {
        err = "invalid --cache-report value: " + *value;
        return false;
      }
    } else if (tok == "--cache-report-format") {
      auto value = next_value("--cache-report-format");
      if (!value.has_value()) return false;
      if (!parse_cache_report_format(*value, opt.cache_report_format)) {
        err = "invalid --cache-report-format value: " + *value;
        return false;
      }
    } else if (tok == "--run-gate") {
      if (!run_only_options_allowed) {
        err = "unknown option: " + tok;
        return false;
      }
      auto value = next_value("--run-gate");
      if (!value.has_value()) return false;
      if (!parse_run_gate(*value, opt.run_gate)) {
        err = "invalid --run-gate value: " + *value;
        return false;
      }
    } else if (tok == "--preflight") {
      if (!run_only_options_allowed) {
        err = "unknown option: " + tok;
        return false;
      }
      auto value = next_value("--preflight");
      if (!value.has_value()) return false;
      if (!parse_preflight_mode(*value, opt.preflight)) {
        err = "invalid --preflight value: " + *value;
        return false;
      }
    } else if (tok == "--cross-stage-reuse") {
      if (!run_only_options_allowed) {
        err = "unknown option: " + tok;
        return false;
      }
      auto value = next_value("--cross-stage-reuse");
      if (!value.has_value()) return false;
      if (!parse_cross_stage_reuse_mode(*value, opt.cross_stage_reuse)) {
        err = "invalid --cross-stage-reuse value: " + *value;
        return false;
      }
    } else if (tok == "--disk-cache") {
      if (!run_only_options_allowed) {
        err = "unknown option: " + tok;
        return false;
      }
      auto value = next_value("--disk-cache");
      if (!value.has_value()) return false;
      if (!parse_disk_cache_mode(*value, opt.disk_cache)) {
        err = "invalid --disk-cache value: " + *value;
        return false;
      }
    } else if (tok == "--disk-cache-ttl-sec") {
      if (!run_only_options_allowed) {
        err = "unknown option: " + tok;
        return false;
      }
      auto value = next_value("--disk-cache-ttl-sec");
      if (!value.has_value()) return false;
      if (!parse_positive_int(*value, opt.disk_cache_ttl_sec)) {
        err = "invalid --disk-cache-ttl-sec value: " + *value;
        return false;
      }
    } else if (tok == "--disk-cache-max-entries") {
      if (!run_only_options_allowed) {
        err = "unknown option: " + tok;
        return false;
      }
      auto value = next_value("--disk-cache-max-entries");
      if (!value.has_value()) return false;
      if (!parse_positive_int(*value, opt.disk_cache_max_entries) || opt.disk_cache_max_entries <= 0) {
        err = "invalid --disk-cache-max-entries value: " + *value;
        return false;
      }
    } else if (tok == "--disk-cache-dir") {
      if (!run_only_options_allowed) {
        err = "unknown option: " + tok;
        return false;
      }
      auto value = next_value("--disk-cache-dir");
      if (!value.has_value()) return false;
      opt.disk_cache_dir = *value;
    } else if (tok == "--disk-cache-prune") {
      if (!run_only_options_allowed) {
        err = "unknown option: " + tok;
        return false;
      }
      opt.disk_cache_prune = true;
    } else if (tok == "--out-dir") {
      auto value = next_value("--out-dir");
      if (!value.has_value()) return false;
      opt.out_dir = *value;
    } else if (tok == "--dir") {
      auto value = next_value("--dir");
      if (!value.has_value()) return false;
      opt.dir = *value;
    } else if (tok == "--out" || tok == "-o") {
      auto value = next_value("-o/--out");
      if (!value.has_value()) return false;
      opt.out_path = fs::path(*value);
    } else if (!tok.empty() && tok[0] == '-') {
      err = "unknown option: " + tok;
      return false;
    } else {
      err = "unexpected positional argument: " + tok;
      return false;
    }
  }

  return true;
}
