# SnkInstall.cmake — install/export rules and the snake_sdk CMake package (EPIC-41, #108).
#
# `cmake --install <build> --prefix <dir>` lays out a self-contained SDK:
#
#   include/snk/<component>/...         public headers of the engine components
#                                       (<component>/include/snk/<component>/, see
#                                       snk_component_headers(), #109): include/ installs to include/
#   include/snk/lib3dparty/...          headers of the bundled submodule libraries (glm, imgui, ...)
#   include/snk/boost/                  the Boost headers the engine needs, merged into one tree
#   lib/snk/*.a                         static engine components and the bundled third-party
#                                       libraries (assimp, glfw, yaml-cpp, imgui, glad, Boost, ...)
#   lib/libsnk_engine.so                SNK_ENGINE_SHARED only
#   bin/snk_engine.dll, lib/snk/snk_engine.lib   SNK_ENGINE_SHARED on Windows
#   lib/cmake/snake_sdk/                snake_sdkConfig.cmake, ...ConfigVersion.cmake, ...Targets.cmake
#
# Consumers: find_package(snake_sdk) and link snk::engine + snk::engine_api. Third-party libraries
# are bundled: their targets are exported in the same set (snk::thirdparty_<name>), so a consumer
# needs nothing but the prefix (and Threads from the system).
#
# Nothing is installed unless `cmake --install` is run. Build `snk_sdk` first: it builds exactly
# the targets the install step copies (the default `all` target skips e.g. EXCLUDE_FROM_ALL Boost).
#
# Third-party targets come from submodules and FetchContent whose own install rules are off or
# not exportable (their include dirs point into the source tree), so this module rewrites their
# include directories into BUILD_INTERFACE/INSTALL_INTERFACE pairs from the outside.

include(CMakePackageConfigHelpers)

set(SNK_SDK_VERSION 0.2.0)
set(SNK_SDK_EXPORT_SET snake_sdk_targets)
set(SNK_SDK_CMAKE_DIR lib/cmake/snake_sdk)

option(SNK_INSTALL_SDK "Generate install/export rules for the snake_sdk package (cmake --install)" ON)

# Link-interface items of `tgt` that name targets. Static libraries also list their PRIVATE
# dependencies: CMake exports those as $<LINK_ONLY:...>, so they must be exported too.
function(_snk_sdk_link_deps out tgt)
	get_target_property(_type ${tgt} TYPE)
	set(_items "")
	get_target_property(_iface ${tgt} INTERFACE_LINK_LIBRARIES)
	if(_iface)
		list(APPEND _items ${_iface})
	endif()
	if(_type STREQUAL "STATIC_LIBRARY")
		get_target_property(_priv ${tgt} LINK_LIBRARIES)
		if(_priv)
			list(APPEND _items ${_priv})
		endif()
	endif()
	set(_res "")
	foreach(_item IN LISTS _items)
		# Build-tree-only items never reach the package.
		string(REGEX REPLACE "\\$<BUILD(_LOCAL)?_INTERFACE:[^>]*>" "" _item "${_item}")
		string(REGEX REPLACE "\\$<[A-Z_]+:|>" " " _item "${_item}")
		string(REGEX MATCHALL "[A-Za-z0-9_.+:-]+" _tokens "${_item}")
		foreach(_tok IN LISTS _tokens)
			if(TARGET ${_tok})
				list(APPEND _res ${_tok})
			endif()
		endforeach()
	endforeach()
	set(${out} "${_res}" PARENT_SCOPE)
endfunction()

function(_snk_sdk_real_target out name)
	get_target_property(_aliased ${name} ALIASED_TARGET)
	if(_aliased)
		set(${out} ${_aliased} PARENT_SCOPE)
	else()
		set(${out} ${name} PARENT_SCOPE)
	endif()
endfunction()

