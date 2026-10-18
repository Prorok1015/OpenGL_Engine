#pragma once

// SNK_API marks classes/functions that must be visible outside the shared engine library.
// Static builds (the default) leave it empty. SNK_ENGINE_SHARED is defined for every consumer of
// the shared library; SNK_ENGINE_BUILD only while compiling the library itself (export side).
#if defined(SNK_ENGINE_SHARED)
	#if defined(_WIN32)
		#if defined(SNK_ENGINE_BUILD)
			#define SNK_API __declspec(dllexport)
		#else
			#define SNK_API __declspec(dllimport)
		#endif
	#else
		#define SNK_API __attribute__((visibility("default")))
	#endif
#else
	#define SNK_API
#endif
