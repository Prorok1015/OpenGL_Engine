#include "rnd_texture_cubemap_desc.h"
#include "desc_system.h"

void rnd::texture_cubemap_desc::deserialize(desc::desc_system& system, const boost::json::object& resource)
{
	texture_desc::deserialize(system, resource);

	if (auto const* p = resource.if_contains("data")) {
		if (p->is_string()) {
			mode = SOURCE_TYPE::SINGLE_FILE;
			single_image = system.get_field_desc<res::picture_resource>(*this, *p);
		} else if (p->is_object()) {
			mode = SOURCE_TYPE::SIX_FACES;
			const auto& obj = p->as_object();
			if (auto* f = obj.if_contains("right"))  
				cube_faces.right = system.get_field_desc<res::picture_resource>(*this, *f);
			if (auto* f = obj.if_contains("left"))   
				cube_faces.left = system.get_field_desc<res::picture_resource>(*this, *f);
			if (auto* f = obj.if_contains("top"))    
				cube_faces.top = system.get_field_desc<res::picture_resource>(*this, *f);
			if (auto* f = obj.if_contains("bottom")) 
				cube_faces.bottom = system.get_field_desc<res::picture_resource>(*this, *f);
			if (auto* f = obj.if_contains("front"))  
				cube_faces.front = system.get_field_desc<res::picture_resource>(*this, *f);
			if (auto* f = obj.if_contains("back"))   
				cube_faces.back = system.get_field_desc<res::picture_resource>(*this, *f);
		}
	}
}

void rnd::texture_cubemap_desc::serialize(json::object& resource) const
{
	texture_desc::serialize(resource);

	if (mode == SOURCE_TYPE::SINGLE_FILE) {
		if (single_image.is_ready())
			resource["data"] = json::value_from(single_image->get_tag());
	} else {
		json::object data_obj;
		data_obj["right"] = json::value_from(cube_faces.right->get_tag());
		data_obj["left"] = json::value_from(cube_faces.left->get_tag());
		data_obj["top"] = json::value_from(cube_faces.top->get_tag());
		data_obj["bottom"] = json::value_from(cube_faces.bottom->get_tag());
		data_obj["front"] = json::value_from(cube_faces.front->get_tag());
		data_obj["back"] = json::value_from(cube_faces.back->get_tag());
		resource["data"] = std::move(data_obj);
	}
}