#include "edt_shader_editor_component.h"
#include "edt_prefab_close.h"
#include "edt_shader_editor_panel.h"
#include "edt_shader_source_document.h"
#include "edt_shader_source_documents.h"
#include <snk/common/logger/engine_log.h>
#include <snk/editor/edt_asset_kind_registry.h>
#include <snk/editor/edt_dialog_service.h>
#include <snk/editor/edt_editor_context.h>
#include <snk/editor/edt_editor_services.h>
#include <format>
#include <optional>
#include <utility>

edt::shader_editor_component::shader_editor_component(ports from)
	: m_ports(std::move(from))
{
	m_panel = std::make_shared<shader_editor_panel>();
	m_panel->set_problems(&m_ports.context.problems());
	m_panel->set_on_close([this] {
		if (m_shown != document_id{}) request_close(m_shown);
	});
	m_panel->set_on_save([this] { save_shown(); });
	m_panel->set_on_revert([this] { request_revert_shown(); });
	if (m_ports.check_source) {
		m_panel->set_on_check([this]() -> std::string {
			const shader_source_document* shader = shader_source_by_id(m_ports.context.documents(), m_shown);
			if (!shader) return {};
			const std::string report = m_ports.check_source(shader->source());
			return shader->is_dirty() ? "The saved file is checked, not the unsaved edits. " + report : report;
		});
	}
}

edt::shader_editor_component::~shader_editor_component() = default;

void edt::shader_editor_component::install(std::vector<registration>& out)
{
	out.push_back(m_ports.context.panels().add({
		.id = m_panel->get_id(), .title = m_panel->get_title(), .slot = dock_slot::floating, .open_by_default = false, .panel = m_panel }));

	asset_kind_registry& kinds = m_ports.context.asset_kinds();
	const auto make = [this](editor_services& services) -> std::unique_ptr<previewable_document> {
		auto document = std::make_unique<shader_source_document>(services);
		if (m_ports.desc_layout) document->set_files(m_ports.desc_layout());
		document->set_lock_source(m_ports.edits_locked);
		return document;
	};
	for (const std::string_view suffix : k_shader_source_suffixes) {
		out.push_back(kinds.add({ .id = shader_source_id(suffix), .suffix = std::string(suffix), .icon = "[G]",
			.open = [this](editor_context&, const res::tag& tag) { open(tag, 0, m_ports.open_is_quiet && m_ports.open_is_quiet()); } }));
		out.push_back(kinds.add_preview({ .id = shader_source_id(suffix), .suffix = std::string(suffix), .make_preview = make, .multiple = true }));
	}

	editor_events& events = m_ports.context.events();
	m_connections.push_back(events.document_activated.subscribe([this](document_id id) {
		if (shader_source_document* shader = shader_source_by_id(m_ports.context.documents(), id)) show_in_window(shader);
	}));
	m_connections.push_back(events.document_closed.subscribe([this](document_id id) {
		if (id == m_shown) {
			m_shown = document_id{};
			m_panel->set_context(nullptr);
			m_panel->set_open(false);
		}
	}));
}

edt::shader_source_document* edt::shader_editor_component::shown() const
{
	return m_shown == document_id{} ? nullptr : shader_source_by_id(m_ports.context.documents(), m_shown);
}

void edt::shader_editor_component::show_in_window(shader_source_document* shader)
{
	m_shown = shader->id();
	m_panel->set_context(shader);
	m_panel->set_open(true);
}

void edt::shader_editor_component::show(document_id id)
{
	if (shader_source_document* shader = shader_source_by_id(m_ports.context.documents(), id)) {
		show_in_window(shader);
		m_ports.context.documents().set_active(id);
	}
}

void edt::shader_editor_component::report(const std::string& text, bool quiet)
{
	egLOG_WARN("editor", "{}", text);
	if (!quiet) m_ports.context.dialogs().message(message_level::error, text);
}

bool edt::shader_editor_component::open(const res::tag& source, int line, bool quiet)
{
	document_manager& documents = m_ports.context.documents();
	const std::string preview_id = shader_preview_id_of(source.view());
	if (preview_id.empty()) {
		report(std::format("'{}' is not a shader source", source.view()), quiet);
		return false;
	}
	shader_source_document* shader = nullptr;
	if (document* open_already = documents.find_by_source(source)) {
		shader = dynamic_cast<shader_source_document*>(open_already);
	}
	if (!shader) {
		auto* opened = dynamic_cast<shader_source_document*>(documents.open_preview(preview_id, source));
		if (!opened) {
			report(std::format("'{}' cannot be opened as a shader source", source.view()), quiet);
			return false;
		}
		if (!opened->is_open()) {
			report(std::format("'{}' was not opened:\n{}", source.pure_name(), opened->last_error()), quiet);
			return false;
		}
		shader = opened;
	}
	show_in_window(shader);
	documents.set_active(shader->id());
	if (line > 0) m_panel->go_to_line(line);
	return true;
}