function(_snk_sdk_is_third_party out tgt)
	set(_res FALSE)
	if(tgt STREQUAL "imgui") # defined in the root scope by cmake/imgui.cmake
		set(_res TRUE)
	else()
		get_target_property(_dir ${tgt} SOURCE_DIR)
		set(_roots "${CMAKE_SOURCE_DIR}/lib3dparty")
		if(boost_SOURCE_DIR)
			list(APPEND _roots "${boost_SOURCE_DIR}")
		endif()
		foreach(_root IN LISTS _roots)
			string(FIND "${_dir}/" "${_root}/" _pos)
			if(_pos EQUAL 0)
				set(_res TRUE)
			endif()
		endforeach()
	endif()
	set(${out} ${_res} PARENT_SCOPE)
endfunction()

# Install-prefix-relative directory the headers of source directory `abs` are copied to.
function(_snk_sdk_include_dest out abs)
	set(_dest "")
	if(boost_SOURCE_DIR)
		string(FIND "${abs}/" "${boost_SOURCE_DIR}/" _pos)
		if(_pos EQUAL 0)
			# libs/<lib>/include of every Boost library merge into one boost/ tree.
			set(_dest include/snk/boost)
		endif()
	endif()
	if(NOT _dest)
		string(FIND "${abs}/" "${CMAKE_SOURCE_DIR}/" _pos)
		if(_pos EQUAL 0)
			file(RELATIVE_PATH _rel "${CMAKE_SOURCE_DIR}" "${abs}")
			if(_rel MATCHES "^core/.*/include$")
				# snk_component_headers(): include/snk/<name>/... lands at
				# <prefix>/include/snk/<name>/..., whichever component it comes from.
				set(_dest include)
			else()
				# Bundled third-party libraries keep their path in the repo.
				set(_dest include/snk/${_rel})
			endif()
		endif()
	endif()
	if(NOT _dest)
		string(FIND "${abs}/" "${CMAKE_BINARY_DIR}/" _pos)
		if(_pos EQUAL 0)
			file(RELATIVE_PATH _rel "${CMAKE_BINARY_DIR}" "${abs}")
			set(_dest include/snk/_generated/${_rel})
		endif()
	endif()
	set(${out} "${_dest}" PARENT_SCOPE)
endfunction()

