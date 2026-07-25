cmake_minimum_required(VERSION 3.20)

project(nebula_protocol_install_snapshot_fixture LANGUAGES CXX)

foreach(required_variable IN ITEMS
    NEBULA_PROTOCOL_SNAPSHOT_MODULE
    NEBULA_PROTOCOL_INSTALL_VERIFY_TEMPLATE
    NEBULA_PROTOCOL_CONTRACT_SOURCE
    NEBULA_PROTOCOL_VALIDATOR_MAIN_SOURCE
    NEBULA_PROTOCOL_CONFIGURE_VALIDATOR_SOURCE)
  if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
    message(FATAL_ERROR "fixture requires ${required_variable}")
  endif()
endforeach()

add_executable(fixture-protocol-validator
  "${NEBULA_PROTOCOL_VALIDATOR_MAIN_SOURCE}"
  "${NEBULA_PROTOCOL_CONTRACT_SOURCE}"
)
set_target_properties(fixture-protocol-validator PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

include("${NEBULA_PROTOCOL_SNAPSHOT_MODULE}")
nebula_prepare_protocol_asset_snapshot(
  PREFIX FIXTURE_PROTOCOL
  MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/boot/uos-x86_64-limine-v1/contract.manifest"
  SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}"
  BINARY_ROOT "${CMAKE_CURRENT_BINARY_DIR}"
  SNAPSHOT_ROOT
    "${CMAKE_CURRENT_BINARY_DIR}/verified-inputs/boot/uos-x86_64-limine-v1"
  CONFIGURE_VALIDATOR_SOURCE "${NEBULA_PROTOCOL_CONFIGURE_VALIDATOR_SOURCE}"
  INSTALL_VALIDATOR "$<TARGET_FILE:fixture-protocol-validator>"
  INSTALL_VERIFY_TEMPLATE "${NEBULA_PROTOCOL_INSTALL_VERIFY_TEMPLATE}"
)

install(SCRIPT "${FIXTURE_PROTOCOL_INSTALL_VERIFY_SCRIPT}")
