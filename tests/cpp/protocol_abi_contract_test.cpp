#include "boot/protocol_abi_contract.hpp"
#include "boot/protocol_abi_identity.hpp"
#include "cli/artifact_digest.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
using nebula::boot::ProtocolAbiContractErrorCode;

constexpr std::string_view kManifestPath = "boot/uos-x86_64-limine-v1/contract.manifest";

bool expect(bool condition, std::string_view message) {
  if (condition)
    return true;
  std::cerr << "protocol-abi-contract-test: " << message << '\n';
  return false;
}

std::optional<fs::path> find_repository_root_from(fs::path candidate) {
  std::error_code error;
  candidate = fs::absolute(candidate, error);
  if (error)
    return std::nullopt;
  if (!fs::is_directory(candidate, error))
    candidate = candidate.parent_path();
  error.clear();
  for (unsigned depth = 0U; depth < 12U && !candidate.empty(); ++depth) {
    if (fs::is_regular_file(candidate / kManifestPath, error) && !error)
      return candidate;
    error.clear();
    const fs::path parent = candidate.parent_path();
    if (parent == candidate)
      break;
    candidate = parent;
  }
  return std::nullopt;
}

std::optional<fs::path> find_repository_root() {
  if (const auto from_source = find_repository_root_from(fs::path(__FILE__)))
    return from_source;
  std::error_code error;
  const fs::path current = fs::current_path(error);
  if (error)
    return std::nullopt;
  return find_repository_root_from(current);
}

std::optional<std::string> read_bounded_file(const fs::path &path, std::uintmax_t max_bytes) {
  std::error_code error;
  const std::uintmax_t size = fs::file_size(path, error);
  if (error || size > max_bytes)
    return std::nullopt;
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return std::nullopt;
  std::string bytes(static_cast<std::size_t>(size), '\0');
  input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (input.gcount() != static_cast<std::streamsize>(bytes.size()))
    return std::nullopt;
  char extra = 0;
  if (input.get(extra))
    return std::nullopt;
  if (!input.eof())
    return std::nullopt;
  return bytes;
}

std::string replace_once(std::string payload, std::string_view from, std::string_view to) {
  const std::size_t position = payload.find(from);
  if (position != std::string::npos)
    payload.replace(position, from.size(), to);
  return payload;
}

std::vector<std::string> split_manifest_lines(std::string_view payload) {
  std::vector<std::string> lines;
  std::size_t start = 0U;
  while (start < payload.size()) {
    const std::size_t end = payload.find('\n', start);
    if (end == std::string_view::npos)
      break;
    lines.emplace_back(payload.substr(start, end - start));
    start = end + 1U;
  }
  return lines;
}

std::string join_manifest_lines(const std::vector<std::string> &lines) {
  std::string payload;
  for (const std::string &line : lines) {
    payload.append(line);
    payload.push_back('\n');
  }
  return payload;
}

std::string remove_line(std::string_view payload, std::string_view key) {
  std::vector<std::string> lines = split_manifest_lines(payload);
  const std::string prefix = std::string(key) + "=";
  const auto found = std::find_if(
    lines.begin(), lines.end(), [&](const std::string &line) { return line.starts_with(prefix); });
  if (found != lines.end())
    lines.erase(found);
  return join_manifest_lines(lines);
}

std::string swap_adjacent_lines(std::string_view payload, std::size_t first) {
  std::vector<std::string> lines = split_manifest_lines(payload);
  if (first + 1U < lines.size())
    std::swap(lines[first], lines[first + 1U]);
  return join_manifest_lines(lines);
}

bool expect_parse_error(std::string_view payload, ProtocolAbiContractErrorCode expected_code,
                        std::string_view label,
                        std::optional<std::size_t> expected_line = std::nullopt,
                        std::string_view expected_field = {}) {
  const nebula::boot::ProtocolAbiContractResult result =
    nebula::boot::parse_protocol_abi_contract(payload);
  bool ok = true;
  ok &= expect(!result.ok(), std::string(label) + " unexpectedly parsed");
  ok &= expect(result.error.code == expected_code,
               std::string(label) + " returned the wrong error code");
  if (expected_line.has_value()) {
    ok &= expect(result.error.line == *expected_line,
                 std::string(label) + " returned the wrong error line");
  }
  if (!expected_field.empty()) {
    ok &= expect(result.error.field == expected_field,
                 std::string(label) + " returned the wrong error field");
  }
  return ok;
}

