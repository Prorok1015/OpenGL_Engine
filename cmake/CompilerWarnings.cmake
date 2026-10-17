# snk::compiler_warnings — warning flags for project code only (EPIC-41 US-41-10).
# Carried by an INTERFACE target that is linked by snk::common (which every project target
# consumes, same as snk::sanitizers). Third-party code (lib3dparty/*, FetchContent Boost) never
# links it, so it keeps its own flags. Warnings are informational: no -Werror.
add_library(snk_compiler_warnings INTERFACE)
if(MSVC)
	# /W3, not /W4: /W4 is too noisy with third-party headers.
	target_compile_options(snk_compiler_warnings INTERFACE $<$<COMPILE_LANGUAGE:CXX>:/W3>)
else()
	target_compile_options(snk_compiler_warnings INTERFACE
		$<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic -Wno-unused-parameter>)
endif()
add_library(snk::compiler_warnings ALIAS snk_compiler_warnings)

# Mark the public include dirs of every third-party target as SYSTEM, so that headers consumed
# from project code (Boost, glm, entt, stb, spdlog, ...) do not report warnings under the flags
# above. Third-party sources themselves are untouched: they never link snk::compiler_warnings.
# Call once, after the last add_subdirectory(): it walks the whole directory tree and treats
# directories under lib3dparty/ and the FetchContent Boost tree as third-party.
function(snk_isolate_third_party_warnings)
	set(_roots "${CMAKE_SOURCE_DIR}/lib3dparty")
	if(boost_SOURCE_DIR)
		list(APPEND _roots "${boost_SOURCE_DIR}")
	endif()
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
		if(NOT _third_party)
			continue()
		endif()
		get_property(_targets DIRECTORY "${_dir}" PROPERTY BUILDSYSTEM_TARGETS)
		foreach(_t IN LISTS _targets)
			get_target_property(_type ${_t} TYPE)
			if(_type STREQUAL "UTILITY")
				continue()
			endif()
			get_target_property(_inc ${_t} INTERFACE_INCLUDE_DIRECTORIES)
			if(_inc)
				set_property(TARGET ${_t} APPEND PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES ${_inc})
			endif()
		endforeach()
	endwhile()
endfunction()
