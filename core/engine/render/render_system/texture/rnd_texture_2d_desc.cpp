#include "rnd_texture_2d_desc.h"
#include "desc_system.h"

void rnd::texture_2d_desc::deserialize(desc::desc_system& system, const json::object& resource)
{
	texture_desc::deserialize(system, resource);

	if (resource.contains("data") && resource.at("data").is_string())
		picture_handle = system.get_field_desc<res::picture_resource>(*this, resource.at("data"));
}

void rnd::texture_2d_desc::serialize(json::object& resource) const
{
	texture_desc::serialize(resource);

	if (picture_handle.is_ready()) {
		resource["data"] = json::value_from(picture_handle->get_tag());
	}
}