bool test_canonical_contract(std::string_view manifest) {
  bool ok = true;
  const nebula::boot::ProtocolAbiContractResult parsed =
    nebula::boot::parse_protocol_abi_contract(manifest);
  ok &= expect(parsed.ok(), "repository manifest did not parse");
  if (!parsed.ok())
    return false;

  const nebula::boot::ProtocolAbiContractSerializationResult serialized =
    nebula::boot::serialize_protocol_abi_contract(*parsed.value);
  ok &= expect(serialized.ok(), "parsed repository manifest did not serialize");
  if (serialized.ok()) {
    ok &= expect(*serialized.payload == manifest,
                 "parse/serialize did not preserve the exact manifest bytes");
  }

  const auto &contract = *parsed.value;
  ok &= expect(contract.contract_id == nebula::boot::kUosX86_64ContractId,
               "contract ID changed unexpectedly");
  ok &= expect(contract.scope == nebula::boot::ProtocolAbiContractScope::ProtocolAbiCandidate,
               "contract scope overclaims implementation status");
  ok &= expect(contract.gate_id == "UOS-BOOT-001" &&
                 contract.gate_status == nebula::boot::ProtocolAbiGateStatus::Planned,
               "UOS-BOOT-001 must remain planned");
  ok &= expect(contract.abi.target_triple == nebula::boot::kUosX86_64TargetTriple &&
                 contract.abi.image_format == nebula::boot::KernelImageFormat::Elf64EtExec &&
                 contract.abi.data_encoding == nebula::boot::ElfDataEncoding::LittleEndian &&
                 contract.abi.machine == nebula::boot::ElfMachine::X86_64,
               "target or future image format contract changed");
  ok &= expect(contract.abi.image_entry_symbol == nebula::boot::kUosX86_64ImageEntrySymbol &&
                 contract.abi.payload_entry_symbol == nebula::boot::kUosX86_64PayloadEntrySymbol,
               "image and payload entry ownership are reversed or ambiguous");
  ok &= expect(contract.abi.high_half_minimum == nebula::boot::kUosX86_64HighHalfMinimum,
               "high-half minimum changed");
  ok &=
    expect(contract.abi.panic_policy == nebula::boot::KernelPanicPolicy::Trap &&
             contract.abi.entry_calling_convention ==
               nebula::boot::KernelEntryCallingConvention::X86_64SystemVRestricted &&
             contract.abi.entry_must_not_return == nebula::boot::kUosX86_64EntryMustNotReturn &&
             contract.abi.entry_fp_simd_allowed == nebula::boot::kUosX86_64EntryFpSimdAllowed &&
             contract.abi.minimum_boot_stack_bytes == nebula::boot::kUosX86_64MinimumBootStackBytes,
           "entry calling convention, trap, noreturn, or stack contract changed");
  ok &=
    expect(!contract.abi.red_zone_allowed && !contract.abi.x87_allowed &&
             !contract.abi.mmx_allowed && !contract.abi.sse_allowed && !contract.abi.sse2_allowed,
           "restricted entry ABI unexpectedly permits red-zone or FP/SIMD state");
  ok &= expect(nebula::boot::kUosX86_64RequiredCompilerAbiArguments ==
                 std::array<std::string_view, 7>{"-m64", "-mabi=sysv", "-mno-red-zone",
                                                 "-mno-80387", "-mno-mmx", "-mno-sse", "-mno-sse2"},
               "shared compiler ABI arguments no longer enforce the restricted entry ABI");

  ok &=
    expect(contract.limine.release_repository == "https://github.com/Limine-Bootloader/Limine" &&
             contract.limine.release_tag == "v12.3.2" &&
             contract.limine.release_tag_object == "5e6ef2a0ae7afcd863639b78aee1dbb6cacf1b45" &&
             contract.limine.release_commit == "8c8a688776735b2b2d12683a032e442583d361db",
           "Limine release identity changed");
  ok &= expect(contract.limine.protocol_repository ==
                   "https://github.com/Limine-Bootloader/limine-protocol" &&
                 contract.limine.protocol_commit == "5b9d13e557590d8eab93fa7449bdd1d7ed72ba8c",
               "manifest is not using the v12.3.2 bootstrap-pinned protocol commit");
  ok &= expect(contract.limine.base_revision == UINT64_C(6) &&
                 contract.limine.base_revision_support_check ==
                   nebula::boot::LimineBaseRevisionSupportCheck::Qword2EqualsZero &&
                 contract.limine.marker_alignment_bytes == UINT64_C(8),
               "base revision support or marker alignment changed");
  ok &= expect(
    contract.limine.request_common_magic ==
      std::array<std::uint64_t, 2>{UINT64_C(0xc7b1dd30df4c8b88), UINT64_C(0x0a82e883a194f07b)},
    "Limine common request magic changed");
  ok &= expect(
    contract.limine.requests_start_marker ==
      std::array<std::uint64_t, 4>{UINT64_C(0xf6b8f4b39de7d1ae), UINT64_C(0xfab91a6940fcb9cf),
                                   UINT64_C(0x785c6ed015d3e316), UINT64_C(0x181e920a7852b9d9)},
    "Limine requests-start marker changed");
  ok &= expect(contract.limine.base_revision_marker ==
                   std::array<std::uint64_t, 3>{UINT64_C(0xf9562b2d5c95a6c8),
                                                UINT64_C(0x6a7b384944536bdc), UINT64_C(6)} &&
                 contract.limine.supported_base_revision_value == UINT64_C(0),
               "Limine base-revision marker or supported value changed");
  ok &= expect(
    contract.limine.requests_end_marker ==
      std::array<std::uint64_t, 2>{UINT64_C(0xadc0e0531bb10d03), UINT64_C(0x9572709f31764c62)},
    "Limine requests-end marker changed");
  ok &= expect(manifest.find("toolchain") == std::string_view::npos &&
                 manifest.find("TBD") == std::string_view::npos &&
                 manifest.find("unresolved") == std::string_view::npos,
               "protocol/ABI candidate contains an unresolved toolchain placeholder");
  return ok;
}

