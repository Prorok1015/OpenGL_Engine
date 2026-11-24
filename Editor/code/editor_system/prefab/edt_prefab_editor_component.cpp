#include "edt_prefab_editor_component.h"
#include "edt_desc_problems.h"
#include "edt_document_cast.h"
#include "edt_inspector_panel.h"
#include "edt_prefab_close.h"
#include "edt_prefab_document.h"
#include "edt_prefab_editor_panel.h"
#include "edt_prefab_shape.h"
#include "edt_prefab_save_all.h"
#include "edt_prefab_variant_creator.h"
#include "edt_save_all.h"
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

namespace
{
	// What a prefab that did not load reported: its own scope of the source "desc", so the scope "prefab" stays the shown prefab's.
	constexpr const char* k_failed_open_scope = "prefab_open";
}

edt::prefab_editor_component::prefab_editor_component(ports from)
	: m_ports(from)
{
	m_panel = std::make_shared<prefab_editor_panel>(m_ports.rnd, m_ports.gui);
	m_panel->set_on_close([this] { request_close(); });
	m_panel->set_on_cancel_loading([this](std::uint64_t id) { cancel_open(id); });
	m_panel->set_on_node_selected([this](const std::vector<std::uint64_t>& path) { select_node(path); });
	m_panel->set_on_apply([this] {
		if (prefab_document* prefab = shown()) apply(*prefab);
	});
	m_panel->set_on_revert([this] {
		prefab_document* prefab = shown();
		if (!prefab) return;
		m_ports.context.dialogs().choose("Revert prefab", std::format("Drop the edits of '{}' and take the file again?", prefab->title()), { "Revert", "Cancel" },
			[this, id = prefab->id(), alive = std::weak_ptr<int>(m_alive)](std::optional<std::size_t> choice) {
				if (!alive.lock() || choice != std::optional<std::size_t>{ 0 }) return;
				prefab_document* open = prefab_by_id(id);
				if (!open) return;
				if (const std::string error = open->revert(); !error.empty()) {
					egLOG_WARN("editor", "{}", error);
					m_ports.context.dialogs().message(message_level::error, std::format("'{}' was not reverted:\n{}", open->title(), error));
				}
			});
	});
	m_panel->set_on_add_shape([this](const mesh::shape_request& request) {
		prefab_document* prefab = shown();
		if (!prefab) return;
		if (const std::string error = add_shape_to_prefab(m_ports.context.actions(), *prefab, request); !error.empty()) {
			egLOG_WARN("editor/shape", "Add shape failed: {}", error);
			m_ports.context.dialogs().message(message_level::error, std::format("The shape was not added:\n{}", error));
		}
	});
	m_panel->set_input_router(m_ports.input_router);
	m_ports.inspector.set_on_node_changed([this] {
		route_node_changed(m_inspector_target, shown(), m_ports.level);
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
		if (m_diagnostics.erase(id) == 0 || id != m_shown) return;
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
		if (prefab_document* prefab = prefab_by_id(m_inspector_target.prefab())) {
			m_ports.inspector.set_selected_node(prefab->working_node(m_inspector_target.path()));
		}
	}));
	// The working copy was replaced (Revert, a reload, another prefab): the node pointer the Inspector holds is stale.
	m_connections.push_back(events.document_changed.subscribe([this](document_id id, std::uint64_t) {
		if (!m_inspector_target.is_prefab() || id != m_inspector_target.prefab()) return;
		prefab_document* prefab = prefab_by_id(id);
		if (!prefab) return;
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
		if (id == m_shown) leave_shown();
	}));
	// A tab, the Unsaved list or a new document brought a prefab to the front: the window shows it.
	m_connections.push_back(events.document_activated.subscribe([this](document_id id) {
		if (m_keep_shown) return;
		if (prefab_document* prefab = prefab_by_id(id)) show_document(*prefab);
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
	m_ports.desc_problems.clear(m_ports.context.problems(), k_failed_open_scope);
	if (prefab_document* open = prefab_for(tag)) {
		m_ports.context.documents().set_active(open->id());
		show_document(*open);
		publish_diagnostics();
		m_panel->set_open(true);
		return;
	}
	open_now(tag);
}

bool edt::prefab_editor_component::opening(const res::tag& tag) const
{
	return std::ranges::any_of(m_pending, [&tag](const std::unique_ptr<pending_open>& open) { return open->watch.tag() == tag; });
}

edt::prefab_document* edt::prefab_editor_component::dirty() const
{
	return dirty_prefab_of(m_ports.context.documents());
}

edt::prefab_document* edt::prefab_editor_component::dirty_for(const res::tag& tag) const
{
	for (prefab_document* prefab : dirty_prefabs_of(m_ports.context.documents())) {
		if (prefab->source_tag() == tag) return prefab;
	}
	return nullptr;
}

bool edt::prefab_editor_component::apply_dirty()
{
	for (prefab_document* prefab : dirty_prefabs_of(m_ports.context.documents())) {
		if (!apply(*prefab)) return false;
	}
	return true;
}

void edt::prefab_editor_component::resolve_unsaved(const save_all_ask& ask, std::function<void(bool resolved)> resolved)
{
	prefab_document* prefab = prefab_by_id(ask.item.id);
	if (!prefab) {
		if (resolved) resolved(false);
		return;
	}
	const prefab_question_ports question{ m_ports.context.dialogs(),
		[this, id = prefab->id()]() -> prefab_document* { return prefab_by_id(id); },
		m_alive };
	ask_prefab_question(question, *prefab, ask.question, std::move(resolved));
}

void edt::prefab_editor_component::close()
{
	m_ports.desc_problems.clear(m_ports.context.problems(), k_failed_open_scope);
	cancel_open();
	release_inspector(true);
	auto& documents = m_ports.context.documents();
	for (prefab_document* prefab : prefabs()) documents.close(prefab->id());
	m_shown = document_id{};
	m_panel->set_context(nullptr);
	m_panel->set_open(false);
}

void edt::prefab_editor_component::close_document(document_id id)
{
	if (m_inspector_target.is_prefab() && m_inspector_target.prefab() == id) release_inspector(true);
	m_ports.context.documents().close(id);
	if (prefabs().empty() && !loading()) m_panel->set_open(false);
}

void edt::prefab_editor_component::request_close()
{
	if (const prefab_document* prefab = shown()) {
		request_close(prefab->id());
		return;
	}
	m_panel->set_open(false);
}

void edt::prefab_editor_component::request_close(document_id id)
{
	prefab_document* prefab = prefab_by_id(id);
	if (!prefab) return;
	// A window that was hidden stays hidden: closing a tab must not bring it back.
	const bool window_was_open = m_panel->is_open();
	confirm_close(prefab, [this, id, window_was_open] {
		close_document(id);
		if (window_was_open && shown()) m_panel->set_open(true);
	});
}

void edt::prefab_editor_component::close_dirty()
{
	for (prefab_document* prefab : dirty_prefabs_of(m_ports.context.documents())) close_document(prefab->id());
}

void edt::prefab_editor_component::close_for_layer_change()
{
	// Reload, quit and project switch wait for a dirty prefab; this is the way in of anything else (a module loaded by hand).
	if (dirty()) egLOG_WARN("editor", "the game layer changes: the prefab editor is closed with unapplied edits");
	close();
}

void edt::prefab_editor_component::show()
{
	if (!shown()) {
		const std::vector<prefab_document*> open = prefabs();
		if (!open.empty()) show_document(*open.back());
	}
	m_panel->set_open(true);
}

void edt::prefab_editor_component::detach_document()
{
	m_shown = document_id{};
	m_panel->set_context(nullptr);
}

std::vector<edt::prefab_document*> edt::prefab_editor_component::prefabs() const
{
	return prefabs_of(m_ports.context.documents());
}

edt::prefab_document* edt::prefab_editor_component::prefab_by_id(document_id id) const
{
	if (id == document_id{}) return nullptr;
	for (prefab_document* prefab : prefabs()) {
		if (prefab->id() == id) return prefab;
	}
	return nullptr;
}

edt::prefab_document* edt::prefab_editor_component::prefab_for(const res::tag& tag) const
{
	for (prefab_document* prefab : prefabs()) {
		if (prefab->source_tag() == tag) return prefab;
	}
	return nullptr;
}

edt::prefab_document* edt::prefab_editor_component::shown() const
{
	return prefab_by_id(m_shown);
}

void edt::prefab_editor_component::show_document(prefab_document& prefab)
{
	if (m_shown == prefab.id()) return;
	if (m_inspector_target.is_prefab()) release_inspector(true);
	m_shown = prefab.id();
	m_panel->clear_selection();
	m_panel->set_context(&prefab);
	publish_diagnostics();
}

void edt::prefab_editor_component::publish_diagnostics()
{
	const auto found = m_diagnostics.find(m_shown);
	if (found == m_diagnostics.end()) {
		m_ports.desc_problems.clear(m_ports.context.problems(), "prefab");
		return;
	}
	m_ports.desc_problems.report(m_ports.context.problems(), "prefab", found->second);
}

void edt::prefab_editor_component::leave_shown()
{
	m_shown = document_id{};
	if (m_inspector_target.is_prefab()) release_inspector(true);
	const std::vector<prefab_document*> left = prefabs();
	if (!left.empty()) {
		show_document(*left.back());
		// The tab strip highlights the active document: it follows the window.
		m_ports.context.documents().set_active(left.back()->id());
		return;
	}
	m_panel->clear_selection();
	m_panel->set_context(nullptr);
	publish_diagnostics();
}

void edt::prefab_editor_component::open_now(const res::tag& tag)
{
	res::resource_system* resources = m_ports.services.resources();
	if (!m_ports.register_frame || !resources) {
		finish_open(tag, {});
		return;
	}
	const bool waiting = std::ranges::any_of(m_pending, [&tag](const std::unique_ptr<pending_open>& open) { return open->watch.tag() == tag; });
	if (waiting) {
		m_panel->set_open(true);
		return;
	}
	m_pending.push_back(std::make_unique<pending_open>());
	m_pending.back()->id = m_next_pending_id++;
	m_pending.back()->watch.begin(resources->require<scn::prefab_desc>(tag));
	if (settle_open()) return;
	m_panel->set_open(true);
	if (m_polling) return;
	m_polling = true;
	m_ports.register_frame("edt/prefab_open", [this, alive = std::weak_ptr<int>(m_alive)] {
		if (!alive.lock()) return false;
		if (loading() && !settle_open()) return true;
		m_polling = false;
		return false;
	});
}

void edt::prefab_editor_component::show_loadings()
{
	std::vector<prefab_editor_panel::loading_item> items;
	items.reserve(m_pending.size());
	for (const auto& pending : m_pending) {
		items.push_back({ pending->id, std::format("Loading '{}'...", pending->watch.tag().pure_name()) });
	}
	m_panel->set_loadings(std::move(items));
}

bool edt::prefab_editor_component::settle_open()
{
	struct ended
	{
		res::tag tag;
		res::res_handle<scn::prefab_desc> handle;
		prefab_open_watch::state outcome;
	};
	std::vector<ended> finished;
	for (auto it = m_pending.begin(); it != m_pending.end();) {
		pending_open& pending = **it;
		const prefab_open_watch::state outcome = pending.watch.poll();
		if (pending.watch.nesting_limit_hit() && !pending.depth_warned) {
			pending.depth_warned = true;
			egLOG_WARN("editor", "'{}' nests prefabs deeper than 32 levels: the deeper ones are not waited for", pending.watch.tag().view());
		}
		if (outcome == prefab_open_watch::state::loading) {
			++it;
			continue;
		}
		if (outcome != prefab_open_watch::state::idle) {
			// The handle outlives finish_open: the cache holds the prefab weakly, and the document opens from what the wait kept loaded.
			finished.push_back({ pending.watch.tag(), pending.watch.take(), outcome });
		}
		it = m_pending.erase(it);
	}
	show_loadings();
	for (const ended& done : finished) {
		if (done.outcome == prefab_open_watch::state::ready) {
			finish_open(done.tag, done.handle);
		} else {
			report_failed_open(done.tag, done.handle, done.outcome == prefab_open_watch::state::timed_out);
		}
	}
	return m_pending.empty();
}

void edt::prefab_editor_component::report_failed_open(const res::tag& tag, const res::res_handle<scn::prefab_desc>& handle, bool timed_out)
{
	const std::vector<res::diagnostic> items = diagnostics_of(handle);
	// Not the scope "prefab": that one follows the prefab the window shows.
	m_ports.desc_problems.report(m_ports.context.problems(), k_failed_open_scope, items);
	std::string reason = timed_out ? std::format("it was still loading after {} s", prefab_open_watch::k_deadline.count()) : std::string{ "the file could not be loaded" };
	if (const auto error = std::ranges::find(items, res::diagnostic_severity::error, &res::diagnostic::severity); !timed_out && error != items.end()) {
		reason = error->reason;
	}
	egLOG_WARN("editor", "'{}' was not opened: {}", tag.view(), reason);
	m_ports.context.dialogs().message(message_level::error, std::format("'{}' was not opened:\n{}", tag.pure_name(), reason));
	if (prefabs().empty() && !loading()) m_panel->set_open(false);
}

void edt::prefab_editor_component::cancel_open()
{
	for (auto& pending : m_pending) pending->watch.cancel();
	m_pending.clear();
	m_panel->set_loadings({});
	if (prefabs().empty()) m_panel->set_open(false);
}

void edt::prefab_editor_component::cancel_open(std::uint64_t id)
{
	const auto found = std::ranges::find(m_pending, id, [](const std::unique_ptr<pending_open>& open) { return open->id; });
	if (found == m_pending.end()) return;
	(*found)->watch.cancel();
	m_pending.erase(found);
	show_loadings();
	if (prefabs().empty() && !loading()) m_panel->set_open(false);
}

void edt::prefab_editor_component::finish_open(const res::tag& tag, const res::res_handle<scn::prefab_desc>& loaded)
{
	const prefab_document* before = shown();
	const bool keep_window = before != nullptr && person_is_working_in_shown();
	const document_id kept_id = before ? before->id() : document_id{};
	m_keep_shown = keep_window;
	prefab_document& prefab = edt::open_prefab(m_ports.context.documents(), tag);
	m_keep_shown = false;
	// The diagnostics of the load the preview made, or of a load again when nothing kept it (no wait: tests, no frame port).
	const auto reread = loaded.is_valid() ? loaded : m_ports.services.resources()->require_sync<scn::prefab_desc>(tag);
	if (prefab.is_open()) {
		m_diagnostics[prefab.id()] = diagnostics_of(reread);
		if (keep_window) {
			// A tab, not the window: the prefab being worked on stays shown and stays the active document.
			m_ports.context.documents().set_active(kept_id);
			return;
		}
		// Its activation made it the shown one before its diagnostics were known.
		show_document(prefab);
		publish_diagnostics();
		m_panel->set_open(true);
	} else {
		m_ports.desc_problems.report(m_ports.context.problems(), k_failed_open_scope, diagnostics_of(reread));
		if (prefabs().empty() && !loading()) m_panel->set_open(false);
	}
}

bool edt::prefab_editor_component::person_is_working_in_shown() const
{
	const prefab_document* prefab = shown();
	if (!prefab) return false;
	return prefab->is_dirty() || m_inspector_target.is_prefab() || (m_ports.person_is_editing && m_ports.person_is_editing());
}

void edt::prefab_editor_component::confirm_close(prefab_document* prefab, std::function<void()> proceed)
{
	const bool dirty = prefab && prefab->is_dirty();
	if (dirty) {
		// The window shows what is asked about and stays while the question is open (closing it asked, nothing was decided).
		show_document(*prefab);
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
		prefab_document* open = prefab_by_id(id);
		return open ? open->save() : save_result::failure("the prefab is not open any more");
	};
	close_ports.report_failure = [this](const std::string& error) {
		egLOG_WARN("editor", "{}", error);
		m_ports.context.dialogs().message(message_level::error, std::format("The prefab was not applied and stays open:\n{}", error));
	};
	close_prefab_with_question(dirty, close_ports, std::move(proceed));
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
				prefab_document* open = prefab_by_id(id);
				if (!open) return;
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
	prefab_document* prefab = shown();
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
