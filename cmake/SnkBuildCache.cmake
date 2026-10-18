# SnkBuildCache.cmake — build-speed options: compiler cache (SNK_COMPILER_CACHE) and the
# precompiled-header switch (SNK_PCH, consumed by SnkPch.cmake).
#
# Included from the root CMakeLists.txt BEFORE project(): the MSVC debug-info policy must be set
# before the first project() call, and the launchers are then inherited by every directory
# (third-party submodules and FetchContent Boost included).

set(SNK_COMPILER_CACHE "AUTO" CACHE STRING
	"Compiler cache: AUTO (use sccache/ccache when found), ON (same, warn when missing), OFF")
set_property(CACHE SNK_COMPILER_CACHE PROPERTY STRINGS AUTO ON OFF)

# AUTO: PCH everywhere, except MSVC with a compiler cache (see SnkPch.cmake for the reasoning).
set(SNK_PCH "AUTO" CACHE STRING "Precompiled headers for project targets: AUTO, ON, OFF")
set_property(CACHE SNK_PCH PROPERTY STRINGS AUTO ON OFF)

set(SNK_COMPILER_CACHE_TOOL "")

# A launcher the user (cache variable, -D, environment or a toolchain/preset) already chose wins.
set(_snk_user_launcher "")
foreach(_lang C CXX)
	if(CMAKE_${_lang}_COMPILER_LAUNCHER)
		set(_snk_user_launcher "${CMAKE_${_lang}_COMPILER_LAUNCHER}")
	elseif(DEFINED ENV{CMAKE_${_lang}_COMPILER_LAUNCHER} AND NOT "$ENV{CMAKE_${_lang}_COMPILER_LAUNCHER}" STREQUAL "")
		set(_snk_user_launcher "$ENV{CMAKE_${_lang}_COMPILER_LAUNCHER}")
	endif()
endforeach()

if(SNK_COMPILER_CACHE STREQUAL "OFF")
	message(STATUS "Compiler cache: disabled (SNK_COMPILER_CACHE=OFF)")
elseif(_snk_user_launcher)
	message(STATUS "Compiler cache: using the launcher from the user configuration (${_snk_user_launcher})")
	if(_snk_user_launcher MATCHES "sccache")
		set(SNK_COMPILER_CACHE_TOOL sccache)
	elseif(_snk_user_launcher MATCHES "ccache")
		set(SNK_COMPILER_CACHE_TOOL ccache)
	endif()
else()
	# ccache handles PCH with GCC/Clang (see below); sccache is the one that works with MSVC
	# (cl.exe). Prefer accordingly, fall back to whichever exists.
	if(CMAKE_HOST_WIN32)
		set(_snk_cache_candidates sccache ccache)
	else()
		set(_snk_cache_candidates ccache sccache)
	endif()
	find_program(SNK_COMPILER_CACHE_PROGRAM NAMES ${_snk_cache_candidates} DOC "Compiler cache executable")
	if(SNK_COMPILER_CACHE_PROGRAM)
		get_filename_component(SNK_COMPILER_CACHE_TOOL "${SNK_COMPILER_CACHE_PROGRAM}" NAME_WE)
		set(_snk_launcher "${SNK_COMPILER_CACHE_PROGRAM}")
		# ccache + PCH: the .gch/include files are rebuilt with fresh mtimes on every fresh checkout
		# or build dir and the PCH carries -D state; without these ccache never gets a hit on
		# TUs that use a PCH. Respect an explicit CCACHE_SLOPPINESS from the environment.
		if(SNK_COMPILER_CACHE_TOOL STREQUAL "ccache" AND NOT SNK_PCH STREQUAL "OFF"
				AND NOT DEFINED ENV{CCACHE_SLOPPINESS})
			set(_snk_launcher "${CMAKE_COMMAND}" -E env
				"CCACHE_SLOPPINESS=pch_defines,time_macros,include_file_mtime,include_file_ctime"
				"${SNK_COMPILER_CACHE_PROGRAM}")
		endif()
		set(CMAKE_C_COMPILER_LAUNCHER "${_snk_launcher}")
		set(CMAKE_CXX_COMPILER_LAUNCHER "${_snk_launcher}")
		message(STATUS "Compiler cache: ${SNK_COMPILER_CACHE_TOOL} (${SNK_COMPILER_CACHE_PROGRAM})")
	elseif(SNK_COMPILER_CACHE STREQUAL "ON")
		message(WARNING "SNK_COMPILER_CACHE=ON but neither sccache nor ccache was found")
	else()
		message(STATUS "Compiler cache: none found (install ccache or sccache to speed up rebuilds)")
	endif()
endif()

# MSVC: /Zi writes one shared PDB through mspdbsrv, which a compiler cache cannot reproduce.
# Embedded debug info (/Z7) keeps it in each object file, so objects are cacheable. Harmless for
# other compilers (the variable is only read by MSVC); the policy exists since CMake 3.25.
if(SNK_COMPILER_CACHE_TOOL AND NOT SNK_COMPILER_CACHE STREQUAL "OFF")
	if(POLICY CMP0141)
		cmake_policy(SET CMP0141 NEW)
		set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
	endif()
endif()
