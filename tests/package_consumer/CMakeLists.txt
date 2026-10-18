# Minimal out-of-tree consumer of the installed SDK (CI job "SDK package"): proves that
# find_package(snake_sdk) resolves and that snk::engine + snk::engine_api compile, link and run
# without the engine source tree or its third-party submodules.
#
#   cmake -S tests/package_consumer -B build/consumer -DCMAKE_PREFIX_PATH=<sdk prefix> \
#         -DCMAKE_BUILD_TYPE=<the configuration the SDK was built with>
cmake_minimum_required(VERSION 3.24)
project(snake_sdk_consumer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(snake_sdk 0.1 CONFIG REQUIRED)
message(STATUS "snake_sdk ${snake_sdk_VERSION} (shared engine: ${snake_sdk_ENGINE_SHARED}) from ${snake_sdk_DIR}")

add_executable(package_consumer main.cpp)
target_link_libraries(package_consumer PRIVATE snk::engine snk::engine_api)

if(WIN32 AND snake_sdk_ENGINE_SHARED)
	# Windows has no rpath: snk_engine.dll (installed to <prefix>/bin) must sit next to the executable.
	add_custom_command(TARGET package_consumer POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:package_consumer> $<TARGET_FILE_DIR:package_consumer>
		COMMAND_EXPAND_LISTS)
endif()
