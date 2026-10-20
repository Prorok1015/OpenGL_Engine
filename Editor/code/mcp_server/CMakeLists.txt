cmake_minimum_required(VERSION 3.16)
project(snk_mcp_server)

# The MCP server of the editor (docs/roadmap/design/AI_INTEGRATION.md section 3.7, decision 7): a STATIC module with
# no dependency on the engine. It uses Boost.JSON and Boost.Asio and nothing else of the project, so it cannot
# end up in the game binary or in the SDK. Only the editor (editor_system) and its own tests link it;
# tools/ci/check_mcp_isolation.sh and cmake/SnkMcpIsolation.cmake keep it that way.
add_library(snk_mcp_server STATIC)
add_library(snk::mcp_server ALIAS snk_mcp_server)

file(GLOB SNK_MCP_SERVER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(snk_mcp_server PRIVATE ${SNK_MCP_SERVER_SOURCES})
snk_component_headers(snk_mcp_server)

find_package(Threads REQUIRED)
# snk::sanitizers and snk::compiler_warnings are build flags, not engine code.
target_link_libraries(snk_mcp_server
	PUBLIC Boost::json
	PRIVATE Boost::asio Threads::Threads snk::sanitizers snk::compiler_warnings)
if(WIN32)
	# Sockets (asio links them through #pragma comment on MSVC; this keeps other toolchains and the consumers honest)
	# and the owner-only access list of the token file (advapi32).
	target_link_libraries(snk_mcp_server PUBLIC ws2_32 mswsock advapi32)
endif()
