# SnkTarget.cmake — per-target defaults for project (non-third-party) targets.
#
# Hidden visibility keeps the binary identical in static builds and makes the exported surface
# explicit (SNK_API, core/core/common/snk_api.h) once engine components are linked into a
# shared library (SNK_ENGINE_SHARED).
function(snk_target_defaults tgt)
	set_target_properties(${tgt} PROPERTIES
		CXX_VISIBILITY_PRESET hidden
		VISIBILITY_INLINES_HIDDEN ON)
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
			endif()
		endforeach()
	endwhile()
endfunction()
