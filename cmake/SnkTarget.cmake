# SnkTarget.cmake — per-target defaults for project (non-third-party) targets.
#
# Hidden visibility keeps the binary identical in static builds and makes the exported surface
# explicit (SNK_API, core/core/common/include/snk/common/snk_api.h) once engine components are linked into a
# shared library (SNK_ENGINE_SHARED).
function(snk_target_defaults tgt)
	set_target_properties(${tgt} PROPERTIES
		CXX_VISIBILITY_PRESET hidden
		VISIBILITY_INLINES_HIDDEN ON)
	# STL members in exported classes: the engine and its consumers share one CRT (/MD), so the
	# DLL-interface warnings carry no information.
	if(MSVC AND SNK_ENGINE_SHARED)
		target_compile_options(${tgt} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/wd4251> $<$<COMPILE_LANGUAGE:CXX>:/wd4275>)
	endif()
endfunction()

# Usage requirements of `libs` without linking them when the engine is a shared library (the code
# then lives in snk_engine); plain link dependencies otherwise.
function(snk_usage_only out)
	set(_res "")
	foreach(_l IN LISTS ARGN)
		if(SNK_ENGINE_SHARED)
			list(APPEND _res "$<COMPILE_ONLY:${_l}>")
		else()
			list(APPEND _res "${_l}")
		endif()
	endforeach()
	set(${out} "${_res}" PARENT_SCOPE)
endfunction()

# Header layout of an engine component (#109, docs/roadmap/design/HEADER_SPLIT.md).
#
#   <component>/include/snk/<name>/*.h   public headers: the only include directory consumers see
#                                        (PUBLIC, or INTERFACE for header-only targets)
#   <component>/ (root)                  sources and internal headers, PRIVATE to the component
#
# Every component must have an include/ directory; SnkInstall.cmake installs it to <prefix>/include.
function(snk_component_headers tgt)
	get_target_property(_type ${tgt} TYPE)
	if(_type STREQUAL "INTERFACE_LIBRARY")
		set(_scope INTERFACE)
	else()
		set(_scope PUBLIC)
	endif()
	set(_public "${CMAKE_CURRENT_SOURCE_DIR}/include")
	if(IS_DIRECTORY "${_public}")
		target_include_directories(${tgt} ${_scope} "$<BUILD_INTERFACE:${_public}>")
		if(NOT _type STREQUAL "INTERFACE_LIBRARY")
			target_include_directories(${tgt} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
		endif()
	else()
		message(FATAL_ERROR "snk_component_headers(${tgt}): ${_public} does not exist")
	endif()
endfunction()

# Applies snk_target_defaults to every compiled target created outside lib3dparty/ and the
# FetchContent Boost tree. Call once, after the last add_subdirectory().
function(snk_apply_target_defaults)
	set(_roots "${CMAKE_SOURCE_DIR}/lib3dparty")
	if(boost_SOURCE_DIR)
		list(APPEND _roots "${boost_SOURCE_DIR}")
	endif()
	# Third-party targets declared directly in the root scope (no add_subdirectory of their own).
	set(_skip imgui)
	set(_todo "${CMAKE_SOURCE_DIR}")
	while(_todo)
		list(POP_FRONT _todo _dir)
		get_property(_subdirs DIRECTORY "${_dir}" PROPERTY SUBDIRECTORIES)
		list(APPEND _todo ${_subdirs})
		set(_third_party FALSE)
		foreach(_root IN LISTS _roots)
			string(FIND "${_dir}/" "${_root}/" _pos)
			if(_pos EQUAL 0)
				set(_third_party TRUE)
			endif()
		endforeach()
		if(_third_party)
			continue()
		endif()
		get_property(_targets DIRECTORY "${_dir}" PROPERTY BUILDSYSTEM_TARGETS)
		foreach(_t IN LISTS _targets)
			if(_t IN_LIST _skip)
				continue()
			endif()
			get_target_property(_type ${_t} TYPE)
			if(_type MATCHES "^(STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY|EXECUTABLE)$")
				snk_target_defaults(${_t})
				snk_pch_apply(${_t})
				# Everything compiled under core/ ends up in snk_engine: it exports (SNK_API).
				if(SNK_ENGINE_SHARED)
					get_target_property(_src ${_t} SOURCE_DIR)
					string(FIND "${_src}/" "${CMAKE_SOURCE_DIR}/core/" _in_core)
					if(_in_core EQUAL 0)
						target_compile_definitions(${_t} PRIVATE SNK_ENGINE_BUILD)
						if(MSVC)
							target_compile_definitions(${_t} PRIVATE "IMGUI_API=__declspec(dllexport)")
						endif()
					elseif(MSVC)
						# imgui is exported by snk_engine (one ImGui context shared with the editor).
						target_compile_definitions(${_t} PRIVATE "IMGUI_API=__declspec(dllimport)")
					endif()
				endif()
			endif()
		endforeach()
	endwhile()
endfunction()
