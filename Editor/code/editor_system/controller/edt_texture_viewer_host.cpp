#include "edt_texture_viewer_host.h"
#include "edt_texture_viewer_panel.h"
#include "edt_texture_window_model.h"
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>
#include <snk/desc/desc_system.h>
#include <snk/editor/edt_editor_context.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/editor/edt_panel_registry.h>
#include <snk/resource/res_system.h>
#include <boost/json.hpp>
#include <filesystem>
#include <format>
#include <optional>
#include <system_error>
#include <utility>

namespace json = boost::json;

namespace
{
	constexpr std::string_view k_texture_type = "texture_2d_desc";
	constexpr std::string_view k_base_texture = "res://base_texture.desc";
	constexpr std::string_view k_texture_suffix = ".txm.desc";
}

edt::picture_texture edt::picture_texture_of(const res::tag& picture)
{
	const std::filesystem::path relative(std::string(picture.relative()));
	picture_texture out;
	out.folder = relative.parent_path().generic_string();
	// The whole suffix: the creator replaces it by the type's own, whatever the asset kinds say.
	out.name = fs_paths::utf8_of(relative.stem()) + std::string(k_texture_suffix);
	out.fields = json::object{ { "__parent", k_base_texture }, { "data", std::string(picture.view()) } };
	return out;
}

json::object edt::initial_document_fields(std::string_view type)
{
	if (type == k_texture_type) return json::object{ { "__parent", k_base_texture } };
	return {};
}

std::string edt::picture_texture_refusal(const document_creator& creator, const res::tag& picture, bool in_top_layer)
{
	if (!in_top_layer) return "The picture is not in the project's own layer: a texture of it would be the project's, apart from it.";
	if (const std::optional<std::string> why = creator.blocker()) return *why;
	const picture_texture planned = picture_texture_of(picture);
	return creator.check(planned.folder, k_texture_type, planned.name);
}

res::tag edt::picture_texture_tag(const document_creator& creator, const res::tag& picture)
{
	const picture_texture planned = picture_texture_of(picture);
	const std::string file = creator.file_name_of(k_texture_type, planned.name);
	return res::tag::make(planned.folder.empty() ? file : std::format("{}/{}", planned.folder, file));
}

edt::document_creation edt::create_picture_texture(const document_creator& creator, const res::tag& picture)
{
	const picture_texture planned = picture_texture_of(picture);
	return creator.create(planned.folder, k_texture_type, planned.name, planned.fields);
}

edt::texture_viewer_host::texture_viewer_host(ports wired)
	: m_ports(std::move(wired))
{
}

edt::texture_viewer_host::~texture_viewer_host()
{
	shutdown();
}

void edt::texture_viewer_host::shutdown()
{
	m_viewers.clear();
}

bool edt::texture_viewer_host::is_open(const res::tag& source) const
{
	return m_viewers.contains(source.view());
}

edt::texture_viewer_panel* edt::texture_viewer_host::find(const res::tag& source) const
{
	const auto found = m_viewers.find(source.view());
	return found == m_viewers.end() ? nullptr : found->second.panel.get();
}

bool edt::texture_viewer_host::focus(const res::tag& source)
{
	texture_viewer_panel* open = find(source);
	if (!open) return false;
	open->set_open(true);
	open->request_focus();
	return true;
}

void edt::texture_viewer_host::close(const res::tag& source)
{
	// The registry holds its own reference while it draws: the window may close itself from its frame.
	if (const auto found = m_viewers.find(source.view()); found != m_viewers.end()) m_viewers.erase(found);
}

void edt::texture_viewer_host::add(const res::tag& source, std::shared_ptr<texture_viewer_panel> panel)
{
	panel->set_on_close([this, source] { close(source); });
	registration registered = m_ports.context.panels().add({
		.id = panel->get_id(), .title = panel->get_title(), .slot = dock_slot::floating, .open_by_default = true,
		.show_in_view_menu = false, .panel = panel });
	m_viewers.emplace(std::string(source.view()), viewer{ std::move(panel), std::move(registered) });
}

bool edt::texture_viewer_host::picture_in_top_layer(const res::tag& picture) const
{
	const desc_file_layout layout = m_ports.layout ? m_ports.layout() : desc_file_layout{};
	if (!layout.top_layer || picture.protocol() != res::tag::default_protocol()) return false;
	std::error_code ec;
	return std::filesystem::is_regular_file(layout.top_layer() / std::filesystem::path(std::string(picture.relative())), ec);
}

void edt::texture_viewer_host::open_picture(const res::tag& picture)
{
	if (focus(picture)) return;
	texture_preview_source preview;
	const json::object body{ { "data", std::string(picture.view()) } };
	preview.body = [body] { return json::value(body); };
	preview.generation = [] { return std::uint64_t{ 0 }; };
	preview.parent = std::string(k_base_texture);
	auto panel = std::make_shared<texture_viewer_panel>(std::format("texture_viewer:{}", ++m_serial), picture, std::move(preview),
		texture_source_of(json::value(body)), 1, m_ports.services,
		preview_view{ .render = &m_ports.context.render(), .gui = &m_ports.context.gui(), .input = m_input_router });
	texture_viewer_panel& viewer = *panel;
	add(picture, std::move(panel));
	refresh_picture(viewer, picture);
}

