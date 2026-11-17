#pragma once

// SNK_EDITOR_API marks the editor classes a game module (game_editor) calls into: the registries (panels, commands
// and menus, inspector renderers, asset kinds), editor_context and the services it hands out. Like SNK_API it is empty
// for a static build; SNK_EDITOR_SHARED is defined for every consumer of the shared editor API library and
// SNK_EDITOR_BUILD only while compiling it (export side). The API holds on the same terms as the engine's: one
// toolchain, one CRT and the SDK build the module was made against (a module names the version it was built for, as
// every module does); there is no C ABI.
#if defined(SNK_EDITOR_SHARED)
	#if defined(_WIN32)
		#if defined(SNK_EDITOR_BUILD)
			#define SNK_EDITOR_API __declspec(dllexport)
		#else
			#define SNK_EDITOR_API __declspec(dllimport)
		#endif
	#else
		#define SNK_EDITOR_API __attribute__((visibility("default")))
	#endif
#else
	#define SNK_EDITOR_API
#endif
