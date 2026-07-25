#include "protocol_abi_contract.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace nebula::boot {
namespace {

constexpr std::array<std::string_view, kProtocolAbiManifestFieldCount> kFieldNames = {
  "manifest_schema",
  "manifest_version",
  "contract_id",
  "contract_scope",
  "gate_id",
  "gate_status",
  "target_triple",
  "image_format",
  "elf_data_encoding",
  "elf_machine",
  "image_entry_symbol",
  "payload_entry_symbol",
  "high_half_minimum",
  "panic_policy",
  "entry_calling_convention",
  "entry_must_not_return",
  "entry_fp_simd_allowed",
  "minimum_boot_stack_bytes",
  "red_zone_allowed",
  "x87_allowed",
  "mmx_allowed",
  "sse_allowed",
  "sse2_allowed",
  "boot_protocol",
  "limine_release_repository",
  "limine_release_tag",
  "limine_release_tag_object",
  "limine_release_commit",
  "limine_protocol_repository",
  "limine_protocol_commit",
  "limine_base_revision",
  "limine_base_revision_support_check",
  "limine_marker_alignment_bytes",
  "limine_request_common_magic_qword_0",
  "limine_request_common_magic_qword_1",
  "limine_requests_start_qword_0",
  "limine_requests_start_qword_1",
  "limine_requests_start_qword_2",
  "limine_requests_start_qword_3",
  "limine_base_revision_qword_0",
  "limine_base_revision_qword_1",
  "limine_base_revision_qword_2_initial",
  "limine_base_revision_qword_2_supported",
  "limine_requests_end_qword_0",
  "limine_requests_end_qword_1",
  "limine_header_source_path",
  "limine_header_vendor_path",
  "limine_header_size",
  "limine_header_sha256",
  "limine_header_spdx",
  "limine_license_source_path",
  "limine_license_vendor_path",
  "limine_license_size",
  "limine_license_sha256",
  "limine_license_spdx",
};

enum class Field : std::size_t {
  ManifestSchema,
  ManifestVersion,
  ContractId,
  ContractScope,
  GateId,
  GateStatus,
  TargetTriple,
  ImageFormat,
  ElfDataEncoding,
  ElfMachine,
  ImageEntrySymbol,
  PayloadEntrySymbol,
  HighHalfMinimum,
  PanicPolicy,
  EntryCallingConvention,
  EntryMustNotReturn,
  EntryFpSimdAllowed,
  MinimumBootStackBytes,
  RedZoneAllowed,
  X87Allowed,
  MmxAllowed,
  SseAllowed,
  Sse2Allowed,
  BootProtocol,
  LimineReleaseRepository,
  LimineReleaseTag,
  LimineReleaseTagObject,
  LimineReleaseCommit,
  LimineProtocolRepository,
  LimineProtocolCommit,
  LimineBaseRevision,
  LimineBaseRevisionSupportCheck,
  LimineMarkerAlignmentBytes,
  LimineRequestCommonMagicQword0,
  LimineRequestCommonMagicQword1,
  LimineRequestsStartQword0,
  LimineRequestsStartQword1,
  LimineRequestsStartQword2,
  LimineRequestsStartQword3,
  LimineBaseRevisionQword0,
  LimineBaseRevisionQword1,
  LimineBaseRevisionQword2Initial,
  LimineBaseRevisionQword2Supported,
  LimineRequestsEndQword0,
  LimineRequestsEndQword1,
  LimineHeaderSourcePath,
  LimineHeaderVendorPath,
  LimineHeaderSize,
  LimineHeaderSha256,
  LimineHeaderSpdx,
  LimineLicenseSourcePath,
  LimineLicenseVendorPath,
  LimineLicenseSize,
  LimineLicenseSha256,
  LimineLicenseSpdx,
  Count,
};

constexpr std::size_t field_index(Field field) noexcept { return static_cast<std::size_t>(field); }

static_assert(field_index(Field::Count) == kProtocolAbiManifestFieldCount);

ProtocolAbiContractError make_error(ProtocolAbiContractErrorCode code, std::size_t line,
                                    std::string_view field, std::string detail) {
  return ProtocolAbiContractError{code, line, std::string(field), std::move(detail)};
}

ProtocolAbiContractResult parse_error(ProtocolAbiContractErrorCode code, std::size_t line,
                                      std::string_view field, std::string detail) {
  ProtocolAbiContractResult result;
  result.error = make_error(code, line, field, std::move(detail));
  return result;
}

ProtocolAbiContractResult parse_error(ProtocolAbiContractError error) {
  ProtocolAbiContractResult result;
  result.error = std::move(error);
  return result;
}

ProtocolAbiContractSerializationResult serialization_error(ProtocolAbiContractErrorCode code,
                                                           std::size_t line, std::string_view field,
                                                           std::string detail) {
  ProtocolAbiContractSerializationResult result;
  result.error = make_error(code, line, field, std::move(detail));
  return result;
}

std::optional<std::size_t> find_field(std::string_view name) {
  const auto found = std::find(kFieldNames.begin(), kFieldNames.end(), name);
  if (found == kFieldNames.end())
    return std::nullopt;
  return static_cast<std::size_t>(found - kFieldNames.begin());
}

struct ParsedManifestLine {
  std::string_view key;
  std::string_view value;
};

std::optional<ProtocolAbiContractError>
parse_manifest_line(std::string_view line, std::size_t line_number, ParsedManifestLine &parsed) {
  if (line.empty()) {
    return make_error(ProtocolAbiContractErrorCode::InvalidLine, line_number, {},
                      "empty manifest lines are forbidden");
  }
  if (line.size() > kProtocolAbiManifestMaxLineBytes) {
    return make_error(ProtocolAbiContractErrorCode::LineTooLong, line_number, {},
                      "manifest line exceeds its byte limit");
  }
  const std::size_t separator = line.find('=');
  if (separator == std::string_view::npos || separator == 0U || separator + 1U == line.size() ||
      line.find('=', separator + 1U) != std::string_view::npos) {
    return make_error(ProtocolAbiContractErrorCode::InvalidLine, line_number, {},
                      "manifest line must contain one non-edge equals sign");
  }
  parsed.key = line.substr(0U, separator);
  parsed.value = line.substr(separator + 1U);
  return std::nullopt;
}

std::size_t line_for_offset(std::string_view payload, std::size_t offset) {
  return 1U + static_cast<std::size_t>(std::count(
                payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(offset), '\n'));
}

bool is_lower_hex(std::string_view value, std::size_t expected_size) {
  return value.size() == expected_size &&
         std::all_of(value.begin(), value.end(), [](unsigned char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         });
}

bool is_canonical_sha256(std::string_view value) { return is_lower_hex(value, 64U); }

bool is_canonical_git_object_id(std::string_view value) { return is_lower_hex(value, 40U); }

bool is_canonical_relative_path(std::string_view value) {
  if (value.empty() || value.size() > kProtocolAbiManifestMaxPathBytes || value.front() == '/' ||
      value.back() == '/' || value.find('\\') != std::string_view::npos ||
      value.find(':') != std::string_view::npos) {
    return false;
  }

  std::size_t component_start = 0U;
  while (component_start < value.size()) {
    const std::size_t separator = value.find('/', component_start);
    const std::size_t component_end =
      separator == std::string_view::npos ? value.size() : separator;
    const std::string_view component =
      value.substr(component_start, component_end - component_start);
    if (component.empty() || component == "." || component == "..")
      return false;
    if (!std::all_of(component.begin(), component.end(), [](unsigned char ch) {
          return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                 ch == '_' || ch == '-' || ch == '.';
        })) {
      return false;
    }
    if (separator == std::string_view::npos)
      break;
    component_start = separator + 1U;
  }
  return true;
}

template <typename UInt> bool parse_canonical_decimal(std::string_view value, UInt &output) {
  static_assert(std::numeric_limits<UInt>::is_integer && !std::numeric_limits<UInt>::is_signed);
  if (value.empty() || (value.size() > 1U && value.front() == '0'))
    return false;
  if (!std::all_of(value.begin(), value.end(),
                   [](unsigned char ch) { return ch >= '0' && ch <= '9'; })) {
    return false;
  }
  UInt parsed = 0U;
  const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed, 10);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size())
    return false;
  output = parsed;
  return true;
}

