#include <snk/project/prj_modules.h>
#include <algorithm>
#include <snk/common/logger/engine_log.h>

prj::module_resolution prj::resolve_modules(const manifest& project, const std::vector<std::string>& known_names,
	bool with_editor_modules)
{
	module_resolution result;
	for (const auto& ref : project.modules) {
		if (ref.kind == module_kind::editor && !with_editor_modules) {
			continue;
		}
		if (std::ranges::find(known_names, ref.name) != known_names.end()) {
			result.resolved.push_back(ref);
		} else if (ref.optional) {
			result.missing_optional.push_back(ref.name);
			egLOG_DEBUG("project", "optional module '{}' of project '{}' is not available", ref.name, project.name);
		} else {
			result.missing_required.push_back(ref.name);
			egLOG_WARN("project", "module '{}' of project '{}' is not available: running without it", ref.name, project.name);
		}
	}
	return result;
}