bool test_repository_vendor_assets(const fs::path &repository_root, std::string_view manifest) {
  const nebula::boot::ProtocolAbiContractResult parsed =
    nebula::boot::parse_protocol_abi_contract(manifest);
  if (!expect(parsed.ok(), "vendor test could not parse the canonical contract"))
    return false;

  bool ok = true;
  const auto &header_identity = parsed.value->limine.header;
  const auto &license_identity = parsed.value->limine.license_file;
  const fs::path header = repository_root / fs::path(header_identity.vendor_path);
  const fs::path license = repository_root / fs::path(license_identity.vendor_path);

  const nebula::cli::FileDigestResult header_digest = nebula::cli::sha256_file(header, 64U * 1024U);
  const nebula::cli::FileDigestResult license_digest =
    nebula::cli::sha256_file(license, 64U * 1024U);
  ok &= expect(header_digest.ok(), "could not hash vendored limine.h");
  ok &= expect(license_digest.ok(), "could not hash vendored Limine protocol LICENSE");
  const nebula::cli::FileDigestResult manifest_digest = nebula::cli::sha256_file(
    repository_root / kManifestPath, nebula::boot::kProtocolAbiManifestMaxBytes);
  ok &= expect(manifest_digest.ok(), "could not hash protocol ABI manifest");
  if (manifest_digest.ok()) {
    ok &= expect(manifest_digest.value->size == nebula::boot::kProtocolAbiManifestSize &&
                   manifest_digest.value->sha256 == nebula::boot::kProtocolAbiManifestSha256,
                 "build-time protocol ABI identity does not match repository bytes");
  }
  if (header_digest.ok()) {
    ok &= expect(header_digest.value->size == header_identity.size &&
                   header_digest.value->sha256 == header_identity.sha256,
                 "vendored limine.h does not match the pinned upstream bytes");
  }
  if (license_digest.ok()) {
    ok &= expect(license_digest.value->size == license_identity.size &&
                   license_digest.value->sha256 == license_identity.sha256,
                 "vendored protocol LICENSE does not match the pinned upstream bytes");
  }

  return ok;
}

