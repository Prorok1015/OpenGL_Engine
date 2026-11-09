# SnkInstall.cmake — install/export rules and the snake_sdk CMake package (EPIC-41, #108).
#
# `cmake --install <build> --prefix <dir>` lays out a self-contained SDK:
#
#   include/snk/<component>/...         public headers of the engine components
#                                       (<component>/include/snk/<component>/, see
#                                       snk_component_headers(), #109): include/ installs to include/
#   include/snk/lib3dparty/...          headers of the bundled submodule libraries (glm, imgui, ...)
#   include/snk/boost/                  the Boost headers the engine needs, merged into one tree
#   lib/snk/*.lib                       a shared build on Windows: Boost.JSON and what it links, for game modules
#                                       (the components are inside snk_engine and exported usage-only, #581)
#   lib/snk/static/*.a                  a static build: the components and bundled third-party libraries
#                                       (assimp, glfw, yaml-cpp, imgui, glad, Boost, ...)
#   lib/libsnk_engine.so                a shared build
#   bin/snk_engine.dll, lib/snk/snk_engine.lib   a shared build on Windows
#   lib/cmake/snake_sdk/                snake_sdkConfig.cmake, ...ConfigVersion.cmake,
#                                       snake_sdk_{shared,static}Targets[-<config>].cmake,
#                                       SnkGameHost.cmake (a static build: snk_add_game_host, #592)
#
# One prefix holds both engines, installed from two builds of the same sources (#223): a shared build
# exports snk::engine (the editor and game modules link it with snk::engine_api; every other target of the set is an
# INTERFACE facade with the usage requirements only, see _snk_sdk_make_facade), a static build
# exports snk::engine_static and snk::game_host (Shipping, #23, #592). The two builds compile the same component sources
# differently (SNK_API dllexport vs nothing), so each has its own export set, file names and target
# names (snk::static_<name>): installing one never overwrites the other. Debug libraries carry a "d"
# (CMAKE_DEBUG_POSTFIX), so a Debug and a Release install share the prefix too. Third-party libraries
# are bundled: their targets are exported in the same sets (snk::thirdparty_<name>), so a consumer
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
set(SNK_SDK_CMAKE_DIR lib/cmake/snake_sdk)
if(SNK_ENGINE_SHARED)
	set(SNK_SDK_KIND shared)
else()
	set(SNK_SDK_KIND static)
endif()
set(SNK_SDK_EXPORT_SET snake_sdk_${SNK_SDK_KIND}_targets)
set(SNK_SDK_TARGETS_FILE snake_sdk_${SNK_SDK_KIND}Targets.cmake)

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
	# The build tree before the source tree: the presets put it inside the source tree (build/<preset>), and a
	# generated header directory must land in the same place whichever configuration built it. Sound only because the
	# build tree never holds the sources: the root CMakeLists.txt refuses an in-source configure (#582).
	if(NOT _dest)
		string(FIND "${abs}/" "${CMAKE_BINARY_DIR}/" _pos)
		if(_pos EQUAL 0)
			file(RELATIVE_PATH _rel "${CMAKE_BINARY_DIR}" "${abs}")
			set(_dest include/snk/_generated/${_rel})
		endif()
	endif()
	if(NOT _dest)
		string(FIND "${abs}/" "${CMAKE_SOURCE_DIR}/" _pos)
		if(_pos EQUAL 0)
			file(RELATIVE_PATH _rel "${CMAKE_SOURCE_DIR}" "${abs}")
			if(_rel MATCHES "^(core/.*|apps/game/host)/include$" OR _rel STREQUAL "Editor/code/editor_system/include")
				# snk_component_headers(): include/snk/<name>/... lands at
				# <prefix>/include/snk/<name>/..., whichever component it comes from.
				set(_dest include)
			else()
				# Bundled third-party libraries keep their path in the repo.
				set(_dest include/snk/${_rel})
			endif()
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

# The shared set exports usage-only stand-ins of the components (#581). snk_engine carries their code, and a
# consumer reaches them only through snk::engine_api's $<COMPILE_ONLY:...>, so their archives would be installed and
# never linked (~1.5 GiB in the Windows Release package). Each target of the export closure but snk_engine and the
# archives a module links itself becomes an INTERFACE facade with the same exported name: its include directories,
# compile definitions, options and features, and its interface links remapped to other facades, without a link
# step. The real targets are not touched: the build tree links as before.
function(_snk_sdk_facade_name out tgt)
	set(${out} "snk_sdk_facade_${tgt}" PARENT_SCOPE)
endfunction()

