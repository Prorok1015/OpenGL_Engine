#pragma once
#include <string>
#include <vector>
#include <snk/common/snk_api.h>
#include <snk/project/prj_manifest.h>

// The game modules a project names in its manifest against the ones the host can provide (EPIC-42 #159). Loading them
// is EPIC-52 (#21, #220); this only tells which are there.
namespace prj
{
	struct module_resolution
	{
		// Named by the manifest and known to the host, in manifest order.
		std::vector<module_ref> resolved;
		std::vector<std::string> missing_required;
		std::vector<std::string> missing_optional;
	};

	// `known_names`: the modules the host can provide. Editor modules (module_kind::editor) count only with
	// `with_editor_modules`. A missing required module is logged as a warning, a missing optional one at debug level:
	// the host runs without either.
	SNK_API module_resolution resolve_modules(const manifest& project, const std::vector<std::string>& known_names,
		bool with_editor_modules);
}
