#pragma once

#include "frontend/source.hpp"

#include <cstdint>
#include <iomanip>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

namespace nebula::frontend {

enum class Severity : std::uint8_t { Error, Warning, Note };
enum class DiagnosticStage : std::uint8_t { Unspecified, Preflight, Build };
enum class DiagnosticRisk : std::uint8_t { Unknown, Low, Medium, High, Critical };

struct Diagnostic {
  Severity severity = Severity::Error;
  std::string code;
  std::string message;
  Span span{};
  DiagnosticStage stage = DiagnosticStage::Unspecified;
  DiagnosticRisk risk = DiagnosticRisk::Unknown;
  std::string category;
  std::string cause;
  std::string impact;
  std::string machine_reason;
  std::string machine_subreason;
  std::string machine_detail;
  std::string machine_trigger_family;
  std::string machine_trigger_family_detail;
  std::string machine_trigger_subreason;
  std::string machine_owner;
  std::string machine_owner_reason;
  std::string machine_owner_reason_detail;
  std::string caused_by_code;
  std::vector<std::string> suggestions;
  bool predictive = false;
  std::optional<double> confidence;
  std::vector<Span> related_spans;

  bool is_error() const { return severity == Severity::Error; }
  bool is_warning() const { return severity == Severity::Warning; }
  bool is_note() const { return severity == Severity::Note; }
};

inline const char* severity_name(Severity s) {
  switch (s) {
  case Severity::Error: return "error";
  case Severity::Warning: return "warning";
  case Severity::Note: return "note";
  }
  return "error";
}

inline const char* stage_name(DiagnosticStage s) {
  switch (s) {
  case DiagnosticStage::Unspecified: return "unspecified";
  case DiagnosticStage::Preflight: return "preflight";
  case DiagnosticStage::Build: return "build";
  }
  return "unspecified";
}

inline const char* risk_name(DiagnosticRisk r) {
  switch (r) {
  case DiagnosticRisk::Unknown: return "unknown";
  case DiagnosticRisk::Low: return "low";
  case DiagnosticRisk::Medium: return "medium";
  case DiagnosticRisk::High: return "high";
  case DiagnosticRisk::Critical: return "critical";
  }
  return "unknown";
}

inline void print_diagnostic(std::ostream& os, const Diagnostic& d) {
  const auto& p = d.span.start;
  os << severity_name(d.severity) << ":";
  if (!d.span.source_path.empty()) os << d.span.source_path << ":";
  os << p.line << ":" << p.col << ": ";
  if (!d.code.empty()) os << d.code << ": ";
  os << d.message;
  bool wrote_meta = false;
  if (d.stage != DiagnosticStage::Unspecified || d.risk != DiagnosticRisk::Unknown ||
      !d.category.empty()) {
    os << " [";
    if (d.stage != DiagnosticStage::Unspecified) {
      os << "stage=" << stage_name(d.stage);
      wrote_meta = true;
    }
    if (d.risk != DiagnosticRisk::Unknown) {
      if (wrote_meta) os << ", ";
      os << "risk=" << risk_name(d.risk);
      wrote_meta = true;
    }
    if (!d.category.empty()) {
      if (wrote_meta) os << ", ";
      os << "category=" << d.category;
    }
    os << "]";
  }
  os << "\n";
  if (!d.cause.empty()) os << "  cause: " << d.cause << "\n";
  if (!d.impact.empty()) os << "  impact: " << d.impact << "\n";
  if (!d.machine_reason.empty()) os << "  machine-reason: " << d.machine_reason << "\n";
  if (!d.machine_subreason.empty()) os << "  machine-subreason: " << d.machine_subreason << "\n";
  if (!d.machine_detail.empty()) os << "  machine-detail: " << d.machine_detail << "\n";
  if (!d.machine_trigger_family.empty()) {
    os << "  machine-trigger-family: " << d.machine_trigger_family << "\n";
  }
  if (!d.machine_trigger_family_detail.empty()) {
    os << "  machine-trigger-family-detail: " << d.machine_trigger_family_detail << "\n";
  }
  if (!d.machine_trigger_subreason.empty()) {
    os << "  machine-trigger-subreason: " << d.machine_trigger_subreason << "\n";
  }
  if (!d.machine_owner.empty()) os << "  machine-owner: " << d.machine_owner << "\n";
  if (!d.machine_owner_reason.empty()) {
    os << "  machine-owner-reason: " << d.machine_owner_reason << "\n";
  }
  if (!d.machine_owner_reason_detail.empty()) {
    os << "  machine-owner-reason-detail: " << d.machine_owner_reason_detail << "\n";
  }
  if (!d.caused_by_code.empty()) os << "  caused-by-code: " << d.caused_by_code << "\n";
  for (const auto& s : d.suggestions) {
    os << "  suggestion: " << s << "\n";
  }
  if (d.predictive) {
    os << "  predictive";
    if (d.confidence.has_value()) {
      std::ostringstream conf;
      conf << std::fixed << std::setprecision(2) << *d.confidence;
      os << " (confidence=" << conf.str() << ")";
    }
    os << "\n";
  }
  for (const auto& rs : d.related_spans) {
    os << "  related: ";
    if (!rs.source_path.empty()) os << rs.source_path << ":";
    os << rs.start.line << ":" << rs.start.col << "-" << rs.end.line << ":" << rs.end.col
       << "\n";
  }
}

} // namespace nebula::frontend
