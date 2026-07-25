#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace nebula::boot {

inline constexpr std::string_view kProtocolAbiManifestSchema = "nebula-uos-protocol-abi-contract";
inline constexpr std::uint32_t kProtocolAbiManifestVersion = 1U;
inline constexpr std::string_view kUosX86_64ContractId = "uos-x86_64-limine-v1";
inline constexpr std::string_view kUosX86_64TargetTriple = "x86_64-unknown-none";
inline constexpr std::string_view kUosX86_64ImageFormat = "elf64-et-exec";
inline constexpr std::string_view kUosX86_64ImageEntrySymbol = "_start";
inline constexpr std::string_view kUosX86_64PayloadEntrySymbol = "__nebula_uos_payload_entry_v1";
inline constexpr std::string_view kUosX86_64PanicPolicy = "trap";
inline constexpr std::string_view kUosX86_64EntryCallingConvention = "x86_64-system-v-restricted";
inline constexpr std::uint64_t kUosX86_64HighHalfMinimum = UINT64_C(0xffffffff80000000);
inline constexpr std::uint64_t kUosX86_64MinimumBootStackBytes = UINT64_C(65536);
inline constexpr bool kUosX86_64EntryMustNotReturn = true;
inline constexpr bool kUosX86_64EntryFpSimdAllowed = false;
inline constexpr bool kUosX86_64RedZoneAllowed = false;
inline constexpr bool kUosX86_64X87Allowed = false;
inline constexpr bool kUosX86_64MmxAllowed = false;
inline constexpr bool kUosX86_64SseAllowed = false;
inline constexpr bool kUosX86_64Sse2Allowed = false;
// Production freestanding compilation must reuse this ordered ABI slice rather than duplicate it.
inline constexpr std::array<std::string_view, 7> kUosX86_64RequiredCompilerAbiArguments = {
  "-m64", "-mabi=sysv", "-mno-red-zone", "-mno-80387", "-mno-mmx", "-mno-sse", "-mno-sse2",
};
// These limits bound the pure manifest codec; referenced-asset limits belong to the verifier.
inline constexpr std::size_t kProtocolAbiManifestFieldCount = 55U;
inline constexpr std::size_t kProtocolAbiManifestMaxFields = 64U;
inline constexpr std::size_t kProtocolAbiManifestMaxBytes = 8U * 1024U;
inline constexpr std::size_t kProtocolAbiManifestMaxLineBytes = 256U;
inline constexpr std::size_t kProtocolAbiManifestMaxPathBytes = 192U;

enum class ProtocolAbiContractErrorCode : std::uint8_t {
  None,
  TooLarge,
  TooManyFields,
  LineTooLong,
  InvalidEncoding,
  InvalidLine,
  SchemaNotFirst,
  UnsupportedSchema,
  UnsupportedVersion,
  UnknownField,
  DuplicateField,
  MissingField,
  NonCanonicalOrder,
  NonCanonicalNumber,
  InvalidDigest,
  InvalidGitObjectId,
  UnsafePath,
  InvalidValue,
  NonCanonicalDocument,
};

struct ProtocolAbiContractError {
  ProtocolAbiContractErrorCode code = ProtocolAbiContractErrorCode::None;
  // Zero means no source line exists. Serializer errors use the canonical output line position.
  std::size_t line = 0U;
  std::string field;
  std::string detail;
};

enum class ProtocolAbiContractScope : std::uint8_t {
  ProtocolAbiCandidate,
};

enum class ProtocolAbiGateStatus : std::uint8_t {
  Planned,
};

enum class KernelImageFormat : std::uint8_t {
  Elf64EtExec,
};

enum class ElfDataEncoding : std::uint8_t {
  LittleEndian,
};

enum class ElfMachine : std::uint8_t {
  X86_64,
};

enum class KernelPanicPolicy : std::uint8_t {
  Trap,
};

