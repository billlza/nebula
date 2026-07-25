include_guard(GLOBAL)

include(CMakeParseArguments)

# Freeze the protocol contract and its manifest-declared vendored assets as one
# configure-time identity. Callers must install only the returned snapshot paths.

function(_nebula_protocol_validate_canonical_contract validator_source manifest binary_root)
  if(CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR
      "protocol ABI configuration requires a native-host C++ validator; "
      "cross-compiling cannot safely provide the install-time host validator"
    )
  endif()

  set(validator_cxx_flags "${CMAKE_CXX_FLAGS}")
  if(NEBULA_STRICT AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    string(APPEND validator_cxx_flags " -Wall -Wextra -Wpedantic")
    if(NEBULA_WERROR)
      string(APPEND validator_cxx_flags " -Werror")
    endif()
  endif()
  set(validator_build_dir "${binary_root}/CMakeFiles/nebula-protocol-abi-configure-validator")
  try_run(
    validator_run_result
    validator_compile_result
    "${validator_build_dir}"
    "${validator_source}"
    CMAKE_FLAGS
      "-DCMAKE_CXX_STANDARD=20"
      "-DCMAKE_CXX_STANDARD_REQUIRED=ON"
      "-DCMAKE_CXX_EXTENSIONS=OFF"
      "-DCMAKE_CXX_FLAGS:STRING=${validator_cxx_flags}"
    COMPILE_OUTPUT_VARIABLE validator_compile_output
    RUN_OUTPUT_VARIABLE validator_run_output
    ARGS "${manifest}"
  )
  if(NOT validator_compile_result)
    message(FATAL_ERROR
      "failed to compile the native-host protocol ABI canonical validator:\n"
      "${validator_compile_output}"
    )
  endif()
  if(NOT validator_run_result STREQUAL "0")
    message(FATAL_ERROR
      "protocol ABI manifest failed the production canonical parser "
      "(exit ${validator_run_result}):\n${validator_run_output}"
    )
  endif()
endfunction()

function(_nebula_protocol_validate_snapshot_location binary_root snapshot_root phase)
  if(NOT EXISTS "${binary_root}" OR NOT IS_DIRECTORY "${binary_root}")
    message(FATAL_ERROR "protocol snapshot BINARY_ROOT must be an existing directory")
  endif()
  if(IS_SYMLINK "${binary_root}")
    message(FATAL_ERROR "protocol snapshot BINARY_ROOT must not be a symbolic link")
  endif()

  get_filename_component(normalized_binary_root "${binary_root}" ABSOLUTE)
  get_filename_component(normalized_snapshot_root "${snapshot_root}" ABSOLUTE)
  file(TO_CMAKE_PATH "${normalized_binary_root}" normalized_binary_root)
  file(TO_CMAKE_PATH "${normalized_snapshot_root}" normalized_snapshot_root)
  file(RELATIVE_PATH snapshot_relative_path
    "${normalized_binary_root}"
    "${normalized_snapshot_root}"
  )
  if(snapshot_relative_path STREQUAL "" OR
     snapshot_relative_path STREQUAL "." OR
     IS_ABSOLUTE "${snapshot_relative_path}" OR
     snapshot_relative_path MATCHES "^\.\.(/|$)")
    message(FATAL_ERROR "protocol SNAPSHOT_ROOT must be a strict descendant of BINARY_ROOT")
  endif()

  set(candidate "${normalized_snapshot_root}")
  set(existing_probe "")
  while(NOT candidate STREQUAL normalized_binary_root)
    if(IS_SYMLINK "${candidate}")
      message(FATAL_ERROR
        "protocol SNAPSHOT_ROOT ${phase} path contains a symbolic-link ancestor: ${candidate}"
      )
    endif()
    if(existing_probe STREQUAL "" AND EXISTS "${candidate}")
      set(existing_probe "${candidate}")
    endif()
    get_filename_component(parent "${candidate}" DIRECTORY)
    if(parent STREQUAL candidate)
      message(FATAL_ERROR "protocol SNAPSHOT_ROOT traversal escaped BINARY_ROOT")
    endif()
    set(candidate "${parent}")
  endwhile()
  if(existing_probe STREQUAL "")
    set(existing_probe "${normalized_binary_root}")
  endif()

  file(REAL_PATH "${normalized_binary_root}" canonical_binary_root)
  file(REAL_PATH "${existing_probe}" canonical_existing_probe)
  file(RELATIVE_PATH existing_probe_relative_path
    "${normalized_binary_root}"
    "${existing_probe}"
  )
  if(existing_probe_relative_path STREQUAL ".")
    set(expected_existing_probe "${canonical_binary_root}")
  else()
    set(expected_existing_probe "${canonical_binary_root}/${existing_probe_relative_path}")
  endif()
  file(TO_CMAKE_PATH "${canonical_existing_probe}" canonical_existing_probe)
  file(TO_CMAKE_PATH "${expected_existing_probe}" expected_existing_probe)
  if(NOT canonical_existing_probe STREQUAL expected_existing_probe)
    message(FATAL_ERROR
      "protocol SNAPSHOT_ROOT ${phase} path is redirected by a symlink, junction, or reparse point"
    )
  endif()
endfunction()

function(_nebula_protocol_manifest_field output manifest field)
  file(STRINGS "${manifest}" matching_lines REGEX "^${field}=.*$")
  list(LENGTH matching_lines matching_line_count)
  if(NOT matching_line_count EQUAL 1)
    message(FATAL_ERROR
      "protocol ABI manifest field '${field}' must occur exactly once; found ${matching_line_count}"
    )
  endif()

  list(GET matching_lines 0 matching_line)
  string(REGEX REPLACE "^${field}=" "" value "${matching_line}")
  if(value STREQUAL "")
    message(FATAL_ERROR "protocol ABI manifest field '${field}' must not be empty")
  endif()
  set(${output} "${value}" PARENT_SCOPE)
endfunction()

function(_nebula_protocol_validate_decimal field value)
  if(NOT value MATCHES "^(0|[1-9][0-9]*)$")
    message(FATAL_ERROR
      "protocol ABI manifest field '${field}' must be a canonical unsigned decimal integer"
    )
  endif()
endfunction()

function(_nebula_protocol_validate_sha256 field value)
  string(LENGTH "${value}" value_length)
  if(NOT value_length EQUAL 64 OR NOT value MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR
      "protocol ABI manifest field '${field}' must be a lowercase SHA-256 digest"
    )
  endif()
endfunction()

function(_nebula_protocol_resolve_asset output source_root field relative_path)
  if(IS_ABSOLUTE "${relative_path}" OR
     NOT relative_path MATCHES "^[A-Za-z0-9._/-]+$" OR
     relative_path MATCHES "(^|/)\.\.?(/|$)" OR
     relative_path MATCHES "//")
    message(FATAL_ERROR
      "protocol ABI manifest field '${field}' must be a normalized repository-relative path"
    )
  endif()

  set(asset_path "${source_root}/${relative_path}")
  if(IS_SYMLINK "${asset_path}")
    message(FATAL_ERROR
      "protocol ABI asset from field '${field}' must not be a symbolic link: ${relative_path}"
    )
  endif()
  file(REAL_PATH "${source_root}" canonical_source_root)
  file(REAL_PATH "${asset_path}" canonical_asset)
  set(source_prefix "${canonical_source_root}/")
  string(FIND "${canonical_asset}" "${source_prefix}" asset_prefix_position)
  if(NOT asset_prefix_position EQUAL 0)
    message(FATAL_ERROR
      "protocol ABI manifest field '${field}' resolves outside the repository source root"
    )
  endif()
  if(NOT EXISTS "${canonical_asset}" OR IS_DIRECTORY "${canonical_asset}")
    message(FATAL_ERROR
      "protocol ABI asset from field '${field}' is not a readable regular file: ${relative_path}"
    )
  endif()
  set(${output} "${canonical_asset}" PARENT_SCOPE)
endfunction()

function(_nebula_protocol_verify_asset label path expected_size expected_sha256)
  if(IS_SYMLINK "${path}")
    message(FATAL_ERROR "${label} must not be a symbolic link: ${path}")
  endif()
  if(NOT EXISTS "${path}" OR IS_DIRECTORY "${path}")
    message(FATAL_ERROR "${label} is not a readable regular file: ${path}")
  endif()
  file(SIZE "${path}" actual_size)
  if(NOT actual_size STREQUAL expected_size)
    message(FATAL_ERROR
      "${label} integrity check failed: expected ${expected_size} bytes, got ${actual_size} bytes"
    )
  endif()
  file(SHA256 "${path}" actual_sha256)
  if(NOT actual_sha256 STREQUAL expected_sha256)
    message(FATAL_ERROR
      "${label} integrity check failed: expected ${expected_size} bytes/${expected_sha256}, "
      "got ${actual_size} bytes/${actual_sha256}"
    )
  endif()
endfunction()

function(nebula_prepare_protocol_asset_snapshot)
  set(one_value_arguments
    PREFIX
    MANIFEST
    SOURCE_ROOT
    BINARY_ROOT
    SNAPSHOT_ROOT
    CONFIGURE_VALIDATOR_SOURCE
    INSTALL_VALIDATOR
    INSTALL_VERIFY_TEMPLATE
  )
  cmake_parse_arguments(ARG "" "${one_value_arguments}" "" ${ARGN})
  if(ARG_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR
      "nebula_prepare_protocol_asset_snapshot received unknown arguments: ${ARG_UNPARSED_ARGUMENTS}"
    )
  endif()
  foreach(required_argument IN LISTS one_value_arguments)
    if(NOT DEFINED ARG_${required_argument} OR ARG_${required_argument} STREQUAL "")
      message(FATAL_ERROR
        "nebula_prepare_protocol_asset_snapshot requires ${required_argument}"
      )
    endif()
  endforeach()
  if(NOT ARG_PREFIX MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
    message(FATAL_ERROR "protocol snapshot PREFIX must be a valid CMake variable prefix")
  endif()
  foreach(path_argument IN ITEMS
      MANIFEST
      SOURCE_ROOT
      BINARY_ROOT
      SNAPSHOT_ROOT
      CONFIGURE_VALIDATOR_SOURCE
      INSTALL_VERIFY_TEMPLATE)
    if(NOT IS_ABSOLUTE "${ARG_${path_argument}}")
      message(FATAL_ERROR
        "nebula_prepare_protocol_asset_snapshot ${path_argument} must be absolute"
      )
    endif()
  endforeach()
  if(IS_SYMLINK "${ARG_MANIFEST}")
    message(FATAL_ERROR "protocol ABI manifest must not be a symbolic link: ${ARG_MANIFEST}")
  endif()
  if(NOT EXISTS "${ARG_MANIFEST}" OR IS_DIRECTORY "${ARG_MANIFEST}")
    message(FATAL_ERROR "protocol ABI manifest is not a readable regular file: ${ARG_MANIFEST}")
  endif()
  if(NOT EXISTS "${ARG_INSTALL_VERIFY_TEMPLATE}" OR
     IS_DIRECTORY "${ARG_INSTALL_VERIFY_TEMPLATE}")
    message(FATAL_ERROR
      "protocol snapshot install verification template is missing: ${ARG_INSTALL_VERIFY_TEMPLATE}"
    )
  endif()
  if(NOT EXISTS "${ARG_CONFIGURE_VALIDATOR_SOURCE}" OR
     IS_DIRECTORY "${ARG_CONFIGURE_VALIDATOR_SOURCE}" OR
     IS_SYMLINK "${ARG_CONFIGURE_VALIDATOR_SOURCE}")
    message(FATAL_ERROR
      "protocol configure validator source is missing or unsafe: "
      "${ARG_CONFIGURE_VALIDATOR_SOURCE}"
    )
  endif()

  file(REAL_PATH "${ARG_SOURCE_ROOT}" canonical_source_root)
  file(REAL_PATH "${ARG_MANIFEST}" canonical_manifest)
  set(source_prefix "${canonical_source_root}/")
  string(FIND "${canonical_manifest}" "${source_prefix}" manifest_prefix_position)
  if(NOT manifest_prefix_position EQUAL 0)
    message(FATAL_ERROR "protocol ABI manifest must resolve inside SOURCE_ROOT")
  endif()

  _nebula_protocol_validate_snapshot_location(
    "${ARG_BINARY_ROOT}" "${ARG_SNAPSHOT_ROOT}" "pre-create"
  )
  _nebula_protocol_validate_snapshot_location(
    "${ARG_BINARY_ROOT}" "${ARG_SNAPSHOT_ROOT}/protocol" "pre-create protocol"
  )
  _nebula_protocol_validate_canonical_contract(
    "${ARG_CONFIGURE_VALIDATOR_SOURCE}" "${canonical_manifest}" "${ARG_BINARY_ROOT}"
  )

  file(SIZE "${canonical_manifest}" manifest_size_before)
  if(manifest_size_before GREATER 8192)
    message(FATAL_ERROR "protocol ABI manifest exceeds the 8192-byte parser boundary")
  endif()
  file(SHA256 "${canonical_manifest}" manifest_sha256_before)

  _nebula_protocol_manifest_field(
    header_vendor_path "${canonical_manifest}" limine_header_vendor_path
  )
  _nebula_protocol_manifest_field(
    header_size "${canonical_manifest}" limine_header_size
  )
  _nebula_protocol_manifest_field(
    header_sha256 "${canonical_manifest}" limine_header_sha256
  )
  _nebula_protocol_manifest_field(
    license_vendor_path "${canonical_manifest}" limine_license_vendor_path
  )
  _nebula_protocol_manifest_field(
    license_size "${canonical_manifest}" limine_license_size
  )
  _nebula_protocol_manifest_field(
    license_sha256 "${canonical_manifest}" limine_license_sha256
  )

  _nebula_protocol_validate_decimal(limine_header_size "${header_size}")
  _nebula_protocol_validate_decimal(limine_license_size "${license_size}")
  _nebula_protocol_validate_sha256(limine_header_sha256 "${header_sha256}")
  _nebula_protocol_validate_sha256(limine_license_sha256 "${license_sha256}")
  _nebula_protocol_resolve_asset(
    header_source "${canonical_source_root}" limine_header_vendor_path "${header_vendor_path}"
  )
  _nebula_protocol_resolve_asset(
    license_source "${canonical_source_root}" limine_license_vendor_path "${license_vendor_path}"
  )
  _nebula_protocol_verify_asset(
    "vendored Limine protocol header" "${header_source}" "${header_size}" "${header_sha256}"
  )
  _nebula_protocol_verify_asset(
    "vendored Limine protocol license" "${license_source}" "${license_size}" "${license_sha256}"
  )

  file(SIZE "${canonical_manifest}" manifest_size_after)
  file(SHA256 "${canonical_manifest}" manifest_sha256_after)
  if(NOT manifest_size_after STREQUAL manifest_size_before OR
     NOT manifest_sha256_after STREQUAL manifest_sha256_before)
    message(FATAL_ERROR "protocol ABI manifest changed while its asset identity was being verified")
  endif()

  set(snapshot_manifest "${ARG_SNAPSHOT_ROOT}/contract.manifest")
  set(snapshot_header "${ARG_SNAPSHOT_ROOT}/protocol/limine.h")
  set(snapshot_license "${ARG_SNAPSHOT_ROOT}/protocol/LICENSE")
  file(MAKE_DIRECTORY "${ARG_SNAPSHOT_ROOT}/protocol")
  _nebula_protocol_validate_snapshot_location(
    "${ARG_BINARY_ROOT}" "${ARG_SNAPSHOT_ROOT}" "post-create"
  )
  _nebula_protocol_validate_snapshot_location(
    "${ARG_BINARY_ROOT}" "${ARG_SNAPSHOT_ROOT}/protocol" "post-create protocol"
  )
  if(IS_SYMLINK "${ARG_SNAPSHOT_ROOT}" OR
     IS_SYMLINK "${ARG_SNAPSHOT_ROOT}/protocol")
    message(FATAL_ERROR "protocol snapshot directories must not be symbolic links")
  endif()
  foreach(snapshot_path IN ITEMS
      "${snapshot_manifest}"
      "${snapshot_header}"
      "${snapshot_license}")
    if(IS_SYMLINK "${snapshot_path}")
      message(FATAL_ERROR "protocol snapshot output must not be a symbolic link: ${snapshot_path}")
    endif()
    if(IS_DIRECTORY "${snapshot_path}")
      message(FATAL_ERROR "protocol snapshot output must not be a directory: ${snapshot_path}")
    endif()
  endforeach()
  configure_file("${canonical_manifest}" "${snapshot_manifest}" COPYONLY)
  configure_file("${header_source}" "${snapshot_header}" COPYONLY)
  configure_file("${license_source}" "${snapshot_license}" COPYONLY)

  _nebula_protocol_verify_asset(
    "verified protocol manifest snapshot"
    "${snapshot_manifest}"
    "${manifest_size_before}"
    "${manifest_sha256_before}"
  )
  _nebula_protocol_verify_asset(
    "verified Limine header snapshot" "${snapshot_header}" "${header_size}" "${header_sha256}"
  )
  _nebula_protocol_verify_asset(
    "verified Limine license snapshot"
    "${snapshot_license}"
    "${license_size}"
    "${license_sha256}"
  )

  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${canonical_manifest}"
    "${header_source}"
    "${license_source}"
  )

  set(NEBULA_PROTOCOL_SNAPSHOT_MANIFEST "${snapshot_manifest}")
  set(NEBULA_PROTOCOL_SNAPSHOT_HEADER "${snapshot_header}")
  set(NEBULA_PROTOCOL_SNAPSHOT_LICENSE "${snapshot_license}")
  set(NEBULA_PROTOCOL_SNAPSHOT_ROOT "${ARG_SNAPSHOT_ROOT}")
  set(NEBULA_PROTOCOL_SNAPSHOT_MANIFEST_SIZE "${manifest_size_before}")
  set(NEBULA_PROTOCOL_SNAPSHOT_MANIFEST_SHA256 "${manifest_sha256_before}")
  set(NEBULA_PROTOCOL_SNAPSHOT_HEADER_SIZE "${header_size}")
  set(NEBULA_PROTOCOL_SNAPSHOT_HEADER_SHA256 "${header_sha256}")
  set(NEBULA_PROTOCOL_SNAPSHOT_LICENSE_SIZE "${license_size}")
  set(NEBULA_PROTOCOL_SNAPSHOT_LICENSE_SHA256 "${license_sha256}")
  set(NEBULA_PROTOCOL_INSTALL_VALIDATOR "${ARG_INSTALL_VALIDATOR}")
  set(NEBULA_PROTOCOL_INSTALL_CMAKE_COMMAND "${CMAKE_COMMAND}")
  set(install_script_root "${ARG_BINARY_ROOT}/CMakeFiles/nebula-protocol-install")
  file(MAKE_DIRECTORY "${install_script_root}")
  set(install_verify_script_template
    "${install_script_root}/verify-and-publish-before-install.in.cmake"
  )
  configure_file(
    "${ARG_INSTALL_VERIFY_TEMPLATE}"
    "${install_verify_script_template}"
    @ONLY
  )
  set(install_verify_script
    "${install_script_root}/verify-and-publish-before-install-$<CONFIG>.cmake"
  )
  file(GENERATE
    OUTPUT "${install_verify_script}"
    INPUT "${install_verify_script_template}"
  )

  set(${ARG_PREFIX}_MANIFEST_SNAPSHOT "${snapshot_manifest}" PARENT_SCOPE)
  set(${ARG_PREFIX}_HEADER_SNAPSHOT "${snapshot_header}" PARENT_SCOPE)
  set(${ARG_PREFIX}_LICENSE_SNAPSHOT "${snapshot_license}" PARENT_SCOPE)
  set(${ARG_PREFIX}_MANIFEST_SIZE "${manifest_size_before}" PARENT_SCOPE)
  set(${ARG_PREFIX}_MANIFEST_SHA256 "${manifest_sha256_before}" PARENT_SCOPE)
  set(${ARG_PREFIX}_HEADER_SIZE "${header_size}" PARENT_SCOPE)
  set(${ARG_PREFIX}_HEADER_SHA256 "${header_sha256}" PARENT_SCOPE)
  set(${ARG_PREFIX}_LICENSE_SIZE "${license_size}" PARENT_SCOPE)
  set(${ARG_PREFIX}_LICENSE_SHA256 "${license_sha256}" PARENT_SCOPE)
  set(${ARG_PREFIX}_INSTALL_VERIFY_SCRIPT "${install_verify_script}" PARENT_SCOPE)
endfunction()
