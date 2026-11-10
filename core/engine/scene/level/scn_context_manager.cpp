#include <snk/scene/level/scn_context_manager.h>
#include "scn_level_switch_slot.h"
#include <snk/scene/level/scn_level_desc.h>
#include <snk/common/eng_profiler.h>
#include <snk/common/logger/engine_log.h>
#include <snk/config/cfg_variables.h>
#include <algorithm>
#include <utility>

namespace {
	// Off in a shipped game (#591): the level desc is not watched, so the set "level" is never rebuilt from disk.
	CFG_VAR_DEF_BOOL(cfg_scene_hot_reload, "scene/hot_reload", true);

	scn::set_options level_set_options()
	{
		return scn::set_options{ .hot_reload = *cfg_scene_hot_reload };
	}

	// Names in desc order; false when a world desc failed to load.
	bool collect_world_names(const scn::level_desc& desc, std::vector<std::string>& out)
	{
		bool ok = true;
		for (const auto& world_desc : desc.get_worlds()) {
			const auto desc_ptr = world_desc.get_sync();
			if (!desc_ptr) {
				ok = false;
				continue;
			}
			out.emplace_back(desc_ptr->get_name());
		}
		return ok;
	}
}

scn::context_manager::context_manager(res::resource_system& resource,
									  scn::ecs_assembler& assembler,
									  ecs::system_factory& system_factory,
									  std::shared_ptr<scn::hot_reload_manager> hot_manager)
	: m_hot_reload_manager(hot_manager)
	, m_assembler(assembler)
	, m_system_factory(system_factory)
	, m_resource(resource)
	, m_level_set(m_sets.emplace_back(make_set(level_set_name, nullptr, level_set_options())).get())
{
	if (!*cfg_scene_hot_reload) {
		egLOG_INFO("scene", "hot reload is off (scene/hot_reload)");
	}
}

scn::context_manager::~context_manager()
{
	// The layers' tasks first (they may touch the worlds), then the sets: the worlds of a layer are built from its
	// services.
	for (const auto& layer : m_layers) {
		layer->stop_tasks();
	}
	m_sets.clear();
}

void scn::context_manager::stop_layer_loads()
{
	for (const auto& layer : m_layers) {
		layer->stop_tasks();
		layer->resources().stop_loading();
	}
}

std::unique_ptr<scn::context_set> scn::context_manager::make_set(std::string_view name, scn::scene_layer* layer,
																 scn::set_options options)
{
	auto hot_reload = layer ? layer->hot_reload() : m_hot_reload_manager;
	auto channel_types = layer ? layer->channel_types() : m_channel_types;
	auto event_types = layer ? layer->event_types() : m_event_types;
	auto set = std::make_unique<scn::context_set>(std::string{ name }, std::move(hot_reload), std::move(channel_types), options,
												  std::move(event_types));
	set->set_layer(layer);
	set->set_subscriptions(layer ? &layer->subscriptions() : &m_subscriptions);
	return set;
}

void scn::context_manager::update(std::chrono::duration<float> dt)
{
	ASSERT_MSG(!m_updating, "context_manager::update re-entered");
	m_updating = true;
	struct updating_reset
	{
		bool& flag;
		~updating_reset() { flag = false; }
	} reset{ m_updating };
	// Before any set runs: no system or subscriber of a set being replaced is on the stack.
	apply_level_switches();
	for (const auto& set : m_sets) {
		if (set->active()) set->update(dt);
	}
	// What this frame's systems asked for starts loading now, so it can be ready by the next frame.
	start_level_loads();

	if (m_hot_reload_manager) {
		m_hot_reload_manager->tick();
	}
	for (const auto& layer : m_layers) {
		layer->hot_reload()->tick();
	}
}

scn::scene_layer& scn::context_manager::create_layer(ds::task_system* tasks)
{
	return *m_layers.emplace_back(std::make_unique<scn::scene_layer>(m_resource, m_assembler, m_system_factory, *m_channel_types, *m_event_types, m_catalog, tasks));
}

