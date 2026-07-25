#include "cli/artifact_digest.hpp"
#include "cli/elf_object.hpp"
#include "boot/protocol_abi_contract.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::size_t kMinimalSectionTable = 168U;
constexpr std::size_t kRelocationSectionTable = 216U;
constexpr std::size_t kMinimalSymbolNamesOffset = kMinimalSectionTable + 5U * 64U;
constexpr std::size_t kRelocationSymbolNamesOffset = kRelocationSectionTable + 6U * 64U;

std::string payload_symbol_names() {
  std::string names(1U, '\0');
  names.append(nebula::boot::kUosX86_64PayloadEntrySymbol);
  names.push_back('\0');
  return names;
}

void write_u16(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint16_t value) {
  bytes.at(offset) = static_cast<std::uint8_t>(value & 0xffU);
  bytes.at(offset + 1U) = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
}

void write_u32(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint32_t value) {
  for (unsigned index = 0; index < 4U; ++index) {
    bytes.at(offset + index) = static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU);
  }
}

void write_u64(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint64_t value) {
  for (unsigned index = 0; index < 8U; ++index) {
    bytes.at(offset + index) = static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU);
  }
}

void write_section(std::vector<std::uint8_t> &bytes, std::size_t section_table, std::size_t index,
                   std::uint32_t name, std::uint32_t type, std::uint64_t flags,
                   std::uint64_t offset, std::uint64_t size, std::uint32_t link, std::uint32_t info,
                   std::uint64_t alignment, std::uint64_t entry_size) {
  const std::size_t base = section_table + index * 64U;
  write_u32(bytes, base, name);
  write_u32(bytes, base + 4U, type);
  write_u64(bytes, base + 8U, flags);
  write_u64(bytes, base + 24U, offset);
  write_u64(bytes, base + 32U, size);
  write_u32(bytes, base + 40U, link);
  write_u32(bytes, base + 44U, info);
  write_u64(bytes, base + 48U, alignment);
  write_u64(bytes, base + 56U, entry_size);
}

std::vector<std::uint8_t> minimal_valid_object() {
  const std::string symbol_names = payload_symbol_names();
  std::vector<std::uint8_t> bytes(kMinimalSymbolNamesOffset + symbol_names.size(), 0U);
  bytes[0] = 0x7fU;
  bytes[1] = 'E';
  bytes[2] = 'L';
  bytes[3] = 'F';
  bytes[4] = 2U;
  bytes[5] = 1U;
  bytes[6] = 1U;
  write_u16(bytes, 16U, 1U);
  write_u16(bytes, 18U, 62U);
  write_u32(bytes, 20U, 1U);
  write_u64(bytes, 40U, kMinimalSectionTable);
  write_u16(bytes, 52U, 64U);
  write_u16(bytes, 58U, 64U);
  write_u16(bytes, 60U, 5U);
  write_u16(bytes, 62U, 4U);

  bytes[64] = 0xccU;
  write_u32(bytes, 96U, 1U);
  bytes[100U] = 0x12U;
  write_u16(bytes, 102U, 1U);
  write_u64(bytes, 112U, 1U);

  std::copy(symbol_names.begin(), symbol_names.end(), bytes.begin() + kMinimalSymbolNamesOffset);
  constexpr std::string_view kSectionNames("\0.text\0.symtab\0.strtab\0.shstrtab\0", 33U);
  std::copy(kSectionNames.begin(), kSectionNames.end(), bytes.begin() + 128);

  write_section(bytes, kMinimalSectionTable, 1U, 1U, 1U, 0x6U, 64U, 1U, 0U, 0U, 16U, 0U);
  write_section(bytes, kMinimalSectionTable, 2U, 7U, 2U, 0U, 72U, 48U, 3U, 1U, 8U, 24U);
  write_section(bytes, kMinimalSectionTable, 3U, 15U, 3U, 0U, kMinimalSymbolNamesOffset,
                symbol_names.size(), 0U, 0U, 1U, 0U);
  write_section(bytes, kMinimalSectionTable, 4U, 23U, 3U, 0U, 128U, 33U, 0U, 0U, 1U, 0U);
  return bytes;
}

