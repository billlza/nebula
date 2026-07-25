#include "cli/elf_object.hpp"

#include "boot/protocol_abi_contract.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nebula::cli {
namespace {

constexpr std::size_t kElfHeaderSize = 64U;
constexpr std::size_t kSectionHeaderSize = 64U;
constexpr std::size_t kSymbolSize = 24U;
constexpr std::size_t kRelaSize = 24U;
constexpr std::size_t kRelSize = 16U;
constexpr std::uint16_t kEtRel = 1U;
constexpr std::uint16_t kMachineX86_64 = 62U;
constexpr std::uint16_t kShnUndef = 0U;
constexpr std::uint16_t kShnAbs = 0xfff1U;
constexpr std::uint16_t kShnXIndex = 0xffffU;
constexpr std::uint32_t kShtNull = 0U;
constexpr std::uint32_t kShtProgBits = 1U;
constexpr std::uint32_t kShtSymtab = 2U;
constexpr std::uint32_t kShtStrtab = 3U;
constexpr std::uint32_t kShtRela = 4U;
constexpr std::uint32_t kShtDynamic = 6U;
constexpr std::uint32_t kShtNoBits = 8U;
constexpr std::uint32_t kShtRel = 9U;
constexpr std::uint32_t kShtDynsym = 11U;
constexpr std::uint32_t kShtInitArray = 14U;
constexpr std::uint32_t kShtFiniArray = 15U;
constexpr std::uint32_t kShtPreinitArray = 16U;
constexpr std::uint64_t kShfWrite = 0x1U;
constexpr std::uint64_t kShfAlloc = 0x2U;
constexpr std::uint64_t kShfExecInstr = 0x4U;
constexpr std::uint64_t kShfMerge = 0x10U;
constexpr std::uint64_t kShfStrings = 0x20U;
constexpr std::uint64_t kShfInfoLink = 0x40U;
constexpr std::uint64_t kShfTls = 0x400U;
constexpr std::uint64_t kAllowedSectionFlags =
  kShfWrite | kShfAlloc | kShfExecInstr | kShfMerge | kShfStrings | kShfInfoLink;
constexpr std::size_t kMaxSections = 4096U;
constexpr std::size_t kMaxSymbols = 1U << 20U;
constexpr std::size_t kMaxRelocations = 1U << 20U;
constexpr std::size_t kMaxStringScanBytes = kMaxFreestandingObjectBytes;

ElfInspectionResult success() { return {true, {}, {}}; }

ElfInspectionResult failure(std::string reason, std::string detail) {
  return {false, std::move(reason), std::move(detail)};
}

bool range_is_valid(std::size_t offset, std::size_t length, std::size_t total) {
  return offset <= total && length <= total - offset;
}

bool ranges_overlap(std::size_t lhs_offset, std::size_t lhs_size, std::size_t rhs_offset,
                    std::size_t rhs_size) {
  if (lhs_size == 0U || rhs_size == 0U)
    return false;
  return lhs_offset < rhs_offset + rhs_size && rhs_offset < lhs_offset + lhs_size;
}

std::optional<std::uint16_t> read_u16(std::span<const std::uint8_t> bytes, std::size_t offset) {
  if (!range_is_valid(offset, 2U, bytes.size()))
    return std::nullopt;
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset]) |
                                    (static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U));
}

