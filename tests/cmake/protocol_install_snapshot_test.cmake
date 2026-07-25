if(NOT DEFINED NEBULA_SOURCE_DIR OR
   NOT DEFINED NEBULA_TEST_BINARY_DIR OR
   NOT DEFINED NEBULA_CMAKE_COMMAND OR
   NOT DEFINED NEBULA_GENERATOR)
  message(FATAL_ERROR "protocol snapshot test requires source, binary, CMake, and generator inputs")
endif()

set(contract_relative_path "boot/uos-x86_64-limine-v1/contract.manifest")
set(header_relative_path "boot/uos-x86_64-limine-v1/protocol/limine.h")
set(license_relative_path "boot/uos-x86_64-limine-v1/protocol/LICENSE")
set(fixture_cmake_source
  "${NEBULA_SOURCE_DIR}/tests/cmake/protocol_install_snapshot_fixture/CMakeLists.txt"
)
set(snapshot_module "${NEBULA_SOURCE_DIR}/boot/prepare_protocol_asset_snapshot.cmake")
set(install_verify_template
  "${NEBULA_SOURCE_DIR}/boot/protocol_asset_install_verify.cmake.in"
)
set(contract_source "${NEBULA_SOURCE_DIR}/boot/protocol_abi_contract.cpp")
set(validator_main_source
  "${NEBULA_SOURCE_DIR}/boot/protocol_abi_contract_validator_main.cpp"
)
set(configure_validator_source
  "${NEBULA_SOURCE_DIR}/boot/protocol_abi_contract_configure_validator.cpp"
)
set(protocol_install_lock_name ".uos-x86_64-limine-v1.install.lock")

function(require_success label result output error)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "${label} failed (${result})\nstdout:\n${output}\nstderr:\n${error}")
  endif()
endfunction()

function(require_failure_containing label result output error expected)
  if(result EQUAL 0)
    message(FATAL_ERROR "${label} unexpectedly succeeded")
  endif()
  set(combined_output "${output}\n${error}")
  string(FIND "${combined_output}" "${expected}" expected_position)
  if(expected_position EQUAL -1)
    message(FATAL_ERROR
      "${label} failed without the expected diagnostic '${expected}'\n${combined_output}"
    )
  endif()
endfunction()

function(prepare_fixture name output_source output_build output_install)
  set(fixture_root "${NEBULA_TEST_BINARY_DIR}/${name}")
  set(fixture_source "${fixture_root}/source")
  set(fixture_build "${fixture_root}/build")
  set(fixture_install "${fixture_root}/install")
  file(REMOVE_RECURSE "${fixture_root}")
  file(MAKE_DIRECTORY "${fixture_source}/boot/uos-x86_64-limine-v1/protocol")
  configure_file("${fixture_cmake_source}" "${fixture_source}/CMakeLists.txt" COPYONLY)
  configure_file(
    "${NEBULA_SOURCE_DIR}/${contract_relative_path}"
    "${fixture_source}/${contract_relative_path}"
    COPYONLY
  )
  configure_file(
    "${NEBULA_SOURCE_DIR}/${header_relative_path}"
    "${fixture_source}/${header_relative_path}"
    COPYONLY
  )
  configure_file(
    "${NEBULA_SOURCE_DIR}/${license_relative_path}"
    "${fixture_source}/${license_relative_path}"
    COPYONLY
  )
  set(${output_source} "${fixture_source}" PARENT_SCOPE)
  set(${output_build} "${fixture_build}" PARENT_SCOPE)
  set(${output_install} "${fixture_install}" PARENT_SCOPE)
endfunction()

function(configure_fixture label source build output_result output_stdout output_stderr)
  execute_process(
    COMMAND "${NEBULA_CMAKE_COMMAND}"
            -S "${source}"
            -B "${build}"
            -G "${NEBULA_GENERATOR}"
            "-DNEBULA_PROTOCOL_SNAPSHOT_MODULE=${snapshot_module}"
            "-DNEBULA_PROTOCOL_INSTALL_VERIFY_TEMPLATE=${install_verify_template}"
            "-DNEBULA_PROTOCOL_CONTRACT_SOURCE=${contract_source}"
            "-DNEBULA_PROTOCOL_VALIDATOR_MAIN_SOURCE=${validator_main_source}"
            "-DNEBULA_PROTOCOL_CONFIGURE_VALIDATOR_SOURCE=${configure_validator_source}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
  )
  set(${output_result} "${result}" PARENT_SCOPE)
  set(${output_stdout} "${output}" PARENT_SCOPE)
  set(${output_stderr} "${error}" PARENT_SCOPE)
endfunction()

function(build_fixture_validator label build)
  execute_process(
    COMMAND "${NEBULA_CMAKE_COMMAND}" --build "${build}"
            --target fixture-protocol-validator
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
  )
  require_success(
    "${label} validator build" "${build_result}" "${build_output}" "${build_error}"
  )
endfunction()