std::vector<std::uint8_t> minimal_valid_object_with_relocation() {
  constexpr std::size_t kRelocationOffset = 176U;
  const std::string symbol_names = payload_symbol_names();
  std::vector<std::uint8_t> bytes(kRelocationSymbolNamesOffset + symbol_names.size(), 0U);
  bytes[0] = 0x7fU;
  bytes[1] = 'E';
  bytes[2] = 'L';
  bytes[3] = 'F';
  bytes[4] = 2U;
  bytes[5] = 1U;
  bytes[6] = 1U;
  write_u16(bytes, 16U, 1U);
  write_u16(bytes, 18U, 62U);
  write_u32(bytes, 20U, 1U);
  write_u64(bytes, 40U, kRelocationSectionTable);
  write_u16(bytes, 52U, 64U);
  write_u16(bytes, 58U, 64U);
  write_u16(bytes, 60U, 6U);
  write_u16(bytes, 62U, 5U);

  bytes[64] = 0xccU;
  bytes[65] = 0xccU;
  bytes[66] = 0xccU;
  bytes[67] = 0xccU;
  write_u32(bytes, 96U, 1U);
  bytes[100U] = 0x12U;
  write_u16(bytes, 102U, 1U);
  write_u64(bytes, 112U, 4U);

  std::copy(symbol_names.begin(), symbol_names.end(), bytes.begin() + kRelocationSymbolNamesOffset);
  constexpr std::string_view kSectionNames("\0.text\0.symtab\0.strtab\0.rela.text\0.shstrtab\0",
                                           44U);
  std::copy(kSectionNames.begin(), kSectionNames.end(), bytes.begin() + 128);

  write_u64(bytes, kRelocationOffset, 0U);
  write_u64(bytes, kRelocationOffset + 8U, (static_cast<std::uint64_t>(1U) << 32U) | 2U);

  write_section(bytes, kRelocationSectionTable, 1U, 1U, 1U, 0x6U, 64U, 4U, 0U, 0U, 16U, 0U);
  write_section(bytes, kRelocationSectionTable, 2U, 7U, 2U, 0U, 72U, 48U, 3U, 1U, 8U, 24U);
  write_section(bytes, kRelocationSectionTable, 3U, 15U, 3U, 0U, kRelocationSymbolNamesOffset,
                symbol_names.size(), 0U, 0U, 1U, 0U);
  write_section(bytes, kRelocationSectionTable, 4U, 23U, 4U, 0x40U, kRelocationOffset, 24U, 2U, 1U,
                8U, 24U);
  write_section(bytes, kRelocationSectionTable, 5U, 34U, 3U, 0U, 128U, 44U, 0U, 0U, 1U, 0U);
  return bytes;
}

std::vector<std::uint8_t> object_with_relocation_to_metadata_symbol() {
  auto bytes = minimal_valid_object_with_relocation();
  bytes.insert(bytes.begin() + 96, 24U, 0U);
  constexpr std::size_t kSectionTable = 240U;
  write_u64(bytes, 40U, kSectionTable);

  write_u16(bytes, 102U, 3U);
  write_u64(bytes, 208U, (static_cast<std::uint64_t>(1U) << 32U) | 2U);

  write_u64(bytes, kSectionTable + 2U * 64U + 32U, 72U);
  write_u32(bytes, kSectionTable + 2U * 64U + 44U, 2U);
  write_u64(bytes, kSectionTable + 3U * 64U + 24U, kRelocationSymbolNamesOffset + 24U);
  write_u64(bytes, kSectionTable + 4U * 64U + 24U, 200U);
  write_u64(bytes, kSectionTable + 5U * 64U + 24U, 152U);
  return bytes;
}

bool expect(bool condition, std::string_view message) {
  if (condition)
    return true;
  std::cerr << "freestanding-support-test: " << message << '\n';
  return false;
}

} // namespace

