#include "edt_prefab_editor_component.h"
#include "edt_desc_problems.h"
#include "edt_document_cast.h"
#include "edt_inspector_panel.h"
#include "edt_prefab_close.h"
#include "edt_prefab_document.h"
#include "edt_prefab_editor_panel.h"
#include "edt_prefab_variant_creator.h"
#include "edt_scene_document.h"
#include <snk/common/logger/engine_log.h>
#include <snk/command/cmd_types.h>
#include <snk/editor/edt_editor_context.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/resource/res_system.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <boost/json.hpp>
#include <algorithm>
#include <format>
#include <memory>
#include <optional>
#include <utility>

edt::prefab_editor_component::prefab_editor_component(ports from)
	: m_ports(from)
{
	m_panel = std::make_shared<prefab_editor_panel>(m_ports.rnd, m_ports.gui);
	m_panel->set_on_close([this] { confirm_close([this] { close(); }); });
	m_panel->set_on_cancel_loading([this] { cancel_open(); });
	m_panel->set_on_node_selected([this](const std::vector<std::uint64_t>& path) { select_node(path); });
	m_panel->set_on_apply([this] {
		if (prefab_document* prefab = prefab_of(m_ports.context.documents())) apply(*prefab);
	});
	m_panel->set_on_revert([this] {
		prefab_document* prefab = prefab_of(m_ports.context.documents());
		if (!prefab) return;
		m_ports.context.dialogs().choose("Revert prefab", std::format("Drop the edits of '{}' and take the file again?", prefab->title()), { "Revert", "Cancel" },
			[this, id = prefab->id(), alive = std::weak_ptr<int>(m_alive)](std::optional<std::size_t> choice) {
				if (!alive.lock() || choice != std::optional<std::size_t>{ 0 }) return;
				prefab_document* open = prefab_of(m_ports.context.documents());
				if (!open || open->id() != id) return;
				if (const std::string error = open->revert(); !error.empty()) {
					egLOG_WARN("editor", "{}", error);
					m_ports.context.dialogs().message(message_level::error, std::format("'{}' was not reverted:\n{}", open->title(), error));
				}
			});
	});
	m_panel->set_input_router(m_ports.input_router);
	m_ports.inspector.set_on_node_changed([this] {
		route_node_changed(m_inspector_target, prefab_of(m_ports.context.documents()), m_ports.level);
	});
}

edt::prefab_editor_component::~prefab_editor_component()
{
	// The Inspector outlives the component and its callback holds `this`.
	m_ports.inspector.set_on_node_changed(nullptr);
}

