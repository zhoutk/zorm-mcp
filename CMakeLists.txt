# ---------------------------------------------------------------------------
# zorm-mcp - the production multi-backend database MCP server (standalone).
#
# Build (expects the two sibling checkouts next to this repo):
#     cmake -S . -B build -G Ninja
#     cmake --build build
#     ctest --test-dir build
#
#   ../zorm      provides the zormlib static library, the runtime-DLL
#                bundling helper and the bundled backend DLLs
#   ../mcp-cpp   provides the mcp protocol library (self-contained SDK)
#
# Targets:
#   zorm_mcp_server      the MCP server (stdio by default, --http optional)
#   mcp_test_zorm_mcp    e2e test (spawns the server as a child over real pipes)
#   demo targets         the validation showcase (demo/), optional
# ---------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.18)
project(zorm-mcp VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_testing()

set(ZORM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../zorm" CACHE PATH "zorm checkout (provides zormlib)")
set(MCP_CPP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../mcp-cpp" CACHE PATH "mcp-cpp checkout (provides mcp)")

if(NOT EXISTS "${ZORM_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "zorm not found at ${ZORM_DIR} - clone it next to this repo")
endif()
if(NOT EXISTS "${MCP_CPP_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "mcp-cpp not found at ${MCP_CPP_DIR} - clone it next to this repo")
endif()

# ---- googletest (added first so both mcp-cpp and the e2e test reuse it) ----
add_subdirectory(${ZORM_DIR}/thirds/googletest-1.18.0 gtest-build EXCLUDE_FROM_ALL)

# ---- mcp-cpp (self-contained SDK; bundled zjson/httplib under its thirds/) --
add_subdirectory(${MCP_CPP_DIR} mcp-cpp-build)

# ---- zorm core (library only; skip zorm's own demo exe concerns is fine) ----
set(CMAKE_ZJSON_ENABLE_TEST OFF)
add_subdirectory(${ZORM_DIR} zorm-build)

# zorm's public surface + zjson (zormlib does not export include dirs) -
# exported once here so every consumer target (server, demo, e2e test) links
# zormlib and gets the right include paths.
target_include_directories(zormlib INTERFACE
  ${ZORM_DIR}/src/include
  ${ZORM_DIR}/thirds/zjson)

# ---- runtime DLL bundling ---------------------------------------------------
# Redefinition of zorm's helper so it resolves against the SIBLING zorm
# checkout (zorm's own copy hardcodes ${CMAKE_SOURCE_DIR}, which points here
# in a standalone build). MCP servers are spawned by an AI client, not by a
# test runner: the child gets the client's environment, which has no MSYS2
# clang64 PATH - the full dependency closure must sit next to the exe.
function(zorm_bundle_runtime_deps target)
  if(NOT WIN32)
    return()
  endif()
  get_filename_component(_compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
  add_custom_command(TARGET ${target} POST_BUILD
      COMMAND "${CMAKE_COMMAND}"
              "-DZORM_TARGET_EXE=$<TARGET_FILE:${target}>"
              "-DZORM_THIRDS_DLL_DIR=${ZORM_DIR}/thirds/bin/win10"
              "-DZORM_COMPILER_BIN=${_compiler_bin}"
              -P "${ZORM_DIR}/cmake/zorm_bundle_runtime_deps.cmake"
      COMMENT "Bundling runtime DLLs next to $<TARGET_FILE_NAME:${target}>"
      VERBATIM)
endfunction()

# ---- backend link dirs (zorm's own link_directories don't reach this scope) -
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
# zorm's public surface + zjson (zormlib does not export include dirs).
target_include_directories(zorm_mcp_server PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${ZORM_DIR}/src/include
  ${ZORM_DIR}/thirds/zjson)
set_target_properties(zorm_mcp_server PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
zorm_bundle_runtime_deps(zorm_mcp_server)

# ---- e2e test ---------------------------------------------------------------

if(TARGET gtest)
  add_executable(mcp_test_zorm_mcp test_zorm_mcp.cpp)
  target_link_libraries(mcp_test_zorm_mcp PRIVATE mcp zormlib gtest gtest_main)
  # The test spawns zorm_mcp_server.exe as its sibling - keep them together.
  set_target_properties(mcp_test_zorm_mcp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  # The spawned server child needs the zorm runtime DLLs (libpq/sqlite3/
  # dmdpi/...). The test prepends this dir to its own PATH before CreateProcess
  # (ctest ENVIRONMENT escaping proved unreliable for PATH on Windows).
  if(WIN32)
    target_compile_definitions(mcp_test_zorm_mcp PRIVATE
        ZORM_DLL_DIR="${ZORM_DIR}/thirds/bin/win10")
  endif()
  add_test(NAME mcp_test_zorm_mcp COMMAND mcp_test_zorm_mcp
      WORKING_DIRECTORY $<TARGET_FILE_DIR:mcp_test_zorm_mcp>)
endif()

# ---- demo (the validation showcase this product was born from) --------------

# demo/CMakeLists builds zorm_server / zorm_analytics_server and their e2e
# test; it needs the same DLL-dir variable the standalone build resolves
# against the sibling zorm checkout.
set(ZORM_THIRDS_DLL_DIR "${ZORM_DIR}/thirds/bin/win10")
add_subdirectory(demo)