bool parse_canonical_qword(std::string_view value, std::uint64_t &output) {
  if (value.size() != 18U || !value.starts_with("0x") || !is_lower_hex(value.substr(2U), 16U)) {
    return false;
  }
  std::uint64_t parsed = 0U;
  const auto result = std::from_chars(value.data() + 2, value.data() + value.size(), parsed, 16);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size())
    return false;
  output = parsed;
  return true;
}

std::string decimal_string(std::uint64_t value) {
  std::array<char, 32> buffer{};
  const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, 10);
  if (result.ec != std::errc{})
    return {};
  return std::string(buffer.data(), result.ptr);
}

std::string qword_string(std::uint64_t value) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string result(18U, '0');
  result[0] = '0';
  result[1] = 'x';
  for (std::size_t index = 0U; index < 16U; ++index) {
    const std::size_t shift = (15U - index) * 4U;
    result[index + 2U] = digits[static_cast<std::size_t>((value >> shift) & 0x0fU)];
  }
  return result;
}

std::optional<std::string_view> scope_name(ProtocolAbiContractScope scope) {
  switch (scope) {
  case ProtocolAbiContractScope::ProtocolAbiCandidate:
    return "protocol-abi-candidate";
  }
  return std::nullopt;
}

std::optional<std::string_view> gate_status_name(ProtocolAbiGateStatus status) {
  switch (status) {
  case ProtocolAbiGateStatus::Planned:
    return "planned";
  }
  return std::nullopt;
}