function(require_snapshot_mutation_rejected name snapshot_relative_path asset_label)
  prepare_fixture(
    "snapshot_mutation_${name}"
    mutation_source
    mutation_build
    mutation_install
  )
  configure_fixture(
    "snapshot_mutation_${name}"
    "${mutation_source}"
    "${mutation_build}"
    configure_result
    configure_output
    configure_error
  )
  require_success(
    "${asset_label} snapshot-mutation fixture configure"
    "${configure_result}"
    "${configure_output}"
    "${configure_error}"
  )
  build_fixture_validator("${asset_label} snapshot-mutation" "${mutation_build}")
  file(APPEND
    "${mutation_build}/verified-inputs/boot/uos-x86_64-limine-v1/${snapshot_relative_path}"
    "snapshot mutation\n"
  )
  execute_process(
    COMMAND "${NEBULA_CMAKE_COMMAND}" --install "${mutation_build}"
            --prefix "${mutation_install}"
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_output
    ERROR_VARIABLE install_error
  )
  require_failure_containing(
    "install after ${asset_label} snapshot mutation"
    "${install_result}"
    "${install_output}"
    "${install_error}"
    "verified protocol snapshot is invalid"
  )
  set(combined_install_output "${install_output}\n${install_error}")
  string(FIND "${combined_install_output}" "${asset_label}" asset_label_position)
  if(asset_label_position EQUAL -1)
    message(FATAL_ERROR
      "install after ${asset_label} snapshot mutation did not identify the changed asset"
    )
  endif()
  if(EXISTS "${mutation_install}/share/nebula/boot/uos-x86_64-limine-v1/contract.manifest" OR
     EXISTS "${mutation_install}/share/nebula/boot/uos-x86_64-limine-v1/protocol/limine.h" OR
     EXISTS "${mutation_install}/share/nebula/boot/uos-x86_64-limine-v1/protocol/LICENSE")
    message(FATAL_ERROR
      "${asset_label} snapshot verification failure partially installed protocol assets"
    )
  endif()
endfunction()

file(REMOVE_RECURSE "${NEBULA_TEST_BINARY_DIR}")
file(MAKE_DIRECTORY "${NEBULA_TEST_BINARY_DIR}")

# A configured build installs only its verified snapshot even if every source
# asset is subsequently changed.
prepare_fixture(source_mutation source_mutation_source source_mutation_build source_mutation_install)
configure_fixture(
  source_mutation
  "${source_mutation_source}"
  "${source_mutation_build}"
  configure_result
  configure_output
  configure_error
)
require_success(
  "source-mutation fixture configure"
  "${configure_result}"
  "${configure_output}"
  "${configure_error}"
)
build_fixture_validator(source-mutation "${source_mutation_build}")
set(snapshot_root
  "${source_mutation_build}/verified-inputs/boot/uos-x86_64-limine-v1"
)
file(SHA256 "${snapshot_root}/contract.manifest" expected_manifest_sha256)
file(SHA256 "${snapshot_root}/protocol/limine.h" expected_header_sha256)
file(SHA256 "${snapshot_root}/protocol/LICENSE" expected_license_sha256)
file(APPEND "${source_mutation_source}/${contract_relative_path}" "source_tree_mutation=1\n")
file(APPEND "${source_mutation_source}/${header_relative_path}" "/* source tree mutation */\n")
file(APPEND "${source_mutation_source}/${license_relative_path}" "source tree mutation\n")
file(WRITE
  "${source_mutation_source}/boot/uos-x86_64-limine-v1/protocol/unexpected-source-file"
  "must not be installed\n"
)
execute_process(
  COMMAND "${NEBULA_CMAKE_COMMAND}" --install "${source_mutation_build}"
          --prefix "${source_mutation_install}"
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_output
  ERROR_VARIABLE install_error
)
require_success(
  "install after source mutation" "${install_result}" "${install_output}" "${install_error}"
)
set(installed_root "${source_mutation_install}/share/nebula/boot/uos-x86_64-limine-v1")
file(SHA256 "${installed_root}/contract.manifest" installed_manifest_sha256)
file(SHA256 "${installed_root}/protocol/limine.h" installed_header_sha256)
file(SHA256 "${installed_root}/protocol/LICENSE" installed_license_sha256)
if(NOT installed_manifest_sha256 STREQUAL expected_manifest_sha256 OR
   NOT installed_header_sha256 STREQUAL expected_header_sha256 OR
   NOT installed_license_sha256 STREQUAL expected_license_sha256)
  message(FATAL_ERROR "installed protocol assets did not preserve the configured snapshot")
endif()
file(GLOB_RECURSE installed_protocol_assets
  RELATIVE "${installed_root}"
  "${installed_root}/*"
)
list(SORT installed_protocol_assets)
set(expected_installed_protocol_assets
  contract.manifest
  protocol/LICENSE
  protocol/limine.h
)
if(NOT installed_protocol_assets STREQUAL expected_installed_protocol_assets)
  message(FATAL_ERROR
    "protocol install must contain exactly the three explicit snapshot files; got "
    "${installed_protocol_assets}"
  )
endif()
execute_process(
  COMMAND "${NEBULA_CMAKE_COMMAND}" --install "${source_mutation_build}"
          --prefix "${source_mutation_install}"
  RESULT_VARIABLE idempotent_result
  OUTPUT_VARIABLE idempotent_output
  ERROR_VARIABLE idempotent_error
)
require_success(
  "idempotent protocol install"
  "${idempotent_result}"
  "${idempotent_output}"
  "${idempotent_error}"
)
file(SHA256 "${installed_root}/contract.manifest" idempotent_manifest_sha256)
file(SHA256 "${installed_root}/protocol/limine.h" idempotent_header_sha256)
file(SHA256 "${installed_root}/protocol/LICENSE" idempotent_license_sha256)
if(NOT idempotent_manifest_sha256 STREQUAL expected_manifest_sha256 OR
   NOT idempotent_header_sha256 STREQUAL expected_header_sha256 OR
   NOT idempotent_license_sha256 STREQUAL expected_license_sha256)
  message(FATAL_ERROR "idempotent protocol install changed the published bundle")
