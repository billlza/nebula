cmake_minimum_required(VERSION 3.20)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" NEBULA_VERSION LIMIT_COUNT 1)
string(STRIP "${NEBULA_VERSION}" NEBULA_VERSION)
if(NOT NEBULA_VERSION MATCHES "^([0-9]+\\.[0-9]+\\.[0-9]+)(-[0-9A-Za-z.-]+)?$")
  message(FATAL_ERROR "VERSION must be a semantic version like 1.0.0 or 1.1.0-dev")
endif()
set(NEBULA_PROJECT_VERSION "${CMAKE_MATCH_1}")

project(nebula VERSION ${NEBULA_PROJECT_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(NEBULA_STRICT "Enable strict compiler warnings" OFF)
option(NEBULA_WERROR "Treat warnings as errors (for strict CI/release gates)" OFF)

# Provenance validation hashes the compiler, its direct tools, generated
# artifacts, and protected inputs repeatedly by design. Keep the small,
# deterministic digest translation unit optimized in diagnostic builds so
# fail-closed validation remains usable without weakening any check. Release,
# RelWithDebInfo, and MinSizeRel retain their own optimization policies.
# MSVC's Debug runtime checks conflict with /O2, so this targeted override is
# limited to GNU-style Clang/GCC frontends until an MSVC lane proves an
# equivalent configuration.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
  if(CMAKE_CONFIGURATION_TYPES)
    set_source_files_properties(cli/artifact_digest.cpp PROPERTIES
      COMPILE_OPTIONS "$<$<CONFIG:Debug>:-O2>"
    )
  elseif(CMAKE_BUILD_TYPE STREQUAL "" OR CMAKE_BUILD_TYPE STREQUAL "Debug")
    set_source_files_properties(cli/artifact_digest.cpp PROPERTIES COMPILE_OPTIONS "-O2")
  endif()
endif()

include(CheckCXXSourceCompiles)
if(APPLE)
  check_cxx_source_compiles([=[
    #include <spawn.h>
    int main() {
      posix_spawnattr_t attributes{};
      return posix_spawnattr_setflags(
        &attributes, static_cast<short>(POSIX_SPAWN_CLOEXEC_DEFAULT));
    }
  ]=] NEBULA_HAVE_POSIX_SPAWN_CLOEXEC_DEFAULT)
  check_cxx_source_compiles([=[
    #include <spawn.h>
    int main() {
      posix_spawn_file_actions_t actions{};
      return posix_spawn_file_actions_addinherit_np(&actions, 0);
    }
  ]=] NEBULA_HAVE_POSIX_SPAWN_FILE_ACTIONS_ADDINHERIT_NP)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  check_cxx_source_compiles([=[
    #define _GNU_SOURCE 1
    #include <spawn.h>
    int main() {
      posix_spawn_file_actions_t actions{};
      return posix_spawn_file_actions_addclosefrom_np(&actions, 3);
    }
  ]=] NEBULA_HAVE_POSIX_SPAWN_FILE_ACTIONS_ADDCLOSEFROM_NP)
endif()
foreach(capability IN ITEMS
    NEBULA_HAVE_POSIX_SPAWN_CLOEXEC_DEFAULT
    NEBULA_HAVE_POSIX_SPAWN_FILE_ACTIONS_ADDINHERIT_NP
    NEBULA_HAVE_POSIX_SPAWN_FILE_ACTIONS_ADDCLOSEFROM_NP)
  if(NOT DEFINED ${capability})
    set(${capability} 0)
  endif()
endforeach()

set(NEBULA_CLI_SOURCES
  cli/main.cpp
  cli/json.cpp
  cli/parse.cpp
  cli/build_run.cpp
  cli/freestanding_object_system.cpp
  cli/project.cpp
  cli/tooling.cpp
  codegen/cpp_backend.cpp
  codegen/freestanding_cpp_emitter.cpp
  codegen/symbol_names.cpp
  frontend/lexer.cpp
  frontend/parser.cpp
  frontend/typecheck.cpp
  nir/lower.cpp
  nir/cfg.cpp
  nir/cfg_ir.cpp
  passes/async_explain.cpp
  passes/call_target_resolver.cpp
  passes/escape_analysis.cpp
  passes/borrow_xstmt.cpp
  passes/rep_owner_infer.cpp
  passes/epistemic_lint.cpp
  passes/dead_code_elim.cpp
)

add_executable(nebula
  cli/dispatch.cpp
  ${NEBULA_CLI_SOURCES}
)

set(NEBULA_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
set(NEBULA_GENERATED_STD_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/std")
file(MAKE_DIRECTORY "${NEBULA_GENERATED_INCLUDE_DIR}")
file(MAKE_DIRECTORY "${NEBULA_GENERATED_STD_DIR}")
file(MAKE_DIRECTORY "${NEBULA_GENERATED_INCLUDE_DIR}/boot")
file(MAKE_DIRECTORY "${NEBULA_GENERATED_INCLUDE_DIR}/cli")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cli/host_process_capabilities.hpp.in"
  "${NEBULA_GENERATED_INCLUDE_DIR}/cli/host_process_capabilities.hpp"
)
set(NEBULA_PROTOCOL_ABI_MANIFEST
  "${CMAKE_CURRENT_SOURCE_DIR}/boot/uos-x86_64-limine-v1/contract.manifest"
)
include("${CMAKE_CURRENT_SOURCE_DIR}/boot/prepare_protocol_asset_snapshot.cmake")
nebula_prepare_protocol_asset_snapshot(
  PREFIX NEBULA_PROTOCOL_ABI
  MANIFEST "${NEBULA_PROTOCOL_ABI_MANIFEST}"
  SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}"
  BINARY_ROOT "${CMAKE_CURRENT_BINARY_DIR}"
  SNAPSHOT_ROOT
    "${CMAKE_CURRENT_BINARY_DIR}/verified-inputs/boot/uos-x86_64-limine-v1"
  CONFIGURE_VALIDATOR_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/boot/protocol_abi_contract_configure_validator.cpp"
  INSTALL_VALIDATOR "$<TARGET_FILE:nebula-protocol-abi-validator>"
  INSTALL_VERIFY_TEMPLATE
    "${CMAKE_CURRENT_SOURCE_DIR}/boot/protocol_asset_install_verify.cmake.in"
)
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/boot/protocol_abi_identity.hpp.in"
  "${NEBULA_GENERATED_INCLUDE_DIR}/boot/protocol_abi_identity.hpp"
)
file(GLOB_RECURSE NEBULA_HEADER_FILES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/boot/*.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/cli/*.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/codegen/*.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/frontend/*.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/nir/*.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/passes/*.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/runtime/*.hpp"
)
foreach(header IN LISTS NEBULA_HEADER_FILES)
  file(RELATIVE_PATH rel "${CMAKE_CURRENT_SOURCE_DIR}" "${header}")
  set(dst "${NEBULA_GENERATED_INCLUDE_DIR}/${rel}")
  get_filename_component(dst_dir "${dst}" DIRECTORY)
  file(MAKE_DIRECTORY "${dst_dir}")
  # Build-tree runtime/tool headers must be the exact bytes consumed by hosted
  # compilation. Absolute-path forwarding wrappers hide changes in the source
  # header from artifact provenance and make the build tree non-relocatable.
  configure_file("${header}" "${dst}" COPYONLY)
endforeach()

file(GLOB_RECURSE NEBULA_STD_FILES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/std/*.nb"
)

set(NEBULA_SYNC_STD_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/sync_std.cmake")
file(WRITE "${NEBULA_SYNC_STD_SCRIPT}" "file(REMOVE_RECURSE \"${NEBULA_GENERATED_STD_DIR}\")\n")
file(APPEND "${NEBULA_SYNC_STD_SCRIPT}" "file(MAKE_DIRECTORY \"${NEBULA_GENERATED_STD_DIR}\")\n")
file(APPEND "${NEBULA_SYNC_STD_SCRIPT}" "file(COPY \"${CMAKE_CURRENT_SOURCE_DIR}/std/\" DESTINATION \"${NEBULA_GENERATED_STD_DIR}\")\n")

add_custom_target(nebula-std-assets ALL
  COMMAND ${CMAKE_COMMAND} -P "${NEBULA_SYNC_STD_SCRIPT}"
  COMMENT "Sync bundled std sources into the build tree"
)

add_dependencies(nebula nebula-std-assets)

target_include_directories(nebula PRIVATE
  ${NEBULA_GENERATED_INCLUDE_DIR}
)

add_library(nebula-cli-support STATIC
  cli/artifact_digest.cpp
  cli/artifact_metadata.cpp
  cli/compiler_process.cpp
  cli/host_process.cpp
  cli/hosted_artifact_transaction.cpp
  cli/hosted_native_dependencies.cpp
  cli/hosted_object_workspace.cpp
  cli/hosted_toolchain.cpp
  cli/freestanding_toolchain.cpp
  cli/log_value.cpp
  cli/path_security.cpp
  cli/termination_signal.cpp
  cli/tool_lookup.cpp
  cli/verified_executable_lease.cpp
)
if(WIN32)
  target_sources(nebula-cli-support PRIVATE
    cli/windows_object_identity.cpp
    cli/windows_private_security.cpp
  )
endif()
target_include_directories(nebula-cli-support PRIVATE
  ${NEBULA_GENERATED_INCLUDE_DIR}
)
if(WIN32)
  target_link_libraries(nebula-cli-support PUBLIC advapi32 bcrypt)
endif()

add_library(nebula-boot-contract STATIC
  boot/protocol_abi_contract.cpp
)
target_include_directories(nebula-boot-contract PUBLIC
  ${NEBULA_GENERATED_INCLUDE_DIR}
)

add_executable(nebula-protocol-abi-validator
  boot/protocol_abi_contract_validator_main.cpp
)
target_link_libraries(nebula-protocol-abi-validator PRIVATE nebula-boot-contract)
add_dependencies(nebula nebula-protocol-abi-validator)

add_library(nebula-freestanding-support STATIC
  cli/elf_object.cpp
)
target_include_directories(nebula-freestanding-support PRIVATE
  ${NEBULA_GENERATED_INCLUDE_DIR}
)
target_link_libraries(nebula-freestanding-support PUBLIC nebula-boot-contract)

add_library(nebula-freestanding-transaction STATIC
  cli/freestanding_object.cpp
)
target_include_directories(nebula-freestanding-transaction PRIVATE
  ${NEBULA_GENERATED_INCLUDE_DIR}
)
target_link_libraries(nebula-freestanding-transaction PUBLIC
  nebula-cli-support
  nebula-freestanding-support
)

target_link_libraries(nebula PRIVATE
  nebula-freestanding-transaction
)

target_compile_definitions(nebula PRIVATE
  NEBULA_VERSION="${NEBULA_VERSION}"
)

function(nebula_enable_project_warnings target)
if(NEBULA_STRICT)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    if(NEBULA_WERROR)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endif()
endfunction()

nebula_enable_project_warnings(nebula)
nebula_enable_project_warnings(nebula-cli-support)
nebula_enable_project_warnings(nebula-freestanding-support)
nebula_enable_project_warnings(nebula-freestanding-transaction)
nebula_enable_project_warnings(nebula-boot-contract)
nebula_enable_project_warnings(nebula-protocol-abi-validator)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

enable_testing()
add_executable(nebula-freestanding-support-tests
  tests/cpp/freestanding_support_test.cpp
)
target_include_directories(nebula-freestanding-support-tests PRIVATE
  ${NEBULA_GENERATED_INCLUDE_DIR}
)
nebula_enable_project_warnings(nebula-freestanding-support-tests)
target_link_libraries(nebula-freestanding-support-tests PRIVATE
  nebula-cli-support
  nebula-freestanding-support
)
add_test(
  NAME nebula-freestanding-support-tests
  COMMAND nebula-freestanding-support-tests
)

add_executable(nebula-protocol-abi-contract-tests
  tests/cpp/protocol_abi_contract_test.cpp
)
target_include_directories(nebula-protocol-abi-contract-tests PRIVATE
  ${NEBULA_GENERATED_INCLUDE_DIR}
)
nebula_enable_project_warnings(nebula-protocol-abi-contract-tests)
target_link_libraries(nebula-protocol-abi-contract-tests PRIVATE
  nebula-boot-contract
  nebula-cli-support
)
add_test(
  NAME nebula-protocol-abi-contract-tests
  COMMAND nebula-protocol-abi-contract-tests
)
add_test(
  NAME nebula-protocol-install-snapshot-tests
  COMMAND ${CMAKE_COMMAND}
          "-DNEBULA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
          "-DNEBULA_TEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/tests/protocol-install-snapshot"
          "-DNEBULA_CMAKE_COMMAND=${CMAKE_COMMAND}"
          "-DNEBULA_GENERATOR=${CMAKE_GENERATOR}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/protocol_install_snapshot_test.cmake"
)

add_executable(nebula-artifact-metadata-tests
  tests/cpp/artifact_metadata_test.cpp
)
target_include_directories(nebula-artifact-metadata-tests PRIVATE
  ${NEBULA_GENERATED_INCLUDE_DIR}
)
nebula_enable_project_warnings(nebula-artifact-metadata-tests)
target_link_libraries(nebula-artifact-metadata-tests PRIVATE nebula-cli-support)
add_test(
  NAME nebula-artifact-metadata-tests
  COMMAND nebula-artifact-metadata-tests
)

add_executable(nebula-hosted-artifact-transaction-tests
  tests/cpp/hosted_artifact_transaction_test.cpp
  cli/artifact_digest.cpp
  cli/artifact_metadata.cpp
  cli/hosted_artifact_transaction.cpp
  cli/path_security.cpp
)
target_include_directories(nebula-hosted-artifact-transaction-tests PRIVATE
  ${NEBULA_GENERATED_INCLUDE_DIR}
)
nebula_enable_project_warnings(nebula-hosted-artifact-transaction-tests)
target_compile_definitions(nebula-hosted-artifact-transaction-tests PRIVATE
  NEBULA_HOSTED_ARTIFACT_TRANSACTION_TESTING=1
)
if(WIN32)
  target_sources(nebula-hosted-artifact-transaction-tests PRIVATE
    cli/windows_object_identity.cpp
    cli/windows_private_security.cpp
  )
  target_link_libraries(nebula-hosted-artifact-transaction-tests PRIVATE advapi32)
endif()
add_test(
  NAME nebula-hosted-artifact-transaction-tests
  COMMAND nebula-hosted-artifact-transaction-tests
)

add_executable(nebula-runtime-result-tests
  tests/cpp/runtime_result_test.cpp
)
target_include_directories(nebula-runtime-result-tests PRIVATE
  ${NEBULA_GENERATED_INCLUDE_DIR}
)
nebula_enable_project_warnings(nebula-runtime-result-tests)
add_test(
  NAME nebula-runtime-result-tests
  COMMAND nebula-runtime-result-tests
)

add_executable(nebula-host-process-tests
  tests/cpp/host_process_test.cpp
  cli/compiler_process.cpp
  cli/host_process.cpp
  cli/termination_signal.cpp
)
target_include_directories(nebula-host-process-tests PRIVATE
  ${NEBULA_GENERATED_INCLUDE_DIR}
)
nebula_enable_project_warnings(nebula-host-process-tests)
target_compile_definitions(nebula-host-process-tests PRIVATE
  NEBULA_HOST_PROCESS_TESTING=1
)
add_test(
  NAME nebula-host-process-tests
  COMMAND nebula-host-process-tests
)

add_executable(nebula-hosted-toolchain-tests
  tests/cpp/hosted_toolchain_test.cpp
)
target_include_directories(nebula-hosted-toolchain-tests PRIVATE
  ${NEBULA_GENERATED_INCLUDE_DIR}
)
nebula_enable_project_warnings(nebula-hosted-toolchain-tests)
target_link_libraries(nebula-hosted-toolchain-tests PRIVATE nebula-cli-support)
add_test(
  NAME nebula-hosted-toolchain-tests
  COMMAND nebula-hosted-toolchain-tests
)

add_executable(nebula-hosted-native-dependencies-tests
  tests/cpp/hosted_native_dependencies_test.cpp
)
target_include_directories(nebula-hosted-native-dependencies-tests PRIVATE
  ${NEBULA_GENERATED_INCLUDE_DIR}
)
nebula_enable_project_warnings(nebula-hosted-native-dependencies-tests)
target_link_libraries(nebula-hosted-native-dependencies-tests PRIVATE nebula-cli-support)
add_test(
  NAME nebula-hosted-native-dependencies-tests
  COMMAND nebula-hosted-native-dependencies-tests
)

add_executable(nebula-hosted-object-workspace-tests
  tests/cpp/hosted_object_workspace_test.cpp
)
target_include_directories(nebula-hosted-object-workspace-tests PRIVATE
  ${NEBULA_GENERATED_INCLUDE_DIR}
)
nebula_enable_project_warnings(nebula-hosted-object-workspace-tests)
target_link_libraries(nebula-hosted-object-workspace-tests PRIVATE nebula-cli-support)
add_test(
  NAME nebula-hosted-object-workspace-tests
  COMMAND nebula-hosted-object-workspace-tests
)

add_executable(nebula-verified-executable-lease-tests
  tests/cpp/verified_executable_lease_test.cpp
  cli/verified_executable_lease.cpp
)
target_include_directories(nebula-verified-executable-lease-tests PRIVATE
  ${NEBULA_GENERATED_INCLUDE_DIR}
)
target_compile_definitions(nebula-verified-executable-lease-tests PRIVATE
  NEBULA_VERIFIED_EXECUTABLE_LEASE_TESTING=1
)
nebula_enable_project_warnings(nebula-verified-executable-lease-tests)
target_link_libraries(nebula-verified-executable-lease-tests PRIVATE nebula-cli-support)
add_test(
  NAME nebula-verified-executable-lease-tests
  COMMAND nebula-verified-executable-lease-tests
)

if(UNIX)
  add_executable(nebula-compiler-process-containment-tests
    tests/cpp/compiler_process_containment_test.cpp
    cli/compiler_process.cpp
  )
  target_include_directories(nebula-compiler-process-containment-tests PRIVATE
    ${NEBULA_GENERATED_INCLUDE_DIR}
  )
  nebula_enable_project_warnings(nebula-compiler-process-containment-tests)
  add_test(
    NAME nebula-compiler-process-containment-tests
    COMMAND nebula-compiler-process-containment-tests
  )

  add_executable(nebula-freestanding-transaction-tests
    tests/cpp/freestanding_transaction_test.cpp
  )
  target_include_directories(nebula-freestanding-transaction-tests PRIVATE
    ${NEBULA_GENERATED_INCLUDE_DIR}
  )
  target_link_libraries(nebula-freestanding-transaction-tests PRIVATE
    nebula-freestanding-transaction
  )
  nebula_enable_project_warnings(nebula-freestanding-transaction-tests)
  add_test(
    NAME nebula-freestanding-transaction-tests
    COMMAND nebula-freestanding-transaction-tests
  )

  add_executable(nebula-freestanding-toolchain-signal-tests
    tests/cpp/freestanding_toolchain_signal_test.cpp
    cli/artifact_digest.cpp
    cli/artifact_metadata.cpp
    cli/compiler_process.cpp
    cli/elf_object.cpp
    cli/freestanding_object.cpp
    cli/freestanding_toolchain.cpp
    cli/host_process.cpp
    cli/log_value.cpp
    cli/path_security.cpp
    cli/termination_signal.cpp
    cli/verified_executable_lease.cpp
  )
  target_include_directories(nebula-freestanding-toolchain-signal-tests PRIVATE
    ${NEBULA_GENERATED_INCLUDE_DIR}
  )
  target_compile_definitions(nebula-freestanding-toolchain-signal-tests PRIVATE
    NEBULA_FREESTANDING_TOOLCHAIN_TESTING=1
    NEBULA_FREESTANDING_TRANSACTION_TESTING=1
    NEBULA_VERIFIED_EXECUTABLE_LEASE_TESTING=1
  )
  nebula_enable_project_warnings(nebula-freestanding-toolchain-signal-tests)
  add_test(
    NAME nebula-freestanding-toolchain-signal-tests
    COMMAND nebula-freestanding-toolchain-signal-tests
  )

  add_executable(nebula-termination-signal-tests
    tests/cpp/termination_signal_test.cpp
    cli/termination_signal.cpp
  )
  target_include_directories(nebula-termination-signal-tests PRIVATE
    ${NEBULA_GENERATED_INCLUDE_DIR}
  )
  nebula_enable_project_warnings(nebula-termination-signal-tests)
  add_test(
    NAME nebula-termination-signal-tests
    COMMAND nebula-termination-signal-tests
  )
endif()

add_test(
  NAME nebula-contract-tests
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run.py
          --suite all
          --report text
          --binary $<TARGET_FILE:nebula>
)

add_custom_target(nebula-contract-tests
  COMMAND ${CMAKE_CTEST_COMMAND} -R nebula-contract-tests --output-on-failure
  DEPENDS nebula
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# Keep this before every install rule so a corrupted protocol snapshot fails
# before the destination prefix is changed.
install(SCRIPT "${NEBULA_PROTOCOL_ABI_INSTALL_VERIFY_SCRIPT}")

install(TARGETS nebula
  RUNTIME DESTINATION bin
)

install(DIRECTORY
  ${CMAKE_CURRENT_SOURCE_DIR}/runtime
  DESTINATION include
  FILES_MATCHING PATTERN "*.hpp"
)

install(DIRECTORY
  ${NEBULA_GENERATED_STD_DIR}/
  DESTINATION share/nebula/std
  FILES_MATCHING PATTERN "*.nb"
)

install(DIRECTORY
  ${CMAKE_CURRENT_SOURCE_DIR}/tooling/registry
  DESTINATION share/nebula
  FILES_MATCHING
    PATTERN "__pycache__" EXCLUDE
    PATTERN "*.pyc" EXCLUDE
    PATTERN "*.py"
    PATTERN "README.md"
)

set(NEBULA_INSTALL_DOCS
  ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
  ${CMAKE_CURRENT_SOURCE_DIR}/VERSION
  ${CMAKE_CURRENT_SOURCE_DIR}/README.md
  ${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md
  ${CMAKE_CURRENT_SOURCE_DIR}/RELEASE_PROCESS.md
  ${CMAKE_CURRENT_SOURCE_DIR}/SECURITY.md
)
file(GLOB NEBULA_INSTALL_GUIDES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/docs/*.md"
)
list(APPEND NEBULA_INSTALL_DOCS ${NEBULA_INSTALL_GUIDES})
set(NEBULA_RELEASE_NOTES "${CMAKE_CURRENT_SOURCE_DIR}/RELEASE_NOTES_v${NEBULA_VERSION}.md")
if(EXISTS "${NEBULA_RELEASE_NOTES}")
  list(APPEND NEBULA_INSTALL_DOCS "${NEBULA_RELEASE_NOTES}")
endif()

install(FILES ${NEBULA_INSTALL_DOCS}
  DESTINATION share/doc/nebula
)

# Keep established top-level docs flattened under share/doc/nebula while preserving
# nested claim-boundary docs by repository-relative directory.
install(DIRECTORY
  ${CMAKE_CURRENT_SOURCE_DIR}/docs/universeos
  DESTINATION share/doc/nebula/docs
  FILES_MATCHING PATTERN "*.md"
)

install(DIRECTORY
  ${CMAKE_CURRENT_SOURCE_DIR}/spec
  DESTINATION share/doc/nebula
  FILES_MATCHING
    PATTERN "*.md"
    PATTERN "*.ebnf"
)

install(DIRECTORY
  ${CMAKE_CURRENT_SOURCE_DIR}/rfcs
  DESTINATION share/doc/nebula
  FILES_MATCHING PATTERN "*.md"
)