void edt::prefab_editor_component::install(std::vector<registration>& out)
{
	editor_events& events = m_ports.context.events();
	// However the preview goes (another prefab, another kind of preview, a module unloading, quitting).
	m_connections.push_back(events.document_closed.subscribe([this](document_id id) {
		if (id == document_id{} || id != m_desc_document) return;
		m_desc_document = document_id{};
		m_ports.desc_problems.clear(m_ports.context.problems(), "prefab");
	}));
	// A level selection takes the Inspector back from the prefab window; the selection wiring of the panels has set its node
	// already, so the Inspector is not blanked. An empty one (level undo, Esc in the viewport) blanked it there: the prefab
	// node stays.
	m_connections.push_back(events.selection_changed.subscribe([this](const selection& sel) {
		if (!sel.empty()) {
			release_inspector(false);
			return;
		}
		if (!m_inspector_target.is_prefab()) return;
		prefab_document* prefab = prefab_of(m_ports.context.documents());
		if (prefab && prefab->id() == m_inspector_target.prefab()) {
			m_ports.inspector.set_selected_node(prefab->working_node(m_inspector_target.path()));
		}
	}));
	// The working copy was replaced (Revert, a reload, another prefab): the node pointer the Inspector holds is stale.
	m_connections.push_back(events.document_changed.subscribe([this](document_id id, std::uint64_t) {
		if (!m_inspector_target.is_prefab() || id != m_inspector_target.prefab()) return;
		prefab_document* prefab = prefab_of(m_ports.context.documents());
		if (!prefab || prefab->id() != id) return;
		if (prefab->working_replaced() == m_inspector_target.replaced()) return;
		m_inspector_target.set_replaced(prefab->working_replaced());
		scn::prefab_desc::prefab_node* node = prefab->working_node(m_inspector_target.path());
		if (!node) {
			release_inspector(true);
			return;
		}
		m_ports.inspector.set_selected_node(node);
	}));
	m_connections.push_back(events.document_closed.subscribe([this](document_id id) {
		if (m_inspector_target.is_prefab() && id == m_inspector_target.prefab()) release_inspector(true);
	}));
	out.push_back(m_ports.context.panels().add({
		.id = m_panel->get_id(), .title = m_panel->get_title(), .slot = dock_slot::floating, .open_by_default = false, .panel = m_panel }));
	out.push_back(m_ports.context.actions().add({ .id = "prefab.create_variant", .label = "Create Prefab Variant",
		.disabled_reason = [this] { return m_ports.desc_layout ? std::string{} : std::string{ "no project is open" }; },
		.execute_call = [this](const cmd::call& call) { return create_variant(call); },
		.args_schema = boost::json::parse(R"json({
			"type": "object",
			"properties": { "base": { "type": "string" }, "name": { "type": "string" } },
			"required": ["base"],
			"additionalProperties": false
		})json"),
		.result_schema = boost::json::parse(R"json({
			"type": "object",
			"properties": { "tag": { "type": "string" } },
			"required": ["tag"],
			"additionalProperties": false
		})json"),
		.enabled = [this] { return static_cast<bool>(m_ports.desc_layout); },
		.category = cmd::agent_category::scene_edit }));
}

cmd::result edt::prefab_editor_component::create_variant(const cmd::call& call)
{
	const boost::json::object& args = call.args.as_object();
	if (!m_ports.desc_layout) return cmd::result::failure(cmd::codes::failed, "no project is open");
	const desc_file_layout files = m_ports.desc_layout();
	prefab_variant_creator creator({
		.descs = [this] { return m_ports.services.descs(); },
		.resources = [this] { return m_ports.services.writable_resources(); },
		.layers = files.layers,
		.blocker = files.write_blocker },
		[this, files, agent = call.from.kind != cmd::origin_kind::ui](const res::tag& tag) {
			desc::desc_system* descs = m_ports.services.descs();
			res::resource_system* resources = m_ports.services.resources();
			return descs && resources ? open_level_claim(*resources, *descs, files, tag, agent) : std::string{};
		});
	const res::tag base{ std::string(args.at("base").as_string()) };
	std::string stem;
	if (const boost::json::value* given = args.if_contains("name"); given && given->is_string()) stem = std::string(given->as_string());
	const document_creation made = creator.create(base, stem);
	if (!made.ok()) return cmd::result::failure(made.busy ? cmd::codes::busy : cmd::codes::invalid_args, made.error);
	// An agent makes the file; a person's call is the one that gets the window (an agent must not open an editor for a person).
	if (call.from.kind == cmd::origin_kind::ui) open(made.tag);
	if (call.from.kind == cmd::origin_kind::agent) {
		m_ports.context.events().agent_action.emit(agent_action_info{ "prefab.create_variant", std::string(made.tag.view()), 0, call.from.session });
	}
	boost::json::object out;
	out["tag"] = std::string(made.tag.view());
	return cmd::result::success(std::move(out));
}

void edt::prefab_editor_component::open(const res::tag& tag)
{
	prefab_document* current = prefab_of(m_ports.context.documents());
	if (current && current->is_open() && current->is_dirty()) {
		if (current->source_tag() == tag) {
			m_panel->set_open(true);
			return;
		}
		confirm_close([this, tag] { open_now(tag); });
		return;
	}
	open_now(tag);
}

edt::prefab_document* edt::prefab_editor_component::dirty() const
{
	return dirty_prefab_of(m_ports.context.documents());
}

bool edt::prefab_editor_component::apply_dirty()
{
	prefab_document* prefab = dirty();
	return !prefab || apply(*prefab);
}