endif()
set(installed_lock
  "${source_mutation_install}/share/nebula/boot/${protocol_install_lock_name}"
)
if(IS_SYMLINK "${installed_lock}" OR
   NOT EXISTS "${installed_lock}" OR
   IS_DIRECTORY "${installed_lock}")
  message(FATAL_ERROR "protocol install did not retain one safe fixed coordination lock")
endif()
file(GLOB installed_stage_residue
  "${source_mutation_install}/share/nebula/boot/.uos-*.stage-*"
)
if(installed_stage_residue)
  message(FATAL_ERROR "idempotent protocol install left stage residue: ${installed_stage_residue}")
endif()
set(expected_install_manifest_entries
  "${installed_lock}"
  "${installed_root}/contract.manifest"
  "${installed_root}/protocol/LICENSE"
  "${installed_root}/protocol/limine.h"
)
file(STRINGS
  "${source_mutation_build}/install_manifest.txt"
  actual_install_manifest_entries
)
list(SORT expected_install_manifest_entries)
list(SORT actual_install_manifest_entries)
if(NOT actual_install_manifest_entries STREQUAL expected_install_manifest_entries)
  message(FATAL_ERROR
    "protocol install manifest must track the persistent coordination lock and exactly three "
    "bundle files; got ${actual_install_manifest_entries}"
  )
endif()

# Install must fail closed before copying any protocol asset if any private
# build-tree snapshot file is changed.
require_snapshot_mutation_rejected(manifest contract.manifest contract.manifest)
require_snapshot_mutation_rejected(header protocol/limine.h limine.h)
require_snapshot_mutation_rejected(license protocol/LICENSE LICENSE)

if(CMAKE_HOST_UNIX)
  prepare_fixture(snapshot_symlink symlink_source symlink_build symlink_install)
  configure_fixture(
    snapshot_symlink
    "${symlink_source}"
    "${symlink_build}"
    configure_result
    configure_output
    configure_error
  )
  require_success(
    "snapshot-symlink fixture configure"
    "${configure_result}"
    "${configure_output}"
    "${configure_error}"
  )
  build_fixture_validator(snapshot-symlink "${symlink_build}")
  set(symlink_snapshot
    "${symlink_build}/verified-inputs/boot/uos-x86_64-limine-v1/protocol/limine.h"
  )
  file(REMOVE "${symlink_snapshot}")
  file(CREATE_LINK
    "${symlink_source}/${header_relative_path}"
    "${symlink_snapshot}"
    SYMBOLIC
    RESULT symlink_result
  )
  if(NOT symlink_result STREQUAL "0")
    message(FATAL_ERROR "could not create isolated snapshot symlink: ${symlink_result}")
  endif()
  execute_process(
    COMMAND "${NEBULA_CMAKE_COMMAND}" --install "${symlink_build}"
            --prefix "${symlink_install}"
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_output
    ERROR_VARIABLE install_error
  )
  require_failure_containing(
    "install with symbolic-link snapshot"
    "${install_result}"
    "${install_output}"
    "${install_error}"
    "symbolic link:"
  )
  if(EXISTS "${symlink_install}/share/nebula/boot/uos-x86_64-limine-v1/contract.manifest")
    message(FATAL_ERROR "snapshot symlink verification failure partially installed protocol assets")
  endif()
endif()

# The manifest identity is authoritative at configure time.
prepare_fixture(asset_mismatch asset_mismatch_source asset_mismatch_build unused_install)
file(APPEND "${asset_mismatch_source}/${license_relative_path}" "asset mismatch\n")
configure_fixture(
  asset_mismatch
  "${asset_mismatch_source}"
  "${asset_mismatch_build}"
  configure_result
  configure_output
  configure_error
)
require_failure_containing(
  "configure with mismatched source asset"
  "${configure_result}"
  "${configure_output}"
  "${configure_error}"
  "vendored Limine protocol license integrity check failed"
)

if(CMAKE_HOST_UNIX)
  prepare_fixture(source_symlink source_symlink_source source_symlink_build unused_install)
  set(source_symlink_asset "${source_symlink_source}/${license_relative_path}")
  file(REMOVE "${source_symlink_asset}")
  file(CREATE_LINK
    "${NEBULA_SOURCE_DIR}/${license_relative_path}"
    "${source_symlink_asset}"
    SYMBOLIC
    RESULT symlink_result
  )
  if(NOT symlink_result STREQUAL "0")
    message(FATAL_ERROR "could not create isolated source symlink: ${symlink_result}")
  endif()
  configure_fixture(
    source_symlink
    "${source_symlink_source}"
    "${source_symlink_build}"
    configure_result
    configure_output
    configure_error
  )
  require_failure_containing(
    "configure with symbolic-link source asset"
    "${configure_result}"
    "${configure_output}"
    "${configure_error}"
    "symbolic link:"
  )
endif()