std::optional<std::string_view> image_format_name(KernelImageFormat format) {
  switch (format) {
  case KernelImageFormat::Elf64EtExec:
    return kUosX86_64ImageFormat;
  }
  return std::nullopt;
}

std::optional<std::string_view> data_encoding_name(ElfDataEncoding encoding) {
  switch (encoding) {
  case ElfDataEncoding::LittleEndian:
    return "little-endian";
  }
  return std::nullopt;
}

std::optional<std::string_view> machine_name(ElfMachine machine) {
  switch (machine) {
  case ElfMachine::X86_64:
    return "x86_64";
  }
  return std::nullopt;
}

std::optional<std::string_view> panic_policy_name(KernelPanicPolicy policy) {
  switch (policy) {
  case KernelPanicPolicy::Trap:
    return kUosX86_64PanicPolicy;
  }
  return std::nullopt;
}

std::optional<std::string_view>
entry_calling_convention_name(KernelEntryCallingConvention convention) {
  switch (convention) {
  case KernelEntryCallingConvention::X86_64SystemVRestricted:
    return kUosX86_64EntryCallingConvention;
  }
  return std::nullopt;
}

std::optional<std::string_view> boot_protocol_name(BootProtocol protocol) {
  switch (protocol) {
  case BootProtocol::Limine:
    return "limine";
  }
  return std::nullopt;
}

std::optional<std::string_view>
base_revision_support_check_name(LimineBaseRevisionSupportCheck check) {
  switch (check) {
  case LimineBaseRevisionSupportCheck::Qword2EqualsZero:
    return "qword-2-equals-zero";
  }
  return std::nullopt;
}

std::optional<std::string_view> protocol_license_name(ProtocolLicense license) {
  switch (license) {
  case ProtocolLicense::ZeroBsd:
    return "0BSD";
  }
  return std::nullopt;
}

ProtocolAbiContract canonical_contract() {
  ProtocolAbiContract contract;
  contract.contract_id = kUosX86_64ContractId;
  contract.scope = ProtocolAbiContractScope::ProtocolAbiCandidate;
  contract.gate_id = "UOS-BOOT-001";
  contract.gate_status = ProtocolAbiGateStatus::Planned;

  contract.abi.target_triple = kUosX86_64TargetTriple;
  contract.abi.image_format = KernelImageFormat::Elf64EtExec;
  contract.abi.data_encoding = ElfDataEncoding::LittleEndian;
  contract.abi.machine = ElfMachine::X86_64;
  contract.abi.image_entry_symbol = kUosX86_64ImageEntrySymbol;
  contract.abi.payload_entry_symbol = kUosX86_64PayloadEntrySymbol;
  contract.abi.high_half_minimum = kUosX86_64HighHalfMinimum;
  contract.abi.panic_policy = KernelPanicPolicy::Trap;
  contract.abi.entry_calling_convention = KernelEntryCallingConvention::X86_64SystemVRestricted;
  contract.abi.entry_must_not_return = kUosX86_64EntryMustNotReturn;
  contract.abi.entry_fp_simd_allowed = kUosX86_64EntryFpSimdAllowed;
  contract.abi.minimum_boot_stack_bytes = kUosX86_64MinimumBootStackBytes;
  contract.abi.red_zone_allowed = kUosX86_64RedZoneAllowed;
  contract.abi.x87_allowed = kUosX86_64X87Allowed;
  contract.abi.mmx_allowed = kUosX86_64MmxAllowed;
  contract.abi.sse_allowed = kUosX86_64SseAllowed;
  contract.abi.sse2_allowed = kUosX86_64Sse2Allowed;

  contract.limine.protocol = BootProtocol::Limine;
  contract.limine.release_repository = "https://github.com/Limine-Bootloader/Limine";
  contract.limine.release_tag = "v12.3.2";
  contract.limine.release_tag_object = "5e6ef2a0ae7afcd863639b78aee1dbb6cacf1b45";
  contract.limine.release_commit = "8c8a688776735b2b2d12683a032e442583d361db";
  contract.limine.protocol_repository = "https://github.com/Limine-Bootloader/limine-protocol";
  contract.limine.protocol_commit = "5b9d13e557590d8eab93fa7449bdd1d7ed72ba8c";
  contract.limine.base_revision = UINT64_C(6);
  contract.limine.base_revision_support_check = LimineBaseRevisionSupportCheck::Qword2EqualsZero;
  contract.limine.marker_alignment_bytes = UINT64_C(8);
  contract.limine.request_common_magic = {
    UINT64_C(0xc7b1dd30df4c8b88),
    UINT64_C(0x0a82e883a194f07b),
  };
  contract.limine.requests_start_marker = {
    UINT64_C(0xf6b8f4b39de7d1ae),
    UINT64_C(0xfab91a6940fcb9cf),
    UINT64_C(0x785c6ed015d3e316),
    UINT64_C(0x181e920a7852b9d9),
  };
  contract.limine.base_revision_marker = {
    UINT64_C(0xf9562b2d5c95a6c8),
    UINT64_C(0x6a7b384944536bdc),
    UINT64_C(0x0000000000000006),
  };
  contract.limine.supported_base_revision_value = UINT64_C(0);
  contract.limine.requests_end_marker = {
    UINT64_C(0xadc0e0531bb10d03),
    UINT64_C(0x9572709f31764c62),
  };
  contract.limine.header = VendoredProtocolAsset{
    "include/limine.h",       "boot/uos-x86_64-limine-v1/protocol/limine.h",
    UINT64_C(17225),          "276db60a383509287d65f14e3f73b28f2258eda923f0d29a8848512db883fc5b",
    ProtocolLicense::ZeroBsd,
  };
  contract.limine.license_file = VendoredProtocolAsset{
    "LICENSE",
    "boot/uos-x86_64-limine-v1/protocol/LICENSE",
    UINT64_C(659),
    "e2b1c35814afb22acbddf7ff567d2b05a467e503628ffea62752bc1f3fa2595c",
    ProtocolLicense::ZeroBsd,
  };
  return contract;
}

