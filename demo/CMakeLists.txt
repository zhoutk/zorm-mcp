# demo/CMakeLists.txt - zorm showcase executables.
#
# zorm is the protagonist; mcp-cpp is the third-party MCP protocol library
# it links (like any other consumer would). Included by the parent zorm-mcp
# CMakeLists, which resolves the thirds/ submodules and ZORM_THIRDS_DLL_DIR.
#
# Targets:
#   zorm_server             single-backend minimal MCP server (four tools)
#   zorm_analytics_server   multi-backend "Zorm Analytics" MCP demo
#   mcp_test_zorm_analytics e2e test (spawns the analytics server as a child)

# ---- zorm_server: single-backend minimal example ---------------------------

add_executable(zorm_server zorm_server.cpp)
target_link_libraries(zorm_server PRIVATE mcp zormlib)
target_include_directories(zorm_server PRIVATE ${ZORM_PUBLIC_INCLUDES})

# ---- zorm_analytics_server: the multi-backend demo --------------------------

add_executable(zorm_analytics_server
  zorm_analytics_server.cpp
  demo_data.cpp
  demo_data.h)
target_link_libraries(zorm_analytics_server PRIVATE mcp zormlib)
target_include_directories(zorm_analytics_server PRIVATE ${ZORM_PUBLIC_INCLUDES})

# Final executables go to the project root bin/.
set_target_properties(zorm_server zorm_analytics_server PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# Runtime DLLs (libpq/sqlite3/dmdpi/...) must resolve when an AI client
# spawns the servers - Claude Code does NOT have run-test's PATH setup.
# Copy them next to the executables (Windows searches the exe dir first).
zorm_mcp_bundle_runtime_deps(zorm_server)
zorm_mcp_bundle_runtime_deps(zorm_analytics_server)

# ---- e2e test ---------------------------------------------------------------

if(TARGET gtest)
  add_executable(mcp_test_zorm_analytics test_zorm_analytics.cpp)
  target_link_libraries(mcp_test_zorm_analytics PRIVATE mcp zormlib gtest gtest_main)
  target_include_directories(mcp_test_zorm_analytics PRIVATE ${ZORM_PUBLIC_INCLUDES})
  set_target_properties(mcp_test_zorm_analytics PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
  # The spawned server child needs the zorm runtime DLLs (libpq/sqlite3/
  # dmdpi/...). The test prepends this dir to its own PATH before CreateProcess
  # (ctest ENVIRONMENT escaping proved unreliable for PATH on Windows).
  if(WIN32)
    target_compile_definitions(mcp_test_zorm_analytics PRIVATE
        ZORM_DLL_DIR="${ZORM_THIRDS_DLL_DIR}")
  endif()
  add_test(NAME mcp_test_zorm_analytics COMMAND mcp_test_zorm_analytics
      WORKING_DIRECTORY $<TARGET_FILE_DIR:mcp_test_zorm_analytics>)
endif()
