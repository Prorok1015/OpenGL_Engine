#include "edt_play_session.h"
#include "edt_input_target.h"
#include "edt_scene_document.h"
#include "edt_scene_editor.h"
#include "edt_world_controller.h"
#include <snk/editor/edt_editor_services.h>
#include <snk/editor/edt_selection_service.h>
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>
#include <snk/desc/desc_schema_ops.h>
#include <snk/ecs/ecs_bus.hpp>
#include <snk/ecs/ecs_random.hpp>
#include <snk/ecs/ecs_system.h>
#include <snk/input/inp_input_router.h>
#include <snk/project/prj_json_text.h>
#include <snk/resource/res_system.h>
#include <snk/resource/res_tag.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_context_set.h>
#include <snk/scene/level/scn_prefab_spawn.h>
#include <snk/scene/level/scn_world_snapshot.h>
#include <snk/scene/scn_component_catalog.h>
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/scene/scn_scene_layer.h>
#include <entt/entt.hpp>
#include <algorithm>
#include <cmath>
#include <exception>
#include <format>
#include <map>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

edt::play_session::play_session(scene_document& document, editor_services& services, world_controller& worlds,
	selection_service& selection, editor_events& events, play_session_options options)
	: m_document(document)
	, m_services(services)
	, m_worlds(worlds)
	, m_selection(selection)
	, m_events(events)
	, m_options(std::move(options))
	, m_seed(ecs::rng::default_seed)
{
	m_world_changed = m_events.active_world_changed.subscribe([this](document_id) {
		if (active()) follow_active_world();
	});
}

edt::play_session::~play_session()
{
	if (active()) stop(play_reason::user);
}

void edt::play_session::abandon()
{
	m_world_changed.disconnect();
	m_worlds_copy.clear();
	m_state = play_state::stopped;
}

std::optional<std::string> edt::play_session::start_refusal() const
{
	if (active()) return std::string{ "Play is running already" };
	if (m_stopping) return std::string{ "Play is stopping" };
	if (!m_services.ctx_manager.lock()) return std::string{ "there is no context manager" };
	if (!m_document.editor_tag.is_valid() || m_document.world_descs.empty()) return std::string{ "no level with a world to play" };
	if (!m_services.assembler() || !m_services.sfactory()) return std::string{ "the scene services are missing" };
	if (m_options.start_blocker) return m_options.start_blocker();
	return std::nullopt;
}

bool edt::play_session::start(play_reason reason)
{
	if (auto refused = start_refusal()) {
		egLOG_WARN("play", "Play not started: {}", *refused);
		return false;
	}
	const auto contexts = m_services.ctx_manager.lock();
	if (!ENGINE_VERIFY(contexts->find_set(k_play_set) == nullptr, "Play not started: the set '{}' is left from before", k_play_set)) {
		return false;
	}
	scn::context_set& level = contexts->level_set();

	// What Stop brings back.
	const std::string* active_name = m_document.active_world_name();
	if (active_name) {
		if (scn::world_context* world = level.find(*active_name)) save_camera_state(world->state(), m_document.camera);
	}
	m_before = snapshot{};
	m_before.camera = m_document.camera;
	m_before.active_world = active_name ? *active_name : std::string{};
	m_before.selected = m_selection.current();
	m_before.level_active = level.active();
	level.for_each([this](scn::world_context& world) { m_before.extract.emplace_back(std::string{ world.get_name() }, world.extract()); });

	// Copies of the document, never the files: Play shows the level as it is edited, saved or not.
	m_worlds_copy = m_document.world_descs;
	const std::vector<std::string> names = m_document.world_names;
	const std::vector<std::vector<std::string>> systems = m_document.worlds_systems_list;
	// What was injected into "play" or asked of its stack before it started is not for this session.
	if (m_options.input) {
		m_options.input->forget_injected(k_play_set);
		m_options.input->forget_commands(k_play_set);
	}
	scn::context_set& play = contexts->create_set(k_play_set, contexts->game_layer(), scn::set_options{ .hot_reload = false });
	// Before the contexts: a context takes the mode, seed and time scale of its set when it is created.
	play.set_mode(ecs::run_mode::play);
	play.set_seed(m_seed);
	play.set_time_scale(m_time_scale);
	m_time_scale = play.time_scale();
	{
		// The tasks the systems and spawners start while the worlds are built belong to the set: Stop cancels them.
		const scn::context_set::current_scope as_current(play);
		for (std::size_t i = 0; i < m_worlds_copy.size(); ++i) {
			const scn::world_desc& copy = m_worlds_copy[i];
			const std::string name = i < names.size() ? names[i] : std::string{ copy.get_name() };
			scn::world_context& world = contexts->create_context(k_play_set, name, scn::context_kind::play,
				i < systems.size() ? systems[i] : copy.get_systems());
			world.set_input_layer(copy.get_input_layer());
			world.set_input_stack(copy.get_input_stack());
			const entt::entity root = world.state().create();
			m_services.assembler()->spawn_from_desc(world.state(), root, copy, name);
		}
	}

	// The level stays as it is, frozen and not drawn; its cameras come back on Stop.
	level.set_active(false);
	level.for_each([](scn::world_context& world) { world.set_extract(false); });

	m_document.set_edits_locked(true);
	mount_user({ m_options.play_profile }, true);
	m_play_camera = m_document.camera;
	m_camera_world.clear();
	follow_active_world();
	egLOG_INFO("play", "Play started: {} world(s), seed {}", m_worlds_copy.size(), m_seed);
	++m_run_number;
	m_reload_number = 0;
	set_state(play_state::playing, reason);
	return true;
}

