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

add_executable(nebula
  cli/main.cpp
  cli/parse.cpp
  cli/build_run.cpp
  cli/dispatch.cpp
  cli/project.cpp
  cli/tooling.cpp
  codegen/cpp_backend.cpp
  frontend/lexer.cpp
  frontend/parser.cpp
  frontend/typecheck.cpp
  nir/lower.cpp
  nir/cfg.cpp
  nir/cfg_ir.cpp
  passes/call_target_resolver.cpp
  passes/escape_analysis.cpp
  passes/borrow_xstmt.cpp
  passes/rep_owner_infer.cpp
  passes/epistemic_lint.cpp
)

set(NEBULA_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
file(MAKE_DIRECTORY "${NEBULA_GENERATED_INCLUDE_DIR}")
file(GLOB_RECURSE NEBULA_HEADER_FILES CONFIGURE_DEPENDS
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
  file(TO_CMAKE_PATH "${header}" header_for_include)
  file(WRITE "${dst}" "#pragma once\n#include \"${header_for_include}\"\n")
endforeach()

target_include_directories(nebula PRIVATE
  ${NEBULA_GENERATED_INCLUDE_DIR}
)

target_compile_definitions(nebula PRIVATE
  NEBULA_VERSION="${NEBULA_VERSION}"
)

if(NEBULA_STRICT)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(nebula PRIVATE -Wall -Wextra -Wpedantic)
    if(NEBULA_WERROR)
      target_compile_options(nebula PRIVATE -Werror)
    endif()
  endif()
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)

enable_testing()
add_test(
  NAME nebula-contract-tests
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run.py
          --suite all
          --report text
          --binary ${CMAKE_BINARY_DIR}/nebula
)

add_custom_target(nebula-contract-tests
  COMMAND ${CMAKE_CTEST_COMMAND} -R nebula-contract-tests --output-on-failure
  DEPENDS nebula
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

install(TARGETS nebula
  RUNTIME DESTINATION bin
)

install(DIRECTORY
  ${CMAKE_CURRENT_SOURCE_DIR}/runtime
  DESTINATION include
  FILES_MATCHING PATTERN "*.hpp"
)

set(NEBULA_INSTALL_DOCS
  ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
  ${CMAKE_CURRENT_SOURCE_DIR}/VERSION
  ${CMAKE_CURRENT_SOURCE_DIR}/README.md
  ${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md
  ${CMAKE_CURRENT_SOURCE_DIR}/RELEASE_PROCESS.md
)
set(NEBULA_RELEASE_NOTES "${CMAKE_CURRENT_SOURCE_DIR}/RELEASE_NOTES_v${NEBULA_VERSION}.md")
if(EXISTS "${NEBULA_RELEASE_NOTES}")
  list(APPEND NEBULA_INSTALL_DOCS "${NEBULA_RELEASE_NOTES}")
endif()

install(FILES ${NEBULA_INSTALL_DOCS}
  DESTINATION share/doc/nebula
)
