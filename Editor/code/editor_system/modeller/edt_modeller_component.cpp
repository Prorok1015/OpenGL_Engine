#include "edt_modeller_component.h"
#include "edt_mesh_document.h"
#include "edt_mesh_documents.h"
#include "edt_mesh_editor_panel.h"
#include "edt_prefab_close.h"
#include <snk/common/logger/engine_log.h>
#include <snk/editor/edt_asset_kind_registry.h>
#include <snk/editor/edt_dialog_service.h>
#include <snk/editor/edt_editor_context.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/editor/edt_problem_list.h>
#include <snk/render/rnd_render_system.h>
#include <snk/render/texture/rnd_texture_manager.h>
#include <format>
#include <optional>
#include <utility>

namespace
{
	constexpr const char* k_problem_source = "mesh";

	edt::problem row_of(const edt::mesh_document& mesh, edt::problem_severity severity, const std::string& message)
	{
		edt::problem row;
		row.severity = severity;
		row.file = std::string(mesh.source().view());
		row.message = message;
		return row;
	}
}

edt::modeller_component::modeller_component(ports from)
	: m_ports(std::move(from))
{
	m_panel = std::make_shared<mesh_editor_panel>(m_ports.rnd, m_ports.gui);
	m_panel->set_input_router(m_ports.input_router);
	m_panel->set_on_close([this] {
		if (m_shown != document_id{}) request_close(m_shown);
	});
	m_panel->set_on_save([this] { save_shown(); });
	m_panel->set_on_revert([this] { revert_shown(); });
	m_panel->set_on_overwrite_geometry([this] {
		if (m_shown != document_id{}) request_overwrite_geometry(m_shown);
	});
}

edt::modeller_component::~modeller_component() = default;

void edt::modeller_component::install(std::vector<registration>& out)
{
	out.push_back(m_ports.context.panels().add({
		.id = m_panel->get_id(), .title = m_panel->get_title(), .slot = dock_slot::floating, .open_by_default = false, .panel = m_panel }));

	const auto open_mesh = [this](editor_context&, const res::tag& tag) { open(tag); };
	asset_kind_registry& kinds = m_ports.context.asset_kinds();
	out.push_back(kinds.add({ .id = "poly", .suffix = ".poly.desc", .open = open_mesh }));
	out.push_back(kinds.add({ .id = "mesh", .suffix = ".geom.desc", .open = open_mesh }));
	const auto make = [this](editor_services& services) -> std::unique_ptr<previewable_document> {
		auto document = std::make_unique<mesh_document>(services);
		if (m_ports.desc_layout) document->set_files(m_ports.desc_layout());
		document->set_lock_source(m_ports.edits_locked);
		document->set_targets_release([this](const std::vector<res::tag>& tags) {
			auto& textures = m_ports.rnd.get_texture_manager();
			for (const res::tag& tag : tags) textures.remove(tag);
		});
		return document;
	};
	out.push_back(kinds.add_preview({ .id = std::string(k_mesh_preview_id), .suffix = ".poly.desc", .make_preview = make, .multiple = true }));
	out.push_back(kinds.add_preview({ .id = std::string(k_mesh_geometry_preview_id), .suffix = ".geom.desc", .make_preview = make, .multiple = true }));

	editor_events& events = m_ports.context.events();
	m_connections.push_back(events.document_activated.subscribe([this](document_id id) {
		if (mesh_document* mesh = mesh_by_id(m_ports.context.documents(), id)) show_in_window(mesh);
	}));
	m_connections.push_back(events.document_closed.subscribe([this](document_id id) {
		if (id == m_shown) {
			m_shown = document_id{};
			m_panel->set_context(nullptr);
			m_panel->set_open(false);
		}
		publish_problems();
	}));
	m_connections.push_back(events.document_changed.subscribe([this](document_id id, std::uint64_t) {
		if (mesh_by_id(m_ports.context.documents(), id)) publish_problems();
	}));
}

edt::mesh_document* edt::modeller_component::shown() const
{
	return m_shown == document_id{} ? nullptr : mesh_by_id(m_ports.context.documents(), m_shown);
}

void edt::modeller_component::show_in_window(mesh_document* mesh)
{
	m_shown = mesh->id();
	m_panel->set_context(mesh);
	m_panel->set_open(true);
}

void edt::modeller_component::show(document_id id)
{
	if (mesh_document* mesh = mesh_by_id(m_ports.context.documents(), id)) {
		show_in_window(mesh);
		m_ports.context.documents().set_active(id);
	}
}

void edt::modeller_component::report(const std::string& text)
{
	egLOG_WARN("editor", "{}", text);
	m_ports.context.dialogs().message(message_level::error, text);
}

void edt::modeller_component::open(const res::tag& source)
{
	document_manager& documents = m_ports.context.documents();
	if (mesh_document* open_already = mesh_of_source(documents, source)) {
		show(open_already->id());
		return;
	}
	const bool geometry = source.view().ends_with(".geom.desc");
	previewable_document* preview = documents.open_preview(geometry ? k_mesh_geometry_preview_id : k_mesh_preview_id, source);
	auto* mesh = dynamic_cast<mesh_document*>(preview);
	if (!mesh) {
		report(std::format("'{}' cannot be opened as a mesh", source.view()));
		return;
	}
	if (!mesh->is_open()) {
		report(std::format("'{}' was not opened:\n{}", source.pure_name(), mesh->last_error()));
		return;
	}
	show_in_window(mesh);
	documents.set_active(mesh->id());
	publish_problems();
}