void edt::play_session::stop(play_reason reason)
{
	if (!active() || m_stopping) return;
	// The state changes (and is announced) last: until then a handler of an event raised here sees Play stopping.
	m_stopping = true;
	if (const auto contexts = m_services.ctx_manager.lock()) {
		if (contexts->find_set(k_play_set)) {
			// The set stops its own tasks before its registries go; the level's tasks of the same layer go on.
			contexts->destroy_set(k_play_set);
		}
	}
	m_worlds_copy.clear();
	m_camera_world.clear();
	m_reload.reset();
	m_suspended = false;
	// A revert asked for and not yet run is dropped: the build that loaded stays, nothing comes back to this session.
	m_reverting = false;
	m_restore_failure.clear();
	m_revert_failure.clear();
	// The poll of a wait stops at its next frame; it never reaches the set that is gone.
	m_waiting.reset();
	++m_wait_generation;
	if (m_options.input) m_options.input->forget_set(k_play_set);
	m_document.set_edits_locked(false);
	mount_user(m_options.editor_profile, false);
	restore_editor();
	m_stopping = false;
	egLOG_INFO("play", "Play stopped");
	set_state(play_state::stopped, reason);
}

bool edt::play_session::pause(play_reason reason)
{
	// During a resource wait the set stays held; the mode it gets afterwards changes (the editor layout pauses, #294).
	if (m_waiting && !m_suspended) {
		if (m_state == play_state::paused && m_waiting->pending_steps == 0) return false;
		m_waiting->mode = ecs::run_mode::pause;
		m_waiting->pending_steps = 0;
		set_state(play_state::paused, reason);
		return true;
	}
	const auto contexts = m_services.ctx_manager.lock();
	scn::context_set* play = contexts && active() && !reloading() ? contexts->find_set(k_play_set) : nullptr;
	if (!play || (m_state == play_state::paused && play->pending_steps() == 0)) return false;
	play->set_mode(ecs::run_mode::pause);
	set_state(play_state::paused, reason);
	return true;
}

bool edt::play_session::resume(play_reason reason)
{
	if (m_waiting && !m_suspended) {
		if (m_state == play_state::playing) return false;
		m_waiting->mode = ecs::run_mode::play;
		m_waiting->pending_steps = 0;
		set_state(play_state::playing, reason);
		return true;
	}
	const auto contexts = m_services.ctx_manager.lock();
	scn::context_set* play = contexts && active() && !reloading() ? contexts->find_set(k_play_set) : nullptr;
	if (!play || m_state == play_state::playing) return false;
	play->set_mode(ecs::run_mode::play);
	set_state(play_state::playing, reason);
	return true;
}

bool edt::play_session::step(std::uint32_t frames, play_reason reason)
{
	const auto contexts = m_services.ctx_manager.lock();
	scn::context_set* play = contexts && active() && !reloading() ? contexts->find_set(k_play_set) : nullptr;
	if (!play) return false;
	play->step(std::max<std::uint32_t>(frames, 1u));
	set_state(play_state::paused, reason);
	return true;
}

std::uint32_t edt::play_session::pending_steps() const
{
	const auto contexts = m_services.ctx_manager.lock();
	const scn::context_set* play = contexts && active() ? contexts->find_set(k_play_set) : nullptr;
	return play ? play->pending_steps() : 0u;
}

std::uint64_t edt::play_session::frames_run() const
{
	const auto contexts = m_services.ctx_manager.lock();
	const scn::context_set* play = contexts && active() ? contexts->find_set(k_play_set) : nullptr;
	return play ? play->frames_run() : 0u;
}

std::vector<std::string> edt::play_session::world_names() const
{
	std::vector<std::string> names;
	const auto contexts = m_services.ctx_manager.lock();
	scn::context_set* play = contexts && active() ? contexts->find_set(k_play_set) : nullptr;
	if (!play) return names;
	names.reserve(play->size());
	play->for_each([&names](scn::world_context& world) { names.emplace_back(world.get_name()); });
	return names;
}