# One INTERFACE_LINK_LIBRARIES item of a facade. `faceted`: the targets that have a facade; `kept`: the real
# targets exported as they are (their archives stay). A facade never links: $<LINK_ONLY:...> and plain library names
# are dropped, a kept archive is used through $<COMPILE_ONLY:...>, and an item that was $<COMPILE_ONLY:...> stays
# so: unwrapped, the consumer would inherit the plain links of what it names (Threads::Threads, ...).
function(_snk_sdk_facade_link_item out item faceted kept)
	set(${out} "" PARENT_SCOPE)
	if(item STREQUAL "" OR item MATCHES "^\\$<LINK_ONLY:")
		return()
	endif()
	set(_wrapped FALSE)
	if(item MATCHES "^\\$<COMPILE_ONLY:([^$<>]+)>$")
		set(item "${CMAKE_MATCH_1}")
		set(_wrapped TRUE)
	endif()
	if(item MATCHES "\\$<")
		# The build-tree-only ones vanish on export. Any other (nested $<COMPILE_ONLY:$<...>>, $<$<CONFIG:..>:tgt>, ...)
		# would be exported unmapped and could link an archive, so it is refused until the mapping handles it.
		if(NOT item MATCHES "^\\$<BUILD(_LOCAL)?_INTERFACE:")
			message(FATAL_ERROR "snake_sdk: a facade cannot map the link item '${item}' (#581): only a target, "
				"$<COMPILE_ONLY:target>, $<LINK_ONLY:...> and $<BUILD_INTERFACE:...> are understood")
		endif()
		return()
	endif()
	if(NOT TARGET ${item})
		return() # a system library: the facade does not link
	endif()
	_snk_sdk_real_target(_real ${item})
	if(_real IN_LIST faceted)
		_snk_sdk_facade_name(_facade ${_real})
		if(_wrapped)
			set(${out} "$<COMPILE_ONLY:${_facade}>" PARENT_SCOPE)
		else()
			set(${out} "${_facade}" PARENT_SCOPE)
		endif()
	elseif(_real IN_LIST kept)
		get_target_property(_type ${_real} TYPE)
		if(_type STREQUAL "INTERFACE_LIBRARY" AND NOT _wrapped)
			set(${out} "${_real}" PARENT_SCOPE)
		else()
			set(${out} "$<COMPILE_ONLY:${_real}>" PARENT_SCOPE)
		endif()
	else()
		# An imported target is found on the consumer side (Threads::Threads); any other is build-only (sanitizers).
		get_target_property(_imported ${_real} IMPORTED)
		if(_imported AND _wrapped)
			set(${out} "$<COMPILE_ONLY:${item}>" PARENT_SCOPE)
		elseif(_imported)
			set(${out} "${item}" PARENT_SCOPE)
		endif()
	endif()
endfunction()

# A property value as get_target_property gives it is set when it is neither unset (<var>-NOTFOUND) nor empty;
# `if(<var>)` would also drop the values CMake reads as false (0, OFF, N, ...).
function(_snk_sdk_property_set out value)
	if("${value}" STREQUAL "" OR "${value}" MATCHES "-NOTFOUND$")
		set(${out} FALSE PARENT_SCOPE)
	else()
		set(${out} TRUE PARENT_SCOPE)
	endif()
endfunction()