void scn::context_manager::destroy_layer(scn::scene_layer& layer)
{
	// A system or a bus subscriber of the layer would be on the stack while its worlds and code go.
	ASSERT_MSG(!any_updating(), "destroy_layer during an update");
	// The layer's tasks may still touch its worlds.
	layer.stop_tasks();
	if (m_game_layer == &layer) {
		set_game_layer(nullptr);
	}
	std::erase_if(m_sets, [&layer](const auto& set) { return set->layer() == &layer; });
	// A world of the layer that outlived it would call into the module's binary: observer thunks and control blocks
	// (event_subscriptions::observe), systems, bus subscribers.
	ASSERT_MSG(std::ranges::none_of(m_sets, [&layer](const auto& set) { return set->layer() == &layer; }),
		"destroy_layer: a world of the layer outlives it");
	layer.catalog().clear();
	const auto it = std::ranges::find_if(m_layers, [&layer](const auto& owned) { return owned.get() == &layer; });
	ASSERT_MSG(it != m_layers.end(), "destroy_layer: the layer was not created by this manager");
	m_layers.erase(it);
}

void scn::context_manager::set_game_layer(scn::scene_layer* layer)
{
	if (m_on_game_layer_change) {
		m_on_game_layer_change(game_layer_change::before);
	}
	if (auto* hot_reload = hot_reload_of(*m_level_set); hot_reload && m_level_set->tag().is_valid()) {
		hot_reload->unwatch_resource(m_level_set->tag());
	}
	m_pending_level_reload = res::tag{};
	// The set "level" stays first in the update order and keeps how it runs; its worlds go with the old layer.
	const auto it = std::ranges::find_if(m_sets, [this](const auto& set) { return set.get() == m_level_set; });
	auto replacement = make_set(level_set_name, layer, level_set_options());
	if (m_level_set->mode() == ecs::run_mode::step) {
		replacement->step(m_level_set->pending_steps());
	} else {
		replacement->set_mode(m_level_set->mode());
	}
	replacement->set_time_scale(m_level_set->time_scale());
	replacement->set_seed(m_level_set->seed());
	replacement->set_active(m_level_set->active());
	replacement->level_switch().closed = !m_level_set_switchable;
	*it = std::move(replacement);
	m_level_set = it->get();
	m_game_layer = layer;
	if (m_on_game_layer_change) {
		m_on_game_layer_change(game_layer_change::after);
	}
}

scn::context_set& scn::context_manager::create_set(std::string_view name, scn::scene_layer* layer, scn::set_options options)
{
	if (auto* existing = find_set(name)) {
		egLOG_ERROR("scene", "create_set: set '{}' already exists, keeping the existing one", name);
		ENGINE_VERIFY(existing->options() == options, "create_set: set '{}' exists with other options (hot_reload {}), they are kept",
					  name, existing->options().hot_reload);
		return *existing;
	}
	return *m_sets.emplace_back(make_set(name, layer, options));
}

void scn::context_manager::destroy_set(std::string_view name)
{
	ASSERT_MSG(name != level_set_name, "destroy_set: the level set lives as long as the manager");
	const auto it = std::ranges::find_if(m_sets, [name](const auto& set) { return set->name() == name; });
	ASSERT_MSG(it != m_sets.end(), "destroy_set: no set with this name");
	m_sets.erase(it);
}

scn::context_set* scn::context_manager::find_set(std::string_view name) const
{
	const auto it = std::ranges::find_if(m_sets, [name](const auto& set) { return set->name() == name; });
	return it == m_sets.end() ? nullptr : it->get();
}

scn::context_set& scn::context_manager::get_set(std::string_view name) const
{
	auto* set = find_set(name);
	ASSERT_MSG(set, "context_manager: no set with this name");
	return *set;
}

scn::world_context& scn::context_manager::create_context(std::string_view set, std::string_view name, context_kind kind,
														 const std::vector<std::string>& systems)
{
	auto& target = get_set(set);
	auto& context = target.create_context(name, target.acquire_free_id(), kind);
	target.add_systems(context, systems, systems_of(target));
	return context;
}

void scn::context_manager::destroy_context(std::string_view set, std::string_view name)
{
	get_set(set).destroy_context(name);
}

scn::world_context* scn::context_manager::find(std::string_view set, std::string_view name) const
{
	const auto* target = find_set(set);
	return target ? target->find(name) : nullptr;
}

scn::world_context& scn::context_manager::create_preview(std::string_view tag, std::string_view name,
														  const std::vector<std::string>& systems)
{
	const std::string set_name = preview_set_name(tag);
	if (find_set(set_name)) {
		destroy_set(set_name);
	}
	auto& set = create_set(set_name, m_game_layer);
	// before the context exists: a context takes the mode of its set when it is created
	set.set_mode(ecs::run_mode::edit);
	return create_context(set_name, name, context_kind::preview, systems);
}