void edt::play_session::set_time_scale(float scale)
{
	const auto contexts = m_services.ctx_manager.lock();
	if (scn::context_set* play = contexts && active() ? contexts->find_set(k_play_set) : nullptr) {
		play->set_time_scale(scale);
		m_time_scale = play->time_scale();
		return;
	}
	// The set validates it again at the next start.
	m_time_scale = std::isfinite(scale) && scale > 0.0f ? scale : 0.0f;
}

void edt::play_session::set_seed(std::uint64_t seed)
{
	m_seed = seed;
	const auto contexts = m_services.ctx_manager.lock();
	if (scn::context_set* play = contexts && active() ? contexts->find_set(k_play_set) : nullptr) {
		play->set_seed(seed);
	}
}

namespace {
	void merge_into(scn::snapshot_report& total, const scn::snapshot_report& part)
	{
		for (const auto& [type, count] : part.unsaved) total.unsaved[type] += count;
		for (const auto& [type, count] : part.unknown) total.unknown[type] += count;
		for (const auto& [type, count] : part.partial) total.partial[type] += count;
		for (const auto& [type, count] : part.removed_fields) total.removed_fields[type] += count;
		total.entities += part.entities;
	}

	// The pending map read as a committed one, so capture_blackboard and restore_blackboard serve both.
	ecs::blackboard_data pending_as_committed(const ecs::blackboard_data& board)
	{
		ecs::blackboard_data view;
		for (const auto& [key, entry] : board.pending()) view.restore(key, entry.type, entry.type_name, entry.value);
		return view;
	}
}

std::string edt::play_session::capture_for_reload()
{
	m_reload.reset();
	m_capture_refusal.clear();
	m_dropped_systems.clear();
	m_restore_failure.clear();
	m_revert_failure.clear();
	m_reverting = false;
	m_reload_started = now();
	const auto contexts = m_services.ctx_manager.lock();
	scn::context_set* play = contexts && active() ? contexts->find_set(k_play_set) : nullptr;
	if (!play) return {};
	scn::scene_layer* layer = play->layer();
	if (!layer) return m_capture_refusal = "the set 'play' has no layer";

	// Every world of the set, also one the game added while playing.
	std::string refusal;
	play->for_each([&](scn::world_context& world) {
		// Commands queued for after another world's next phase land now: they hold the module's code.
		world.apply_commands();
		const auto strays = scn::unregistered_storages(world.state(), layer->catalog());
		if (!strays.empty() && refusal.empty()) {
			refusal = std::format("the world '{}' holds {}, which no snapshot can save", world.get_name(), strays.front().type);
		}
	});
	if (!refusal.empty()) return m_capture_refusal = refusal;

	reload_snapshot snapshot;
	play->for_each([&](scn::world_context& world) {
		saved_world saved;
		saved.name = std::string{ world.get_name() };
		saved.systems = world.systems().value_or(std::vector<std::string>{});
		// A system with an instance keeps state in its fields; system_entry stays as old modules built it (ABI).
		const ecs::system_registry& systems = layer->systems().registry();
		if (const ecs::system_plan* plan = world.plan()) {
			for (const ecs::phase_plan& phase : plan->phases) {
				for (const ecs::system_node& node : phase.nodes) {
					const bool engines = systems.parent() && systems.parent()->find(node.name);
					if (node.instance.owner && !engines
						&& std::ranges::find(snapshot.systems_restarted, node.name) == snapshot.systems_restarted.end()) {
						egLOG_WARN("hot_reload", "the system '{}' keeps state in its fields: it starts again after the module reload", node.name);
						snapshot.systems_restarted.push_back(node.name);
					}
				}
			}
		}
		saved.input_layer = world.input_layer();
		saved.input_stack = world.input_stack();
		if (const auto* bus = world.state().ctx().find<ecs::world_bus>()) snapshot.bus_events += bus->queued();
		scn::snapshot_report report;
		saved.state = scn::capture_world(world, layer->catalog(), report);
		for (const auto& [type, count] : report.unsaved) {
			egLOG_WARN("hot_reload", "the module reload drops {} ({}) of the world '{}': it has no schema", type, count, saved.name);
		}
		merge_into(snapshot.report, report);
		snapshot.worlds.push_back(std::move(saved));
	});
	const ecs::blackboard_data& board = play->channel().blackboard();
	scn::snapshot_report board_report;
	snapshot.blackboard = scn::capture_blackboard(board, layer->blackboard_types(), board_report);
	snapshot.blackboard_pending = scn::capture_blackboard(pending_as_committed(board), layer->blackboard_types(), board_report);
	for (const auto& [type, count] : board_report.unsaved) {
		egLOG_WARN("hot_reload", "the module reload drops {} blackboard value(s) of {}: the type has no schema", count, type);
	}
	merge_into(snapshot.report, board_report);
	// During a wait the set is held in Pause: what it will run in is the wait's.
	snapshot.mode = m_waiting ? m_waiting->mode : play->mode();
	snapshot.frames_run = m_waiting ? m_waiting->frames_run : play->frames_run();
	snapshot.pending_steps = m_waiting ? m_waiting->pending_steps : play->pending_steps();
	if (m_options.input) snapshot.input_stack = m_options.input->stack(k_play_set).snapshot();
	m_reload = std::move(snapshot);
	return {};
}

