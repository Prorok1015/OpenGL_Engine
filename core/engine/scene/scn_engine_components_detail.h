#pragma once
#include <snk/scene/scn_component_catalog.h>
#include <snk/common/engine_assert.h>
#include <string>
#include <string_view>

// Shared by the files that register the engine's components (scn_engine_components*.cpp): the schemas are described
// over several files because one file with all of them exceeds MSVC's limit of sections per object file.
namespace scn::detail
{
	template<typename T>
	void add_engine_component(component_catalog& catalog, std::string_view name)
	{
		const auto* entry = catalog.add<T>(std::string{ name }, desc::bind_described<T>(name, 1, desc::schema_kind::component));
		ASSERT_MSG(entry, "engine component '{}' is registered twice", name);
	}

	template<typename T>
	void add_engine_derived(component_catalog& catalog, std::string_view name)
	{
		const auto* entry = catalog.add_derived<T>(std::string{ name });
		ASSERT_MSG(entry, "engine component '{}' is registered twice", name);
	}

	void register_render_components(component_catalog& catalog);
	void register_animation_components(component_catalog& catalog);
}
