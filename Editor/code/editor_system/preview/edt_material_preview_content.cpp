#include "edt_material_preview_content.h"
#include "edt_data_document.h"
#include "edt_material_window_model.h"
#include "edt_mesh_base_shapes.h"
#include "edt_shape_menu_model.h"
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>
#include <snk/desc/desc_system.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/resource/res_system.h>
#include <boost/json.hpp>
#include <atomic>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json = boost::json;

namespace
{
	const res::tag k_sky_shader{ "res://shaders/sky.shader.desc" };
	const res::tag k_sky_vertex{ "res://shaders/sky.vert" };

	std::string_view shape_name(edt::preview_shape shape)
	{
		switch (shape) {
		case edt::preview_shape::cube: return "box";
		case edt::preview_shape::plane: return "plane";
		case edt::preview_shape::sphere: break;
		}
		return "sphere";
	}
}

std::string_view edt::preview_shape_label(preview_shape shape)
{
	switch (shape) {
	case preview_shape::cube: return "Cube";
	case preview_shape::plane: return "Plane";
	case preview_shape::sphere: break;
	}
	return "Sphere";
}

std::string edt::preview_shape_geometry(preview_shape shape)
{
	return std::format("res://shapes/{}.geom.desc", shape_name(shape));
}

bool edt::is_sky_material(const material_shader& shader)
{
	return shader.shader == k_sky_shader || shader.vertex == k_sky_vertex;
}

edt::material_preview_content::material_preview_content(data_document& document, material_window_model& model, editor_services& services)
	: m_document(document)
	, m_model(model)
	, m_services(services)
{
	static std::atomic<std::uint64_t> s_previews{ 0 };
	m_material_tag = res::tag{ std::format("memory://editor/material/{}.desc", ++s_previews) };
}

edt::material_preview_content::~material_preview_content()
{
	release();
}

edt::preview_content::state edt::material_preview_content::update()
{
	m_sky_material = is_sky_material(m_model.shader());
	if (m_sky_material) return state::none;
	const bool changed = !m_material_written || m_written_generation != m_document.generation();
	if (changed && !write_material()) return state::waiting;
	return changed ? state::changed : state::unchanged;
}

void edt::material_preview_content::build(scn::prefab_desc::prefab_node& root)
{
	root.name = "material";
	root.children.push_back(mesh::mesh_node("shape", node_fields()));
	m_node_shape = m_shape;
}

void edt::material_preview_content::refresh(scn::prefab_desc::prefab_node& root)
{
	// The range of a base shape is a bake of it: made again only when the shape changes, not at every edit.
	if (m_node_shape == m_shape || root.children.empty()) return;
	root.children.front().components["mesh"].overrides = node_fields();
	m_node_shape = m_shape;
}

bool edt::material_preview_content::write_material()
{
	res::resource_system* writer = m_services.writable_resources();
	desc::desc_system* descs = m_services.descs();
	if (!writer || !descs) return false;
	const json::value shown = m_document.shown();
	json::object body = shown.is_object() ? shown.as_object() : json::object{};
	std::vector<std::string> headers;
	for (const auto& entry : body) {
		const std::string_view key(entry.key().data(), entry.key().size());
		if (key.starts_with("__")) headers.emplace_back(key);
	}
	// The parent chain is folded into what is shown: the copy has no __parent, and no __guid of the file it copies.
	for (const std::string& key : headers) body.erase(key);
	body["__type"] = m_document.type();
	body["__version"] = m_document.current_version();
	const json::value content(std::move(body));
	try {
		writer->store(m_material_tag, descs->serialize_to_bytes(content));
	} catch (const std::exception& failure) {
		egLOG_WARN("Editor/Material", "The material preview cannot be written: {}", failure.what());
		return false;
	}
	writer->signal_changed(m_material_tag);
	m_material_written = true;
	m_written_generation = m_document.generation();
	return true;
}

void edt::material_preview_content::release()
{
	if (!m_material_written) return;
	if (res::resource_system* writer = m_services.writable_resources()) {
		writer->erase(m_material_tag);
		writer->signal_changed(m_material_tag);
	}
	m_material_written = false;
}

json::object edt::material_preview_content::node_fields() const
{
	const mesh::base_shape* shape = mesh::find_base_shape(shape_name(m_shape));
	ASSERT_MSG(shape != nullptr, "the preview shapes are base shapes of core/res/shapes");
	json::object fields = shape ? mesh::mesh_fields(mesh::base_shape_path(*shape), mesh::base_shape_range(*shape)) : json::object{};
	fields["material"] = std::string(m_material_tag.view());
	return fields;
}