void edt::play_session::suspend_for_swap()
{
	const auto contexts = m_services.ctx_manager.lock();
	if (scn::context_set* play = contexts ? contexts->find_set(k_play_set) : nullptr) {
		if (scn::world_context* twin = m_camera_world.empty() ? nullptr : play->find(m_camera_world)) {
			save_camera_state(twin->state(), m_play_camera);
		}
		contexts->destroy_set(k_play_set);
	}
	m_camera_world.clear();
	m_waiting.reset();
	++m_wait_generation;
	// The copies hold descs of the module's types: they go before its binary does.
	m_worlds_copy.clear();
	if (m_options.input) {
		// Only a swap that happens loses them: a reload the host refuses leaves the queues of the running Play alone.
		if (m_reload) {
			m_reload->injected_dropped = m_options.input->take_injected(k_play_set).size();
			m_reload->stack_commands_dropped = m_options.input->take_commands(k_play_set).size();
		}
		m_options.input->forget_set(k_play_set);
	}
	if (m_options.forget_runtime_entities) m_options.forget_runtime_entities();
	m_suspended = true;
}

bool edt::play_session::restore_after_swap(scn::snapshot_report& report, std::string& failure)
{
	const auto contexts = m_services.ctx_manager.lock();
	scn::scene_layer* layer = contexts ? contexts->game_layer() : nullptr;
	if (!layer || !m_reload) {
		failure = "there is no game layer to restore Play in";
		return false;
	}
	if (contexts->find_set(k_play_set)) {
		failure = "a set 'play' is there already";
		return false;
	}
	m_dropped_systems.clear();
	reload_snapshot& saved = *m_reload;
	m_worlds_copy = m_document.world_descs;

	scn::context_set& play = contexts->create_set(k_play_set, layer, scn::set_options{ .hot_reload = false });
	play.set_mode(ecs::run_mode::pause);
	play.set_seed(m_seed);
	play.set_time_scale(m_time_scale);
	desc::read_context read;
	read.system = &layer->descs();
	{
		// The entities existed already: the module's observers must not count them again (HOT_RELOAD_PLAY.md 6, step 4).
		const auto muted = layer->subscriptions().mute_observers();
		const scn::context_set::current_scope as_current(play);
		for (const saved_world& world_saved : saved.worlds) {
			// A system the new build no longer has is left out by name (HOT_RELOAD_PLAY.md 7); new ones are not added.
			std::vector<std::string> systems;
			systems.reserve(world_saved.systems.size());
			for (const std::string& name : world_saved.systems) {
				if (layer->systems().registry().find(name)) {
					systems.push_back(name);
					continue;
				}
				egLOG_WARN("hot_reload", "the new build has no system '{}': the world '{}' runs without it", name, world_saved.name);
				m_dropped_systems.push_back(std::format("{}: {}", world_saved.name, name));
			}
			scn::world_context& world = contexts->create_context(k_play_set, world_saved.name, scn::context_kind::play, systems);
			world.set_input_layer(world_saved.input_layer);
			world.set_input_stack(world_saved.input_stack);
			scn::snapshot_report world_report;
			const bool restored = scn::restore_world(world, world_saved.state, layer->catalog(), read, world_report);
			for (const auto& [type, count] : world_report.unknown) {
				egLOG_WARN("hot_reload", "the new module no longer knows {} ({}) of the world '{}': dropped", type, count, world_saved.name);
			}
			for (const auto& [type, count] : world_report.partial) {
				egLOG_WARN("hot_reload", "{} ({}) of the world '{}' changed: a field kept its default", type, count, world_saved.name);
			}
			for (const auto& [type, count] : world_report.removed_fields) {
				egLOG_WARN("hot_reload", "{} ({}) of the world '{}' lost a field the new build no longer has", type, count, world_saved.name);
			}
			merge_into(report, world_report);
			if (!restored) {
				failure = std::format("the world '{}' could not be restored", world_saved.name);
				return false;
			}
			// Not built from its desc: spawn_prefab gets the new layer's assembler here (#551).
			if (!scn::bind_prefab_spawning(world.state(), layer->assembler())) {
				egLOG_WARN("hot_reload", "the world '{}' has no root after the restore: spawn_prefab is off in it", world_saved.name);
			}
		}
		ecs::blackboard_data& board = play.channel().restorable_blackboard();
		scn::restore_blackboard(board, saved.blackboard, layer->blackboard_types(), read, report);
		ecs::blackboard_data pending;
		scn::restore_blackboard(pending, saved.blackboard_pending, layer->blackboard_types(), read, report);
		for (const auto& [key, entry] : pending.committed()) {
			board.write(key, entry.type, entry.type_name, entry.value, ecs::blackboard_data::external_writer);
		}
	}
	if (m_options.input) {
		std::vector<inp::world_ref> worlds;
		play.for_each([&](scn::world_context& world) { worlds.push_back({ std::string{ world.get_name() }, world.desc_index() }); });
		saved.input_stack_restored = m_options.input->stack(k_play_set).restore(saved.input_stack, worlds);
		if (saved.input_stack_restored.skipped > 0 || saved.input_stack_restored.off_worlds_skipped > 0) {
			egLOG_WARN("hot_reload", "{} input layer(s) and {} switched-off world(s) belong to a world the new run does not have: left out",
				saved.input_stack_restored.skipped, saved.input_stack_restored.off_worlds_skipped);
		}
	}
	// Neither run nor shown until what the restored components refer to has loaded (HOT_RELOAD_PLAY.md 6, step 6).
	play.restore_run_state(saved.frames_run, 0);
	play.set_active(false);
	m_waiting = resource_wait{ saved.mode, saved.frames_run, saved.pending_steps, now() };

	// The level was built again in the new layer: hidden, as Start left it.
	scn::context_set& level = contexts->level_set();
	level.set_active(false);
	level.for_each([](scn::world_context& world) { world.set_extract(false); });
	follow_active_world();
	++m_reload_number;

	egLOG_INFO("play", "Play kept its world across the module reload: {} world(s), {} entities, {} bus event(s) and the mail "
		"in flight dropped (count unknown, #668)", saved.worlds.size(), report.entities, saved.bus_events);
	return true;
}

