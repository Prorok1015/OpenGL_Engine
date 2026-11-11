#pragma once
#include <snk/scene/level/scn_world.h>
#include <snk/scene/level/scn_level_desc.h>
#include <snk/scene/level/scn_world_desc.h>
#include <snk/scene/level/scn_hot_reload_manager.h>
#include <snk/scene/level/scn_set_channel.h>
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/scene/scn_event_subscriptions.h>
#include <snk/ecs/ecs_bus.hpp>
#include <snk/ecs/ecs_fixed_stepper.hpp>
#include <snk/ecs/ecs_invoker.hpp>
#include <snk/ecs/ecs_random.hpp>
#include <snk/ecs/ecs_system_plan.hpp>
#include <snk/ecs/ecs_system_registry.hpp>
#include <snk/resource/res_tag.h>
#include <snk/common/engine_assert.h>
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/common/snk_api.h>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace scn
{
	class scene_layer;
	namespace detail { struct level_switch_slot; }

	// How the context_manager builds a set.
	struct set_options
	{
		// False: the contexts' registries are never attached to hot reload, so a resource change does not reach
		// them (a Play set runs on copies, ADR-07).
		bool hot_reload = true;

		bool operator==(const set_options&) const = default;
	};

	// A group of contexts stepped together: one mode, one seed, one phase order. Owned by the
	// context_manager (the level is the set "level").
	// A context keeps its address until it is destroyed.
	class SNK_API context_set
	{
	public:
		// `channel_types` and `event_types` are the registries of message and event types shared by the sets of a
		// manager (or of a layer); null gives the set a registry of its own. With `options.hot_reload` false
		// `hot_manager` is not kept.
		context_set(std::string name, std::shared_ptr<scn::hot_reload_manager> hot_manager,
					std::shared_ptr<ecs::channel_registry> channel_types = nullptr, set_options options = {},
					std::shared_ptr<ecs::bus_registry> event_types = nullptr);
		~context_set();
		context_set(const context_set&) = delete;
		context_set& operator=(const context_set&) = delete;
		context_set(context_set&&) = delete;
		context_set& operator=(context_set&&) = delete;

		const std::string& name() const { return m_name; }
		const res::tag& tag() const { return m_tag; }
		// The layer whose worlds these are (MODULE_LAYER.md); null: the base's. Set by the context_manager before
		// the first context: a context takes the layer's meta context when it is created.
		scene_layer* layer() const { return m_layer; }
		void set_layer(scene_layer* layer) { m_layer = layer; }

		// The tasks a game module started for this set (gs::module_context::run on the main thread while the set is
		// current(), and the tasks those start in turn for the layer). Each is a task of the layer as well; stopping the
		// set's tasks leaves those of the layer's other sets running. Their main-thread continuations never run once
		// the set is gone.
		// Precondition of stop_tasks (so of destroying the set): no task of the set waits for main-thread work
		// (a then_main, a post_main, a resource the main thread finishes): wait_idle does not drain the main queue,
		// such a task deadlocks it.
		const ds::lifetime& tasks() const { return m_tasks; }
		// Cancels the set's tasks and waits for them on the layer's task system. Main thread, not from inside one of
		// the set's tasks or their continuations. Idempotent; the set does it before its contexts go.
		void stop_tasks();
		// The set whose contexts are being built, updated or loaded now on this thread (the main thread); null outside
		// and on every other thread.
		static context_set* current();
		// Makes `set` current() for its lifetime: for code that builds a set's worlds outside the set (a spawn from a
		// desc), so the tasks its systems and spawners start belong to the set.
		class SNK_API current_scope
		{
		public:
			explicit current_scope(context_set& set);
			~current_scope();
			current_scope(const current_scope&) = delete;
			current_scope& operator=(const current_scope&) = delete;

		private:
			context_set* m_previous;
		};
		// An inactive set is skipped by context_manager::update.
		bool active() const { return m_active; }
		void set_active(bool active) { m_active = active; }
		const set_options& options() const { return m_options; }
		// set_options::hot_reload: the contexts' registries are attached to the hot reload manager, when there is one.
		bool hot_reload() const { return m_options.hot_reload; }

		// The data channel of this set (blackboard + mailbox). It is exchanged once at the start of
		// update(), before the contexts' begin_frame, and only while the set runs: in Edit and Pause it
		// is frozen together with the time (Step does one exchange). clear() (also a level load) empties it;
		// the blackboard survives reload_context, the reloaded context's inbox and outbox do not.
		scn::set_channel& channel() { return m_channel; }
		const scn::set_channel& channel() const { return m_channel; }

		// The event types of the set's worlds (WORLD_BUS.md 3). Every world has an ecs::world_bus in its ctx, bound
		// to them; a type registered later is bound at the next update(). Delivery: right after the builtin `update`
		// phase and its commands, in Play and Step. Pause queues, Edit drops.
		ecs::bus_registry& event_types() { return *m_event_types; }

		// An event from outside the phases into the bus of world `context`: first in its next delivery, with an empty
		// `from`. Main thread, never during update() (it would get ahead of the world's own events).
		template <class T>
		void post_event(std::string_view context, T payload)
		{
			ASSERT_MSG(!ds::task_system::on_worker(), "set '{}': post_event from a task, use then_main", m_name);
			ASSERT_MSG(!m_updating, "set '{}': post_event during the set's update", m_name);
			auto* world = find(context);
			ASSERT_MSG(world, "set '{}': post_event to a world that does not exist: '{}'", m_name, context);
			if (!world) return;
			bind_events(*world).template post<T>(std::move(payload));
		}

		// True inside update().
		bool updating() const { return m_updating; }

		// The bus subscriptions the set's worlds hear (their layer's, or the base's): connected to each world when its
		// bus is bound, before a delivery. Set by the context_manager; null: none. Outlives the set.
		void set_subscriptions(event_subscriptions* subscriptions) { m_subscriptions = subscriptions; }

		// One frame of ECS for every context: the phases in order, fixed_update stepped by the
		// accumulator, the command buffer applied after every phase. What runs depends on the mode.
		void update(std::chrono::duration<float> dt);

		// Edit: only run_in_edit systems. Play: all (default). Pause: only run_in_edit systems with
		// time frozen. Step: the next update() runs exactly one frame, then the mode becomes Pause.
		// The mode survives clear() and level loads.
		void set_mode(ecs::run_mode mode);
		ecs::run_mode mode() const { return m_mode; }

		// Step mode for `frames` (>= 1) frames: each of the next `frames` update() calls runs one full frame of
		// exactly one fixed step, then the mode becomes Pause. set_mode(step) is step(1); any set_mode cancels it.
		// 0 is refused with an error log and runs one frame.
		void step(uint32_t frames);
		// Frames of the current step still to run; 0 outside Step.
		uint32_t pending_steps() const { return m_pending_steps; }
		// Frames update() ran in Play or Step since the set was made (Edit and Pause frames are not counted).
		uint64_t frames_run() const { return m_frames_run; }
		// The counters of a set restored from a snapshot (the mode is set first): the frames run and, in Step, the
		// frames of the step still to run.
		void restore_run_state(uint64_t frames_run, uint32_t pending_steps);

		// At 60 fps a frame of scale s needs about s fixed steps, and fixed_stepper runs at most 8 a frame (its
		// default max_steps): beyond 8 simulated time would be dropped, so the scale is capped there.
		static constexpr float max_time_scale = 8.0f;
		// Multiplies the frame delta of every context in Edit and Play (a step stays one fixed step). Default 1;
		// 0 freezes the time without changing the mode. Refused with an error log: negative or NaN (-> 0), above
		// max_time_scale or +inf (-> max_time_scale). Survives clear() and level loads.
		void set_time_scale(float scale);
		float time_scale() const { return m_time_scale; }

		// Every context owns an `ecs::rng` seeded from this seed and its id. Reseeds the existing
		// contexts; contexts created later derive from it as well.
		void set_seed(uint64_t seed);
		uint64_t seed() const { return m_seed; }

		// A context created into a non-empty set joins at the same point of the step as the others.
		scn::world_context& create_context(std::string_view name, uint32_t id, context_kind kind = context_kind::runtime);
		void destroy_context(std::string_view name);

		std::size_t size() const { return m_contexts.size(); }
		bool has(std::string_view name) const { return m_context_ids.contains(std::string{ name }); }

		// Smallest non-negative id not currently in use. Prefer this over size() when picking an id
		// for a new context: size() collides when ids are non-contiguous (after a destroy).
		uint32_t acquire_free_id() const {
			uint32_t id = 0;
			while (m_contexts.contains(id)) ++id;
			return id;
		}

		// In id order.
		template<typename Fn>
		void for_each(Fn&& fn) {
			for (const auto& [id, context] : m_contexts) {
				fn(*context);
			}
		}

		template<typename Fn>
		void for_each(Fn&& fn) const {
			for (const auto& [id, context] : m_contexts) {
				fn(*context);
			}
		}

		scn::world_context& get(std::string_view name) {
			auto it = m_context_ids.find(std::string{ name });
			ASSERT_MSG(it != m_context_ids.end(), "level doesn't contain this type of world");
			return *m_contexts.at(it->second);
		}

		// Null when there is no such context: for callers holding a name that can outlive it (a
		// level reload rebuilds the contexts from the desc and drops runtime-created ones).
		scn::world_context* find(std::string_view name) const {
			auto it = m_context_ids.find(std::string{ name });
			return it == m_context_ids.end() ? nullptr : m_contexts.at(it->second).get();
		}

		scn::world_context& get(uint32_t id) const {
			auto it = m_contexts.find(id);
			ASSERT_MSG(it != m_contexts.end(), "level doesn't contain world with this ID");
			return *it->second;
		}

		// Names of the worlds of the last loaded level desc, in desc order. Contexts created later
		// are not part of it.
		const std::vector<std::string>& desc_world_names() const { return m_desc_world_names; }

		// One context of `kind` per world of the desc.
		void load_from_desc(const level_desc& desc, ecs::system_factory& desc_system, scn::ecs_assembler& assambler,
							context_kind kind = context_kind::level);

		// Recreates the context under the same id and kind.
		void reload_context(
			std::string_view name,
			const world_desc& desc,
			ecs::system_factory& sf,
			scn::ecs_assembler& assambler);

		// Remembers the context's system list and builds its execution plan (once, not per frame).
		void add_systems(
			scn::world_context& context,
			const std::vector<std::string>& systems,
			ecs::system_factory& sf);

		// The system list of the contexts changed (hot reload): rebuilds every plan.
		// Reload contract (systems only): per-context system instances (registered in place, with
		// ARGS...) are re-created, so they pick up new parameters and lose their runtime state;
		// shared instances (registered as a shared_ptr) are kept as they are. Mode, seed, the
		// fixed-step accumulator and the random streams are not touched; only a full
		// load_from_desc / reload_context re-seeds the streams.
		void load_systems_from_desc(const level_desc& desc, ecs::system_factory& desc_system);

		// The phase table of `sf` changed after plans were built (a game phase was added): rebuilds
		// every plan so systems of the new phase run and every context agrees on the phase order.
		// Game phases should be registered before the first load; this is the fallback.
		// Per-context system instances are re-created, like in load_systems_from_desc.
		void rebuild_all_plans(ecs::system_factory& sf);

		// Plan of a context, null when it has no systems registered.
		const ecs::system_plan* plan_of(uint32_t id) const {
			auto it = m_contexts.find(id);
			return it == m_contexts.end() ? nullptr : it->second->plan();
		}

		// Destroys every context; mode and seed stay.
		void clear();

		// What scn::request_level asked of this set (#564); context_manager::update carries it out.
		detail::level_switch_slot& level_switch() { return *m_level_switch; }
		const detail::level_switch_slot& level_switch() const { return *m_level_switch; }

	private:
		void rebuild_context_plan(uint32_t id, ecs::system_factory& sf);
		// Takes the phase order from sf and re-resolves every context's phases against it.
		void refresh_plan_slots(const ecs::system_factory& sf);
		// All phases of one context back to back; fixed_update plays the context's own fixed steps.
		void update_context(uint32_t id, world_context& context, ecs::run_mode mode);
		void run_context_phase(uint32_t id, world_context& context, size_t order_index, ecs::run_mode mode);
		void destroy_by_id(uint32_t id);
		void begin_frame_channel(ecs::run_mode mode);
		// The world's bus, with every type of m_event_types bound.
		ecs::world_bus& bind_events(world_context& context);
		// Contexts keep their own mode; the set only mirrors its mode into them.
		void sync_context_mode();
		ecs::runtime_context create_runtime_context(size_t own_id);

		std::string m_name;
		res::tag m_tag;
		scene_layer* m_layer = nullptr;
		std::vector<std::string> m_desc_world_names;
		bool m_active = true;
		set_options m_options;
		// Null when the set takes no part in hot reload.
		std::shared_ptr<scn::hot_reload_manager> m_hot_reload_manager;
		// Before the contexts: their ctx holds pointers into it, so it must outlive them.
		scn::set_channel m_channel;
		std::vector<ecs::channel_participant> m_participants;
		// Before the contexts: their buses' slots are made by these types.
		std::shared_ptr<ecs::bus_registry> m_event_types;
		bool m_updating = false;

		ecs::run_mode m_mode = ecs::run_mode::play;
		uint32_t m_pending_steps = 0;
		uint64_t m_frames_run = 0;
		float m_time_scale = 1.0f;
		uint64_t m_seed = ecs::rng::default_seed;

		// Phase order the set runs (copy of the table at the last plan build). Each context
		// resolves its plan against it when plans are built, so a frame does no searching.
		std::vector<ecs::phase_id> m_phase_order = ecs::phase_table{}.ordered();
		// Ordered by id, so contexts always run in id order.
		std::map<uint32_t, std::unique_ptr<scn::world_context>> m_contexts;
		std::unordered_map<std::string, uint32_t> m_context_ids;
		// Last: the tasks may touch the contexts.
		ds::lifetime m_tasks{ ds::lifetime_option::cancel_on_destroy };
		// After every older field so the offsets of the inline accessors of a game module built earlier stay; a plain number, so it does not change the destruction order above.
		uint32_t m_next_desc_index = 0;
		event_subscriptions* m_subscriptions = nullptr;
		std::unique_ptr<detail::level_switch_slot> m_level_switch;
	};
}