void edt::shader_editor_component::save_shown()
{
	shader_source_document* shader = shown();
	if (!shader) return;
	m_panel->flush_edits();
	if (const save_result saved = shader->save(); !saved.saved) report(std::format("'{}' was not saved:\n{}", shader->title(), saved.error));
}

void edt::shader_editor_component::request_revert_shown()
{
	shader_source_document* shader = shown();
	if (!shader) return;
	m_ports.context.dialogs().choose("Revert shader", std::format("Drop the edits of '{}' and take the file again?", shader->title()), { "Revert", "Cancel" },
		[this, id = shader->id(), alive = std::weak_ptr<int>(m_alive)](std::optional<std::size_t> choice) {
			if (!alive.lock() || choice != std::optional<std::size_t>{ 0 }) return;
			// The window may show another shader by now (or none): the answer was about the one asked.
			if (m_shown != id) return;
			m_panel->request_revert();
			if (!m_panel->error().empty()) report(std::format("The shader was not reverted:\n{}", m_panel->error()));
		});
}

void edt::shader_editor_component::confirm_close(shader_source_document& shader, std::function<void()> proceed)
{
	const document_id id = shader.id();
	if (id == m_shown) m_panel->flush_edits();
	prefab_close_ports close_ports;
	close_ports.ask = [this](std::function<void(unsaved_choice)> answer) {
		m_ports.context.dialogs().confirm_unsaved([answer = std::move(answer), alive = std::weak_ptr<int>(m_alive)](unsaved_choice choice) {
			if (alive.lock()) answer(choice);
		});
	};
	close_ports.apply = [this, id]() -> save_result {
		shader_source_document* open = shader_source_by_id(m_ports.context.documents(), id);
		return open ? open->save() : save_result::failure("the shader is not open any more");
	};
	close_ports.report_failure = [this](const std::string& error) {
		report(std::format("The shader was not saved and stays open:\n{}", error));
	};
	if (shader.is_dirty()) m_panel->set_open(true);
	close_prefab_with_question(shader.is_dirty(), close_ports, std::move(proceed));
}

void edt::shader_editor_component::request_close(document_id id)
{
	shader_source_document* shader = shader_source_by_id(m_ports.context.documents(), id);
	if (!shader) return;
	confirm_close(*shader, [this, id] { close_now(id); });
}

void edt::shader_editor_component::close_now(document_id id)
{
	if (id == m_shown) {
		m_panel->set_context(nullptr);
		m_shown = document_id{};
		m_panel->set_open(false);
	}
	m_ports.context.documents().close(id);
}

bool edt::shader_editor_component::save_active()
{
	document* active = m_ports.context.documents().active();
	if (!active || active->kind() != "shader_source") return false;
	shader_source_document* shader = shader_source_by_id(m_ports.context.documents(), active->id());
	if (!shader) return false;
	if (shader->id() == m_shown) m_panel->flush_edits();
	if (const save_result saved = shader->save(); !saved.saved) report(std::format("'{}' was not saved:\n{}", shader->title(), saved.error));
	return true;
}

bool edt::shader_editor_component::dirty() const
{
	m_panel->flush_edits();
	return !dirty_shader_sources_of(m_ports.context.documents()).empty();
}

bool edt::shader_editor_component::save_dirty()
{
	m_panel->flush_edits();
	for (shader_source_document* shader : dirty_shader_sources_of(m_ports.context.documents())) {
		if (const save_result saved = shader->save(); !saved.saved) {
			report(std::format("'{}' was not saved:\n{}", shader->title(), saved.error));
			return false;
		}
	}
	return true;
}

void edt::shader_editor_component::drop_dirty()
{
	for (shader_source_document* shader : dirty_shader_sources_of(m_ports.context.documents())) close_now(shader->id());
}

void edt::shader_editor_component::detach_document()
{
	m_panel->set_context(nullptr);
	m_shown = document_id{};
}