bool test_schema_and_order_fail_closed(std::string_view canonical) {
  bool ok = true;
  ok &= expect_parse_error(
    replace_once(std::string(canonical), "manifest_schema=nebula-uos-protocol-abi-contract",
                 "manifest_schema=nebula-uos-protocol-abi-contract-v2"),
    ProtocolAbiContractErrorCode::UnsupportedSchema, "unsupported schema", 1U, "manifest_schema");
  ok &= expect_parse_error(replace_once(std::string(canonical) + "future_field=1\n",
                                        "manifest_schema=nebula-uos-protocol-abi-contract",
                                        "manifest_schema=nebula-uos-protocol-abi-contract-v2"),
                           ProtocolAbiContractErrorCode::UnsupportedSchema,
                           "unsupported schema must win over future fields", 1U, "manifest_schema");
  ok &= expect_parse_error(
    replace_once(std::string(canonical), "manifest_version=1", "manifest_version=2"),
    ProtocolAbiContractErrorCode::UnsupportedVersion, "unsupported manifest version", 2U,
    "manifest_version");
  ok &=
    expect_parse_error(replace_once(std::string(canonical) + "future_field=1\n",
                                    "manifest_version=1", "manifest_version=2"),
                       ProtocolAbiContractErrorCode::UnsupportedVersion,
                       "unsupported version must win over future fields", 2U, "manifest_version");
  ok &= expect_parse_error(
    replace_once(std::string(canonical), "manifest_version=1", "manifest_version=01"),
    ProtocolAbiContractErrorCode::NonCanonicalNumber, "leading-zero manifest version", 2U,
    "manifest_version");
  ok &=
    expect_parse_error(replace_once(std::string(canonical), "manifest_schema=", "future_schema="),
                       ProtocolAbiContractErrorCode::SchemaNotFirst, "unknown key before schema",
                       1U, "manifest_schema");
  ok &= expect_parse_error(std::string(canonical) + "future_field=1\n",
                           ProtocolAbiContractErrorCode::UnknownField, "unknown field", 56U,
                           "future_field");
  ok &= expect_parse_error(std::string(canonical) + "gate_status=planned\n",
                           ProtocolAbiContractErrorCode::DuplicateField, "duplicate field", 56U,
                           "gate_status");
  ok &= expect_parse_error(remove_line(canonical, "panic_policy"),
                           ProtocolAbiContractErrorCode::MissingField, "missing field", 0U,
                           "panic_policy");

  for (std::size_t index = 0U; index + 1U < nebula::boot::kProtocolAbiManifestFieldCount; ++index) {
    const ProtocolAbiContractErrorCode expected =
      index == 0U ? ProtocolAbiContractErrorCode::SchemaNotFirst
                  : ProtocolAbiContractErrorCode::NonCanonicalOrder;
    ok &= expect_parse_error(swap_adjacent_lines(canonical, index), expected,
                             "adjacent field-order swap", index + 1U);
  }

  std::string too_many_fields;
  for (std::size_t index = 0U; index < nebula::boot::kProtocolAbiManifestMaxFields + 1U; ++index)
    too_many_fields.append("x=1\n");
  ok &=
    expect_parse_error(too_many_fields, ProtocolAbiContractErrorCode::TooManyFields,
                       "field-count overflow", nebula::boot::kProtocolAbiManifestMaxFields + 1U);
  return ok;
}