std::optional<std::uint32_t> read_u32(std::span<const std::uint8_t> bytes, std::size_t offset) {
  if (!range_is_valid(offset, 4U, bytes.size()))
    return std::nullopt;
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
         (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
         (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

std::optional<std::uint64_t> read_u64(std::span<const std::uint8_t> bytes, std::size_t offset) {
  if (!range_is_valid(offset, 8U, bytes.size()))
    return std::nullopt;
  std::uint64_t value = 0U;
  for (unsigned index = 0; index < 8U; ++index) {
    value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8U);
  }
  return value;
}

std::optional<std::size_t> to_size(std::uint64_t value) {
  if (value > static_cast<std::uint64_t>(static_cast<std::size_t>(-1)))
    return std::nullopt;
  return static_cast<std::size_t>(value);
}

bool is_power_of_two(std::uint64_t value) { return value != 0U && (value & (value - 1U)) == 0U; }

struct Section {
  std::uint32_t name_offset = 0U;
  std::uint32_t type = 0U;
  std::uint64_t flags = 0U;
  std::uint64_t address = 0U;
  std::size_t offset = 0U;
  std::size_t size = 0U;
  std::uint32_t link = 0U;
  std::uint32_t info = 0U;
  std::uint64_t alignment = 0U;
  std::uint64_t entry_size = 0U;
  std::string_view name;
};

class StringTableReader {
public:
  StringTableReader(std::span<const std::uint8_t> bytes, const Section &table)
      : bytes_(bytes), table_(table) {}

  std::optional<std::string_view> read(std::uint32_t offset) {
    const std::size_t relative = static_cast<std::size_t>(offset);
    if (relative >= table_.size || remaining_scan_bytes_ == 0U)
      return std::nullopt;

    const std::size_t available = table_.size - relative;
    const std::size_t scan_limit = std::min(available, remaining_scan_bytes_);
    const auto *start = bytes_.data() + table_.offset + relative;
    for (std::size_t length = 0U; length < scan_limit; ++length) {
      if (start[length] != 0U)
        continue;
      remaining_scan_bytes_ -= length + 1U;
      return std::string_view(reinterpret_cast<const char *>(start), length);
    }
    remaining_scan_bytes_ -= scan_limit;
    return std::nullopt;
  }

private:
  std::span<const std::uint8_t> bytes_;
  const Section &table_;
  std::size_t remaining_scan_bytes_ = kMaxStringScanBytes;
};

bool is_forbidden_section_name(std::string_view name) {
  static const std::set<std::string_view> forbidden = {
    ".ctors",  ".dtors",         ".dynamic",    ".dynstr",
    ".dynsym", ".eh_frame",      ".fini_array", ".gcc_except_table",
    ".got",    ".got.plt",       ".init_array", ".interp",
    ".plt",    ".preinit_array", ".tbss",       ".tdata",
  };
  return forbidden.find(name) != forbidden.end();
}

bool is_allowed_section_type(std::uint32_t type) {
  switch (type) {
  case kShtNull:
  case kShtProgBits:
  case kShtSymtab:
  case kShtStrtab:
  case kShtRela:
  case kShtNoBits:
  case kShtRel:
    return true;
  default:
    return false;
  }
}

std::string escaped_elf_name(std::string_view value) {
  constexpr std::size_t kMaxNameBytes = 160U;
  constexpr char kHex[] = "0123456789abcdef";
  const std::size_t length = std::min(value.size(), kMaxNameBytes);
  std::string escaped;
  escaped.reserve(length + 16U);
  escaped.push_back('"');
  for (std::size_t index = 0; index < length; ++index) {
    const unsigned char byte = static_cast<unsigned char>(value[index]);
    if (byte == '\\' || byte == '"') {
      escaped.push_back('\\');
      escaped.push_back(static_cast<char>(byte));
    } else if (byte < 0x20U || byte >= 0x7fU) {
      escaped += "\\x";
      escaped.push_back(kHex[byte >> 4U]);
      escaped.push_back(kHex[byte & 0x0fU]);
    } else {
      escaped.push_back(static_cast<char>(byte));
    }
  }
  if (length != value.size())
    escaped += "...<truncated>";
  escaped.push_back('"');
  return escaped;
}

ElfInspectionResult parse_sections(std::span<const std::uint8_t> bytes, std::size_t section_offset,
                                   std::uint16_t section_count, std::vector<Section> &sections) {
  if (section_count == 0U || section_count > kMaxSections) {
    return failure("section-count", "ELF section count is zero or exceeds the bounded limit");
  }
  if (static_cast<std::size_t>(section_count) >
      (bytes.size() - section_offset) / kSectionHeaderSize) {
    return failure("section-bounds", "ELF section table extends beyond the object");
  }

  sections.reserve(section_count);
  std::size_t total_allocated_size = 0U;
  for (std::size_t index = 0; index < section_count; ++index) {
    const std::size_t base = section_offset + index * kSectionHeaderSize;
    const auto name = read_u32(bytes, base);
    const auto type = read_u32(bytes, base + 4U);
    const auto flags = read_u64(bytes, base + 8U);
    const auto address = read_u64(bytes, base + 16U);
    const auto raw_offset = read_u64(bytes, base + 24U);
    const auto raw_size = read_u64(bytes, base + 32U);
    const auto link = read_u32(bytes, base + 40U);
    const auto info = read_u32(bytes, base + 44U);
    const auto alignment = read_u64(bytes, base + 48U);
    const auto entry_size = read_u64(bytes, base + 56U);
    if (!name || !type || !flags || !address || !raw_offset || !raw_size || !link || !info ||
        !alignment || !entry_size) {
      return failure("section-header", "ELF section header is truncated");
    }
    const auto offset = to_size(*raw_offset);
    const auto size = to_size(*raw_size);
    if (!offset || !size) {
      return failure("section-bounds", "ELF section offset or size is not representable");
    }
    if (*size > kMaxFreestandingObjectBytes) {
      return failure("section-size", "ELF logical section size exceeds the bounded limit");
    }
    if ((*flags & kShfWrite) != 0U && (*flags & kShfExecInstr) != 0U) {
      return failure("section-permissions", "ELF section requests writable executable memory");
    }
    if ((*flags & kShfAlloc) != 0U) {
      if (*size > kMaxFreestandingObjectBytes - total_allocated_size) {
        return failure("section-size",
                       "ELF aggregate allocated section size exceeds the bounded limit");
      }
      total_allocated_size += *size;
    }
    if (*alignment != 0U && !is_power_of_two(*alignment)) {
      return failure("section-alignment", "ELF section alignment is not a power of two");
    }
    if (*type == kShtNoBits) {
      if (*offset > bytes.size()) {
        return failure("section-bounds", "SHT_NOBITS section offset exceeds the object");
      }
    } else if (!range_is_valid(*offset, *size, bytes.size())) {
      return failure("section-bounds", "ELF section contents extend beyond the object");
    }
    sections.push_back(
      {*name, *type, *flags, *address, *offset, *size, *link, *info, *alignment, *entry_size, {}});
  }

  const Section &null_section = sections.front();
  if (null_section.name_offset != 0U || null_section.type != kShtNull || null_section.flags != 0U ||
      null_section.address != 0U || null_section.offset != 0U || null_section.size != 0U ||
      null_section.link != 0U || null_section.info != 0U || null_section.alignment != 0U ||
      null_section.entry_size != 0U) {
    return failure("section-zero",
                   "ELF null section is not canonical; extended numbering is unsupported");
  }

  const std::size_t section_table_size =
    static_cast<std::size_t>(section_count) * kSectionHeaderSize;
  for (std::size_t index = 1U; index < sections.size(); ++index) {
    const Section &section = sections[index];
    if (section.type == kShtNoBits || section.size == 0U)
      continue;
    if (section.offset < kElfHeaderSize ||
        ranges_overlap(section.offset, section.size, section_offset, section_table_size)) {
      return failure("section-overlap",
                     "ELF section contents overlap the ELF header or section table");
    }
    if (section.alignment > 1U && section.offset % section.alignment != 0U) {
      return failure("section-alignment", "ELF section file offset violates its alignment");
    }
    for (std::size_t other_index = index + 1U; other_index < sections.size(); ++other_index) {
      const Section &other = sections[other_index];
      if (other.type == kShtNoBits || other.size == 0U)
        continue;
      if (ranges_overlap(section.offset, section.size, other.offset, other.size)) {
        return failure("section-overlap", "ELF file-backed sections overlap each other");
      }
    }
  }
  return success();
}

ElfInspectionResult name_and_validate_sections(std::span<const std::uint8_t> bytes,
                                               std::uint16_t string_table_index,
                                               std::vector<Section> &sections) {
  if (string_table_index == 0U || string_table_index == kShnXIndex ||
      string_table_index >= sections.size()) {
    return failure("section-name-table", "ELF section-name string table index is invalid");
  }
  const Section &names = sections[string_table_index];
  if (names.type != kShtStrtab || names.size == 0U || bytes[names.offset] != 0U) {
    return failure("section-name-table", "ELF section-name table is not a canonical string table");
  }

  bool has_executable_code = false;
  StringTableReader name_reader(bytes, names);
  for (std::size_t index = 0; index < sections.size(); ++index) {
    auto name = name_reader.read(sections[index].name_offset);
    if (!name.has_value()) {
      return failure(
        "section-name",
        "ELF section name is out of range, overlong, unterminated, or exceeds the scan budget");
    }
    sections[index].name = *name;

    const Section &section = sections[index];
    if ((section.flags & kShfTls) != 0U || is_forbidden_section_name(section.name)) {
      return failure("forbidden-section",
                     "ELF contains forbidden section " + escaped_elf_name(section.name));
    }
    if (section.type == kShtDynamic || section.type == kShtDynsym ||
        section.type == kShtInitArray || section.type == kShtFiniArray ||
        section.type == kShtPreinitArray) {
      return failure("forbidden-section-type",
                     "ELF contains dynamic, TLS, or initializer section type");
    }
    if (!is_allowed_section_type(section.type)) {
      return failure("section-type", "ELF contains an unsupported section type in " +
                                       escaped_elf_name(section.name));
    }
    if (section.address != 0U) {
      return failure("section-address", "relocatable ELF section has a nonzero address");
    }
    if ((section.flags & ~kAllowedSectionFlags) != 0U) {
      return failure("section-flags", "ELF section contains unsupported flags");
    }
    if (section.type == kShtProgBits && (section.flags & kShfExecInstr) != 0U &&
        section.size != 0U) {
      has_executable_code = true;
    }
  }
  if (!has_executable_code) {
    return failure("executable-section", "ELF has no non-empty executable code section");
  }
  return success();
}

struct SymbolTableInfo {
  std::size_t section_index = 0U;
  std::size_t symbol_count = 0U;
  std::vector<std::uint16_t> symbol_sections;
};

ElfInspectionResult validate_symbols(std::span<const std::uint8_t> bytes,
                                     const std::vector<Section> &sections,
                                     SymbolTableInfo &symtab_info) {
  std::optional<std::size_t> symtab_index;
  for (std::size_t index = 0; index < sections.size(); ++index) {
    if (sections[index].type != kShtSymtab)
      continue;
    if (symtab_index.has_value()) {
      return failure("symtab-count", "ELF contains more than one static symbol table");
    }
    symtab_index = index;
  }
  if (!symtab_index.has_value()) {
    return failure("symtab-missing", "ELF has no static symbol table");
  }

  const Section &symbols = sections[*symtab_index];
  if (symbols.entry_size != kSymbolSize || symbols.size == 0U || symbols.size % kSymbolSize != 0U) {
    return failure("symtab-shape", "ELF symbol table entry size or total size is invalid");
  }
  const std::size_t count = symbols.size / kSymbolSize;
  if (count > kMaxSymbols || symbols.info == 0U || symbols.info > count) {
    return failure("symtab-count", "ELF symbol count or local-symbol boundary is invalid");
  }
  if (symbols.link == 0U || symbols.link >= sections.size() ||
      sections[symbols.link].type != kShtStrtab) {
    return failure("symtab-string-table", "ELF symbol table does not link to a valid string table");
  }
  const Section &strings = sections[symbols.link];
  if (strings.size == 0U || bytes[strings.offset] != 0U) {
    return failure("symtab-string-table", "ELF symbol string table is not canonical");
  }

  std::size_t payload_entry_count = 0U;
  std::vector<std::uint16_t> symbol_sections;
  symbol_sections.reserve(count);
  StringTableReader name_reader(bytes, strings);
  for (std::size_t index = 0; index < count; ++index) {
    const std::size_t base = symbols.offset + index * kSymbolSize;
    const auto name_offset = read_u32(bytes, base);
    const std::uint8_t info = bytes[base + 4U];
    const std::uint8_t other = bytes[base + 5U];
    const auto section_index = read_u16(bytes, base + 6U);
    const auto value = read_u64(bytes, base + 8U);
    const auto size = read_u64(bytes, base + 16U);
    if (!name_offset || !section_index || !value || !size) {
      return failure("symbol-header", "ELF symbol entry is truncated");
    }
    symbol_sections.push_back(*section_index);
    auto name = name_reader.read(*name_offset);
    if (!name.has_value()) {
      return failure(
        "symbol-name",
        "ELF symbol name is out of range, overlong, unterminated, or exceeds the scan budget");
    }
    if (index == 0U) {
      if (*name_offset != 0U || info != 0U || other != 0U || *section_index != 0U || *value != 0U ||
          *size != 0U) {
        return failure("symbol-zero", "ELF null symbol is not canonical");
      }
      continue;
    }
    if (*section_index == kShnUndef) {
      return failure("undefined-symbol",
                     name->empty() ? "ELF contains an unnamed undefined symbol"
                                   : "ELF contains undefined symbol " + escaped_elf_name(*name));
    }
    if (*section_index != kShnAbs && *section_index >= sections.size()) {
      return failure("symbol-section", "ELF symbol references an invalid section index");
    }

    const std::uint8_t binding = static_cast<std::uint8_t>(info >> 4U);
    const std::uint8_t type = static_cast<std::uint8_t>(info & 0x0fU);
    const std::uint8_t visibility = static_cast<std::uint8_t>(other & 0x03U);
    if ((other & 0xfcU) != 0U) {
      return failure("symbol-other", "ELF symbol has nonzero reserved visibility bits");
    }
    if (type > 4U) {
      return failure("symbol-type", "ELF symbol uses an unsupported symbol type");
    }
    const bool is_local_index = index < symbols.info;
    if ((binding == 0U) != is_local_index || binding > 2U) {
      return failure("symbol-binding",
                     "ELF symbol binding disagrees with the local-symbol boundary");
    }
    if (*section_index != kShnAbs) {
      const auto symbol_value = to_size(*value);
      const auto symbol_size = to_size(*size);
      if (!symbol_value || !symbol_size) {
        return failure("symbol-bounds", "ELF symbol value or size is not representable");
      }
      const Section &owning_section = sections[*section_index];
      if (*symbol_value > owning_section.size ||
          *symbol_size > owning_section.size - *symbol_value) {
        return failure("symbol-bounds", "ELF symbol range extends beyond its defining section");
      }
    }
    if (*name == nebula::boot::kUosX86_64PayloadEntrySymbol) {
      ++payload_entry_count;
      const bool has_entry_section = *section_index != kShnAbs && *section_index < sections.size();
      const Section *entry_section = has_entry_section ? &sections[*section_index] : nullptr;
      if (binding != 1U || type != 2U || visibility != 0U || entry_section == nullptr ||
          entry_section->type != kShtProgBits ||
          (entry_section->flags & (kShfAlloc | kShfExecInstr)) != (kShfAlloc | kShfExecInstr) ||
          (entry_section->flags & kShfWrite) != 0U || *size == 0U) {
        return failure("payload-entry-symbol",
                       std::string(nebula::boot::kUosX86_64PayloadEntrySymbol) +
                         " is not one global default-visible function with code");
      }
    } else if (binding == 1U || binding == 2U) {
      return failure("global-symbol",
                     "ELF exposes unexpected global or weak symbol " + escaped_elf_name(*name));
    }
  }
  if (payload_entry_count != 1U) {
    return failure("payload-entry-symbol",
                   "ELF must define exactly one global function named " +
                     std::string(nebula::boot::kUosX86_64PayloadEntrySymbol));
  }

  symtab_info = {*symtab_index, count, std::move(symbol_sections)};
  return success();
}

std::optional<std::size_t> relocation_width(std::uint32_t type) {
  switch (type) {
  case 0U:
    return 0U; // R_X86_64_NONE
  case 1U:
    return 8U; // R_X86_64_64
  case 2U:
    return 4U; // R_X86_64_PC32
  case 4U:
    return 4U; // R_X86_64_PLT32
  case 10U:
    return 4U; // R_X86_64_32
  case 11U:
    return 4U; // R_X86_64_32S
  case 12U:
    return 2U; // R_X86_64_16
  case 13U:
    return 2U; // R_X86_64_PC16
  case 14U:
    return 1U; // R_X86_64_8
  case 15U:
    return 1U; // R_X86_64_PC8
  case 24U:
    return 8U; // R_X86_64_PC64
  default:
    return std::nullopt;
  }
}

ElfInspectionResult validate_relocations(std::span<const std::uint8_t> bytes,
                                         const std::vector<Section> &sections,
                                         const SymbolTableInfo &symtab) {
  for (const Section &relocation : sections) {
    if (relocation.type != kShtRela && relocation.type != kShtRel)
      continue;
    const std::size_t required_entry_size = relocation.type == kShtRela ? kRelaSize : kRelSize;
    if (relocation.entry_size != required_entry_size ||
        relocation.size % required_entry_size != 0U) {
      return failure("relocation-shape", "ELF relocation section has an invalid entry size");
    }
    const std::size_t count = relocation.size / required_entry_size;
    if (count > kMaxRelocations) {
      return failure("relocation-count", "ELF relocation count exceeds the bounded limit");
    }
    if (relocation.link != symtab.section_index || relocation.info == 0U ||
        relocation.info >= sections.size()) {
      return failure("relocation-link",
                     "ELF relocation section links to an invalid table or target");
    }
    const Section &target = sections[relocation.info];
    if (target.type != kShtProgBits || (target.flags & kShfAlloc) == 0U) {
      return failure("relocation-target",
                     "ELF relocation target is not an allocatable PROGBITS section");
    }
    for (std::size_t index = 0; index < count; ++index) {
      const std::size_t base = relocation.offset + index * required_entry_size;
      const auto offset = read_u64(bytes, base);
      const auto info = read_u64(bytes, base + 8U);
      if (!offset || !info) {
        return failure("relocation-header", "ELF relocation entry is truncated");
      }
      const std::uint32_t type = static_cast<std::uint32_t>(*info & 0xffffffffU);
      const auto width = relocation_width(type);
      if (!width.has_value()) {
        return failure("relocation-type", "ELF relocation uses an unsupported x86_64 type");
      }
      const auto relocation_offset = to_size(*offset);
      if (!relocation_offset || *relocation_offset > target.size ||
          *width > target.size - *relocation_offset) {
        return failure("relocation-offset",
                       "ELF relocation write extends beyond its target section");
      }
      const std::uint64_t symbol_index = *info >> 32U;
      if (symbol_index >= symtab.symbol_count || (type != 0U && symbol_index == 0U)) {
        return failure("relocation-symbol",
                       "ELF relocation references an invalid or null symbol index");
      }
      if (type != 0U) {
        const std::uint16_t symbol_section = symtab.symbol_sections[symbol_index];
        if (symbol_section != kShnAbs && (symbol_section >= sections.size() ||
                                          (sections[symbol_section].type != kShtProgBits &&
                                           sections[symbol_section].type != kShtNoBits) ||
                                          (sections[symbol_section].flags & kShfAlloc) == 0U)) {
          return failure("relocation-symbol-section",
                         "ELF relocation references a symbol outside allocatable data or code");
        }
      }
    }
  }
  return success();
}

} // namespace

ElfInspectionResult inspect_freestanding_elf64_x86_64(std::span<const std::uint8_t> bytes) {
  if (bytes.size() > kMaxFreestandingObjectBytes) {
    return failure("file-too-large", "ELF object exceeds the 64 MiB inspection limit");
  }
  if (bytes.size() < kElfHeaderSize) {
    return failure("truncated-header", "ELF object is shorter than the 64-byte ELF64 header");
  }
  if (bytes[0] != 0x7fU || bytes[1] != 'E' || bytes[2] != 'L' || bytes[3] != 'F') {
    return failure("magic", "object does not have ELF magic");
  }
  if (bytes[4] != 2U || bytes[5] != 1U || bytes[6] != 1U) {
    return failure("ident", "object must be ELF64, little-endian, current-version ELF");
  }
  if (std::any_of(bytes.begin() + 7, bytes.begin() + 16,
                  [](std::uint8_t byte) { return byte != 0U; })) {
    return failure("ident", "object must use ELFOSABI_NONE, ABI version zero, and zero padding");
  }

  const auto type = read_u16(bytes, 16U);
  const auto machine = read_u16(bytes, 18U);
  const auto version = read_u32(bytes, 20U);
  const auto entry = read_u64(bytes, 24U);
  const auto program_offset = read_u64(bytes, 32U);
  const auto raw_section_offset = read_u64(bytes, 40U);
  const auto flags = read_u32(bytes, 48U);
  const auto header_size = read_u16(bytes, 52U);
  const auto program_entry_size = read_u16(bytes, 54U);
  const auto program_count = read_u16(bytes, 56U);
  const auto section_entry_size = read_u16(bytes, 58U);
  const auto section_count = read_u16(bytes, 60U);
  const auto section_name_index = read_u16(bytes, 62U);
  if (!type || !machine || !version || !entry || !program_offset || !raw_section_offset || !flags ||
      !header_size || !program_entry_size || !program_count || !section_entry_size ||
      !section_count || !section_name_index) {
    return failure("truncated-header", "ELF64 header fields are truncated");
  }
  if (*type != kEtRel || *machine != kMachineX86_64 || *version != 1U) {
    return failure("target", "object must be current-version ET_REL for EM_X86_64");
  }
  if (*entry != 0U || *program_offset != 0U || *program_count != 0U || *program_entry_size != 0U) {
    return failure("program-header",
                   "relocatable object must have no entry address or program headers");
  }
  if (*flags != 0U || *header_size != kElfHeaderSize || *section_entry_size != kSectionHeaderSize) {
    return failure("header-shape", "ELF64 header size, flags, or section-entry size is invalid");
  }
  const auto section_offset = to_size(*raw_section_offset);
  if (!section_offset || *section_offset < kElfHeaderSize || *section_offset > bytes.size() ||
      *section_offset % 8U != 0U) {
    return failure("section-bounds", "ELF section table offset is invalid");
  }

  std::vector<Section> sections;
  auto result = parse_sections(bytes, *section_offset, *section_count, sections);
  if (!result.ok())
    return result;
  result = name_and_validate_sections(bytes, *section_name_index, sections);
  if (!result.ok())
    return result;

  SymbolTableInfo symtab;
  result = validate_symbols(bytes, sections, symtab);
  if (!result.ok())
    return result;
  return validate_relocations(bytes, sections, symtab);
}

} // namespace nebula::cli
