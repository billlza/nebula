// CMake try_run accepts one source file in the minimum supported CMake
// version. Include the production codec and validator entry point so configure
// validation executes those exact implementations rather than a second schema.
#include "protocol_abi_contract.cpp"
#include "protocol_abi_contract_validator_main.cpp"