std::chrono::steady_clock::time_point edt::play_session::now() const
{
	return m_options.now ? m_options.now() : std::chrono::steady_clock::now();
}

std::size_t edt::play_session::reloading_resources() const
{
	if (!m_waiting) return 0;
	if (m_options.pending_resources) return m_options.pending_resources();
	const auto contexts = m_services.ctx_manager.lock();
	scn::scene_layer* layer = contexts ? contexts->game_layer() : nullptr;
	return layer ? layer->resources().pending_loads() : 0;
}

void edt::play_session::wait_for_resources()
{
	if (!m_waiting) return;
	if (!m_options.each_frame || reloading_resources() == 0) {
		end_wait(false);
		return;
	}
	const std::uint64_t generation = ++m_wait_generation;
	// One id per wait: register_implicit ignores a taken id, and the poll of an older wait may still be listed.
	m_options.each_frame(std::format("edt/play_reload_resources/{}", generation),
		[this, alive = std::weak_ptr<int>(m_alive), generation] {
			if (alive.expired() || generation != m_wait_generation || !m_waiting) return false;
			const bool timed_out = now() - m_waiting->since >= k_resource_wait_limit;
			if (!timed_out && reloading_resources() > 0) return true;
			end_wait(timed_out);
			return false;
		});
}

void edt::play_session::end_wait(bool timed_out)
{
	if (!m_waiting) return;
	if (timed_out) {
		egLOG_WARN("play", "Play runs on after the module reload with {} resource(s) still loading ({} s)",
			reloading_resources(), k_resource_wait_limit.count());
	}
	const resource_wait wait = *std::exchange(m_waiting, std::nullopt);
	++m_wait_generation;
	const auto contexts = m_services.ctx_manager.lock();
	scn::context_set* play = contexts ? contexts->find_set(k_play_set) : nullptr;
	if (!play) return;
	play->set_active(true);
	if (wait.mode == ecs::run_mode::step && wait.pending_steps > 0) {
		play->step(wait.pending_steps);
	} else {
		play->set_mode(wait.mode == ecs::run_mode::play ? ecs::run_mode::play : ecs::run_mode::pause);
	}
	play->restore_run_state(wait.frames_run, wait.pending_steps);
}

void edt::play_session::before_module_swap()
{
	if (!active()) return;
	if (m_reload) {
		suspend_for_swap();
		return;
	}
	m_resume_after_swap = true;
	m_paused_before_swap = m_state == play_state::paused;
	stop(play_reason::stopped_for_reload);
}

