#pragma once

// SNK_MODEL_IMPORT_API marks what the model import library (snk_model_import_assimp, the one owner of assimp) exports.
// Empty for a static build; SNK_MODEL_IMPORT_SHARED is defined for every consumer of the shared library and
// SNK_MODEL_IMPORT_BUILD only while compiling it (export side). Same shape as SNK_API and SNK_EDITOR_API.
#if defined(SNK_MODEL_IMPORT_SHARED)
	#if defined(_WIN32)
		#if defined(SNK_MODEL_IMPORT_BUILD)
			#define SNK_MODEL_IMPORT_API __declspec(dllexport)
		#else
			#define SNK_MODEL_IMPORT_API __declspec(dllimport)
		#endif
	#else
		#define SNK_MODEL_IMPORT_API __attribute__((visibility("default")))
	#endif
#else
	#define SNK_MODEL_IMPORT_API
#endif