using RenderedValues = std::array<std::string, kProtocolAbiManifestFieldCount>;

std::optional<ProtocolAbiContractError> render_contract_fields(const ProtocolAbiContract &contract,
                                                               RenderedValues &values) {
  values[field_index(Field::ManifestSchema)] = std::string(kProtocolAbiManifestSchema);
  values[field_index(Field::ManifestVersion)] = decimal_string(kProtocolAbiManifestVersion);
  values[field_index(Field::ContractId)] = contract.contract_id;

  const auto assign_enum =
    [&](Field field,
        std::optional<std::string_view> value) -> std::optional<ProtocolAbiContractError> {
    const std::size_t index = field_index(field);
    if (!value.has_value()) {
      return make_error(ProtocolAbiContractErrorCode::InvalidValue, index + 1U, kFieldNames[index],
                        "enum value is outside the v1 contract");
    }
    values[index] = *value;
    return std::nullopt;
  };

  if (auto error = assign_enum(Field::ContractScope, scope_name(contract.scope)))
    return error;
  values[field_index(Field::GateId)] = contract.gate_id;
  if (auto error = assign_enum(Field::GateStatus, gate_status_name(contract.gate_status)))
    return error;
  values[field_index(Field::TargetTriple)] = contract.abi.target_triple;
  if (auto error = assign_enum(Field::ImageFormat, image_format_name(contract.abi.image_format)))
    return error;
  if (auto error =
        assign_enum(Field::ElfDataEncoding, data_encoding_name(contract.abi.data_encoding))) {
    return error;
  }
  if (auto error = assign_enum(Field::ElfMachine, machine_name(contract.abi.machine)))
    return error;
  values[field_index(Field::ImageEntrySymbol)] = contract.abi.image_entry_symbol;
  values[field_index(Field::PayloadEntrySymbol)] = contract.abi.payload_entry_symbol;
  values[field_index(Field::HighHalfMinimum)] = qword_string(contract.abi.high_half_minimum);
  if (auto error = assign_enum(Field::PanicPolicy, panic_policy_name(contract.abi.panic_policy)))
    return error;
  if (auto error =
        assign_enum(Field::EntryCallingConvention,
                    entry_calling_convention_name(contract.abi.entry_calling_convention))) {
    return error;
  }
  values[field_index(Field::EntryMustNotReturn)] = contract.abi.entry_must_not_return ? "1" : "0";
  values[field_index(Field::EntryFpSimdAllowed)] = contract.abi.entry_fp_simd_allowed ? "1" : "0";
  values[field_index(Field::MinimumBootStackBytes)] =
    decimal_string(contract.abi.minimum_boot_stack_bytes);
  values[field_index(Field::RedZoneAllowed)] = contract.abi.red_zone_allowed ? "1" : "0";
  values[field_index(Field::X87Allowed)] = contract.abi.x87_allowed ? "1" : "0";
  values[field_index(Field::MmxAllowed)] = contract.abi.mmx_allowed ? "1" : "0";
  values[field_index(Field::SseAllowed)] = contract.abi.sse_allowed ? "1" : "0";
  values[field_index(Field::Sse2Allowed)] = contract.abi.sse2_allowed ? "1" : "0";

  if (auto error = assign_enum(Field::BootProtocol, boot_protocol_name(contract.limine.protocol)))
    return error;
  values[field_index(Field::LimineReleaseRepository)] = contract.limine.release_repository;
  values[field_index(Field::LimineReleaseTag)] = contract.limine.release_tag;
  values[field_index(Field::LimineReleaseTagObject)] = contract.limine.release_tag_object;
  values[field_index(Field::LimineReleaseCommit)] = contract.limine.release_commit;
  values[field_index(Field::LimineProtocolRepository)] = contract.limine.protocol_repository;
  values[field_index(Field::LimineProtocolCommit)] = contract.limine.protocol_commit;
  values[field_index(Field::LimineBaseRevision)] = decimal_string(contract.limine.base_revision);
  if (auto error = assign_enum(
        Field::LimineBaseRevisionSupportCheck,
        base_revision_support_check_name(contract.limine.base_revision_support_check))) {
    return error;
  }
  values[field_index(Field::LimineMarkerAlignmentBytes)] =
    decimal_string(contract.limine.marker_alignment_bytes);
  values[field_index(Field::LimineRequestCommonMagicQword0)] =
    qword_string(contract.limine.request_common_magic[0]);
  values[field_index(Field::LimineRequestCommonMagicQword1)] =
    qword_string(contract.limine.request_common_magic[1]);
  values[field_index(Field::LimineRequestsStartQword0)] =
    qword_string(contract.limine.requests_start_marker[0]);
  values[field_index(Field::LimineRequestsStartQword1)] =
    qword_string(contract.limine.requests_start_marker[1]);
  values[field_index(Field::LimineRequestsStartQword2)] =
    qword_string(contract.limine.requests_start_marker[2]);
  values[field_index(Field::LimineRequestsStartQword3)] =
    qword_string(contract.limine.requests_start_marker[3]);
  values[field_index(Field::LimineBaseRevisionQword0)] =
    qword_string(contract.limine.base_revision_marker[0]);
  values[field_index(Field::LimineBaseRevisionQword1)] =
    qword_string(contract.limine.base_revision_marker[1]);
  values[field_index(Field::LimineBaseRevisionQword2Initial)] =
    qword_string(contract.limine.base_revision_marker[2]);
  values[field_index(Field::LimineBaseRevisionQword2Supported)] =
    qword_string(contract.limine.supported_base_revision_value);
  values[field_index(Field::LimineRequestsEndQword0)] =
    qword_string(contract.limine.requests_end_marker[0]);
  values[field_index(Field::LimineRequestsEndQword1)] =
    qword_string(contract.limine.requests_end_marker[1]);

  values[field_index(Field::LimineHeaderSourcePath)] = contract.limine.header.source_path;
  values[field_index(Field::LimineHeaderVendorPath)] = contract.limine.header.vendor_path;
  values[field_index(Field::LimineHeaderSize)] = decimal_string(contract.limine.header.size);
  values[field_index(Field::LimineHeaderSha256)] = contract.limine.header.sha256;
  if (auto error = assign_enum(Field::LimineHeaderSpdx,
                               protocol_license_name(contract.limine.header.license))) {
    return error;
  }
  values[field_index(Field::LimineLicenseSourcePath)] = contract.limine.license_file.source_path;
  values[field_index(Field::LimineLicenseVendorPath)] = contract.limine.license_file.vendor_path;
  values[field_index(Field::LimineLicenseSize)] = decimal_string(contract.limine.license_file.size);
  values[field_index(Field::LimineLicenseSha256)] = contract.limine.license_file.sha256;
  if (auto error = assign_enum(Field::LimineLicenseSpdx,
                               protocol_license_name(contract.limine.license_file.license))) {
    return error;
  }

  for (const Field field : {Field::LimineHeaderSourcePath, Field::LimineHeaderVendorPath,
                            Field::LimineLicenseSourcePath, Field::LimineLicenseVendorPath}) {
    const std::size_t index = field_index(field);
    if (!is_canonical_relative_path(values[index])) {
      return make_error(ProtocolAbiContractErrorCode::UnsafePath, index + 1U, kFieldNames[index],
                        "path must be a canonical repository-relative ASCII path");
    }
  }

  for (const Field field : {Field::LimineHeaderSha256, Field::LimineLicenseSha256}) {
    const std::size_t index = field_index(field);
    if (!is_canonical_sha256(values[index])) {
      return make_error(ProtocolAbiContractErrorCode::InvalidDigest, index + 1U, kFieldNames[index],
                        "SHA-256 must be 64 lowercase hexadecimal digits");
    }
  }

  for (const Field field :
       {Field::LimineReleaseTagObject, Field::LimineReleaseCommit, Field::LimineProtocolCommit}) {
    const std::size_t index = field_index(field);
    if (!is_canonical_git_object_id(values[index])) {
      return make_error(ProtocolAbiContractErrorCode::InvalidGitObjectId, index + 1U,
                        kFieldNames[index],
                        "Git object ID must be 40 lowercase hexadecimal digits");
    }
  }

  return std::nullopt;
}