# Duplicate identity fields are rejected rather than resolved by first/last
# match semantics.
prepare_fixture(duplicate_field duplicate_source duplicate_build unused_install)
file(STRINGS
  "${duplicate_source}/${contract_relative_path}"
  duplicate_line
  REGEX "^limine_header_size="
)
file(APPEND "${duplicate_source}/${contract_relative_path}" "${duplicate_line}\n")
configure_fixture(
  duplicate_field
  "${duplicate_source}"
  "${duplicate_build}"
  configure_result
  configure_output
  configure_error
)
require_failure_containing(
  "configure with duplicate manifest field"
  "${configure_result}"
  "${configure_output}"
  "${configure_error}"
  "manifest field appears more than once"
)

# A full-contract field mutation must be rejected by the production parser,
# even though all six extracted asset fields remain unchanged.
prepare_fixture(canonical_gate canonical_gate_source canonical_gate_build unused_install)
file(READ "${canonical_gate_source}/${contract_relative_path}" canonical_gate_manifest)
string(REPLACE
  "gate_status=planned"
  "gate_status=active"
  canonical_gate_manifest
  "${canonical_gate_manifest}"
)
file(WRITE
  "${canonical_gate_source}/${contract_relative_path}"
  "${canonical_gate_manifest}"
)
configure_fixture(
  canonical_gate
  "${canonical_gate_source}"
  "${canonical_gate_build}"
  configure_result
  configure_output
  configure_error
)
require_failure_containing(
  "configure with non-canonical gate status"
  "${configure_result}"
  "${configure_output}"
  "${configure_error}"
  "production canonical parser"
)

# cmake --install is not allowed to skip canonical validation when the native
# install-time validator target was not built.
prepare_fixture(validator_missing validator_missing_source validator_missing_build validator_missing_install)
configure_fixture(
  validator_missing
  "${validator_missing_source}"
  "${validator_missing_build}"
  configure_result
  configure_output
  configure_error
)
require_success(
  "validator-missing fixture configure"
  "${configure_result}"
  "${configure_output}"
  "${configure_error}"
)
execute_process(
  COMMAND "${NEBULA_CMAKE_COMMAND}" --install "${validator_missing_build}"
          --prefix "${validator_missing_install}"
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_output
  ERROR_VARIABLE install_error
)
require_failure_containing(
  "install without native validator"
  "${install_result}"
  "${install_output}"
  "${install_error}"
  "native-host protocol ABI validator"
)
if(EXISTS "${validator_missing_install}")
  message(FATAL_ERROR "missing validator failure changed the install prefix")
endif()

# A pre-existing snapshot ancestor link must be rejected before it can redirect
# MAKE_DIRECTORY or COPYONLY outside BINARY_ROOT.
if(CMAKE_HOST_UNIX)
  prepare_fixture(ancestor_symlink ancestor_source ancestor_build unused_install)
  set(ancestor_outside "${NEBULA_TEST_BINARY_DIR}/ancestor-symlink-outside")
  file(MAKE_DIRECTORY "${ancestor_build}" "${ancestor_outside}")
  file(CREATE_LINK
    "${ancestor_outside}"
    "${ancestor_build}/verified-inputs"
    SYMBOLIC
    RESULT symlink_result
  )
  if(NOT symlink_result STREQUAL "0")
    message(FATAL_ERROR "could not create isolated snapshot-ancestor symlink: ${symlink_result}")
  endif()
  configure_fixture(
    ancestor_symlink
    "${ancestor_source}"
    "${ancestor_build}"
    configure_result
    configure_output
    configure_error
  )
  require_failure_containing(
    "configure with snapshot ancestor symlink"
    "${configure_result}"
    "${configure_output}"
    "${configure_error}"
    "symbolic-link ancestor"
  )
  file(GLOB_RECURSE outside_entries RELATIVE "${ancestor_outside}" "${ancestor_outside}/*")
  if(outside_entries)
    message(FATAL_ERROR "snapshot ancestor symlink caused an out-of-root write: ${outside_entries}")
  endif()
endif()

# Extra snapshot entries are rejected rather than ignored by an explicit-file
# copy list.
prepare_fixture(snapshot_extra snapshot_extra_source snapshot_extra_build snapshot_extra_install)
configure_fixture(
  snapshot_extra
  "${snapshot_extra_source}"
  "${snapshot_extra_build}"
  configure_result
  configure_output
  configure_error
)
require_success(
  "snapshot-extra fixture configure"
  "${configure_result}"
  "${configure_output}"
  "${configure_error}"
)
build_fixture_validator(snapshot-extra "${snapshot_extra_build}")
file(WRITE
  "${snapshot_extra_build}/verified-inputs/boot/uos-x86_64-limine-v1/unexpected"
  "unexpected\n"
)
execute_process(
  COMMAND "${NEBULA_CMAKE_COMMAND}" --install "${snapshot_extra_build}"
          --prefix "${snapshot_extra_install}"
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_output
  ERROR_VARIABLE install_error
)
require_failure_containing(
  "install with extra snapshot entry"
  "${install_result}"
  "${install_output}"
  "${install_error}"
  "verified protocol snapshot is invalid"
)
if(EXISTS "${snapshot_extra_install}/share/nebula/boot/uos-x86_64-limine-v1")
  message(FATAL_ERROR "extra snapshot entry produced a partial protocol install")
endif()