enum class KernelEntryCallingConvention : std::uint8_t {
  X86_64SystemVRestricted,
};

enum class BootProtocol : std::uint8_t {
  Limine,
};

enum class LimineBaseRevisionSupportCheck : std::uint8_t {
  Qword2EqualsZero,
};

enum class ProtocolLicense : std::uint8_t {
  ZeroBsd,
};

struct VendoredProtocolAsset {
  std::string source_path;
  std::string vendor_path;
  std::uint64_t size = 0U;
  std::string sha256;
  ProtocolLicense license = ProtocolLicense::ZeroBsd;

  bool operator==(const VendoredProtocolAsset &) const = default;
};

struct X86_64ProtocolAbi {
  std::string target_triple;
  KernelImageFormat image_format = KernelImageFormat::Elf64EtExec;
  ElfDataEncoding data_encoding = ElfDataEncoding::LittleEndian;
  ElfMachine machine = ElfMachine::X86_64;
  std::string image_entry_symbol;
  std::string payload_entry_symbol;
  std::uint64_t high_half_minimum = 0U;
  KernelPanicPolicy panic_policy = KernelPanicPolicy::Trap;
  KernelEntryCallingConvention entry_calling_convention =
    KernelEntryCallingConvention::X86_64SystemVRestricted;
  bool entry_must_not_return = false;
  bool entry_fp_simd_allowed = true;
  std::uint64_t minimum_boot_stack_bytes = 0U;
  bool red_zone_allowed = true;
  bool x87_allowed = true;
  bool mmx_allowed = true;
  bool sse_allowed = true;
  bool sse2_allowed = true;

  bool operator==(const X86_64ProtocolAbi &) const = default;
};

struct LimineProtocolContract {
  BootProtocol protocol = BootProtocol::Limine;
  std::string release_repository;
  std::string release_tag;
  std::string release_tag_object;
  std::string release_commit;
  std::string protocol_repository;
  std::string protocol_commit;
  std::uint64_t base_revision = 0U;
  LimineBaseRevisionSupportCheck base_revision_support_check =
    LimineBaseRevisionSupportCheck::Qword2EqualsZero;
  std::uint64_t marker_alignment_bytes = 0U;
  std::array<std::uint64_t, 2> request_common_magic{};
  std::array<std::uint64_t, 4> requests_start_marker{};
  std::array<std::uint64_t, 3> base_revision_marker{};
  std::uint64_t supported_base_revision_value = 0U;
  std::array<std::uint64_t, 2> requests_end_marker{};
  VendoredProtocolAsset header;
  VendoredProtocolAsset license_file;

  bool operator==(const LimineProtocolContract &) const = default;
};

struct ProtocolAbiContract {
  std::string contract_id;
  ProtocolAbiContractScope scope = ProtocolAbiContractScope::ProtocolAbiCandidate;
  std::string gate_id;
  ProtocolAbiGateStatus gate_status = ProtocolAbiGateStatus::Planned;
  X86_64ProtocolAbi abi;
  LimineProtocolContract limine;

  bool operator==(const ProtocolAbiContract &) const = default;
};

struct ProtocolAbiContractResult {
  std::optional<ProtocolAbiContract> value;
  ProtocolAbiContractError error;

  [[nodiscard]] bool ok() const noexcept {
    return value.has_value() && error.code == ProtocolAbiContractErrorCode::None;
  }
};

struct ProtocolAbiContractSerializationResult {
  std::optional<std::string> payload;
  ProtocolAbiContractError error;

  [[nodiscard]] bool ok() const noexcept {
    return payload.has_value() && error.code == ProtocolAbiContractErrorCode::None;
  }
};

[[nodiscard]] ProtocolAbiContractResult parse_protocol_abi_contract(std::string_view payload);

[[nodiscard]] ProtocolAbiContractSerializationResult
serialize_protocol_abi_contract(const ProtocolAbiContract &contract);

} // namespace nebula::boot