bool test_encoding_and_resource_bounds(std::string_view canonical) {
  bool ok = true;
  ok &= expect_parse_error({}, ProtocolAbiContractErrorCode::MissingField, "empty manifest", 0U,
                           "manifest_schema");
  ok &= expect_parse_error(std::string(nebula::boot::kProtocolAbiManifestMaxBytes + 1U, 'x'),
                           ProtocolAbiContractErrorCode::TooLarge, "oversized manifest");
  ok &= expect_parse_error(
    "manifest_schema=" + std::string(nebula::boot::kProtocolAbiManifestMaxLineBytes, 'x') + "\n",
    ProtocolAbiContractErrorCode::LineTooLong, "oversized manifest line", 1U);
  ok &= expect_parse_error(std::string(canonical.substr(0U, canonical.size() - 1U)),
                           ProtocolAbiContractErrorCode::InvalidLine, "missing final LF", 55U);
  ok &= expect_parse_error(std::string(canonical) + "\n", ProtocolAbiContractErrorCode::InvalidLine,
                           "extra final LF", 56U);
  ok &= expect_parse_error(
    replace_once(std::string(canonical), "gate_status=planned", "gate_status==planned"),
    ProtocolAbiContractErrorCode::InvalidLine, "multiple separators", 6U);

  std::string bom = std::string("\xef\xbb\xbf") + std::string(canonical);
  ok &= expect_parse_error(bom, ProtocolAbiContractErrorCode::InvalidEncoding, "UTF-8 BOM", 1U);
  std::string nul(canonical);
  nul[10U] = '\0';
  ok &= expect_parse_error(nul, ProtocolAbiContractErrorCode::InvalidEncoding, "NUL byte", 1U);
  std::string non_ascii(canonical);
  non_ascii[10U] = static_cast<char>(0x80U);
  ok &= expect_parse_error(non_ascii, ProtocolAbiContractErrorCode::InvalidEncoding,
                           "non-ASCII byte", 1U);
  ok &= expect_parse_error(
    replace_once(std::string(canonical), "gate_status=planned", "gate_status=plan\tned"),
    ProtocolAbiContractErrorCode::InvalidEncoding, "TAB byte", 6U);
  ok &= expect_parse_error(replace_once(std::string(canonical),
                                        "manifest_schema=nebula-uos-protocol-abi-contract\n",
                                        "manifest_schema=nebula-uos-protocol-abi-contract\r\n"),
                           ProtocolAbiContractErrorCode::InvalidEncoding, "CRLF", 1U);
  return ok;
}

