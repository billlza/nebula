#include "protocol_abi_contract.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

namespace fs = std::filesystem;

int validate_manifest(const fs::path &path) {
  std::error_code error;
  const std::uintmax_t size = fs::file_size(path, error);
  if (error) {
    std::cerr << "protocol-abi-validator: cannot inspect manifest: " << error.message() << '\n';
    return 2;
  }
  if (size > nebula::boot::kProtocolAbiManifestMaxBytes) {
    std::cerr << "protocol-abi-validator: manifest exceeds "
              << nebula::boot::kProtocolAbiManifestMaxBytes << " bytes\n";
    return 2;
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    std::cerr << "protocol-abi-validator: cannot open manifest\n";
    return 2;
  }
  std::string payload(static_cast<std::size_t>(size), '\0');
  input.read(payload.data(), static_cast<std::streamsize>(payload.size()));
  if (input.gcount() != static_cast<std::streamsize>(payload.size())) {
    std::cerr << "protocol-abi-validator: manifest changed or became unreadable while reading\n";
    return 2;
  }
  char trailing = 0;
  if (input.get(trailing) || !input.eof()) {
    std::cerr << "protocol-abi-validator: manifest changed while reading\n";
    return 2;
  }

  const nebula::boot::ProtocolAbiContractResult parsed =
    nebula::boot::parse_protocol_abi_contract(payload);
  if (!parsed.ok()) {
    std::cerr << "protocol-abi-validator: canonical contract rejected";
    if (parsed.error.line != 0U)
      std::cerr << " at line " << parsed.error.line;
    if (!parsed.error.field.empty())
      std::cerr << " field '" << parsed.error.field << "'";
    if (!parsed.error.detail.empty())
      std::cerr << ": " << parsed.error.detail;
    std::cerr << '\n';
    return 1;
  }

  const nebula::boot::ProtocolAbiContractSerializationResult serialized =
    nebula::boot::serialize_protocol_abi_contract(*parsed.value);
  if (!serialized.ok() || *serialized.payload != payload) {
    std::cerr << "protocol-abi-validator: canonical contract did not replay to exact input bytes\n";
    return 1;
  }
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: nebula-protocol-abi-validator <contract.manifest>\n";
    return 2;
  }
  return validate_manifest(fs::path(argv[1]));
}
