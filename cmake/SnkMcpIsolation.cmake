# SnkMcpIsolation.cmake - keeps the MCP server module out of the engine, the games and the SDK.
#
# snk_mcp_server (Editor/code/mcp_server) is a STATIC library for the editor only (docs/roadmap/design/AI_INTEGRATION.md
# section 3.7, decision 7). A game binary must not be able to contain a network server, so:
#   - no target under core/ (the engine layers, what a game links and what the SDK exports) may reach it through its link
#     interface, directly or through other libraries (for example by linking snk::editor_system);
#   - snk::engine and snk::engine_api, the closure the SDK installs, may not reach it either;
#   - the module itself may not link anything under core/: it stays free of the engine.
# tools/ci/check_mcp_isolation.sh checks the sources and the installed SDK; this is the check on the CMake graph, and it
# runs in every configure, so a violation fails every job and not only the one that has the script.

# Names of the targets in the link interface of `tgt` (LINK_LIBRARIES too for targets that are linked, not only used).
function(_snk_mcp_link_deps out tgt)
	get_target_property(_type ${tgt} TYPE)
	set(_items "")
	get_target_property(_iface ${tgt} INTERFACE_LINK_LIBRARIES)
	if(_iface)
		list(APPEND _items ${_iface})
	endif()
	if(_type MATCHES "^(STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY|EXECUTABLE)$")
		get_target_property(_priv ${tgt} LINK_LIBRARIES)
		if(_priv)
			list(APPEND _items ${_priv})
		endif()
	endif()
	set(_res "")
	foreach(_item IN LISTS _items)
		# $<BUILD_INTERFACE:x>, $<LINK_ONLY:x>, $<COMPILE_ONLY:x>: keep the name, drop the wrapper.
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

# Call once, after the last add_subdirectory() (and before snk_install_sdk()).
function(snk_check_mcp_isolation)
	if(NOT TARGET snk_mcp_server)
		return()
	endif()

	# 1. Roots: the SDK closure and every target created under core/ (third-party trees inside it are skipped).
	set(_roots snk::engine snk::engine_api)
	set(_core "${CMAKE_SOURCE_DIR}/core")
	set(_todo "${_core}")
	while(_todo)
		list(POP_FRONT _todo _dir)
		string(FIND "${_dir}/" "${CMAKE_SOURCE_DIR}/lib3dparty/" _in_3p)
		if(_in_3p EQUAL 0)
			continue()
		endif()
		get_property(_subdirs DIRECTORY "${_dir}" PROPERTY SUBDIRECTORIES)
		list(APPEND _todo ${_subdirs})
		get_property(_targets DIRECTORY "${_dir}" PROPERTY BUILDSYSTEM_TARGETS)
		list(APPEND _roots ${_targets})
	endwhile()

	# 2. Nothing reachable from a root is the module. One visited set for all roots: a target that was walked
	# without finding the module cannot lead to it from another root either.
	set(_seen "")
	foreach(_root IN LISTS _roots)
		set(_queue ${_root})
		while(_queue)
			list(POP_FRONT _queue _name)
			if(NOT TARGET ${_name})
				continue()
			endif()
			get_target_property(_real ${_name} ALIASED_TARGET)
			if(_real)
				set(_name ${_real})
			endif()
			if(_name IN_LIST _seen)
				continue()
			endif()
			list(APPEND _seen ${_name})
			if(_name STREQUAL "snk_mcp_server")
				message(FATAL_ERROR "snk_mcp_server must be linked into the editor only, but ${_root} (the engine, a game or the SDK) "
					"reaches it through its link interface. See docs/roadmap/design/AI_INTEGRATION.md section 3.7.")
			endif()
			_snk_mcp_link_deps(_deps ${_name})
			list(APPEND _queue ${_deps})
		endwhile()
	endforeach()

	# 3. The module reaches nothing under core/.
	set(_seen "")
	set(_queue snk_mcp_server)
	while(_queue)
		list(POP_FRONT _queue _name)
		if(NOT TARGET ${_name})
			continue()
		endif()
		get_target_property(_real ${_name} ALIASED_TARGET)
		if(_real)
			set(_name ${_real})
		endif()
		if(_name IN_LIST _seen)
			continue()
		endif()
		list(APPEND _seen ${_name})
		get_target_property(_imported ${_name} IMPORTED)
		if(NOT _imported)
			get_target_property(_src ${_name} SOURCE_DIR)
			string(FIND "${_src}/" "${_core}/" _in_core)
			if(_in_core EQUAL 0)
				message(FATAL_ERROR "snk_mcp_server must not depend on the engine, but it reaches ${_name} (${_src}). "
					"See docs/roadmap/design/AI_INTEGRATION.md section 3.7.")
			endif()
		endif()
		_snk_mcp_link_deps(_deps ${_name})
		list(APPEND _queue ${_deps})
	endwhile()
endfunction()