bool test_value_and_path_validation(std::string_view canonical) {
  bool ok = true;
  ok &= expect_parse_error(replace_once(std::string(canonical), "minimum_boot_stack_bytes=65536",
                                        "minimum_boot_stack_bytes=065536"),
                           ProtocolAbiContractErrorCode::NonCanonicalNumber,
                           "leading-zero stack size", 18U, "minimum_boot_stack_bytes");
  ok &= expect_parse_error(
    replace_once(std::string(canonical), "red_zone_allowed=0", "red_zone_allowed=00"),
    ProtocolAbiContractErrorCode::NonCanonicalNumber, "noncanonical boolean", 19U,
    "red_zone_allowed");
  ok &= expect_parse_error(
    replace_once(std::string(canonical), "red_zone_allowed=0", "red_zone_allowed=2"),
    ProtocolAbiContractErrorCode::InvalidValue, "out-of-domain canonical boolean", 19U,
    "red_zone_allowed");
  ok &= expect_parse_error(
    replace_once(std::string(canonical), "entry_fp_simd_allowed=0", "entry_fp_simd_allowed=1"),
    ProtocolAbiContractErrorCode::InvalidValue, "FP/SIMD umbrella policy enabled", 17U,
    "entry_fp_simd_allowed");
  ok &=
    expect_parse_error(replace_once(std::string(canonical), "high_half_minimum=0xffffffff80000000",
                                    "high_half_minimum=0xFFFFFFFF80000000"),
                       ProtocolAbiContractErrorCode::NonCanonicalNumber, "uppercase fixed qword",
                       13U, "high_half_minimum");
  ok &=
    expect_parse_error(replace_once(std::string(canonical), "high_half_minimum=0xffffffff80000000",
                                    "high_half_minimum=0xffffffff8000000"),
                       ProtocolAbiContractErrorCode::NonCanonicalNumber, "short fixed qword", 13U,
                       "high_half_minimum");
  ok &= expect_parse_error(
    replace_once(
      std::string(canonical),
      "limine_header_sha256=276db60a383509287d65f14e3f73b28f2258eda923f0d29a8848512db883fc5b",
      "limine_header_sha256=276DB60A383509287D65F14E3F73B28F2258EDA923F0D29A8848512DB883FC5B"),
    ProtocolAbiContractErrorCode::InvalidDigest, "uppercase digest", 49U, "limine_header_sha256");
  ok &= expect_parse_error(
    replace_once(
      std::string(canonical),
      "limine_header_sha256=276db60a383509287d65f14e3f73b28f2258eda923f0d29a8848512db883fc5b",
      "limine_header_sha256=276db60a383509287d65f14e3f73b28f2258eda923f0d29a8848512db883fc5"),
    ProtocolAbiContractErrorCode::InvalidDigest, "short digest", 49U, "limine_header_sha256");
  ok &= expect_parse_error(
    replace_once(
      std::string(canonical),
      "limine_header_sha256=276db60a383509287d65f14e3f73b28f2258eda923f0d29a8848512db883fc5b",
      "limine_header_sha256=0000000000000000000000000000000000000000000000000000000000000000"),
    ProtocolAbiContractErrorCode::InvalidValue, "wrong canonical digest", 49U,
    "limine_header_sha256");
  ok &= expect_parse_error(
    replace_once(std::string(canonical),
                 "limine_protocol_commit=5b9d13e557590d8eab93fa7449bdd1d7ed72ba8c",
                 "limine_protocol_commit=5B9D13E557590D8EAB93FA7449BDD1D7ED72BA8C"),
    ProtocolAbiContractErrorCode::InvalidGitObjectId, "uppercase Git object ID", 30U,
    "limine_protocol_commit");
  ok &= expect_parse_error(
    replace_once(std::string(canonical),
                 "limine_protocol_commit=5b9d13e557590d8eab93fa7449bdd1d7ed72ba8c",
                 "limine_protocol_commit=5b9d13e557590d8eab93fa7449bdd1d7ed72ba8"),
    ProtocolAbiContractErrorCode::InvalidGitObjectId, "short Git object ID", 30U,
    "limine_protocol_commit");
  ok &= expect_parse_error(
    replace_once(std::string(canonical),
                 "limine_protocol_commit=5b9d13e557590d8eab93fa7449bdd1d7ed72ba8c",
                 "limine_protocol_commit=0000000000000000000000000000000000000000"),
    ProtocolAbiContractErrorCode::InvalidValue, "wrong canonical Git object ID", 30U,
    "limine_protocol_commit");
  ok &= expect_parse_error(replace_once(std::string(canonical), "limine_header_size=17225",
                                        "limine_header_size=18446744073709551616"),
                           ProtocolAbiContractErrorCode::NonCanonicalNumber,
                           "overflowing decimal size", 48U, "limine_header_size");

  const std::array<std::string_view, 11> unsafe_paths = {
    "/absolute/limine.h", "../limine.h",    ".",    "a//b", "a/./b", "a/../b",
    "C:/limine.h",        "//server/share", "a\\b", "a:b",  "a/",
  };
  for (const std::string_view unsafe_path : unsafe_paths) {
    ok &= expect_parse_error(
      replace_once(std::string(canonical),
                   "limine_header_vendor_path=boot/uos-x86_64-limine-v1/protocol/limine.h",
                   std::string("limine_header_vendor_path=") + std::string(unsafe_path)),
      ProtocolAbiContractErrorCode::UnsafePath, "unsafe vendor path", 47U,
      "limine_header_vendor_path");
  }
  ok &= expect_parse_error(
    replace_once(std::string(canonical),
                 "limine_header_vendor_path=boot/uos-x86_64-limine-v1/protocol/limine.h",
                 "limine_header_vendor_path=" +
                   std::string(nebula::boot::kProtocolAbiManifestMaxPathBytes + 1U, 'a')),
    ProtocolAbiContractErrorCode::UnsafePath, "oversized vendor path", 47U,
    "limine_header_vendor_path");

  ok &= expect_parse_error(replace_once(std::string(canonical), "minimum_boot_stack_bytes=65536",
                                        "minimum_boot_stack_bytes=65537"),
                           ProtocolAbiContractErrorCode::InvalidValue, "wrong canonical stack size",
                           18U, "minimum_boot_stack_bytes");
  ok &= expect_parse_error(replace_once(std::string(canonical), "limine_release_tag=v12.3.2",
                                        "limine_release_tag=v12.3.3"),
                           ProtocolAbiContractErrorCode::InvalidValue,
                           "wrong but syntactically valid release tag", 26U, "limine_release_tag");
  ok &= expect_parse_error(
    replace_once(std::string(canonical),
                 "limine_header_vendor_path=boot/uos-x86_64-limine-v1/protocol/limine.h",
                 "limine_header_vendor_path=boot/uos-x86_64-limine-v1/protocol/other.h"),
    ProtocolAbiContractErrorCode::InvalidValue, "wrong canonical vendor path", 47U,
    "limine_header_vendor_path");
  return ok;
}