res::tag edt::existing_picture_texture(const document_creator& creator, res::resource_system& resources, const desc::desc_system& descs,
	const desc_file_layout& layout, const res::tag& picture)
{
	const res::tag texture = picture_texture_tag(creator, picture);
	const desc_file_read read = read_desc_file(resources, descs, layout, texture);
	if (!read.error.empty()) return {};
	boost::system::error_code ec;
	const json::value file = json::parse(std::string_view(reinterpret_cast<const char*>(read.bytes.data()), read.bytes.size()), ec);
	if (ec) return {};
	const texture_source source = texture_source_of(file);
	return source.from == texture_source::kind::file && source.file == picture.view() ? texture : res::tag{};
}

res::tag edt::texture_viewer_host::texture_of_picture(const res::tag& picture) const
{
	const document_creator* creator = m_ports.creator ? m_ports.creator() : nullptr;
	res::resource_system* resources = m_ports.services.resources();
	const desc::desc_system* descs = m_ports.services.descs();
	if (!creator || !resources || !descs || !m_ports.layout) return {};
	return existing_picture_texture(*creator, *resources, *descs, m_ports.layout(), picture);
}

void edt::texture_viewer_host::refresh_picture(texture_viewer_panel& viewer, const res::tag& picture)
{
	if (const res::tag existing = texture_of_picture(picture); existing.is_valid()) {
		viewer.set_existing([this, existing] {
			if (m_ports.open_texture) m_ports.open_texture(existing);
		});
		return;
	}
	viewer.set_existing({});
	const document_creator* creator = m_ports.creator ? m_ports.creator() : nullptr;
	if (!creator) return;
	viewer.set_create([this, picture] { create_for(picture); }, picture_texture_refusal(*creator, picture, picture_in_top_layer(picture)));
}

bool edt::texture_viewer_host::is_lower_layer_desc(const res::tag& tag) const
{
	res::resource_system* resources = m_ports.services.resources();
	const desc::desc_system* descs = m_ports.services.descs();
	if (!resources || !descs || !m_ports.layout) return false;
	const desc_file_read read = read_desc_file(*resources, *descs, m_ports.layout(), tag);
	return read.error.empty() && !read.top_layer;
}

void edt::texture_viewer_host::open_desc(const res::tag& desc)
{
	if (focus(desc)) return;
	json::value file;
	if (res::resource_system* resources = m_ports.services.resources(); resources && m_ports.services.descs() && m_ports.layout) {
		const desc_file_read read = read_desc_file(*resources, *m_ports.services.descs(), m_ports.layout(), desc);
		boost::system::error_code ec;
		file = json::parse(std::string_view(reinterpret_cast<const char*>(read.bytes.data()), read.bytes.size()), ec);
		if (ec) file = json::object{};
	}
	const json::object header = effective_texture_header(file);
	const json::value* data = header.if_contains("data");
	const json::value* levels = data && data->is_object() ? data->as_object().if_contains("mip_levels") : nullptr;
	const int mip_levels = levels && levels->is_int64() ? static_cast<int>(levels->as_int64()) : 1;
	texture_preview_source preview;
	// A child of the file that sets nothing: it reads what the runtime reads of the file, its parents included.
	preview.body = [] { return json::value(json::object{}); };
	preview.generation = [] { return std::uint64_t{ 0 }; };
	preview.parent = std::string(desc.view());
	auto panel = std::make_shared<texture_viewer_panel>(std::format("texture_viewer:{}", ++m_serial), desc, std::move(preview),
		texture_source_of(file), mip_levels, m_ports.services,
		preview_view{ .render = &m_ports.context.render(), .gui = &m_ports.context.gui(), .input = m_input_router });
	add(desc, std::move(panel));
}

bool edt::texture_viewer_host::create_for(const res::tag& picture)
{
	texture_viewer_panel* panel = find(picture);
	const document_creator* creator = m_ports.creator ? m_ports.creator() : nullptr;
	if (!panel || !creator) return false;
	const document_creation made = create_picture_texture(*creator, picture);
	if (!made.ok()) {
		egLOG_WARN("editor", "No texture was made for '{}': {}", picture.view(), made.error);
		panel->set_error(made.error);
		refresh_picture(*panel, picture);
		return false;
	}
	panel->set_error({});
	// The texture is there now: the button opens it.
	refresh_picture(*panel, picture);
	if (m_ports.open_texture) m_ports.open_texture(made.tag);
	return true;
}

void edt::texture_viewer_host::release_previews()
{
	for (auto& [source, open] : m_viewers) open.panel->stage().release_world();
}

void edt::texture_viewer_host::rebuild_previews()
{
	for (auto& [source, open] : m_viewers) open.panel->stage().rebuild_world();
}