# An existing partial target is a conflict. In particular, a directory at the
# LICENSE path must not allow the preceding files to be overwritten first.
prepare_fixture(target_directory target_directory_source target_directory_build target_directory_install)
configure_fixture(
  target_directory
  "${target_directory_source}"
  "${target_directory_build}"
  configure_result
  configure_output
  configure_error
)
require_success(
  "target-directory fixture configure"
  "${configure_result}"
  "${configure_output}"
  "${configure_error}"
)
build_fixture_validator(target-directory "${target_directory_build}")
set(target_directory_root
  "${target_directory_install}/share/nebula/boot/uos-x86_64-limine-v1"
)
file(MAKE_DIRECTORY "${target_directory_root}/protocol/LICENSE")
file(WRITE "${target_directory_root}/contract.manifest" "caller-owned manifest\n")
file(WRITE "${target_directory_root}/protocol/limine.h" "caller-owned header\n")
file(WRITE "${target_directory_root}/protocol/LICENSE/owner" "caller-owned directory\n")
execute_process(
  COMMAND "${NEBULA_CMAKE_COMMAND}" --install "${target_directory_build}"
          --prefix "${target_directory_install}"
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_output
  ERROR_VARIABLE install_error
)
require_failure_containing(
  "install over directory conflict"
  "${install_result}"
  "${install_output}"
  "${install_error}"
  "refusing to modify an existing non-identical protocol bundle"
)
file(READ "${target_directory_root}/contract.manifest" preserved_manifest)
file(READ "${target_directory_root}/protocol/limine.h" preserved_header)
file(READ "${target_directory_root}/protocol/LICENSE/owner" preserved_owner)
if(NOT preserved_manifest STREQUAL "caller-owned manifest\n" OR
   NOT preserved_header STREQUAL "caller-owned header\n" OR
   NOT preserved_owner STREQUAL "caller-owned directory\n")
  message(FATAL_ERROR "directory conflict changed caller-owned protocol target content")
endif()
file(GLOB stage_residue "${target_directory_install}/share/nebula/boot/.uos-*.stage-*")
if(stage_residue)
  message(FATAL_ERROR "directory conflict left protocol stage residue: ${stage_residue}")
endif()
if(EXISTS
    "${target_directory_install}/share/nebula/boot/${protocol_install_lock_name}" OR
   IS_SYMLINK
    "${target_directory_install}/share/nebula/boot/${protocol_install_lock_name}")
  message(FATAL_ERROR "directory conflict changed the install parent by creating a lock")
endif()

# An otherwise exact target containing one extra file is also a zero-mutation
# conflict.
prepare_fixture(target_extra target_extra_source target_extra_build target_extra_install)
configure_fixture(
  target_extra
  "${target_extra_source}"
  "${target_extra_build}"
  configure_result
  configure_output
  configure_error
)
require_success(
  "target-extra fixture configure"
  "${configure_result}"
  "${configure_output}"
  "${configure_error}"
)
build_fixture_validator(target-extra "${target_extra_build}")
set(target_extra_root "${target_extra_install}/share/nebula/boot/uos-x86_64-limine-v1")
file(MAKE_DIRECTORY "${target_extra_root}/protocol")
configure_file(
  "${target_extra_build}/verified-inputs/boot/uos-x86_64-limine-v1/contract.manifest"
  "${target_extra_root}/contract.manifest"
  COPYONLY
)
configure_file(
  "${target_extra_build}/verified-inputs/boot/uos-x86_64-limine-v1/protocol/limine.h"
  "${target_extra_root}/protocol/limine.h"
  COPYONLY
)
configure_file(
  "${target_extra_build}/verified-inputs/boot/uos-x86_64-limine-v1/protocol/LICENSE"
  "${target_extra_root}/protocol/LICENSE"
  COPYONLY
)
file(WRITE "${target_extra_root}/.caller-extra" "preserve me\n")
execute_process(
  COMMAND "${NEBULA_CMAKE_COMMAND}" --install "${target_extra_build}"
          --prefix "${target_extra_install}"
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_output
  ERROR_VARIABLE install_error
)
require_failure_containing(
  "install over target with extra file"
  "${install_result}"
  "${install_output}"
  "${install_error}"
  "refusing to modify an existing non-identical protocol bundle"
)
file(READ "${target_extra_root}/.caller-extra" preserved_extra)
if(NOT preserved_extra STREQUAL "preserve me\n")
  message(FATAL_ERROR "target-extra conflict changed caller-owned content")
endif()
file(GLOB target_extra_stages "${target_extra_install}/share/nebula/boot/.uos-*.stage-*")
if(target_extra_stages)
  message(FATAL_ERROR "target-extra conflict left protocol stage residue: ${target_extra_stages}")
endif()
if(EXISTS "${target_extra_install}/share/nebula/boot/${protocol_install_lock_name}" OR
   IS_SYMLINK "${target_extra_install}/share/nebula/boot/${protocol_install_lock_name}")
  message(FATAL_ERROR "target-extra conflict changed the install parent by creating a lock")
endif()

