#include <snk/project/prj_modules.h>
#include <exception>
#include <stdexcept>
#include <utility>
#include <snk/common/logger/engine_log.h>

void prj::module_catalog::add(std::string name, factory make)
{
	m_factories.insert_or_assign(std::move(name), std::move(make));
}

const prj::module_catalog::factory* prj::module_catalog::find(std::string_view name) const
{
	const auto it = m_factories.find(name);
	return it == m_factories.end() ? nullptr : &it->second;
}

prj::module_load_report prj::load_project_modules(const manifest& project, const module_catalog& catalog,
	bool with_editor_modules, app::modules::module_loader& loader, ds::app_data_storage& data)
{
	module_load_report report;
	for (const auto& ref : project.modules) {
		if (ref.kind == module_kind::editor && !with_editor_modules) {
			continue;
		}

		const auto* make = catalog.find(ref.name);
		if (!make) {
			report.missing.push_back(ref.name);
			if (ref.optional) {
				egLOG_INFO("project", "optional module '{}' of project '{}' is not available", ref.name, project.name);
			} else {
				egLOG_WARN("project", "module '{}' of project '{}' is not available: running without it", ref.name, project.name);
			}
			continue;
		}

		try {
			auto module = (*make)();
			if (!module) {
				throw std::runtime_error("its factory returned no module");
			}
			loader.load(std::move(module), data);
			report.loaded.push_back(ref.name);
			egLOG_INFO("project", "loaded module '{}' of project '{}'", ref.name, project.name);
		} catch (const std::exception& e) {
			report.failed.push_back(ref.name);
			egLOG_ERROR("project", "module '{}' of project '{}' failed to load: {}", ref.name, project.name, e.what());
		}
	}
	return report;
}
