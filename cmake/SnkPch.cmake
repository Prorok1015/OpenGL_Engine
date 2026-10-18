# SnkPch.cmake — precompiled headers for project targets (SNK_PCH, see SnkBuildCache.cmake).
#
# One PCH (cmake/pch/snk_pch.h: heavy stdlib + glm + EnTT + Boost.JSON) is built once by the
# `snk_pch` owner target, which links the same usage requirements as the core layer, and reused
# by every other project library through REUSE_FROM. unit_tests builds its own PCH on top of it
# (Boost.Test). Third-party targets (lib3dparty/, FetchContent Boost) never get a PCH.
#
# Only STATIC/SHARED libraries reuse the owner's PCH: executables are compiled -fPIE instead of
# -fPIC, which Clang rejects as a PCH mismatch.

set(SNK_PCH_DIR "${CMAKE_CURRENT_LIST_DIR}/pch")

# Decide once whether PCH is on. AUTO is ON except for MSVC with a compiler cache: sccache cannot
# cache /Yu compilations (it falls back to running cl.exe), so PCH would silently turn every
# cache hit into a miss. A cached build wins there (cold: slower; warm CI/dev rebuilds: much
# faster); pass -DSNK_PCH=ON to get PCH on MSVC regardless.
set(SNK_PCH_ENABLED OFF)
if(SNK_PCH STREQUAL "ON")
	set(SNK_PCH_ENABLED ON)
elseif(SNK_PCH STREQUAL "AUTO")
	if(MSVC AND SNK_COMPILER_CACHE_TOOL)
		message(STATUS "Precompiled headers: off (MSVC + ${SNK_COMPILER_CACHE_TOOL} cannot cache PCH compilations; -DSNK_PCH=ON overrides)")
	else()
		set(SNK_PCH_ENABLED ON)
	endif()
elseif(NOT SNK_PCH STREQUAL "OFF")
	message(FATAL_ERROR "SNK_PCH must be AUTO, ON or OFF (got '${SNK_PCH}')")
endif()

# Creates the owner of the shared PCH. Call after the core layer exists.
function(snk_pch_create_owner)
	if(NOT SNK_PCH_ENABLED)
		return()
	endif()
	set(_src "${CMAKE_BINARY_DIR}/snk_pch/snk_pch_owner.cpp")
	if(NOT EXISTS "${_src}")
		file(WRITE "${_src}" "// Carries the shared precompiled header (cmake/SnkPch.cmake); intentionally empty.\n")
	endif()
	add_library(snk_pch OBJECT "${_src}")
	# Mirrors the usage requirements the PCH'd headers see in the engine libraries. Third-party
	# targets only: linking a project library would make the owner depend on its own users.
	target_link_libraries(snk_pch PRIVATE glm EnTT Boost::json snk::sanitizers snk::compiler_warnings)
	target_precompile_headers(snk_pch PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${SNK_PCH_DIR}/snk_pch.h>")
	snk_pch_compile_options(snk_pch)
endfunction()

# Flags every PCH user needs on top of the PCH itself.
function(snk_pch_compile_options tgt)
	# ccache hashes the preprocessed output; for GCC it needs -fpch-preprocess to see through
	# `-include cmake_pch.hxx` (the .gch) instead of treating the compilation as uncacheable.
	if(SNK_COMPILER_CACHE_TOOL STREQUAL "ccache" AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
		target_compile_options(${tgt} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fpch-preprocess>)
	endif()
endfunction()

# TRUE in `out` when `tgt` gets the usage requirements (include dirs, definitions) of all `libs`
# through its link interface, directly or transitively. Resolved at configure time, so call it
# after the last target_link_libraries().
function(snk_pch_sees_all out tgt)
	set(_todo ${tgt})
	set(_seen "")
	while(_todo)
		list(POP_FRONT _todo _t)
		if(_t IN_LIST _seen)
			continue()
		endif()
		list(APPEND _seen ${_t})
		if(NOT TARGET ${_t})
			continue()
		endif()
		get_target_property(_aliased ${_t} ALIASED_TARGET)
		if(_aliased)
			list(APPEND _todo ${_aliased})
			continue()
		endif()
		get_target_property(_type ${_t} TYPE)
		if(_t STREQUAL tgt)
			get_target_property(_deps ${_t} LINK_LIBRARIES)
		elseif(_type STREQUAL "EXECUTABLE")
			continue()
		else()
			get_target_property(_deps ${_t} INTERFACE_LINK_LIBRARIES)
		endif()
		if(_deps)
			# $<COMPILE_ONLY:x> / $<LINK_ONLY:x> wrappers (shared engine build): keep the name.
			string(REGEX REPLACE "\\$<(COMPILE_ONLY|LINK_ONLY|BUILD_INTERFACE|BUILD_LOCAL_INTERFACE):([^>]*)>" "\\2" _deps "${_deps}")
			list(APPEND _todo ${_deps})
		endif()
	endwhile()
	set(_ok TRUE)
	foreach(_lib IN LISTS ARGN)
		get_target_property(_real ${_lib} ALIASED_TARGET)
		if(NOT _real)
			set(_real ${_lib})
		endif()
		if(NOT _lib IN_LIST _seen AND NOT _real IN_LIST _seen)
			set(_ok FALSE)
		endif()
	endforeach()
	set(${out} ${_ok} PARENT_SCOPE)
endfunction()

# Reuses the shared PCH in `tgt`. Called from snk_apply_target_defaults for project targets.
function(snk_pch_apply tgt)
	if(NOT SNK_PCH_ENABLED OR tgt STREQUAL "snk_pch")
		return()
	endif()
	get_target_property(_type ${tgt} TYPE)
	if(tgt STREQUAL "unit_tests")
		target_precompile_headers(unit_tests PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${SNK_PCH_DIR}/snk_pch_tests.h>")
		snk_pch_compile_options(unit_tests)
		# These define BOOST_TEST_* macros before their first Boost.Test include (the header-only
		# `included/` runner, DYN_LINK); a PCH would include Boost.Test ahead of the define.
		set_source_files_properties(
			"${CMAKE_SOURCE_DIR}/tests/unit_tests.cpp" "${CMAKE_SOURCE_DIR}/tests/edt_panel_tests.cpp"
			PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
	elseif(_type MATCHES "^(STATIC_LIBRARY|SHARED_LIBRARY)$")
		# A PCH is only valid for compilations whose relevant definitions equal the owner's (GCC
		# rejects it otherwise, Clang errors), and the headers must be reachable for them. Both
		# hold exactly for targets that see the same third-party libraries as the owner.
		snk_pch_sees_all(_compatible ${tgt} glm EnTT Boost::json)
		if(_compatible)
			target_precompile_headers(${tgt} REUSE_FROM snk_pch)
			snk_pch_compile_options(${tgt})
		endif()
	endif()
endfunction()
