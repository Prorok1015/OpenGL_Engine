# cmake -P tools/ci/test_sdk_msvc_check.cmake — what the SDK package refuses of an MSVC consumer (#219).
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/SnkMsvcCheck.cmake")

function(expect consumer engine runtime fragment)
	snk_sdk_msvc_problem(_problem "${consumer}" "${engine}" "${runtime}")
	if(fragment STREQUAL "")
		if(NOT _problem STREQUAL "")
			message(FATAL_ERROR "MSVC ${consumer}, engine ${engine}, runtime '${runtime}': refused (${_problem})")
		endif()
	elseif(NOT _problem MATCHES "${fragment}")
		message(FATAL_ERROR "MSVC ${consumer}, engine ${engine}, runtime '${runtime}': expected '${fragment}', got '${_problem}'")
	endif()
endfunction()

expect(1943 1944 "" "MSVC 1943 \\(_MSC_VER\\), older than the MSVC 1944")
expect(1944 1944 "" "")
expect(1951 1944 "" "")
expect(1951 1944 "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL" "")
expect(1951 1944 "MultiThreadedDLL" "")
expect(1951 1944 "MultiThreaded" "static C runtime")
expect(1951 1944 "MultiThreaded$<$<CONFIG:Debug>:Debug>" "static C runtime")
# An SDK built without MSVC records no version: only the runtime is checked.
expect(1930 "" "" "")
message(STATUS "snk_sdk_msvc_problem: all cases pass")