std::optional<ProtocolAbiContractError> render_contract_values(const ProtocolAbiContract &contract,
                                                               RenderedValues &values) {
  if (auto error = render_contract_fields(contract, values))
    return error;

  RenderedValues canonical_values;
  if (auto error = render_contract_fields(canonical_contract(), canonical_values))
    return error;
  for (std::size_t index = 0U; index < values.size(); ++index) {
    if (values[index] != canonical_values[index]) {
      return make_error(ProtocolAbiContractErrorCode::InvalidValue, index + 1U, kFieldNames[index],
                        "value does not match the fixed v1 contract");
    }
  }
  return std::nullopt;
}

std::string render_manifest(const RenderedValues &values) {
  std::string payload;
  payload.reserve(3U * 1024U);
  for (std::size_t index = 0U; index < values.size(); ++index) {
    payload.append(kFieldNames[index]);
    payload.push_back('=');
    payload.append(values[index]);
    payload.push_back('\n');
  }
  return payload;
}

bool is_decimal_field(std::size_t index) {
  return index == field_index(Field::ManifestVersion) ||
         index == field_index(Field::MinimumBootStackBytes) ||
         index == field_index(Field::LimineBaseRevision) ||
         index == field_index(Field::LimineMarkerAlignmentBytes) ||
         index == field_index(Field::LimineHeaderSize) ||
         index == field_index(Field::LimineLicenseSize);
}