void scn::context_manager::destroy_preview(std::string_view tag)
{
	const std::string set_name = preview_set_name(tag);
	if (find_set(set_name)) {
		destroy_set(set_name);
	}
}

scn::world_context* scn::context_manager::find_preview(std::string_view tag) const
{
	const auto* set = find_set(preview_set_name(tag));
	if (!set) return nullptr;
	scn::world_context* found = nullptr;
	set->for_each([&found](scn::world_context& context) {
		if (!found && context.kind() == context_kind::preview) found = &context;
	});
	return found;
}

bool scn::context_manager::load_level(const res::tag& level_tag)
{
	auto& set = get_set(level_set_name);
	auto level_res = resources_of(set).require_sync<scn::level_desc>(level_tag);
	if (!level_res.is_ready()) {
		egLOG("Level Manager", "Failed to load level '{}'", level_tag.view());
		return false;
	}
	if (auto* hot_reload = hot_reload_of(set); hot_reload && set.tag().is_valid()) {
		hot_reload->unwatch_resource(set.tag());
	}
	set.clear();
	m_pending_level_reload = res::tag{};
	// A request of the level loaded before is not for this one.
	auto& slot = set.level_switch();
	slot.status = level_switch_status::idle;
	slot.level = res::tag{};
	slot.loading = res::res_handle<scn::level_desc>{};
	set.load_from_desc(*level_res, systems_of(set), assembler_of(set));
	watch_level(level_tag);
	return true;
}

void scn::context_manager::watch_level(const res::tag& level_tag)
{
	auto* hot_reload = hot_reload_of(get_set(level_set_name));
	if (!hot_reload) return;
	// CI reads it: the editor watches its level, the shipping host (scene/hot_reload off) must not.
	egLOG_INFO("scene", "level '{}' watched for hot reload", level_tag.view());
	hot_reload->watch_resource(level_tag, [this](const res::tag& changed_tag) {
		if (!get_set(level_set_name).active()) {
			// Play: the level is frozen and hidden; rebuilding it now would bring back its cameras.
			egLOG("Level Manager", "level reload '{}' deferred: the level set is inactive", changed_tag.view());
			m_pending_level_reload = changed_tag;
			return;
		}
		reload_level(changed_tag);
	});
}

void scn::context_manager::set_level_set_switchable(bool switchable)
{
	m_level_set_switchable = switchable;
	auto& slot = m_level_set->level_switch();
	slot.closed = !switchable;
	slot.closed_said = false;
	if (!switchable && slot.status == level_switch_status::loading) {
		slot.status = level_switch_status::idle;
		slot.loading = res::res_handle<scn::level_desc>{};
	}
}

bool scn::context_manager::level_switch_pending() const
{
	return std::ranges::any_of(m_sets, [](const auto& set) { return set->level_switch().status == level_switch_status::loading; });
}

void scn::context_manager::start_level_loads()
{
	for (const auto& set : m_sets) {
		auto& slot = set->level_switch();
		if (slot.status == level_switch_status::loading && !slot.loading.is_valid()) {
			slot.loading = resources_of(*set).require<scn::level_desc>(slot.level);
		}
	}
}

void scn::context_manager::apply_level_switches()
{
	const auto fail = [](scn::context_set& set, detail::level_switch_slot& slot, std::string_view why) {
		egLOG_WARN("scene", "request_level '{}' of set '{}': {}; the current level stays", slot.level.view(), set.name(), why);
		slot.status = level_switch_status::failed;
		slot.loading = res::res_handle<scn::level_desc>{};
	};
	// A callback may create or destroy sets: walk a copy.
	std::vector<scn::context_set*> sets;
	sets.reserve(m_sets.size());
	for (const auto& set : m_sets) sets.push_back(set.get());
	for (scn::context_set* set : sets) {
		if (std::ranges::none_of(m_sets, [set](const auto& owned) { return owned.get() == set; })) continue;
		auto& slot = set->level_switch();
		if (slot.status != level_switch_status::loading) continue;
		if (!slot.loading.is_valid()) slot.loading = resources_of(*set).require<scn::level_desc>(slot.level);
		if (slot.loading.has_error()) {
			fail(*set, slot, "the level desc did not load");
			continue;
		}
		if (!slot.loading.is_ready()) continue;
		bool worlds_ready = true;
		bool worlds_failed = false;
		for (const auto& world : slot.loading->get_worlds()) {
			worlds_failed = worlds_failed || world.has_error();
			worlds_ready = worlds_ready && world.is_ready();
		}
		if (worlds_failed) {
			fail(*set, slot, "a world desc of the level did not load");
			continue;
		}
		if (!worlds_ready) continue;
		const res::res_handle<scn::level_desc> desc = std::exchange(slot.loading, res::res_handle<scn::level_desc>{});
		slot.status = level_switch_status::idle;
		// A copy: a spawner or observer of the new worlds may ask for yet another level, which rewrites the slot.
		const res::tag level = slot.level;
		switch_level(*set, level, *desc);
		if (m_on_level_switched) m_on_level_switched(*set);
	}
}