if(CMAKE_HOST_UNIX)
  prepare_fixture(target_symlink target_symlink_source target_symlink_build target_symlink_install)
  configure_fixture(
    target_symlink
    "${target_symlink_source}"
    "${target_symlink_build}"
    configure_result
    configure_output
    configure_error
  )
  require_success(
    "target-symlink fixture configure"
    "${configure_result}"
    "${configure_output}"
    "${configure_error}"
  )
  build_fixture_validator(target-symlink "${target_symlink_build}")
  set(target_symlink_parent "${target_symlink_install}/share/nebula/boot")
  set(target_symlink_outside "${NEBULA_TEST_BINARY_DIR}/target-symlink-outside")
  file(MAKE_DIRECTORY "${target_symlink_parent}" "${target_symlink_outside}")
  file(WRITE "${target_symlink_outside}/owner" "caller-owned symlink target\n")
  file(CREATE_LINK
    "${target_symlink_outside}"
    "${target_symlink_parent}/uos-x86_64-limine-v1"
    SYMBOLIC
    RESULT symlink_result
  )
  if(NOT symlink_result STREQUAL "0")
    message(FATAL_ERROR "could not create isolated target symlink: ${symlink_result}")
  endif()
  execute_process(
    COMMAND "${NEBULA_CMAKE_COMMAND}" --install "${target_symlink_build}"
            --prefix "${target_symlink_install}"
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_output
    ERROR_VARIABLE install_error
  )
  require_failure_containing(
    "install over target symlink"
    "${install_result}"
    "${install_output}"
    "${install_error}"
    "symbolic-link ancestor"
  )
  file(READ "${target_symlink_outside}/owner" symlink_owner)
  if(NOT symlink_owner STREQUAL "caller-owned symlink target\n")
    message(FATAL_ERROR "target symlink conflict changed its caller-owned destination")
  endif()

  prepare_fixture(destdir_install destdir_source destdir_build unused_install)
  configure_fixture(
    destdir_install
    "${destdir_source}"
    "${destdir_build}"
    configure_result
    configure_output
    configure_error
  )
  require_success(
    "DESTDIR fixture configure"
    "${configure_result}"
    "${configure_output}"
    "${configure_error}"
  )
  build_fixture_validator(DESTDIR "${destdir_build}")
  set(destdir_root "${NEBULA_TEST_BINARY_DIR}/destdir-root")
  execute_process(
    COMMAND "${NEBULA_CMAKE_COMMAND}" -E env "DESTDIR=${destdir_root}"
            "${NEBULA_CMAKE_COMMAND}" --install "${destdir_build}"
            --prefix "/nebula-logical-prefix"
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_output
    ERROR_VARIABLE install_error
  )
  require_success(
    "protocol install with DESTDIR"
    "${install_result}"
    "${install_output}"
    "${install_error}"
  )
  set(destdir_bundle
    "${destdir_root}/nebula-logical-prefix/share/nebula/boot/uos-x86_64-limine-v1"
  )
  file(GLOB_RECURSE destdir_entries RELATIVE "${destdir_bundle}" "${destdir_bundle}/*")
  list(SORT destdir_entries)
  if(NOT destdir_entries STREQUAL expected_installed_protocol_assets)
    message(FATAL_ERROR "DESTDIR protocol install produced an invalid tree: ${destdir_entries}")
  endif()
  set(destdir_lock
    "${destdir_root}/nebula-logical-prefix/share/nebula/boot/${protocol_install_lock_name}"
  )
  if(IS_SYMLINK "${destdir_lock}" OR
     NOT EXISTS "${destdir_lock}" OR
     IS_DIRECTORY "${destdir_lock}")
    message(FATAL_ERROR "DESTDIR protocol install did not create a safe coordination lock")
  endif()

  # The explicit DESTDIR is the trusted anchor. A link below that boundary is
  # still rejected and must not redirect parent creation outside the anchor.
  prepare_fixture(
    destdir_below_anchor_symlink
    destdir_symlink_source
    destdir_symlink_build
    unused_install
  )
  configure_fixture(
    destdir_below_anchor_symlink
    "${destdir_symlink_source}"
    "${destdir_symlink_build}"
    configure_result
    configure_output
    configure_error
  )
  require_success(
    "DESTDIR below-anchor-symlink fixture configure"
    "${configure_result}"
    "${configure_output}"
    "${configure_error}"
  )
  build_fixture_validator(DESTDIR-below-anchor-symlink "${destdir_symlink_build}")
  set(destdir_symlink_anchor "${NEBULA_TEST_BINARY_DIR}/destdir-symlink-anchor")
  set(destdir_symlink_outside "${NEBULA_TEST_BINARY_DIR}/destdir-symlink-outside")
  file(MAKE_DIRECTORY "${destdir_symlink_anchor}" "${destdir_symlink_outside}")
  file(WRITE "${destdir_symlink_outside}/owner" "caller-owned outside directory\n")
  file(CREATE_LINK
    "${destdir_symlink_outside}"
    "${destdir_symlink_anchor}/nebula-logical-prefix"
    SYMBOLIC
    RESULT symlink_result
  )
  if(NOT symlink_result STREQUAL "0")
    message(FATAL_ERROR "could not create isolated below-anchor symlink: ${symlink_result}")
  endif()
  execute_process(
    COMMAND "${NEBULA_CMAKE_COMMAND}" -E env "DESTDIR=${destdir_symlink_anchor}"
            "${NEBULA_CMAKE_COMMAND}" --install "${destdir_symlink_build}"
            --prefix "/nebula-logical-prefix"
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_output
    ERROR_VARIABLE install_error
  )
  require_failure_containing(
    "DESTDIR install through below-anchor symlink"
    "${install_result}"
    "${install_output}"
    "${install_error}"
    "below the trusted install anchor"
  )
  file(READ "${destdir_symlink_outside}/owner" destdir_symlink_owner)
  if(NOT destdir_symlink_owner STREQUAL "caller-owned outside directory\n")
    message(FATAL_ERROR "below-anchor symlink failure changed caller-owned outside content")
  endif()
  file(GLOB_RECURSE destdir_symlink_outside_entries
    RELATIVE "${destdir_symlink_outside}"
    "${destdir_symlink_outside}/*"
  )
  if(NOT destdir_symlink_outside_entries STREQUAL "owner")
    message(FATAL_ERROR
      "below-anchor symlink redirected install writes outside: ${destdir_symlink_outside_entries}"
    )
  endif()

  # macOS exposes /tmp as a trusted system symlink to /private/tmp. The anchor
  # itself may canonicalize; only components below the explicit DESTDIR are
  # required to remain direct descendants.
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND IS_SYMLINK "/tmp")
    string(RANDOM LENGTH 32 ALPHABET 0123456789abcdef tmp_destdir_token)
    set(tmp_destdir_root "/tmp/nebula-protocol-destdir-${tmp_destdir_token}")
    if(EXISTS "${tmp_destdir_root}" OR IS_SYMLINK "${tmp_destdir_root}")
      message(FATAL_ERROR "random /tmp DESTDIR collision: ${tmp_destdir_root}")
    endif()
    execute_process(
      COMMAND "${NEBULA_CMAKE_COMMAND}" -E env "DESTDIR=${tmp_destdir_root}"
              "${NEBULA_CMAKE_COMMAND}" --install "${destdir_build}"
              --prefix "/nebula-logical-prefix"
      RESULT_VARIABLE install_result
      OUTPUT_VARIABLE install_output
      ERROR_VARIABLE install_error
    )
    require_success(
      "protocol install with symlink-canonicalized /tmp DESTDIR"
      "${install_result}"
      "${install_output}"
      "${install_error}"
    )
    set(tmp_destdir_bundle
      "${tmp_destdir_root}/nebula-logical-prefix/share/nebula/boot/uos-x86_64-limine-v1"
    )
    file(GLOB_RECURSE tmp_destdir_entries
      RELATIVE "${tmp_destdir_bundle}"
      "${tmp_destdir_bundle}/*"
    )
    list(SORT tmp_destdir_entries)
    if(NOT tmp_destdir_entries STREQUAL expected_installed_protocol_assets)
      message(FATAL_ERROR "/tmp DESTDIR install produced an invalid tree: ${tmp_destdir_entries}")
    endif()
    if(IS_SYMLINK "${tmp_destdir_root}")
      message(FATAL_ERROR "random /tmp DESTDIR root was replaced by a symbolic link")
    endif()
    file(REMOVE_RECURSE "${tmp_destdir_root}")
    if(EXISTS "${tmp_destdir_root}" OR IS_SYMLINK "${tmp_destdir_root}")
      message(FATAL_ERROR "could not remove isolated /tmp DESTDIR fixture")
    endif()
  endif()

  prepare_fixture(concurrent_install concurrent_source concurrent_build concurrent_install)
  configure_fixture(
    concurrent_install
    "${concurrent_source}"
    "${concurrent_build}"
    configure_result
    configure_output
    configure_error
  )
  require_success(
    "concurrent fixture configure"
    "${configure_result}"
    "${configure_output}"
    "${configure_error}"
  )
  build_fixture_validator(concurrent "${concurrent_build}")
  string(CONCAT concurrent_command
    "\"${NEBULA_CMAKE_COMMAND}\" --install \"${concurrent_build}\" --prefix \"${concurrent_install}\" & first=$!; "
    "\"${NEBULA_CMAKE_COMMAND}\" --install \"${concurrent_build}\" --prefix \"${concurrent_install}\" & second=$!; "
    "wait $first; first_rc=$?; wait $second; second_rc=$?; "
    "test $first_rc -eq 0 -a $second_rc -eq 0"
  )
  execute_process(
    COMMAND /bin/sh -c "${concurrent_command}"
    RESULT_VARIABLE concurrent_result
    OUTPUT_VARIABLE concurrent_output
    ERROR_VARIABLE concurrent_error
  )
  require_success(
    "concurrent protocol installs"
    "${concurrent_result}"
    "${concurrent_output}"
    "${concurrent_error}"
  )
  set(concurrent_root "${concurrent_install}/share/nebula/boot/uos-x86_64-limine-v1")
  file(GLOB_RECURSE concurrent_entries RELATIVE "${concurrent_root}" "${concurrent_root}/*")
  list(SORT concurrent_entries)
  if(NOT concurrent_entries STREQUAL expected_installed_protocol_assets)
    message(FATAL_ERROR "concurrent protocol install produced an invalid tree: ${concurrent_entries}")
  endif()
  file(GLOB concurrent_stages "${concurrent_install}/share/nebula/boot/.uos-*.stage-*")
  if(concurrent_stages)
    message(FATAL_ERROR "concurrent protocol install left stage residue: ${concurrent_stages}")
  endif()
  set(concurrent_lock
    "${concurrent_install}/share/nebula/boot/${protocol_install_lock_name}"
  )
  if(IS_SYMLINK "${concurrent_lock}" OR
     NOT EXISTS "${concurrent_lock}" OR
     IS_DIRECTORY "${concurrent_lock}")
    message(FATAL_ERROR "concurrent protocol installs did not share one safe fixed lock")
  endif()

  # Holding the fixed lock beyond the bounded acquisition timeout must fail
  # explicitly before any stage or destination bundle is created.
  prepare_fixture(lock_timeout lock_timeout_source lock_timeout_build lock_timeout_install)
  configure_fixture(
    lock_timeout
    "${lock_timeout_source}"
    "${lock_timeout_build}"
    configure_result
    configure_output
    configure_error
  )
  require_success(
    "lock-timeout fixture configure"
    "${configure_result}"
    "${configure_output}"
    "${configure_error}"
  )
  build_fixture_validator(lock-timeout "${lock_timeout_build}")
  set(lock_timeout_parent "${lock_timeout_install}/share/nebula/boot")
  set(lock_timeout_path "${lock_timeout_parent}/${protocol_install_lock_name}")
  set(lock_timeout_ready "${NEBULA_TEST_BINARY_DIR}/lock-timeout-ready")
  set(lock_holder_script "${NEBULA_TEST_BINARY_DIR}/hold-protocol-install-lock.cmake")
  file(MAKE_DIRECTORY "${lock_timeout_parent}")
  file(WRITE "${lock_holder_script}" [=[
if(NOT DEFINED LOCK_PATH OR
   NOT DEFINED READY_PATH OR
   NOT DEFINED CMAKE_COMMAND_PATH)
  message(FATAL_ERROR "lock holder requires LOCK_PATH, READY_PATH, and CMAKE_COMMAND_PATH")
endif()
file(LOCK "${LOCK_PATH}" GUARD PROCESS TIMEOUT 0 RESULT_VARIABLE lock_result)
if(NOT lock_result STREQUAL "0")
  message(FATAL_ERROR "test lock holder could not acquire lock: ${lock_result}")
endif()
file(WRITE "${READY_PATH}" "locked\n")
execute_process(
  COMMAND "${CMAKE_COMMAND_PATH}" -E sleep 32
  RESULT_VARIABLE sleep_result
)
if(NOT sleep_result STREQUAL "0")
  message(FATAL_ERROR "test lock holder sleep failed: ${sleep_result}")
endif()
]=])
  string(CONCAT lock_timeout_command
    "\"${NEBULA_CMAKE_COMMAND}\" \"-DLOCK_PATH=${lock_timeout_path}\" "
    "\"-DREADY_PATH=${lock_timeout_ready}\" "
    "\"-DCMAKE_COMMAND_PATH=${NEBULA_CMAKE_COMMAND}\" -P \"${lock_holder_script}\" & holder=$!; "
    "attempt=0; while test ! -f \"${lock_timeout_ready}\"; do "
    "if ! kill -0 $holder 2>/dev/null; then wait $holder; exit 91; fi; "
    "attempt=$((attempt + 1)); if test $attempt -ge 100; then kill $holder; wait $holder; exit 92; fi; "
    "sleep 0.05; done; "
    "\"${NEBULA_CMAKE_COMMAND}\" --install \"${lock_timeout_build}\" "
    "--prefix \"${lock_timeout_install}\"; installer_rc=$?; "
    "wait $holder; holder_rc=$?; "
    "if test $holder_rc -ne 0; then exit $holder_rc; fi; exit $installer_rc"
  )
  execute_process(
    COMMAND /bin/sh -c "${lock_timeout_command}"
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_output
    ERROR_VARIABLE install_error
  )
  require_failure_containing(
    "protocol install coordination lock timeout"
    "${install_result}"
    "${install_output}"
    "${install_error}"
    "failed to acquire protocol install coordination lock"
  )
  set(lock_timeout_bundle "${lock_timeout_parent}/uos-x86_64-limine-v1")
  if(EXISTS "${lock_timeout_bundle}" OR IS_SYMLINK "${lock_timeout_bundle}")
    message(FATAL_ERROR "lock timeout created a protocol destination bundle")
  endif()
  file(GLOB lock_timeout_stages "${lock_timeout_parent}/.uos-*.stage-*")
  if(lock_timeout_stages)
    message(FATAL_ERROR "lock timeout left protocol stage residue: ${lock_timeout_stages}")
  endif()
  if(IS_SYMLINK "${lock_timeout_path}" OR
     NOT EXISTS "${lock_timeout_path}" OR
     IS_DIRECTORY "${lock_timeout_path}")
    message(FATAL_ERROR "lock timeout did not preserve a safe reusable fixed lock")
  endif()
  execute_process(
    COMMAND "${NEBULA_CMAKE_COMMAND}" --install "${lock_timeout_build}"
            --prefix "${lock_timeout_install}"
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_output
    ERROR_VARIABLE install_error
  )
  require_success(
    "protocol install after coordination lock release"
    "${install_result}"
    "${install_output}"
    "${install_error}"
  )
  file(GLOB lock_release_stages "${lock_timeout_parent}/.uos-*.stage-*")
  if(lock_release_stages)
    message(FATAL_ERROR "post-timeout protocol install left stage residue: ${lock_release_stages}")
  endif()
endif()
