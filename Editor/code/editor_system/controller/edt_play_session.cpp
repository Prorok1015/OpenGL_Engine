#include "edt_play_session.h"
#include "edt_input_target.h"
#include "edt_scene_document.h"
#include "edt_scene_editor.h"
#include "edt_world_controller.h"
#include <snk/editor/edt_editor_services.h>
#include <snk/editor/edt_selection_service.h>
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>
#include <snk/ecs/ecs_random.hpp>
#include <snk/input/inp_input_router.h>
#include <snk/resource/res_system.h>
#include <snk/resource/res_tag.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_context_set.h>
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/scene/scn_scene_layer.h>
#include <entt/entt.hpp>
#include <algorithm>
#include <cmath>
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
	if (!m_services.ctx_manager.lock()) return std::string{ "there is no context manager" };
	if (!m_document.editor_tag.is_valid() || m_document.world_descs.empty()) return std::string{ "no level with a world to play" };
	if (!m_services.assembler() || !m_services.sfactory()) return std::string{ "the scene services are missing" };
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
	scn::context_set& play = contexts->create_set(k_play_set, contexts->game_layer(), scn::set_options{ .hot_reload = false });
	// Before the contexts: a context takes the mode, seed and time scale of its set when it is created.
	play.set_mode(ecs::run_mode::play);
	play.set_seed(m_seed);
	play.set_time_scale(m_time_scale);
	m_time_scale = play.time_scale();
	for (std::size_t i = 0; i < m_worlds_copy.size(); ++i) {
		const scn::world_desc& copy = m_worlds_copy[i];
		const std::string name = i < names.size() ? names[i] : std::string{ copy.get_name() };
		scn::world_context& world = contexts->create_context(k_play_set, name, scn::context_kind::play,
			i < systems.size() ? systems[i] : copy.get_systems());
		world.set_input_layer(copy.get_input_layer());
		const entt::entity root = world.state().create();
		m_services.assembler()->spawn_from_desc(world.state(), root, copy, name);
	}

	// The level stays as it is, frozen and not drawn; its cameras come back on Stop.
	level.set_active(false);
	level.for_each([](scn::world_context& world) { world.set_extract(false); });

	m_document.set_edits_locked(true);
	mount_user(m_options.play_profile, true);
	m_play_camera = m_document.camera;
	m_camera_world.clear();
	follow_active_world();
	egLOG_INFO("play", "Play started: {} world(s), seed {}", m_worlds_copy.size(), m_seed);
	set_state(play_state::playing, reason);
	return true;
}

void edt::play_session::stop(play_reason reason)
{
	if (!active()) return;
	if (const auto contexts = m_services.ctx_manager.lock()) {
		if (scn::context_set* play = contexts->find_set(k_play_set)) {
			// A task of the game module may hold a play registry: none may run once the set is gone. The layer is the
			// level's too, so its tasks stop as well; a module restarts what its edit-mode systems need.
			if (scn::scene_layer* layer = play->layer()) layer->stop_tasks();
			contexts->destroy_set(k_play_set);
		}
	}
	m_worlds_copy.clear();
	m_camera_world.clear();
	if (m_options.input) m_options.input->forget_set(k_play_set);
	m_document.set_edits_locked(false);
	mount_user(m_options.editor_profile, false);
	restore_editor();
	egLOG_INFO("play", "Play stopped");
	set_state(play_state::stopped, reason);
}

bool edt::play_session::pause(play_reason reason)
{
	const auto contexts = m_services.ctx_manager.lock();
	scn::context_set* play = contexts && active() ? contexts->find_set(k_play_set) : nullptr;
	if (!play || (m_state == play_state::paused && play->pending_steps() == 0)) return false;
	play->set_mode(ecs::run_mode::pause);
	set_state(play_state::paused, reason);
	return true;
}

bool edt::play_session::resume(play_reason reason)
{
	const auto contexts = m_services.ctx_manager.lock();
	scn::context_set* play = contexts && active() ? contexts->find_set(k_play_set) : nullptr;
	if (!play || m_state == play_state::playing) return false;
	play->set_mode(ecs::run_mode::play);
	set_state(play_state::playing, reason);
	return true;
}

bool edt::play_session::step(std::uint32_t frames, play_reason reason)
{
	const auto contexts = m_services.ctx_manager.lock();
	scn::context_set* play = contexts && active() ? contexts->find_set(k_play_set) : nullptr;
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

void edt::play_session::before_module_swap()
{
	if (!active()) return;
	m_resume_after_swap = true;
	stop(play_reason::stopped_for_reload);
}

void edt::play_session::after_module_swap()
{
	if (!std::exchange(m_resume_after_swap, false)) return;
	if (start(play_reason::module_reloaded)) {
		egLOG_INFO("play", "Play started again after the game module was reloaded");
	} else {
		egLOG_WARN("play", "Play did not start again after the game module was reloaded");
	}
}

void edt::play_session::set_state(play_state state, play_reason reason)
{
	if (m_state == state) return;
	m_state = state;
	egLOG("play", "{} ({})", play_state_name(state), play_reason_name(reason));
	m_events.play_state_changed.emit(state, reason);
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

void edt::play_session::mount_user(const std::filesystem::path& root, bool fresh)
{
	if (!m_options.profile_resources || root.empty()) return;
	if (fresh) {
		// Every session starts with an empty profile: nothing saved by the previous one is seen.
		std::error_code ec;
		std::filesystem::remove_all(root, ec);
		if (ec) egLOG_WARN("play", "the play profile '{}' was not emptied: {}", root.string(), ec.message());
		std::filesystem::create_directories(root, ec);
		if (ec) egLOG_WARN("play", "the play profile '{}' was not created: {}", root.string(), ec.message());
	}
	if (!m_options.profile_resources->remount(res::tag::user, { root })) {
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