void edt::prefab_editor_component::close()
{
	m_open_watch.cancel();
	m_panel->set_loading({});
	release_inspector(true);
	auto& documents = m_ports.context.documents();
	if (prefab_document* prefab = prefab_of(documents)) {
		documents.close(prefab->id());
	}
	m_panel->set_context(nullptr);
	m_panel->set_open(false);
}

void edt::prefab_editor_component::drop_dirty_document()
{
	auto& documents = m_ports.context.documents();
	if (prefab_document* preview = prefab_of(documents); preview != nullptr && preview->is_dirty()) {
		documents.close(preview->id());
	}
}

void edt::prefab_editor_component::close_for_layer_change()
{
	// Reload, quit and project switch wait for a dirty prefab; this is the way in of anything else (a module loaded by hand).
	if (dirty()) egLOG_WARN("editor", "the game layer changes: the prefab editor is closed with unapplied edits");
	close();
}

void edt::prefab_editor_component::show()
{
	m_panel->set_open(true);
}

void edt::prefab_editor_component::detach_document()
{
	m_panel->set_context(nullptr);
}

void edt::prefab_editor_component::open_now(const res::tag& tag)
{
	res::resource_system* resources = m_ports.services.resources();
	if (!m_ports.register_frame || !resources) {
		finish_open(tag, {});
		return;
	}
	m_depth_warned = false;
	m_open_watch.begin(resources->require<scn::prefab_desc>(tag));
	if (settle_open()) return;
	m_panel->set_loading(std::format("Loading '{}'...", tag.pure_name()));
	m_panel->set_open(true);
	if (m_polling) return;
	m_polling = true;
	m_ports.register_frame("edt/prefab_open", [this, alive = std::weak_ptr<int>(m_alive)] {
		if (!alive.lock()) return false;
		if (m_open_watch.active() && !settle_open()) return true;
		m_polling = false;
		return false;
	});
}

bool edt::prefab_editor_component::settle_open()
{
	const prefab_open_watch::state outcome = m_open_watch.poll();
	if (m_open_watch.nesting_limit_hit() && !m_depth_warned) {
		m_depth_warned = true;
		egLOG_WARN("editor", "'{}' nests prefabs deeper than 32 levels: the deeper ones are not waited for", m_open_watch.tag().view());
	}
	if (outcome == prefab_open_watch::state::loading) return false;
	if (outcome == prefab_open_watch::state::idle) return true;
	const res::tag tag = m_open_watch.tag();
	// The handle outlives finish_open: the cache holds the prefab weakly, and the document opens from what the wait kept loaded.
	const res::res_handle<scn::prefab_desc> handle = m_open_watch.take();
	m_panel->set_loading({});
	if (outcome == prefab_open_watch::state::ready) {
		finish_open(tag, handle);
	} else {
		report_failed_open(tag, handle, outcome == prefab_open_watch::state::timed_out);
	}
	return true;
}

void edt::prefab_editor_component::report_failed_open(const res::tag& tag, const res::res_handle<scn::prefab_desc>& handle, bool timed_out)
{
	const std::vector<res::diagnostic> items = diagnostics_of(handle);
	m_ports.desc_problems.report(m_ports.context.problems(), "prefab", items);
	std::string reason = timed_out ? std::format("it was still loading after {} s", prefab_open_watch::k_deadline.count()) : std::string{ "the file could not be loaded" };
	if (const auto error = std::ranges::find(items, res::diagnostic_severity::error, &res::diagnostic::severity); !timed_out && error != items.end()) {
		reason = error->reason;
	}
	egLOG_WARN("editor", "'{}' was not opened: {}", tag.view(), reason);
	m_ports.context.dialogs().message(message_level::error, std::format("'{}' was not opened:\n{}", tag.pure_name(), reason));
	const prefab_document* open = prefab_of(m_ports.context.documents());
	if (!open || !open->is_open()) m_panel->set_open(false);
}

void edt::prefab_editor_component::cancel_open()
{
	m_open_watch.cancel();
	m_panel->set_loading({});
	const prefab_document* open = prefab_of(m_ports.context.documents());
	if (!open || !open->is_open()) m_panel->set_open(false);
}