void edt::modeller_component::save_shown()
{
	mesh_document* mesh = shown();
	if (!mesh) return;
	if (const save_result saved = mesh->save(); !saved.saved) report(std::format("'{}' was not saved:\n{}", mesh->title(), saved.error));
	publish_problems();
}

void edt::modeller_component::request_overwrite_geometry(document_id id)
{
	mesh_document* mesh = mesh_by_id(m_ports.context.documents(), id);
	if (!mesh) return;
	m_ports.context.dialogs().choose("Overwrite geometry",
		mesh->overwrite_geometry_question(), { "Overwrite", "Cancel" },
		[this, id = mesh->id(), alive = std::weak_ptr<int>(m_alive)](std::optional<std::size_t> choice) {
			if (!alive.lock() || choice != std::optional<std::size_t>{ 0 }) return;
			mesh_document* open = mesh_by_id(m_ports.context.documents(), id);
			if (!open) return;
			if (const save_result saved = open->overwrite_geometry(); !saved.saved) report(std::format("'{}' was not saved:\n{}", open->title(), saved.error));
			publish_problems();
		});
}

void edt::modeller_component::revert_shown()
{
	mesh_document* mesh = shown();
	if (!mesh) return;
	m_ports.context.dialogs().choose("Revert mesh", std::format("Drop the edits of '{}' and take the file again?", mesh->title()), { "Revert", "Cancel" },
		[this, id = mesh->id(), alive = std::weak_ptr<int>(m_alive)](std::optional<std::size_t> choice) {
			if (!alive.lock() || choice != std::optional<std::size_t>{ 0 }) return;
			mesh_document* open = mesh_by_id(m_ports.context.documents(), id);
			if (!open) return;
			if (const std::string error = open->revert(); !error.empty()) report(std::format("'{}' was not reverted:\n{}", open->title(), error));
			publish_problems();
		});
}

void edt::modeller_component::confirm_close(mesh_document& mesh, std::function<void()> proceed)
{
	const document_id id = mesh.id();
	prefab_close_ports close_ports;
	close_ports.ask = [this](std::function<void(unsaved_choice)> answer) {
		m_ports.context.dialogs().confirm_unsaved([answer = std::move(answer), alive = std::weak_ptr<int>(m_alive)](unsaved_choice choice) {
			if (alive.lock()) answer(choice);
		});
	};
	close_ports.apply = [this, id]() -> save_result {
		mesh_document* open = mesh_by_id(m_ports.context.documents(), id);
		return open ? open->save() : save_result::failure("the mesh is not open any more");
	};
	close_ports.report_failure = [this](const std::string& error) {
		report(std::format("The mesh was not saved and stays open:\n{}", error));
	};
	if (mesh.is_dirty()) m_panel->set_open(true);
	close_prefab_with_question(mesh.is_dirty(), close_ports, std::move(proceed));
}

void edt::modeller_component::request_close(document_id id)
{
	mesh_document* mesh = mesh_by_id(m_ports.context.documents(), id);
	if (!mesh) return;
	confirm_close(*mesh, [this, id] { close_now(id); });
}

void edt::modeller_component::close_now(document_id id)
{
	if (id == m_shown) {
		m_panel->set_context(nullptr);
		m_shown = document_id{};
		m_panel->set_open(false);
	}
	m_ports.context.documents().close(id);
	publish_problems();
}

bool edt::modeller_component::dirty() const
{
	return !dirty_meshes_of(m_ports.context.documents()).empty();
}

bool edt::modeller_component::save_dirty()
{
	for (mesh_document* mesh : dirty_meshes_of(m_ports.context.documents())) {
		if (const save_result saved = mesh->save(); !saved.saved) {
			report(std::format("'{}' was not saved:\n{}", mesh->title(), saved.error));
			publish_problems();
			return false;
		}
	}
	publish_problems();
	return true;
}

void edt::modeller_component::drop_dirty()
{
	for (mesh_document* mesh : dirty_meshes_of(m_ports.context.documents())) close_now(mesh->id());
}

void edt::modeller_component::detach_document()
{
	m_panel->set_context(nullptr);
	m_shown = document_id{};
}

void edt::modeller_component::park_for_layer_change()
{
	for (mesh_document* mesh : meshes_of(m_ports.context.documents())) mesh->release_preview_world();
}

void edt::modeller_component::restore_after_layer_change()
{
	for (mesh_document* mesh : meshes_of(m_ports.context.documents())) mesh->rebuild_preview_world();
}

void edt::modeller_component::publish_problems()
{
	std::vector<problem> items;
	for (mesh_document* mesh : meshes_of(m_ports.context.documents())) {
		if (mesh->geometry_stale()) {
			items.push_back(row_of(*mesh, problem_severity::error, mesh->stale_geometry_message()));
		} else if (mesh->geometry_mismatch()) {
			items.push_back(row_of(*mesh, problem_severity::warning, mesh->geometry_mismatch_message()));
		}
	}
	if (items.empty()) {
		m_ports.context.problems().clear(k_problem_source);
	} else {
		m_ports.context.problems().set(k_problem_source, std::move(items));
	}
}