bool test_serializer_rejects_invalid_struct(std::string_view canonical) {
  const nebula::boot::ProtocolAbiContractResult parsed =
    nebula::boot::parse_protocol_abi_contract(canonical);
  if (!expect(parsed.ok(), "serializer test could not parse canonical contract"))
    return false;

  bool ok = true;
  nebula::boot::ProtocolAbiContract invalid = *parsed.value;
  invalid.limine.header.vendor_path = "../limine.h";
  auto serialized = nebula::boot::serialize_protocol_abi_contract(invalid);
  ok &=
    expect(!serialized.ok() && serialized.error.code == ProtocolAbiContractErrorCode::UnsafePath &&
             serialized.error.line == 47U && serialized.error.field == "limine_header_vendor_path",
           "serializer accepted an unsafe vendor path");

  invalid = *parsed.value;
  invalid.limine.header.sha256 = std::string(64U, 'A');
  serialized = nebula::boot::serialize_protocol_abi_contract(invalid);
  ok &= expect(!serialized.ok() &&
                 serialized.error.code == ProtocolAbiContractErrorCode::InvalidDigest &&
                 serialized.error.field == "limine_header_sha256",
               "serializer accepted a noncanonical digest");

  invalid = *parsed.value;
  invalid.abi.image_entry_symbol = "other";
  serialized = nebula::boot::serialize_protocol_abi_contract(invalid);
  ok &= expect(!serialized.ok() &&
                 serialized.error.code == ProtocolAbiContractErrorCode::InvalidValue &&
                 serialized.error.line == 11U && serialized.error.field == "image_entry_symbol",
               "serializer accepted a changed image entry symbol");

  invalid = *parsed.value;
  invalid.abi.image_format = static_cast<nebula::boot::KernelImageFormat>(255U);
  serialized = nebula::boot::serialize_protocol_abi_contract(invalid);
  ok &= expect(!serialized.ok() &&
                 serialized.error.code == ProtocolAbiContractErrorCode::InvalidValue &&
                 serialized.error.line == 8U && serialized.error.field == "image_format",
               "serializer accepted an unknown image-format enum");
  return ok;
}

} // namespace

int main() {
  const std::optional<fs::path> repository_root = find_repository_root();
  if (!expect(repository_root.has_value(), "could not locate the repository root"))
    return 1;
  const std::optional<std::string> manifest =
    read_bounded_file(*repository_root / kManifestPath, nebula::boot::kProtocolAbiManifestMaxBytes);
  if (!expect(manifest.has_value(), "could not read the repository protocol ABI manifest"))
    return 1;

  bool ok = true;
  ok &= test_canonical_contract(*manifest);
  ok &= test_repository_vendor_assets(*repository_root, *manifest);
  ok &= test_schema_and_order_fail_closed(*manifest);
  ok &= test_encoding_and_resource_bounds(*manifest);
  ok &= test_value_and_path_validation(*manifest);
  ok &= test_serializer_rejects_invalid_struct(*manifest);
  return ok ? 0 : 1;
}
