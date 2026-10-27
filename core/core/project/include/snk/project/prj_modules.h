#pragma once
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <snk/application/app_module_interface.h>
#include <snk/application/app_module_loader.hpp>
#include <snk/common/ds/ds_store.hpp>
#include <snk/common/snk_api.h>
#include <snk/project/prj_manifest.h>

// The game modules a project names in its manifest (EPIC-42 #159). The source build links them into the host and
// lists them in a module_catalog; loading them from DLLs is EPIC-52 (#21).
namespace prj
{
	class SNK_API module_catalog
	{
	public:
		using factory = std::function<std::unique_ptr<app::modules::module_interface>()>;

		// A second factory under the same name replaces the first.
		void add(std::string name, factory make);
		const factory* find(std::string_view name) const;
		bool empty() const { return m_factories.empty(); }

	private:
		std::map<std::string, factory, std::less<>> m_factories;
	};

	struct module_load_report
	{
		std::vector<std::string> loaded;
		// Named by the manifest, not in the catalog; `optional` ones included.
		std::vector<std::string> missing;
		// Found, but the loader refused them or they threw while starting.
		std::vector<std::string> failed;
	};

	// Loads the manifest's modules in manifest order, after the host's own modules are initialized. Editor modules
	// (module_kind::editor) only with `with_editor_modules`. A missing or failing module is logged (a missing
	// optional one at info level) and skipped: the host keeps running without it.
	SNK_API module_load_report load_project_modules(const manifest& project, const module_catalog& catalog,
		bool with_editor_modules, app::modules::module_loader& loader, ds::app_data_storage& data);
}