function(_snk_sdk_make_facade tgt faceted kept)
	# Every usage requirement a consumer can inherit, but INTERFACE_LINK_LIBRARIES (remapped below). The link-side
	# ones reach a consumer only through a plain link to the facade, as they reached it through the real target.
	set(_properties
		INTERFACE_INCLUDE_DIRECTORIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES INTERFACE_COMPILE_DEFINITIONS
		INTERFACE_COMPILE_OPTIONS INTERFACE_COMPILE_FEATURES INTERFACE_PRECOMPILE_HEADERS INTERFACE_SOURCES
		INTERFACE_POSITION_INDEPENDENT_CODE INTERFACE_LINK_OPTIONS INTERFACE_LINK_DIRECTORIES INTERFACE_LINK_DEPENDS
		INTERFACE_AUTOUIC_OPTIONS INTERFACE_AUTOMOC_MACRO_NAMES COMPATIBLE_INTERFACE_BOOL COMPATIBLE_INTERFACE_STRING
		COMPATIBLE_INTERFACE_NUMBER_MIN COMPATIBLE_INTERFACE_NUMBER_MAX EXPORT_PROPERTIES)
	# Custom properties a target exports (EXPORT_PROPERTIES) go along with it.
	get_target_property(_exported ${tgt} EXPORT_PROPERTIES)
	_snk_sdk_property_set(_has_exported "${_exported}")
	if(_has_exported)
		list(APPEND _properties ${_exported})
	endif()
	# A COMPATIBLE_INTERFACE_* list names properties whose values a consumer compares as INTERFACE_<name>.
	foreach(_kind BOOL STRING NUMBER_MIN NUMBER_MAX)
		get_target_property(_compatible ${tgt} COMPATIBLE_INTERFACE_${_kind})
		_snk_sdk_property_set(_has_compatible "${_compatible}")
		if(_has_compatible)
			foreach(_name IN LISTS _compatible)
				list(APPEND _properties INTERFACE_${_name})
			endforeach()
		endif()
	endforeach()
	list(REMOVE_DUPLICATES _properties)
	_snk_sdk_facade_name(_facade ${tgt})
	add_library(${_facade} INTERFACE)
	get_target_property(_name ${tgt} EXPORT_NAME)
	_snk_sdk_property_set(_has_name "${_name}")
	if(NOT _has_name)
		set(_name ${tgt})
	endif()
	set_property(TARGET ${_facade} PROPERTY EXPORT_NAME ${_name})
	foreach(_prop IN LISTS _properties)
		get_target_property(_value ${tgt} ${_prop})
		_snk_sdk_property_set(_has "${_value}")
		if(_has)
			set_property(TARGET ${_facade} PROPERTY ${_prop} "${_value}")
		endif()
	endforeach()
	get_target_property(_links ${tgt} INTERFACE_LINK_LIBRARIES)
	_snk_sdk_property_set(_has_links "${_links}")
	set(_new "")
	if(_has_links)
		foreach(_item IN LISTS _links)
			_snk_sdk_facade_link_item(_mapped "${_item}" "${faceted}" "${kept}")
			if(NOT "${_mapped}" STREQUAL "")
				list(APPEND _new "${_mapped}")
			endif()
		endforeach()
	endif()
	if(NOT "${_new}" STREQUAL "")
		list(REMOVE_DUPLICATES _new)
		set_property(TARGET ${_facade} PROPERTY INTERFACE_LINK_LIBRARIES "${_new}")
	endif()
endfunction()

