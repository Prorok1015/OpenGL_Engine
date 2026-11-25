#include "edt_texture_preview_content.h"
#include "edt_mesh_base_shapes.h"
#include "edt_shape_menu_model.h"
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>
#include <snk/desc/desc_system.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/resource/res_system.h>
#include <boost/json.hpp>
#include <atomic>
#include <exception>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace json = boost::json;

namespace
{
	constexpr std::string_view k_texture_type = "texture_2d_desc";
	constexpr std::string_view k_material_type = "material_desc";
	constexpr std::string_view k_base_material = "res://base_material.desc";
	// The define of the engine's scene shader that samples the first sampler as the diffuse colour.
	constexpr std::string_view k_diffuse_define = "USE_TXM_AS_DIFFUSE";
}

edt::texture_preview_content::texture_preview_content(texture_preview_source source, editor_services& services)
	: m_source(std::move(source))
	, m_services(services)
{
	static std::atomic<std::uint64_t> s_previews{ 0 };
	const std::uint64_t serial = ++s_previews;
	m_texture_tag = res::tag{ std::format("memory://editor/texture/{}.txm.desc", serial) };
	m_material_tag = res::tag{ std::format("memory://editor/texture/{}.mat.desc", serial) };
}

edt::texture_preview_content::~texture_preview_content()
{
	release();
}

edt::preview_content::state edt::texture_preview_content::update()
{
	if (!m_source.body) return state::none;
	const std::uint64_t generation = m_source.generation ? m_source.generation() : 0;
	const bool changed = !m_texture_written || !m_material_written || m_written_generation != generation;
	if (!changed) return state::unchanged;
	if (!write_texture() || (!m_material_written && !write_material())) return state::waiting;
	m_written_generation = generation;
	return state::changed;
}

void edt::texture_preview_content::build(scn::prefab_desc::prefab_node& root)
{
	root.name = "texture";
	const mesh::base_shape* plane = mesh::find_base_shape("plane");
	ASSERT_MSG(plane != nullptr, "the plane is a base shape of core/res/shapes");
	json::object fields = plane ? mesh::mesh_fields(mesh::base_shape_path(*plane), mesh::base_shape_range(*plane)) : json::object{};
	fields["material"] = std::string(m_material_tag.view());
	root.children.push_back(mesh::mesh_node("plane", fields));
}

void edt::texture_preview_content::refresh(scn::prefab_desc::prefab_node&)
{
	// The plane and its material stay; only what the texture copy holds changes, and the stage's reconcile reloads it.
}

edt::preview_framing edt::texture_preview_content::framing() const
{
	// The unit plane lies in XZ facing +Y: straight down at it, near enough that it fills the view.
	return { .distance = 1.3f, .pitch_degrees = -90.f };
}

bool edt::texture_preview_content::store(const res::tag& tag, const json::value& content)
{
	res::resource_system* writer = m_services.writable_resources();
	desc::desc_system* descs = m_services.descs();
	if (!writer || !descs) {
		report_write_failure("the project resources are not available");
		return false;
	}
	try {
		if (!writer->store(tag, descs->serialize_to_bytes(content))) {
			report_write_failure("the memory copy was refused");
			return false;
		}
	} catch (const std::exception& failure) {
		report_write_failure(failure.what());
		return false;
	}
	writer->signal_changed(tag);
	return true;
}

bool edt::texture_preview_content::write_texture()
{
	desc::desc_system* descs = m_services.descs();
	const json::value given = m_source.body();
	json::object body;
	if (given.is_object()) {
		for (const auto& entry : given.as_object()) {
			// The file's own form, its parent kept: "data" over a parent's "pixels" (or a null taking them away) is read as the
			// runtime reads the file. A flattened copy would hold both sources, which it refuses.
			const std::string_view key = entry.key();
			if (key.starts_with("__") && key != "__parent") continue;
			body[key] = entry.value();
		}
	}
	body["__type"] = k_texture_type;
	body["__version"] = descs ? descs->current_version(k_texture_type) : 1;
	if (!m_source.parent.empty()) body["__parent"] = m_source.parent;
	if (!store(m_texture_tag, json::value(std::move(body)))) return false;
	m_texture_written = true;
	return true;
}

bool edt::texture_preview_content::write_material()
{
	desc::desc_system* descs = m_services.descs();
	const json::object material{
		{ "__type", k_material_type },
		{ "__version", descs ? descs->current_version(k_material_type) : 1 },
		{ "__parent", k_base_material },
		{ "defines", json::array{ k_diffuse_define } },
		{ "samplers", json::array{ std::string(m_texture_tag.view()) } },
	};
	if (!store(m_material_tag, json::value(material))) return false;
	m_material_written = true;
	return true;
}

void edt::texture_preview_content::report_write_failure(std::string_view why)
{
	// update() retries every frame: one warning per generation of the source.
	const std::uint64_t generation = m_source.generation ? m_source.generation() : 0;
	if (m_write_failure_logged && m_failure_generation == generation) return;
	m_write_failure_logged = true;
	m_failure_generation = generation;
	egLOG_WARN("Editor/Texture", "The texture preview cannot be written: {}", why);
}

void edt::texture_preview_content::release()
{
	if (!m_texture_written && !m_material_written) return;
	if (res::resource_system* writer = m_services.writable_resources()) {
		for (const res::tag* tag : { &m_material_tag, &m_texture_tag }) {
			writer->erase(*tag);
			writer->signal_changed(*tag);
		}
	}
	m_texture_written = false;
	m_material_written = false;
}
