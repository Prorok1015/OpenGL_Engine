#include "rnd_geometry_desc_adapter.h"
#include <snk/desc/desc_base.hpp>
#include <snk/desc/desc_system.h>
#include <snk/resource/res_diagnostics.h>
#include <format>
#include <string>
#include <utility>

rnd::geometry_desc_adapter::geometry_desc_adapter(::res::resource_system& res_system, ::desc::desc_system& desc_system)
	: m_descs(res_system, desc_system)
{
}

std::vector<::res::res_dep> rnd::geometry_desc_adapter::collect_dependencies(const ::res::tag& tag,
	const std::vector<std::byte>& raw_data) const
{
	return m_descs.collect_dependencies(tag, raw_data);
}

std::shared_ptr<::res::resource_entry> rnd::geometry_desc_adapter::deserialize(const ::res::tag& tag,
	const std::vector<std::byte>& raw_data) const
{
	return deserialize(tag, raw_data, ::res::res_deps{});
}

std::shared_ptr<::res::resource_entry> rnd::geometry_desc_adapter::deserialize(const ::res::tag& tag,
	const std::vector<std::byte>& raw_data, const ::res::res_deps& deps) const
{
	auto entry = m_descs.deserialize(tag, raw_data, deps);
	if (!std::dynamic_pointer_cast<rnd::geometry_desc>(entry)) {
		const auto* loaded = dynamic_cast<const ::desc::desc_base*>(entry.get());
		const std::string type = loaded ? std::string(loaded->get_type()) : std::string("resource");
		throw ::res::load_error(::res::diagnostic_kind::unknown_type, "/__type",
			std::format("desc '{}' is a '{}', not a geometry_desc", tag.view(), type));
	}
	return entry;
}

std::vector<std::byte> rnd::geometry_desc_adapter::serialize(const ::res::tag& tag,
	const std::shared_ptr<::res::resource_entry>& resource) const
{
	return m_descs.serialize(tag, resource);
}
