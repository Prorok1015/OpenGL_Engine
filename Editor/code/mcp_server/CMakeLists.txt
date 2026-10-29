cmake_minimum_required(VERSION 3.16)
project(snk_mcp_server)

# The MCP server of the editor (docs/roadmap/design/AI_INTEGRATION.md section 3.7, decision 7).
#
# It depends on NOTHING from the engine: no snk::common, no egLOG, no ds::task_system. Only Boost.JSON,
# Boost.Asio and Boost.Beast (plus the build-wide sanitizer and warning flags). The editor implements mcp::host
# (edt::mcp_bridge_host) and links this target; the engine, the game and the SDK never do. That is checked in CI:
# tools/ci/check_mcp_server_isolation.sh and the configure-time check in the root CMakeLists.txt.
add_library(snk_mcp_server STATIC)
add_library(snk::mcp_server ALIAS snk_mcp_server)

file(GLOB SNK_MCP_SERVER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(snk_mcp_server PRIVATE ${SNK_MCP_SERVER_SOURCES})
target_include_directories(snk_mcp_server PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

find_package(Threads REQUIRED)
# boost::json::value is part of the public headers; asio and beast stay inside the .cpp files.
target_link_libraries(snk_mcp_server
	PUBLIC Boost::json $<BUILD_INTERFACE:snk::sanitizers>
	PRIVATE Boost::asio Boost::beast Threads::Threads $<BUILD_INTERFACE:snk::compiler_warnings>)

if(WIN32)
	# Asio wants the target Windows version; NOMINMAX keeps windows.h from defining min/max.
	target_compile_definitions(snk_mcp_server PRIVATE _WIN32_WINNT=0x0A00 NOMINMAX)
	target_link_libraries(snk_mcp_server PRIVATE ws2_32 mswsock)
endif()
if(MSVC)
	# Beast/Asio instantiate enough templates to overflow the default section count.
	target_compile_options(snk_mcp_server PRIVATE /bigobj)
endif()