bool is_boolean_field(std::size_t index) {
  return index == field_index(Field::EntryMustNotReturn) ||
         index == field_index(Field::EntryFpSimdAllowed) ||
         index == field_index(Field::RedZoneAllowed) || index == field_index(Field::X87Allowed) ||
         index == field_index(Field::MmxAllowed) || index == field_index(Field::SseAllowed) ||
         index == field_index(Field::Sse2Allowed);
}

bool is_qword_field(std::size_t index) {
  return index == field_index(Field::HighHalfMinimum) ||
         (index >= field_index(Field::LimineRequestCommonMagicQword0) &&
          index <= field_index(Field::LimineRequestsEndQword1));
}

bool is_path_field(std::size_t index) {
  return index == field_index(Field::LimineHeaderSourcePath) ||
         index == field_index(Field::LimineHeaderVendorPath) ||
         index == field_index(Field::LimineLicenseSourcePath) ||
         index == field_index(Field::LimineLicenseVendorPath);
}

bool is_digest_field(std::size_t index) {
  return index == field_index(Field::LimineHeaderSha256) ||
         index == field_index(Field::LimineLicenseSha256);
}

bool is_git_object_field(std::size_t index) {
  return index == field_index(Field::LimineReleaseTagObject) ||
         index == field_index(Field::LimineReleaseCommit) ||
         index == field_index(Field::LimineProtocolCommit);
}

std::optional<ProtocolAbiContractError>
validate_lexical_value(std::size_t index, std::string_view value, std::size_t line) {
  if (is_decimal_field(index)) {
    std::uint64_t ignored = 0U;
    if (!parse_canonical_decimal(value, ignored)) {
      return make_error(ProtocolAbiContractErrorCode::NonCanonicalNumber, line, kFieldNames[index],
                        "decimal integer is not canonical or is out of range");
    }
  } else if (is_boolean_field(index)) {
    std::uint64_t parsed = 0U;
    if (!parse_canonical_decimal(value, parsed)) {
      return make_error(ProtocolAbiContractErrorCode::NonCanonicalNumber, line, kFieldNames[index],
                        "boolean integer is not canonical or is out of range");
    }
    if (parsed > 1U) {
      return make_error(ProtocolAbiContractErrorCode::InvalidValue, line, kFieldNames[index],
                        "boolean integer must be exactly 0 or 1");
    }
  } else if (is_qword_field(index)) {
    std::uint64_t ignored = 0U;
    if (!parse_canonical_qword(value, ignored)) {
      return make_error(ProtocolAbiContractErrorCode::NonCanonicalNumber, line, kFieldNames[index],
                        "qword must use 0x followed by 16 lowercase hexadecimal digits");
    }
  } else if (is_path_field(index)) {
    if (!is_canonical_relative_path(value)) {
      return make_error(ProtocolAbiContractErrorCode::UnsafePath, line, kFieldNames[index],
                        "path must be a canonical repository-relative ASCII path");
    }
  } else if (is_digest_field(index)) {
    if (!is_canonical_sha256(value)) {
      return make_error(ProtocolAbiContractErrorCode::InvalidDigest, line, kFieldNames[index],
                        "SHA-256 must be 64 lowercase hexadecimal digits");
    }
  } else if (is_git_object_field(index)) {
    if (!is_canonical_git_object_id(value)) {
      return make_error(ProtocolAbiContractErrorCode::InvalidGitObjectId, line, kFieldNames[index],
                        "Git object ID must be 40 lowercase hexadecimal digits");
    }
  }
  return std::nullopt;
}

} // namespace