void edt::play_session::after_module_swap(const core::reload_report& report)
{
	if (std::exchange(m_suspended, false)) {
		const bool built = report.status == core::reload_status::reloaded || report.status == core::reload_status::rolled_back;
		const bool second_pass = std::exchange(m_reverting, false);
		scn::snapshot_report restored;
		std::string failure;
		bool kept = false;
		if (built) {
			// An exception of the new module's code (a deserializer, a system's constructor) is a failed restore (§9).
			try {
				kept = restore_after_swap(restored, failure);
			} catch (const std::exception& error) {
				failure = std::format("an exception: {}", error.what());
			} catch (...) {
				failure = "an exception of an unknown type";
			}
			if (!kept) egLOG_ERROR("hot_reload", "Play could not be restored after the module reload: {}", failure);
		}
		// Once per reload, only for a new build that loaded: its revert brings the previous build back, and Play in it.
		if (!kept && !second_pass && report.status == core::reload_status::reloaded && m_options.revert_module) {
			await_revert(report.module, std::move(failure));
			return;
		}
		if (!failure.empty() && m_restore_failure.empty()) m_restore_failure = failure;
		if (second_pass && !built) m_revert_failure = report.message;
		report_reload(report, restored, kept ? (second_pass ? "reverted" : "kept") : "stopped");
		if (!kept) {
			egLOG_WARN("hot_reload", "Play stopped: its world was not brought back after the module reload ({})",
				built ? m_restore_failure : report.message);
			stop(play_reason::stopped_for_reload);
		} else {
			wait_for_resources();
		}
		m_reload.reset();
		return;
	}
	m_reload.reset();
	if (!std::exchange(m_resume_after_swap, false)) return;
	const bool paused = std::exchange(m_paused_before_swap, false);
	const bool started = start(play_reason::module_reloaded);
	if (started) {
		if (paused) pause(play_reason::module_reloaded);
		egLOG_INFO("play", "Play started again after the game module was reloaded");
	} else {
		egLOG_WARN("play", "Play did not start again after the game module was reloaded");
	}
	report_reload(report, {}, started ? "restarted" : "stopped");
	m_capture_refusal.clear();
}

void edt::play_session::await_revert(const std::string& module, std::string failure)
{
	if (const auto contexts = m_services.ctx_manager.lock(); contexts && contexts->find_set(k_play_set)) {
		contexts->destroy_set(k_play_set);
	}
	m_camera_world.clear();
	m_waiting.reset();
	++m_wait_generation;
	m_worlds_copy.clear();
	if (m_options.input) m_options.input->forget_set(k_play_set);
	if (m_options.forget_runtime_entities) m_options.forget_runtime_entities();
	m_restore_failure = std::move(failure);
	m_reverting = true;
	m_suspended = true;
	egLOG_WARN("hot_reload", "the new build could not restore Play ({}): the previous build is loaded again", m_restore_failure);
	m_options.revert_module(module);
}

void edt::play_session::run_revert(const std::function<core::reload_report()>& revert, const std::function<void()>& forget)
{
	if (!m_reverting) {
		if (forget) forget();
		egLOG_WARN("hot_reload", "the revert of the game module is cancelled: Play was stopped, the new build stays loaded");
		if (m_options.notify) {
			m_options.notify("The revert of the game module was cancelled by Stop: the new build stays loaded.");
		}
		return;
	}
	end_revert(revert ? revert() : core::reload_report{ core::reload_status::refused, "nothing can take the build back" });
}

void edt::play_session::end_revert(const core::reload_report& report)
{
	// Stop dropped the revert (m_reverting), or a swap of the revert already brought Play back (not suspended).
	if (!std::exchange(m_reverting, false) || !m_suspended) return;
	// The revert never swapped: no after_module_swap comes, nothing runs Play.
	m_suspended = false;
	m_revert_failure = report.message;
	egLOG_WARN("hot_reload", "Play stopped: the previous build was not loaded again ({})", report.message);
	report_reload(report, {}, "stopped");
	stop(play_reason::stopped_for_reload);
	m_reload.reset();
}

namespace {
	boost::json::object counts_json(const std::map<std::string, std::size_t>& counts)
	{
		boost::json::object out;
		for (const auto& [type, count] : counts) out[type] = count;
		return out;
	}

	// The entities of a capture_world snapshot and the components they hold.
	std::pair<std::size_t, std::size_t> count_entities(const boost::json::object& state)
	{
		std::size_t entities = 0;
		std::size_t components = 0;
		const auto* list = state.if_contains("entities");
		if (!list || !list->is_array()) return { 0, 0 };
		for (const boost::json::value& entity : list->as_array()) {
			++entities;
			const auto* parts = entity.is_object() ? entity.as_object().if_contains("components") : nullptr;
			if (parts && parts->is_object()) components += parts->as_object().size();
		}
		return { entities, components };
	}

	boost::json::array strings_json(const std::vector<std::string>& values)
	{
		boost::json::array out;
		for (const std::string& value : values) out.emplace_back(value);
		return out;
	}

	std::string join(const std::vector<std::string>& values)
	{
		std::string out;
		for (const std::string& value : values) out += std::format("{}{}", out.empty() ? "" : ", ", value);
		return out;
	}

	std::string counts_text(const std::map<std::string, std::size_t>& counts)
	{
		std::string out;
		for (const auto& [type, count] : counts) out += std::format("{}{} ({})", out.empty() ? "" : ", ", type, count);
		return out;
	}
}

