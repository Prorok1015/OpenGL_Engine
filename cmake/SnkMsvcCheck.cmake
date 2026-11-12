# SnkMsvcCheck.cmake — what the snake_sdk package refuses of an MSVC consumer (#219). Installed next to the config, which
# includes it; tools/ci/test_sdk_msvc_check.cmake runs it under `cmake -P`.

# snk_sdk_msvc_problem(<out> <consumer MSVC_VERSION> <engine MSVC_VERSION> <consumer CMAKE_MSVC_RUNTIME_LIBRARY>)
# Sets <out> to why the consumer cannot use the engine, or to "" when it can. The engine is built with the oldest MSVC
# the SDK supports; a game module needs the same or a newer one (the engine refuses an older module when it loads it,
# app_module_entry.h) and the DLL runtime, which every module shares with snk_engine.dll.
function(snk_sdk_msvc_problem out consumer engine runtime)
	set(_problem "")
	if(engine AND consumer LESS engine)
		set(_problem "this compiler is MSVC ${consumer} (_MSC_VER), older than the MSVC ${engine} the engine of this SDK "
			"was built with. Use Visual Studio 2022 17.14 or newer with an MSVC of at least ${engine}: the engine refuses "
			"a module built with an older one.")
	elseif(runtime AND NOT runtime MATCHES "DLL")
		set(_problem "CMAKE_MSVC_RUNTIME_LIBRARY is \"${runtime}\", a static C runtime: a module shares the DLL runtime "
			"(/MD) with snk_engine.dll. Set it to MultiThreadedDLL.")
	endif()
	set(${out} "${_problem}" PARENT_SCOPE)
endfunction()