ProtocolAbiContractResult parse_protocol_abi_contract(std::string_view payload) {
  if (payload.size() > kProtocolAbiManifestMaxBytes) {
    return parse_error(ProtocolAbiContractErrorCode::TooLarge, 0U, {},
                       "protocol ABI manifest exceeds its byte limit");
  }
  if (payload.empty()) {
    return parse_error(ProtocolAbiContractErrorCode::MissingField, 0U, kFieldNames.front(),
                       "protocol ABI manifest is empty");
  }

  for (std::size_t offset = 0U; offset < payload.size(); ++offset) {
    const unsigned char ch = static_cast<unsigned char>(payload[offset]);
    if (ch == '\n')
      continue;
    if (ch < 0x20U || ch > 0x7eU) {
      return parse_error(ProtocolAbiContractErrorCode::InvalidEncoding,
                         line_for_offset(payload, offset), {},
                         "manifest must contain printable ASCII and LF only");
    }
  }
  if (payload.back() != '\n') {
    return parse_error(ProtocolAbiContractErrorCode::InvalidLine,
                       line_for_offset(payload, payload.size()), {},
                       "manifest must end with exactly one LF-delimited field");
  }
  const std::size_t encoded_field_count =
    static_cast<std::size_t>(std::count(payload.begin(), payload.end(), '\n'));
  if (encoded_field_count > kProtocolAbiManifestMaxFields) {
    return parse_error(ProtocolAbiContractErrorCode::TooManyFields,
                       kProtocolAbiManifestMaxFields + 1U, {},
                       "manifest exceeds its field-count limit");
  }

  const std::size_t first_line_end = payload.find('\n');
  ParsedManifestLine first_line;
  if (auto error = parse_manifest_line(payload.substr(0U, first_line_end), 1U, first_line))
    return parse_error(std::move(*error));
  if (first_line.key != kFieldNames[field_index(Field::ManifestSchema)]) {
    return parse_error(ProtocolAbiContractErrorCode::SchemaNotFirst, 1U,
                       kFieldNames[field_index(Field::ManifestSchema)],
                       "manifest_schema must be the first field");
  }
  if (first_line.value != kProtocolAbiManifestSchema) {
    return parse_error(ProtocolAbiContractErrorCode::UnsupportedSchema, 1U,
                       kFieldNames[field_index(Field::ManifestSchema)],
                       "manifest schema is unsupported");
  }

  const std::size_t second_line_start = first_line_end + 1U;
  if (second_line_start == payload.size()) {
    return parse_error(ProtocolAbiContractErrorCode::MissingField, 0U,
                       kFieldNames[field_index(Field::ManifestVersion)],
                       "manifest version field is missing");
  }
  const std::size_t second_line_end = payload.find('\n', second_line_start);
  ParsedManifestLine second_line;
  if (auto error = parse_manifest_line(
        payload.substr(second_line_start, second_line_end - second_line_start), 2U, second_line)) {
    return parse_error(std::move(*error));
  }
  if (second_line.key != kFieldNames[field_index(Field::ManifestVersion)]) {
    if (second_line.key == kFieldNames[field_index(Field::ManifestSchema)]) {
      return parse_error(ProtocolAbiContractErrorCode::DuplicateField, 2U, second_line.key,
                         "manifest field appears more than once");
    }
    if (!find_field(second_line.key).has_value()) {
      return parse_error(ProtocolAbiContractErrorCode::UnknownField, 2U, second_line.key,
                         "manifest field is not defined by schema v1");
    }
    return parse_error(ProtocolAbiContractErrorCode::NonCanonicalOrder, 2U, second_line.key,
                       "manifest_version must be the second field");
  }
  std::uint64_t manifest_version = 0U;
  if (!parse_canonical_decimal(second_line.value, manifest_version)) {
    return parse_error(ProtocolAbiContractErrorCode::NonCanonicalNumber, 2U, second_line.key,
                       "manifest version is not a canonical decimal integer");
  }
  if (manifest_version != kProtocolAbiManifestVersion) {
    return parse_error(ProtocolAbiContractErrorCode::UnsupportedVersion, 2U, second_line.key,
                       "manifest version is unsupported");
  }

  std::array<std::string_view, kProtocolAbiManifestFieldCount> values{};
  std::array<std::size_t, kProtocolAbiManifestFieldCount> lines{};
  std::array<bool, kProtocolAbiManifestFieldCount> seen{};
  std::array<std::size_t, kProtocolAbiManifestMaxFields> order{};
  std::size_t parsed_fields = 0U;
  std::size_t line_number = 1U;
  std::size_t line_start = 0U;

  while (line_start < payload.size()) {
    const std::size_t line_end = payload.find('\n', line_start);
    const std::string_view line = payload.substr(line_start, line_end - line_start);
    ParsedManifestLine parsed_line;
    if (auto error = parse_manifest_line(line, line_number, parsed_line))
      return parse_error(std::move(*error));
    if (parsed_fields == kProtocolAbiManifestMaxFields) {
      return parse_error(ProtocolAbiContractErrorCode::TooManyFields, line_number, {},
                         "manifest exceeds its field-count limit");
    }

    const std::optional<std::size_t> index = find_field(parsed_line.key);
    if (!index.has_value()) {
      return parse_error(ProtocolAbiContractErrorCode::UnknownField, line_number, parsed_line.key,
                         "manifest field is not defined by schema v1");
    }
    if (seen[*index]) {
      return parse_error(ProtocolAbiContractErrorCode::DuplicateField, line_number, parsed_line.key,
                         "manifest field appears more than once");
    }
    seen[*index] = true;
    values[*index] = parsed_line.value;
    lines[*index] = line_number;
    order[parsed_fields++] = *index;
    line_start = line_end + 1U;
    ++line_number;
  }

  if (!seen.front()) {
    return parse_error(ProtocolAbiContractErrorCode::MissingField, 0U, kFieldNames.front(),
                       "manifest schema field is missing");
  }
  if (order.front() != 0U) {
    return parse_error(ProtocolAbiContractErrorCode::SchemaNotFirst, 1U, kFieldNames.front(),
                       "manifest_schema must be the first field");
  }
  for (std::size_t index = 0U; index < seen.size(); ++index) {
    if (!seen[index]) {
      return parse_error(ProtocolAbiContractErrorCode::MissingField, 0U, kFieldNames[index],
                         "required manifest field is missing");
    }
  }
  for (std::size_t position = 0U; position < kProtocolAbiManifestFieldCount; ++position) {
    if (order[position] != position) {
      const std::size_t actual = order[position];
      return parse_error(ProtocolAbiContractErrorCode::NonCanonicalOrder, position + 1U,
                         kFieldNames[actual],
                         "manifest fields are not in the schema-defined canonical order");
    }
  }

  ProtocolAbiContract canonical = canonical_contract();
  RenderedValues canonical_values;
  if (auto error = render_contract_fields(canonical, canonical_values)) {
    return parse_error(ProtocolAbiContractErrorCode::NonCanonicalDocument, 0U, {},
                       "the built-in v1 protocol ABI contract is invalid: " + error->detail);
  }
  for (std::size_t index = 0U; index < values.size(); ++index) {
    if (auto error = validate_lexical_value(index, values[index], lines[index])) {
      ProtocolAbiContractResult result;
      result.error = std::move(*error);
      return result;
    }
    if (values[index] != canonical_values[index]) {
      return parse_error(ProtocolAbiContractErrorCode::InvalidValue, lines[index],
                         kFieldNames[index], "value does not match the fixed v1 contract");
    }
  }

  ProtocolAbiContractResult result;
  result.value = std::move(canonical);
  const ProtocolAbiContractSerializationResult serialized =
    serialize_protocol_abi_contract(*result.value);
  if (!serialized.ok() || *serialized.payload != payload) {
    return parse_error(ProtocolAbiContractErrorCode::NonCanonicalDocument, 0U, {},
                       "parsed manifest does not replay to its exact source bytes");
  }
  return result;
}

ProtocolAbiContractSerializationResult
serialize_protocol_abi_contract(const ProtocolAbiContract &contract) {
  RenderedValues values;
  if (auto error = render_contract_values(contract, values)) {
    ProtocolAbiContractSerializationResult result;
    result.error = std::move(*error);
    return result;
  }
  ProtocolAbiContractSerializationResult result;
  result.payload = render_manifest(values);
  if (result.payload->size() > kProtocolAbiManifestMaxBytes) {
    return serialization_error(ProtocolAbiContractErrorCode::TooLarge, 0U, {},
                               "serialized protocol ABI manifest exceeds its byte limit");
  }
  return result;
}

} // namespace nebula::boot
