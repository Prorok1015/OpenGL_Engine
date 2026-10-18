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

find_package(snake_sdk 0.2 CONFIG REQUIRED)
message(STATUS "snake_sdk ${snake_sdk_VERSION} (shared engine: ${snake_sdk_ENGINE_SHARED}) from ${snake_sdk_DIR}")

add_executable(package_consumer main.cpp)
target_link_libraries(package_consumer PRIVATE snk::engine snk::engine_api)

# Header self-containment (#109): one translation unit per installed header of the components that
# live under include/snk/<name>/, compiled against the installed SDK. Off by default; the SDK CI
# jobs turn it on. Add a component to SNK_CONSUMER_HEADER_COMPONENTS when it gains public headers.
option(SNK_CONSUMER_HEADER_CHECK "Compile one TU per installed public header of the migrated components" OFF)
set(SNK_CONSUMER_HEADER_COMPONENTS application core_input window driver ecs desc resource common config input image game gui render scene core engine)
if(SNK_CONSUMER_HEADER_CHECK)
	get_filename_component(_sdk_prefix "${snake_sdk_DIR}/../../.." ABSOLUTE)
	if(NOT IS_DIRECTORY "${_sdk_prefix}/include/snk")
		message(FATAL_ERROR "SDK prefix ${_sdk_prefix} (derived from ${snake_sdk_DIR}, expected <prefix>/lib/cmake/snake_sdk) has no include/snk")
	endif()
	set(_header_tus "")
	set(_idx 0)
	foreach(_comp IN LISTS SNK_CONSUMER_HEADER_COMPONENTS)
		file(GLOB_RECURSE _headers RELATIVE "${_sdk_prefix}/include" "${_sdk_prefix}/include/snk/${_comp}/*")
		list(SORT _headers)
		if(NOT _headers)
			message(FATAL_ERROR "no installed headers under ${_sdk_prefix}/include/snk/${_comp}")
		endif()
		foreach(_hdr IN LISTS _headers)
			string(MAKE_C_IDENTIFIER "${_hdr}" _id)
			set(_tu "${CMAKE_CURRENT_BINARY_DIR}/header_tus/${_idx}_${_id}.cpp")
			file(GENERATE OUTPUT "${_tu}" CONTENT "#include <${_hdr}>\n")
			list(APPEND _header_tus "${_tu}")
			math(EXPR _idx "${_idx} + 1")
		endforeach()
	endforeach()
	message(STATUS "header self-containment: ${_idx} headers")
	add_library(package_consumer_headers OBJECT ${_header_tus})
	target_link_libraries(package_consumer_headers PRIVATE snk::engine snk::engine_api)
endif()

if(WIN32 AND snake_sdk_ENGINE_SHARED)
	# Windows has no rpath: snk_engine.dll (installed to <prefix>/bin) must sit next to the executable.
	add_custom_command(TARGET package_consumer POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:package_consumer> $<TARGET_FILE_DIR:package_consumer>
		COMMAND_EXPAND_LISTS)
endif()