void scn::context_manager::switch_level(scn::context_set& set, const res::tag& level_tag, const scn::level_desc& desc)
{
	// The kind of the desc's worlds (Play's stay kind::play); a world made while running says nothing.
	context_kind kind = context_kind::level;
	bool found = false;
	set.for_each([&](scn::world_context& context) {
		if (found || context.kind() == context_kind::runtime) return;
		kind = context.kind();
		found = true;
	});
	if (&set == m_level_set) {
		if (auto* hot_reload = hot_reload_of(set); hot_reload && set.tag().is_valid()) {
			hot_reload->unwatch_resource(set.tag());
		}
		m_pending_level_reload = res::tag{};
	}
	// The old level's tasks may hold its registries: they stop before the worlds go, as when a set is destroyed.
	const auto waited_from = std::chrono::steady_clock::now();
	set.stop_tasks();
	if (const auto waited = std::chrono::steady_clock::now() - waited_from; waited > std::chrono::milliseconds(100)) {
		egLOG_WARN("scene", "set '{}': the tasks of the level left took {} ms to stop; a task that waits for the main thread "
			"would hang the switch", set.name(), std::chrono::duration_cast<std::chrono::milliseconds>(waited).count());
	}
	set.clear();
	set.load_from_desc(desc, systems_of(set), assembler_of(set), kind);
	if (&set == m_level_set) watch_level(level_tag);
	egLOG_INFO("scene", "set '{}' switched to the level '{}'", set.name(), level_tag.view());
}

void scn::context_manager::apply_pending_level_reload()
{
	if (!m_pending_level_reload.is_valid()) return;
	const res::tag changed_tag = std::exchange(m_pending_level_reload, res::tag{});
	reload_level(changed_tag);
}

void scn::context_manager::reload_level(const res::tag& changed_tag)
{
	if (!resources_of(get_set(level_set_name)).exists(changed_tag)) {
		// Deleted or moved away: the level stays as it is.
		egLOG_DEBUG("Level Manager", "level reload '{}' skipped: the file is gone", changed_tag.view());
		return;
	}
	auto level_res = resources_of(get_set(level_set_name)).require_sync<scn::level_desc>(changed_tag);
	if (!level_res.is_ready()) {
		if (m_on_reload_result) m_on_reload_result(changed_tag, level_res);
		egLOG("Level Manager", "Failed to reload level '{0}'", changed_tag.view());
		return;
	}
	auto& set = get_set(level_set_name);
	std::vector<std::string> new_names;
	const bool names_ok = collect_world_names(*level_res, new_names);
	// Only now: the worlds are separate loads, and their diagnostics reach the level's handle once they settled.
	if (m_on_reload_result) m_on_reload_result(changed_tag, level_res);
	if (!names_ok) {
		// load_from_desc dereferences every world handle, so a broken world file must not reach it
		egLOG_WARN("Level Manager", "level reload '{}': world desc failed to load, keeping the current level", changed_tag.view());
		return;
	}
	// compare against the desc's worlds only (in desc order, which decides context ids): runtime-created contexts are not part of it
	const std::vector<std::string> old_names = set.desc_world_names();
	const bool is_full_reload = new_names != old_names;

	if (is_full_reload) {
		// drop only the level contexts; runtime-created ones keep their state
		for (const auto& name : old_names) {
			if (set.has(name)) set.destroy_context(name);
		}
		// a runtime context sharing a name with a new desc world is replaced by the desc one
		for (const auto& name : new_names) {
			if (set.has(name)) set.destroy_context(name);
		}
		set.load_from_desc(*level_res, systems_of(set), assembler_of(set));
	} else {
		set.load_systems_from_desc(*level_res, systems_of(set));
	}
	if (m_on_reload) m_on_reload();
}