void edt::play_session::report_reload(const core::reload_report& outcome, const scn::snapshot_report& restored, std::string_view play)
{
	scn::snapshot_report dropped = m_reload ? m_reload->report : scn::snapshot_report{};
	const std::size_t captured_entities = dropped.entities;
	merge_into(dropped, restored);
	dropped.entities = captured_entities;
	const std::vector<std::string> restarted = m_reload ? m_reload->systems_restarted : std::vector<std::string>{};
	const bool lost_something = !dropped.unsaved.empty() || !dropped.unknown.empty() || !dropped.partial.empty()
		|| !dropped.removed_fields.empty() || !m_dropped_systems.empty() || !restarted.empty();

	if (m_options.notify) {
		if (play == "kept" && !lost_something) {
			m_options.notify({});
		} else if (play == "stopped") {
			std::string message = m_restore_failure.empty()
				? std::string{ "Play stopped: its state was not brought back after the game module was reloaded." }
				: std::format("Play stopped: its state was not brought back after the game module was reloaded ({}).", m_restore_failure);
			if (!m_revert_failure.empty()) message += std::format(" The previous build was not loaded again: {}.", m_revert_failure);
			m_options.notify(message);
		} else if (play == "reverted") {
			m_options.notify(std::format("The new build of the game module could not restore Play ({}): the previous build runs "
				"again and Play goes on. Build the module again to retry.", m_restore_failure));
		} else if (play == "restarted") {
			m_options.notify(m_capture_refusal.empty()
				? std::string{ "Play started again after the game module was reloaded: its state was not kept." }
				: std::format("Play started again after the game module was reloaded: its state was not kept ({}).", m_capture_refusal));
		} else if (lost_something) {
			std::string what;
			if (!dropped.unsaved.empty()) what += std::format(" Dropped, no schema: {}.", counts_text(dropped.unsaved));
			if (!dropped.unknown.empty()) what += std::format(" Dropped, unknown to the new build: {}.", counts_text(dropped.unknown));
			if (!dropped.partial.empty()) what += std::format(" A field kept its default: {}.", counts_text(dropped.partial));
			if (!dropped.removed_fields.empty()) what += std::format(" A field the new build no longer has: {}.", counts_text(dropped.removed_fields));
			if (!m_dropped_systems.empty()) what += std::format(" Systems the new build no longer has: {}.", join(m_dropped_systems));
			if (!restarted.empty()) what += std::format(" Systems whose fields start again: {}.", join(restarted));
			m_options.notify(std::format("Play kept its world across the module reload, not all of it.{}", what));
		}
	}
	const auto started = std::exchange(m_reload_started, std::nullopt);
	if (m_options.project_root.empty()) return;

	boost::json::object out;
	out["written_at"] = std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
	if (started) {
		out["duration_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(now() - *started).count();
	}
	out["module_reload"] = boost::json::object{ { "status", to_string(outcome.status) }, { "message", outcome.message } };
	out["play"] = play;
	if (!m_capture_refusal.empty()) out["not_kept_because"] = m_capture_refusal;
	if (!m_restore_failure.empty()) out["restore_failed_because"] = m_restore_failure;
	if (!m_revert_failure.empty()) out["revert_failed_because"] = m_revert_failure;
	out["entities"] = dropped.entities;
	out["dropped_no_schema"] = counts_json(dropped.unsaved);
	out["dropped_unknown_type"] = counts_json(dropped.unknown);
	out["field_kept_default"] = counts_json(dropped.partial);
	out["field_removed"] = counts_json(dropped.removed_fields);
	out["dropped_systems"] = strings_json(m_dropped_systems);
	out["systems_restarted"] = strings_json(restarted);
	if (m_reload) {
		out["bus_events_dropped"] = m_reload->bus_events;
		out["mail_dropped"] = "unknown (#668)";
		out["input_layers_restored"] = m_reload->input_stack_restored.restored;
		out["input_layers_skipped"] = m_reload->input_stack_restored.skipped;
		out["input_off_worlds_skipped"] = m_reload->input_stack_restored.off_worlds_skipped;
		out["input_injected_dropped"] = m_reload->injected_dropped;
		out["input_commands_dropped"] = m_reload->stack_commands_dropped;
		boost::json::array worlds;
		for (const saved_world& world : m_reload->worlds) {
			boost::json::array systems;
			for (const std::string& system : world.systems) systems.emplace_back(system);
			const auto [entities, components] = count_entities(world.state);
			boost::json::object entry{ { "name", world.name }, { "systems", std::move(systems) }, { "input_layer", world.input_layer },
				{ "entities", entities }, { "components", components } };
			if (m_options.report_full_snapshot) entry["state"] = world.state;
			worlds.emplace_back(std::move(entry));
		}
		out["worlds"] = std::move(worlds);
		out["blackboard_values"] = m_reload->blackboard.size() + m_reload->blackboard_pending.size();
		if (m_options.report_full_snapshot) {
			out["blackboard"] = m_reload->blackboard;
			out["blackboard_pending"] = m_reload->blackboard_pending;
		}
	}
	if (const auto error = prj::write_project_file(m_options.project_root, std::filesystem::path{ ".cache" } / "hot_reload" / "last_play_snapshot.json",
			prj::to_indented_json(out))) {
		egLOG_WARN("hot_reload", "the module reload report was not written: {}", *error);
	}
}

void edt::play_session::set_state(play_state state, play_reason reason)
{
	if (m_state == state) return;
	m_state = state;
	egLOG("play", "{} ({})", play_state_name(state), play_reason_name(reason));
	m_events.play_state_changed.emit(state, reason);
}

void edt::play_session::on_level_switched()
{
	if (!active()) return;
	// The world the camera was in is gone with the old level: nothing of it to save.
	m_camera_world.clear();
	// New registries may reuse the old ones' addresses and entity ids: the Game view's and the viewport's picks go.
	if (m_options.forget_runtime_entities) m_options.forget_runtime_entities();
	// What was injected into the old worlds or asked of their stack is not for the new ones.
	if (m_options.input) {
		m_options.input->forget_injected(k_play_set);
		m_options.input->forget_commands(k_play_set);
	}
	follow_active_world();
	egLOG_INFO("play", "Play switched to another level; the edited level is unchanged");
}

void edt::play_session::follow_active_world()
{
	const auto contexts = m_services.ctx_manager.lock();
	scn::context_set* play = contexts ? contexts->find_set(k_play_set) : nullptr;
	if (!play) return;
	if (!m_camera_world.empty()) {
		if (scn::world_context* old = play->find(m_camera_world)) {
			entt::registry& reg = old->state();
			save_camera_state(reg, m_play_camera);
			const auto view = reg.view<editor_camera_tag>();
			const std::vector<entt::entity> cameras(view.begin(), view.end());
			for (const entt::entity camera : cameras) reg.destroy(camera);
		}
		m_camera_world.clear();
	}
	const std::string* name = m_document.active_world_name();
	if (!name) return;
	if (scn::world_context* twin = play->find(*name)) {
		inject_camera(twin->state(), m_play_camera);
		m_camera_world = *name;
	}
}

void edt::play_session::mount_user(const std::vector<std::filesystem::path>& roots, bool fresh)
{
	if (!m_options.profile_resources || roots.empty() || std::ranges::any_of(roots, [](const auto& root) { return root.empty(); })) return;
	if (fresh) {
		// Every session starts with an empty profile: nothing saved by the previous one is seen.
		for (const std::filesystem::path& root : roots) {
			std::error_code ec;
			std::filesystem::remove_all(root, ec);
			if (ec) egLOG_WARN("play", "the play profile '{}' was not emptied: {}", root.string(), ec.message());
			std::filesystem::create_directories(root, ec);
			if (ec) egLOG_WARN("play", "the play profile '{}' was not created: {}", root.string(), ec.message());
		}
	}
	if (!m_options.profile_resources->remount(res::tag::user, roots)) {
		egLOG_WARN("play", "user:// stays where it was: it is not mounted");
	}
}

namespace {
	// The node `selected` names is still in the document (same world, same path, same place inside an instance).
	bool still_there(edt::scene_document& document, const edt::selection& selected)
	{
		const std::string* active = document.active_world_name();
		if (!active || *active != selected.world) return false;
		auto& children = document.world_descs[static_cast<std::size_t>(document.active_world_idx)].get_root().children;
		const auto address = edt::scene_editor::resolve_node_path(children, selected.path);
		return address.owner && address.target == selected.target;
	}
}

void edt::play_session::restore_editor()
{
	const auto contexts = m_services.ctx_manager.lock();
	if (!contexts) return;
	scn::context_set& level = contexts->level_set();
	level.set_active(m_before.level_active);
	for (const auto& [name, extract] : m_before.extract) {
		if (scn::world_context* world = level.find(name)) world->set_extract(extract);
	}
	// A change of the level desc that came during Play is applied now that the level runs again (the editor resyncs).
	contexts->apply_pending_level_reload();

	if (!m_before.active_world.empty()) {
		const auto& names = m_document.world_names;
		const auto it = std::ranges::find(names, m_before.active_world);
		const int index = static_cast<int>(it - names.begin());
		if (it != names.end() && index != m_document.active_world_idx) m_worlds.switch_to_world(index);
	}
	m_document.camera = m_before.camera;
	if (const std::string* name = m_document.active_world_name()) {
		if (scn::world_context* world = level.find(*name)) inject_camera(world->state(), m_document.camera);
	}

	const selection saved = std::exchange(m_before.selected, selection{});
	if (saved.empty()) {
		if (m_selection.has_selection()) m_selection.clear();
		return;
	}
	if (still_there(m_document, saved)) {
		if (!m_options.restore_selection) {
			m_selection.select(saved);
			return;
		}
		if (m_options.restore_selection(saved)) return;
	}
	egLOG("play", "the node selected before Play is gone: deselected");
	m_selection.clear();
}