# Rewrites the include directories of `tgt` so the target can be exported and installs their
# headers. Returns FALSE in `ok` when a directory lives outside the source and build trees.
function(_snk_sdk_prepare_includes tgt ok)
	set(${ok} TRUE PARENT_SCOPE)
	get_target_property(_inc ${tgt} INTERFACE_INCLUDE_DIRECTORIES)
	if(NOT _inc)
		return()
	endif()
	set(_new "")
	set(_new_sys "")
	foreach(_entry IN LISTS _inc)
		if(_entry MATCHES "^\\$<INSTALL_INTERFACE:")
			continue() # replaced below by the SDK layout
		endif()
		if(_entry MATCHES "^\\$<BUILD_INTERFACE:([^>]*)>$")
			set(_entry "${CMAKE_MATCH_1}")
		endif()
		if(_entry MATCHES "\\$<")
			list(APPEND _new "${_entry}")
			list(APPEND _new_sys "${_entry}")
			continue()
		endif()
		get_filename_component(_abs "${_entry}" ABSOLUTE)
		get_target_property(_internal ${tgt} SNK_SDK_INTERNAL_HEADERS)
		get_target_property(_src ${tgt} SOURCE_DIR)
		# include_base/ (snk_component_headers): base-only headers, never installed either.
		if((_internal AND _abs STREQUAL "${_src}/include") OR _abs STREQUAL "${_src}/include_base")
			# snk_component_headers(SDK_INTERNAL): in-tree consumers only, nothing installed or exported.
			list(APPEND _new "$<BUILD_INTERFACE:${_abs}>")
			list(APPEND _new_sys "$<BUILD_INTERFACE:${_abs}>")
			continue()
		endif()
		_snk_sdk_include_dest(_dest "${_abs}")
		if(NOT _dest)
			message(WARNING "snake_sdk: include directory of ${tgt} is outside the source/build tree: ${_abs}")
			set(${ok} FALSE PARENT_SCOPE)
			return()
		endif()
		list(APPEND _new "$<BUILD_INTERFACE:${_abs}>" "$<INSTALL_INTERFACE:${_dest}>")
		# CMake only prefixes relative INSTALL_INTERFACE paths for INTERFACE_INCLUDE_DIRECTORIES.
		list(APPEND _new_sys "$<BUILD_INTERFACE:${_abs}>" "$<INSTALL_INTERFACE:$<INSTALL_PREFIX>/${_dest}>")

		get_property(_done GLOBAL PROPERTY SNK_SDK_INSTALLED_HEADER_DIRS)
		if(NOT _abs IN_LIST _done)
			set_property(GLOBAL APPEND PROPERTY SNK_SDK_INSTALLED_HEADER_DIRS "${_abs}")
			set(_filter "")
			string(FIND "${_abs}/" "${CMAKE_SOURCE_DIR}/lib3dparty/" _in_3p)
			if(_in_3p EQUAL 0)
				# Submodule roots double as include dirs (glm): leave out tests, docs, samples.
				set(_filter PATTERN "test" EXCLUDE PATTERN "tests" EXCLUDE PATTERN "doc" EXCLUDE
					PATTERN "docs" EXCLUDE PATTERN "cmake" EXCLUDE PATTERN "examples" EXCLUDE
					PATTERN "samples" EXCLUDE)
			endif()
			install(DIRECTORY "${_abs}/" DESTINATION "${_dest}" COMPONENT snake_sdk
				FILES_MATCHING
				PATTERN "*.h" PATTERN "*.hpp" PATTERN "*.hh" PATTERN "*.hxx" PATTERN "*.inl" PATTERN "*.ipp"
				${_filter})
		endif()
	endforeach()
	set_property(TARGET ${tgt} PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${_new}")
	get_target_property(_sys ${tgt} INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)
	if(_sys)
		set_property(TARGET ${tgt} PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_new_sys}")
	endif()
endfunction()

# Replaces full paths of system libraries in the link items of `tgt` with bare names (/usr/lib/<arch>/librt.a
# -> rt). GLFW links the find_library() results of rt and m by full path; exported as is, they
# pin the SDK to the machine it was built on (#239). Paths inside the source and build trees are left alone.
function(_snk_sdk_bare_system_libs tgt)
	foreach(_prop INTERFACE_LINK_LIBRARIES LINK_LIBRARIES)
		get_target_property(_libs ${tgt} ${_prop})
		if(NOT _libs)
			continue()
		endif()
		set(_new "")
		set(_changed FALSE)
		foreach(_item IN LISTS _libs)
			if(_item MATCHES "^/" AND NOT _item MATCHES "^(${CMAKE_SOURCE_DIR}|${CMAKE_BINARY_DIR})/")
				get_filename_component(_name "${_item}" NAME_WE)
				string(REGEX REPLACE "^lib" "" _name "${_name}")
				if(_name)
					list(APPEND _new "${_name}")
					set(_changed TRUE)
					continue()
				endif()
			endif()
			list(APPEND _new "${_item}")
		endforeach()
		if(_changed)
			set_property(TARGET ${tgt} PROPERTY ${_prop} "${_new}")
		endif()
	endforeach()
endfunction()

# Call once, after the last add_subdirectory() and after snk_isolate_third_party_warnings().
function(snk_install_sdk)
	if(NOT SNK_INSTALL_SDK)
		return()
	endif()

	# 1. Everything the public targets need at build/link time, transitively.
	set(_todo snk::engine snk::engine_api)
	set(_closure "")
	set(_imported "")
	while(_todo)
		list(POP_FRONT _todo _name)
		_snk_sdk_real_target(_real ${_name})
		if(_real IN_LIST _closure OR _real IN_LIST _imported OR _real MATCHES "^snk_(sanitizers|compiler_warnings)$")
			continue()
		endif()
		get_target_property(_is_imported ${_real} IMPORTED)
		if(_is_imported)
			list(APPEND _imported ${_real})
			continue()
		endif()
		get_target_property(_type ${_real} TYPE)
		if(NOT _type MATCHES "^(STATIC_LIBRARY|SHARED_LIBRARY|INTERFACE_LIBRARY)$")
			message(WARNING "snake_sdk: ${_real} is a ${_type}; the SDK package is not generated")
			return()
		endif()
		list(APPEND _closure ${_real})
		_snk_sdk_link_deps(_deps ${_real})
		list(APPEND _todo ${_deps})
	endwhile()

	# Only Threads is found on the consumer side (find_dependency in the config file).
	foreach(_imp IN LISTS _imported)
		if(NOT _imp STREQUAL "Threads::Threads")
			message(WARNING "snake_sdk: depends on the imported target ${_imp} (system package?); the SDK package is not generated")
			return()
		endif()
	endforeach()

	# 2. Make every target exportable, then install it.
	foreach(_t IN LISTS _closure)
		_snk_sdk_prepare_includes(${_t} _ok)
		if(NOT _ok)
			message(WARNING "snake_sdk: the SDK package is not generated")
			return()
		endif()
		_snk_sdk_bare_system_libs(${_t})
		# Keep the exported names equal to the in-tree snk:: aliases.
		if(_t STREQUAL "snk_engine")
			set_property(TARGET ${_t} PROPERTY EXPORT_NAME engine)
		elseif(_t STREQUAL "snk_api")
			set_property(TARGET ${_t} PROPERTY EXPORT_NAME api)
		endif()
		_snk_sdk_is_third_party(_3p ${_t})
		if(_3p)
			set_property(TARGET ${_t} PROPERTY EXPORT_NAME thirdparty_${_t})
		endif()
	endforeach()

	install(TARGETS ${_closure} EXPORT ${SNK_SDK_EXPORT_SET}
		ARCHIVE DESTINATION lib/snk
		LIBRARY DESTINATION lib
		RUNTIME DESTINATION bin
		COMPONENT snake_sdk)
	install(EXPORT ${SNK_SDK_EXPORT_SET}
		NAMESPACE snk::
		FILE snake_sdkTargets.cmake
		DESTINATION ${SNK_SDK_CMAKE_DIR}
		COMPONENT snake_sdk)

	configure_package_config_file(
		"${CMAKE_SOURCE_DIR}/cmake/snake_sdkConfig.cmake.in"
		"${CMAKE_BINARY_DIR}/snake_sdkConfig.cmake"
		INSTALL_DESTINATION ${SNK_SDK_CMAKE_DIR})
	write_basic_package_version_file(
		"${CMAKE_BINARY_DIR}/snake_sdkConfigVersion.cmake"
		VERSION ${SNK_SDK_VERSION}
		COMPATIBILITY SameMajorVersion)
	install(FILES
		"${CMAKE_BINARY_DIR}/snake_sdkConfig.cmake"
		"${CMAKE_BINARY_DIR}/snake_sdkConfigVersion.cmake"
		DESTINATION ${SNK_SDK_CMAKE_DIR}
		COMPONENT snake_sdk)

	# The same package for this build tree, in <build>/snake_sdk (#434): an editor run from the build tree builds a
	# project's game module against it without `cmake --install`. Paths point into the build and source trees.
	set(_tree_dir "${CMAKE_BINARY_DIR}/snake_sdk")
	export(EXPORT ${SNK_SDK_EXPORT_SET}
		NAMESPACE snk::
		FILE "${_tree_dir}/snake_sdkTargets.cmake")
	configure_package_config_file(
		"${CMAKE_SOURCE_DIR}/cmake/snake_sdkConfig.cmake.in"
		"${_tree_dir}/snake_sdkConfig.cmake"
		INSTALL_DESTINATION "${_tree_dir}"
		INSTALL_PREFIX "${CMAKE_BINARY_DIR}")
	configure_file("${CMAKE_BINARY_DIR}/snake_sdkConfigVersion.cmake" "${_tree_dir}/snake_sdkConfigVersion.cmake" COPYONLY)

	# Builds exactly what `cmake --install` copies (INTERFACE libraries have nothing to build).
	set(_buildable "")
	foreach(_t IN LISTS _closure)
		get_target_property(_type ${_t} TYPE)
		if(NOT _type STREQUAL "INTERFACE_LIBRARY")
			list(APPEND _buildable ${_t})
		endif()
	endforeach()
	add_custom_target(snk_sdk DEPENDS ${_buildable})
	message(STATUS "snake_sdk ${SNK_SDK_VERSION}: ${CMAKE_INSTALL_PREFIX} (cmake --install after building snk_sdk)")
endfunction()
