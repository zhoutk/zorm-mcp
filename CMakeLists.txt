# ---------------------------------------------------------------------------
# zorm-mcp - the production multi-backend database MCP server.
#
# Build (from the project root, Release by default):
#     cmake --preset clang-rlz
#     cmake --build build -j 8
#     ctest --test-dir build
#
#   build/  intermediate artifacts
#   bin/    final executables (zorm_mcp_server.exe, demo servers, test exes)
#
# Third-party dependencies are git submodules under thirds/ - treat them as
# normal external projects: change them in their own repositories, then pull
# the new revision here with
#     git submodule update --remote thirds/zorm thirds/mcp-cpp
#
#   thirds/zorm      zormlib + backend DLLs + zjson
#   thirds/mcp-cpp   the MCP protocol library
#
# Targets:
#   zorm_mcp_server      the MCP server (stdio by default, --http optional)
#   mcp_test_zorm_mcp    e2e test (spawns the server as a child over real pipes)
#   demo targets         the validation showcase (demo/)
# ---------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.18)
project(zorm-mcp VERSION 1.1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Release is the normal build here; Debug stays opt-in via -DCMAKE_BUILD_TYPE.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

enable_testing()

set(ZORM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirds/zorm" CACHE PATH "zorm checkout")
set(MCP_CPP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirds/mcp-cpp" CACHE PATH "mcp-cpp checkout")

if(NOT EXISTS "${ZORM_DIR}/CMakeLists.txt")
  message(FATAL_ERROR
    "zorm not found at ${ZORM_DIR} - run: git submodule update --init --recursive")
endif()
if(NOT EXISTS "${MCP_CPP_DIR}/CMakeLists.txt")
  message(FATAL_ERROR
    "mcp-cpp not found at ${MCP_CPP_DIR} - run: git submodule update --init --recursive")
endif()

# ---- googletest (this project's own copy, used by the e2e tests) ------------
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/thirds/googletest-1.18.0
                 gtest-build EXCLUDE_FROM_ALL)

# ---- third-party: zorm and mcp-cpp -----------------------------------------
# Built as libraries only - their tests, examples and CLI tools stay in their
# own projects.
set(CMAKE_ZJSON_ENABLE_TEST OFF)   # zorm: no zorm tests
set(ZORM_BUILD_DEMO OFF)           # zorm: no zorm CLI demo
set(MCP_BUILD_TESTS OFF)           # mcp-cpp: no mcp tests
set(MCP_BUILD_EXAMPLES OFF)        # mcp-cpp: no examples/benchmarks

add_subdirectory(${MCP_CPP_DIR} mcp-cpp-build)
add_subdirectory(${ZORM_DIR} zorm-build)

# ---- optional source coverage (llvm-cov) -----------------------------------
# Only OUR targets (added after this point) are instrumented; third-party
# libraries above stay clean. Usage:
#   cmake --preset clang-rlz -B build-cov -DZORM_MCP_COVERAGE=ON
#   cmake --build build-cov -j 8
#   LLVM_PROFILE_FILE=$PWD/build-cov/prof-%p.profraw ctest --test-dir build-cov
#   llvm-profdata merge -sparse build-cov/*.profraw -o build-cov/zorm.profdata
#   llvm-cov report bin/zorm_mcp_server.exe -instr-profile=build-cov/zorm.profdata
option(ZORM_MCP_COVERAGE "Instrument this project's sources for llvm-cov" OFF)
if(ZORM_MCP_COVERAGE)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "ZORM_MCP_COVERAGE requires Clang")
  endif()
  add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
  add_link_options(-fprofile-instr-generate)
endif()

# ---- runtime DLL bundling (this project's own helper) ----------------------
# AI clients spawn the server with their own environment (no MSYS2 clang64
# PATH), so the full dependency closure must sit next to the exe. Helper:
# cmake/zorm_mcp_bundle_runtime_deps.cmake (this project's own copy - the
# zorm project keeps its own for its tests).
function(zorm_mcp_bundle_runtime_deps target)
  if(NOT WIN32)
    return()
  endif()
  get_filename_component(_compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
  add_custom_command(TARGET ${target} POST_BUILD
      COMMAND "${CMAKE_COMMAND}"
              "-DZORM_TARGET_EXE=$<TARGET_FILE:${target}>"
              "-DZORM_THIRDS_DLL_DIR=${ZORM_DIR}/thirds/bin/win10"
              "-DZORM_COMPILER_BIN=${_compiler_bin}"
              -P "${CMAKE_SOURCE_DIR}/cmake/zorm_mcp_bundle_runtime_deps.cmake"
      COMMENT "Bundling runtime DLLs next to $<TARGET_FILE_NAME:${target}>"
      VERBATIM)
endfunction()

# ---- third-party include/link surface --------------------------------------
# zorm's public headers (Idb.h / DbBase.h / GlobalConstants.h) and zjson.
set(ZORM_PUBLIC_INCLUDES
  ${ZORM_DIR}/src/include
  ${ZORM_DIR}/thirds/zjson)

# Backend client libraries (libpq / sqlite3 / dmdpi) live in zorm's thirds.
link_directories(${ZORM_DIR}/thirds/sqlit3/lib
                 ${ZORM_DIR}/thirds/pgsql/lib
                 ${ZORM_DIR}/thirds/dm8/dpi)

# Match zorm's in-tree convention: consumers see no dllimport on ZORM_API.
add_definitions(-DZORM_LIB)

# ---- the server -------------------------------------------------------------

add_executable(zorm_mcp_server
  src/app.cpp
  src/json_util.cpp
  src/guide.cpp
  src/meta_tools.cpp
  src/data_tools.cpp
  src/sql_tools.cpp
  src/resources.cpp
  src/main.cpp)
target_link_libraries(zorm_mcp_server PRIVATE mcp zormlib)
target_include_directories(zorm_mcp_server PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${ZORM_PUBLIC_INCLUDES})
set_target_properties(zorm_mcp_server PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
zorm_mcp_bundle_runtime_deps(zorm_mcp_server)

# ---- e2e test ---------------------------------------------------------------

if(TARGET gtest)
  add_executable(mcp_test_zorm_mcp test_zorm_mcp.cpp)
  target_link_libraries(mcp_test_zorm_mcp PRIVATE mcp zormlib gtest gtest_main)
  target_include_directories(mcp_test_zorm_mcp PRIVATE ${ZORM_PUBLIC_INCLUDES})
  # The test spawns zorm_mcp_server.exe as its sibling - keep them together.
  set_target_properties(mcp_test_zorm_mcp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
  # The spawned server child needs the zorm runtime DLLs (libpq/sqlite3/
  # dmdpi/...). The test prepends this dir to its own PATH before CreateProcess
  # (ctest ENVIRONMENT escaping proved unreliable for PATH on Windows).
  if(WIN32)
    target_compile_definitions(mcp_test_zorm_mcp PRIVATE
        ZORM_DLL_DIR="${ZORM_DIR}/thirds/bin/win10")
  endif()
  add_test(NAME mcp_test_zorm_mcp COMMAND mcp_test_zorm_mcp
      WORKING_DIRECTORY $<TARGET_FILE_DIR:mcp_test_zorm_mcp>)

  # Unit tests for the pure helpers (no child process, no database).
  add_executable(mcp_test_zorm_unit
    test_zorm_unit.cpp
    src/json_util.cpp
    src/guide.cpp
    src/app.cpp)
  target_link_libraries(mcp_test_zorm_unit PRIVATE mcp zormlib gtest gtest_main)
  target_include_directories(mcp_test_zorm_unit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${ZORM_PUBLIC_INCLUDES})
  set_target_properties(mcp_test_zorm_unit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
  zorm_mcp_bundle_runtime_deps(mcp_test_zorm_unit)
  add_test(NAME mcp_test_zorm_unit COMMAND mcp_test_zorm_unit
      WORKING_DIRECTORY $<TARGET_FILE_DIR:mcp_test_zorm_unit>)
endif()

# ---- demo (the validation showcase this product was born from) --------------

set(ZORM_THIRDS_DLL_DIR "${ZORM_DIR}/thirds/bin/win10")
add_subdirectory(demo)