# Call once, after the last add_subdirectory() and after snk_isolate_third_party_warnings().
function(snk_install_sdk)
	if(NOT SNK_INSTALL_SDK)
		return()
	endif()

	# 1. Everything the public targets need at build/link time, transitively.
	set(_todo snk::engine snk::engine_api)
	# The shipping host library (#592) exists in the static build only.
	if(NOT SNK_ENGINE_SHARED AND TARGET snk::game_host)
		list(APPEND _todo snk::game_host)
	endif()
	if(SNK_ENGINE_SHARED AND TARGET snk::editor_api)
		# The registries a game_editor module registers into (#587): a shared library of its own, next to snk_engine.
		list(APPEND _todo snk::editor_api)
	endif()
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
		elseif(_t STREQUAL "snk_editor_api")
			set_property(TARGET ${_t} PROPERTY EXPORT_NAME editor_api)
		endif()
		_snk_sdk_is_third_party(_3p ${_t})
		if(_3p)
			set_property(TARGET ${_t} PROPERTY EXPORT_NAME thirdparty_${_t})
		endif()
		if(NOT SNK_ENGINE_SHARED)
			if(_t STREQUAL "engine")
				set_property(TARGET ${_t} PROPERTY EXPORT_NAME engine_static)
			elseif(_t STREQUAL "game_host")
				# Only in the static set, so no static_ prefix: snk_add_game_host links snk::game_host in-tree and
				# from the SDK alike.
				set_property(TARGET ${_t} PROPERTY EXPORT_NAME game_host)
			else()
				get_target_property(_name ${_t} EXPORT_NAME)
				if(NOT _name)
					set(_name ${_t})
				endif()
				set_property(TARGET ${_t} PROPERTY EXPORT_NAME static_${_name})
			endif()
		endif()
	endforeach()

	# 3. The shared set: snk_engine, what a module links itself, and facades for the rest (see _snk_sdk_make_facade).
	if(SNK_ENGINE_SHARED)
		set(_archive_dir lib/snk)
		# MSVC: snk_engine.dll exports only SNK_API, so a module that instantiates the desc (de)serialization
		# templates links its own Boost.JSON (SPIKE-218 §3.6, as tests/module_dll does). On Linux the engine's copy
		# is used. Kept with everything it links (Boost.Container and header-only Boost).
		set(_kept "")
		if(MSVC)
			set(_kept_todo Boost::json)
			while(_kept_todo)
				list(POP_FRONT _kept_todo _name)
				_snk_sdk_real_target(_real ${_name})
				if(_real IN_LIST _kept OR NOT _real IN_LIST _closure)
					continue()
				endif()
				list(APPEND _kept ${_real})
				_snk_sdk_link_deps(_deps ${_real})
				list(APPEND _kept_todo ${_deps})
			endwhile()
		endif()
		if(snk_editor_api IN_LIST _closure)
			list(APPEND _kept snk_editor_api)
		endif()
		set(_faceted "")
		foreach(_t IN LISTS _closure)
			if(NOT _t STREQUAL "snk_engine" AND NOT _t IN_LIST _kept)
				list(APPEND _faceted ${_t})
			endif()
		endforeach()
		set(_export snk_engine ${_kept})
		foreach(_t IN LISTS _faceted)
			_snk_sdk_make_facade(${_t} "${_faceted}" "${_kept}")
			_snk_sdk_facade_name(_facade ${_t})
			list(APPEND _export ${_facade})
		endforeach()
	else()
		set(_archive_dir lib/snk/static)
		set(_export ${_closure})
	endif()
	# COMPONENT after an artifact kind applies to that kind only: each kind names it, or `--component snake_sdk`
	# leaves the libraries out.
	install(TARGETS ${_export} EXPORT ${SNK_SDK_EXPORT_SET}
		ARCHIVE DESTINATION ${_archive_dir} COMPONENT snake_sdk
		LIBRARY DESTINATION lib COMPONENT snake_sdk
		RUNTIME DESTINATION bin COMPONENT snake_sdk)
	install(EXPORT ${SNK_SDK_EXPORT_SET}
		NAMESPACE snk::
		FILE ${SNK_SDK_TARGETS_FILE}
		DESTINATION ${SNK_SDK_CMAKE_DIR}
		COMPONENT snake_sdk)
	if(MSVC AND SNK_ENGINE_SHARED)
		install(FILES $<TARGET_PDB_FILE:snk_engine> DESTINATION bin OPTIONAL COMPONENT snake_sdk)
		if(TARGET snk_editor_api)
			install(FILES $<TARGET_PDB_FILE:snk_editor_api> DESTINATION bin OPTIONAL COMPONENT snake_sdk)
		endif()
		# The Microsoft C++ runtime next to snk_engine.dll and editor.exe (R11, #223); Shipping (#23) copies it next
		# to the game. Not for Debug: the debug runtime is not redistributable. No UCRT: it is part of Windows 10+.
		# The shared build only, so the static install into the same prefix does not write the same files again.
		if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
			set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION bin)
			set(CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT snake_sdk)
			set(CMAKE_INSTALL_DEBUG_LIBRARIES OFF)
			set(CMAKE_INSTALL_UCRT_LIBRARIES OFF)
			include(InstallRequiredSystemLibraries)
			if(NOT CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
				message(WARNING "snake_sdk: InstallRequiredSystemLibraries found no MSVC runtime to install")
			endif()
		endif()
	endif()

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
	# snk_add_game_host() for a consumer of snk::game_host; the config includes it when the static set is there. The
	# static build alone installs it, so the shared install into the same prefix writes nothing twice.
	if(NOT SNK_ENGINE_SHARED AND TARGET game_host)
		install(FILES "${CMAKE_SOURCE_DIR}/cmake/SnkGameHost.cmake" DESTINATION ${SNK_SDK_CMAKE_DIR} COMPONENT snake_sdk)
	endif()

	# The same package for this build tree, in <build>/snake_sdk (#434): an editor run from the build tree builds a
	# project's game module against it without `cmake --install`. Paths point into the build and source trees.
	set(_tree_dir "${CMAKE_BINARY_DIR}/snake_sdk")
	export(EXPORT ${SNK_SDK_EXPORT_SET}
		NAMESPACE snk::
		FILE "${_tree_dir}/${SNK_SDK_TARGETS_FILE}")
	configure_package_config_file(
		"${CMAKE_SOURCE_DIR}/cmake/snake_sdkConfig.cmake.in"
		"${_tree_dir}/snake_sdkConfig.cmake"
		INSTALL_DESTINATION "${_tree_dir}"
		INSTALL_PREFIX "${CMAKE_BINARY_DIR}")
	configure_file("${CMAKE_BINARY_DIR}/snake_sdkConfigVersion.cmake" "${_tree_dir}/snake_sdkConfigVersion.cmake" COPYONLY)
	if(NOT SNK_ENGINE_SHARED AND TARGET game_host)
		configure_file("${CMAKE_SOURCE_DIR}/cmake/SnkGameHost.cmake" "${_tree_dir}/SnkGameHost.cmake" COPYONLY)
	endif()

	# Builds exactly what `cmake --install` copies (INTERFACE libraries have nothing to build).
	set(_buildable "")
	foreach(_t IN LISTS _export)
		get_target_property(_type ${_t} TYPE)
		if(NOT _type STREQUAL "INTERFACE_LIBRARY")
			list(APPEND _buildable ${_t})
		endif()
	endforeach()
	add_custom_target(snk_sdk DEPENDS ${_buildable})
	message(STATUS "snake_sdk ${SNK_SDK_VERSION} (${SNK_SDK_KIND} engine): ${CMAKE_INSTALL_PREFIX} (cmake --install after building snk_sdk)")
endfunction()