void edt::prefab_editor_component::finish_open(const res::tag& tag, const res::res_handle<scn::prefab_desc>& loaded)
{
	release_inspector(true);
	prefab_document& prefab = edt::open_prefab(m_ports.context.documents(), tag);
	// The diagnostics of the load the preview made, or of a load again when nothing kept it (no wait: tests, no frame port).
	const auto reread = loaded.is_valid() ? loaded : m_ports.services.resources()->require_sync<scn::prefab_desc>(tag);
	m_ports.desc_problems.report(m_ports.context.problems(), "prefab", diagnostics_of(reread));
	m_desc_document = prefab.is_open() ? prefab.id() : document_id{};
	if (prefab.is_open()) {
		m_panel->set_context(&prefab);
		m_panel->set_open(true);
	} else {
		m_panel->set_open(false);
	}
}

void edt::prefab_editor_component::confirm_close(std::function<void()> proceed)
{
	prefab_document* prefab = dirty();
	if (prefab) {
		// The window stays while the question is open (closing it asked, nothing was decided).
		m_panel->set_open(true);
	}
	const document_id id = prefab ? prefab->id() : document_id{};
	prefab_close_ports close_ports;
	close_ports.ask = [this](std::function<void(unsaved_choice)> answer) {
		m_ports.context.dialogs().confirm_unsaved([answer = std::move(answer), alive = std::weak_ptr<int>(m_alive)](unsaved_choice choice) {
			if (alive.lock()) answer(choice);
		});
	};
	close_ports.apply = [this, id]() -> save_result {
		prefab_document* open = prefab_of(m_ports.context.documents());
		return open && open->id() == id ? open->save() : save_result::failure("the prefab is not open any more");
	};
	close_ports.report_failure = [this](const std::string& error) {
		egLOG_WARN("editor", "{}", error);
		m_ports.context.dialogs().message(message_level::error, std::format("The prefab was not applied and stays open:\n{}", error));
	};
	close_prefab_with_question(prefab != nullptr, close_ports, std::move(proceed));
}

bool edt::prefab_editor_component::apply(prefab_document& prefab)
{
	const save_result saved = prefab.save();
	if (saved.saved) return true;
	egLOG_WARN("editor", "{}", saved.error);
	if (prefab.layer_refused()) {
		m_ports.context.dialogs().choose("Apply to a variant",
			std::format("{}\n\nThe edits stay in the window. The file can be saved whole instead, without '__parent': it then stops following its base prefab.", saved.error),
			{ "Save without base", "Cancel" },
			[this, id = prefab.id(), alive = std::weak_ptr<int>(m_alive)](std::optional<std::size_t> choice) {
				if (!alive.lock() || choice != std::optional<std::size_t>{ 0 }) return;
				prefab_document* open = prefab_of(m_ports.context.documents());
				if (!open || open->id() != id) return;
				if (const save_result whole = open->save_without_base(); !whole.saved) {
					egLOG_WARN("editor", "{}", whole.error);
					m_ports.context.dialogs().message(message_level::error, std::format("'{}' was not saved:\n{}", open->title(), whole.error));
				}
			});
		return false;
	}
	m_ports.context.dialogs().message(message_level::error, std::format("'{}' was not applied:\n{}", prefab.title(), saved.error));
	return false;
}

void edt::prefab_editor_component::select_node(const std::vector<std::uint64_t>& path)
{
	prefab_document* prefab = prefab_of(m_ports.context.documents());
	if (!prefab) return;
	// The level's selection ends first (its event would otherwise hand the Inspector back to the level).
	m_ports.context.selection().clear();
	m_inspector_target.select_prefab(prefab->id(), path, prefab->working_replaced());
	m_ports.inspector.set_selected_node(prefab->working_node(path));
}

void edt::prefab_editor_component::release_inspector(bool blank_inspector)
{
	if (!m_inspector_target.is_prefab()) return;
	// A drag or a typed value that has not committed yet belongs to the prefab: route it while the target still says so.
	m_ports.inspector.flush_pending_commit();
	m_inspector_target.reset();
	if (blank_inspector) m_ports.inspector.set_selected_node(nullptr);
	m_panel->clear_selection();
}