int main() {
  bool ok = true;
  const std::vector<std::uint8_t> empty;
  ok &= expect(nebula::cli::sha256_hex(empty) ==
                 "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
               "SHA-256 empty vector mismatch");
  const std::vector<std::uint8_t> abc = {'a', 'b', 'c'};
  ok &= expect(nebula::cli::sha256_hex(abc) ==
                 "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
               "SHA-256 abc vector mismatch");
  auto expect_a_digest = [&](std::size_t length, std::string_view expected) {
    const std::vector<std::uint8_t> input(length, static_cast<std::uint8_t>('a'));
    ok &= expect(nebula::cli::sha256_hex(input) == expected,
                 "SHA-256 repeated-a boundary vector mismatch");
  };
  expect_a_digest(55U, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
  expect_a_digest(56U, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
  expect_a_digest(63U, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34");
  expect_a_digest(64U, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
  expect_a_digest(65U, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0");
  expect_a_digest(1'000'000U, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

  const auto valid = minimal_valid_object();
  ok &= expect(nebula::cli::inspect_freestanding_elf64_x86_64(valid).ok(),
               "minimal valid ELF object was rejected");

  std::vector<std::uint8_t> truncated(63U, 0U);
  auto result = nebula::cli::inspect_freestanding_elf64_x86_64(truncated);
  ok &= expect(!result.ok() && result.reason == "truncated-header",
               "truncated ELF was not rejected with a stable reason");

  auto wrong_machine = valid;
  write_u16(wrong_machine, 18U, 183U);
  result = nebula::cli::inspect_freestanding_elf64_x86_64(wrong_machine);
  ok &= expect(!result.ok() && result.reason == "target", "wrong ELF machine was not rejected");

  auto noncanonical_ident = valid;
  noncanonical_ident[7U] = 3U;
  result = nebula::cli::inspect_freestanding_elf64_x86_64(noncanonical_ident);
  ok &= expect(!result.ok() && result.reason == "ident", "noncanonical ELF OSABI was not rejected");

  auto undefined_symbol = valid;
  write_u16(undefined_symbol, 102U, 0U);
  result = nebula::cli::inspect_freestanding_elf64_x86_64(undefined_symbol);
  ok &= expect(!result.ok() && result.reason == "undefined-symbol",
               "undefined symbol was not rejected");

  auto control_name_symbol = undefined_symbol;
  control_name_symbol[kMinimalSymbolNamesOffset + 1U] = '\n';
  result = nebula::cli::inspect_freestanding_elf64_x86_64(control_name_symbol);
  ok &= expect(!result.ok() && result.reason == "undefined-symbol" &&
                 result.detail.find("\\x0a") != std::string::npos &&
                 result.detail.find('\n') == std::string::npos,
               "control byte in ELF symbol name was not escaped in diagnostics");

  auto out_of_bounds_section = valid;
  write_u64(out_of_bounds_section, kMinimalSectionTable + 64U + 24U,
            out_of_bounds_section.size() + 1U);
  result = nebula::cli::inspect_freestanding_elf64_x86_64(out_of_bounds_section);
  ok &= expect(!result.ok() && result.reason == "section-bounds",
               "out-of-bounds section was not rejected");

  auto oversized_nobits_section = valid;
  write_u32(oversized_nobits_section, kMinimalSectionTable + 64U + 4U, 8U);
  write_u64(oversized_nobits_section, kMinimalSectionTable + 64U + 32U,
            nebula::cli::kMaxFreestandingObjectBytes + 1U);
  result = nebula::cli::inspect_freestanding_elf64_x86_64(oversized_nobits_section);
  ok &= expect(!result.ok() && result.reason == "section-size",
               "oversized logical SHT_NOBITS section was not rejected");

  auto writable_executable_section = valid;
  write_u64(writable_executable_section, kMinimalSectionTable + 64U + 8U, 0x7U);
  result = nebula::cli::inspect_freestanding_elf64_x86_64(writable_executable_section);
  ok &= expect(!result.ok() && result.reason == "section-permissions",
               "writable executable section was not rejected");

  auto out_of_bounds_entry = valid;
  write_u64(out_of_bounds_entry, 104U, 1U);
  result = nebula::cli::inspect_freestanding_elf64_x86_64(out_of_bounds_entry);
  ok &= expect(!result.ok() && result.reason == "symbol-bounds",
               "out-of-bounds payload entry range was not rejected");

  auto reserved_symbol_other = valid;
  reserved_symbol_other[101U] = 0x80U;
  result = nebula::cli::inspect_freestanding_elf64_x86_64(reserved_symbol_other);
  ok &= expect(!result.ok() && result.reason == "symbol-other",
               "reserved symbol visibility bits were not rejected");

  auto invalid_symbol_binding = valid;
  invalid_symbol_binding[100U] = 0xa2U;
  result = nebula::cli::inspect_freestanding_elf64_x86_64(invalid_symbol_binding);
  ok &= expect(!result.ok() && result.reason == "symbol-binding",
               "reserved symbol binding was not rejected");

  auto dynamic_section = valid;
  write_u32(dynamic_section, kMinimalSectionTable + 64U + 4U, 6U);
  result = nebula::cli::inspect_freestanding_elf64_x86_64(dynamic_section);
  ok &= expect(!result.ok() && result.reason == "forbidden-section-type",
               "dynamic section was not rejected");

  auto missing_entry = valid;
  missing_entry[kMinimalSymbolNamesOffset + 1U] = 'x';
  result = nebula::cli::inspect_freestanding_elf64_x86_64(missing_entry);
  ok &= expect(!result.ok() &&
                 (result.reason == "global-symbol" || result.reason == "payload-entry-symbol"),
               "missing payload entry was not rejected");

  auto image_entry_owned_by_payload = valid;
  const std::string symbol_names = payload_symbol_names();
  std::fill_n(image_entry_owned_by_payload.begin() + kMinimalSymbolNamesOffset, symbol_names.size(),
              0U);
  constexpr std::string_view kImageEntrySymbol("_start\0", 7U);
  std::copy(kImageEntrySymbol.begin(), kImageEntrySymbol.end(),
            image_entry_owned_by_payload.begin() + kMinimalSymbolNamesOffset + 1U);
  result = nebula::cli::inspect_freestanding_elf64_x86_64(image_entry_owned_by_payload);
  ok &= expect(!result.ok() && result.reason == "global-symbol",
               "payload object was allowed to own the future image _start symbol");

  auto weak_payload_entry = valid;
  weak_payload_entry[100U] = 0x22U;
  result = nebula::cli::inspect_freestanding_elf64_x86_64(weak_payload_entry);
  ok &= expect(!result.ok() && result.reason == "payload-entry-symbol",
               "weak payload entry was not rejected");

  auto unterminated_symbol_name = valid;
  unterminated_symbol_name.back() = 'a';
  result = nebula::cli::inspect_freestanding_elf64_x86_64(unterminated_symbol_name);
  ok &= expect(!result.ok() && result.reason == "symbol-name",
               "unterminated symbol name was not rejected by the bounded reader");

  const auto valid_relocation = minimal_valid_object_with_relocation();
  ok &= expect(nebula::cli::inspect_freestanding_elf64_x86_64(valid_relocation).ok(),
               "valid bounded PC32 relocation was rejected");

  auto invalid_entry_section = valid_relocation;
  write_u32(invalid_entry_section, kRelocationSectionTable + 64U + 4U, 3U);
  write_u32(invalid_entry_section, kRelocationSectionTable + 4U * 64U + 4U, 1U);
  write_u64(invalid_entry_section, kRelocationSectionTable + 4U * 64U + 8U, 0x6U);
  result = nebula::cli::inspect_freestanding_elf64_x86_64(invalid_entry_section);
  ok &= expect(!result.ok() && result.reason == "payload-entry-symbol",
               "payload entry in a non-PROGBITS section was not rejected");

  auto wide_relocation = valid_relocation;
  write_u64(wide_relocation, 184U, (static_cast<std::uint64_t>(1U) << 32U) | 1U);
  write_u64(wide_relocation, 176U, 1U);
  result = nebula::cli::inspect_freestanding_elf64_x86_64(wide_relocation);
  ok &= expect(!result.ok() && result.reason == "relocation-offset",
               "out-of-bounds relocation write width was not rejected");

  auto invalid_relocation_target = valid_relocation;
  write_u32(invalid_relocation_target, kRelocationSectionTable + 4U * 64U + 44U, 3U);
  result = nebula::cli::inspect_freestanding_elf64_x86_64(invalid_relocation_target);
  ok &= expect(!result.ok() && result.reason == "relocation-target",
               "relocation targeting a string table was not rejected");

  auto null_relocation_symbol = valid_relocation;
  write_u64(null_relocation_symbol, 184U, 2U);
  result = nebula::cli::inspect_freestanding_elf64_x86_64(null_relocation_symbol);
  ok &= expect(!result.ok() && result.reason == "relocation-symbol",
               "non-NONE relocation using the null symbol was not rejected");

  const auto metadata_relocation_symbol = object_with_relocation_to_metadata_symbol();
  result = nebula::cli::inspect_freestanding_elf64_x86_64(metadata_relocation_symbol);
  ok &= expect(!result.ok() && result.reason == "relocation-symbol-section",
               "relocation using a metadata-section symbol was not rejected");

  auto unsupported_relocation = valid_relocation;
  write_u64(unsupported_relocation, 184U, (static_cast<std::uint64_t>(1U) << 32U) | 8U);
  result = nebula::cli::inspect_freestanding_elf64_x86_64(unsupported_relocation);
  ok &= expect(!result.ok() && result.reason == "relocation-type",
               "dynamic-only relocation type was not rejected");

  // A bounded mutation corpus: every single-byte mutation must produce either
  // a valid result or a structured rejection, never an empty error/crash.
  for (std::size_t index = 0; index < valid.size(); ++index) {
    auto mutated = valid;
    mutated[index] ^= 0xffU;
    result = nebula::cli::inspect_freestanding_elf64_x86_64(mutated);
    ok &= expect(result.ok() || !result.reason.empty(),
                 "mutation produced an unstructured inspector failure");
    if (!ok)
      break;
  }

  return ok ? 0 : 1;
}
