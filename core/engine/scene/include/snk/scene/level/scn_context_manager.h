#pragma once
#include <snk/scene/level/scn_context_set.h>
#include <snk/scene/level/scn_world_context.h>
#include <snk/scene/level/scn_hot_reload_manager.h>
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/scene/scn_scene_layer.h>
#include <snk/resource/res_system.h>
#include <snk/resource/res_tag.h>
#include <snk/ecs/ecs_bus.hpp>
#include <snk/ecs/ecs_channel.hpp>
#include <snk/ecs/ecs_system_registry.hpp>
#include <snk/common/engine_assert.h>
#include <snk/common/snk_api.h>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace scn
{
	// Owns every simulation context, grouped into named sets, and steps them each frame. Sets run
	// in the order they were created. The level is the set "level"; the shared services (resources,
	// system factory, assembler, hot reload) are injected once. A set belongs to the base (these services)
	// or to a layer the manager owns (MODULE_LAYER.md), whose services then build its worlds.
	class SNK_API context_manager
	{
	public:
		static constexpr std::string_view level_set_name = "level";
		static constexpr std::string_view preview_set_prefix = "preview:";

		// Name of the set that holds the preview of `tag`.
		static std::string preview_set_name(std::string_view tag) { return std::string{ preview_set_prefix }.append(tag); }

		context_manager(res::resource_system& resource,
						scn::ecs_assembler& assembler,
						ecs::system_factory& system_factory,
						std::shared_ptr<scn::hot_reload_manager> hot_manager = nullptr);
		~context_manager();
		context_manager(const context_manager&) = delete;
		context_manager(context_manager&&) = delete;
		context_manager& operator=(const context_manager&) = delete;
		context_manager& operator=(context_manager&&) = delete;

		// Steps every active set, then ticks hot reload once.
		void update(std::chrono::duration<float> dt);

		// The set "level". It lives as long as the manager, but set_game_layer() replaces it.
		scn::context_set& level_set() { return *m_level_set; }

		// A layer over the manager's services; lives until destroy_layer() or the manager. `tasks`: the task system the
		// layer's tasks (scene_layer::tasks) run on.
		scn::scene_layer& create_layer(ds::task_system* tasks = nullptr);
		// Stops the layer's tasks, destroys its sets, then the layer. The game layer's set "level" goes back to the
		// base, empty.
		void destroy_layer(scn::scene_layer& layer);
		// Every layer's tasks and resource loads stop (scene_layer::stop_tasks, resource_system::stop_loading); the
		// layers stay. Shutdown, before the scene types go (#653).
		void stop_layer_loads();
		// The layer the set "level" and the previews belong to; null: the base services.
		scn::scene_layer* game_layer() const { return m_game_layer; }
		// Replaces the set "level" with an empty one on `layer` (null: the base) and makes it the game layer. Mode
		// (with the frames left of a step), time scale, seed and the active flag of the set carry over.
		void set_game_layer(scn::scene_layer* layer);

		enum class game_layer_change { before, after };
		// Called by set_game_layer: `before` while the old game layer and its set "level" are still there (let go
		// of what refers to them), `after` once the new one is in place. One listener (the editor); null removes it.
		void set_on_game_layer_change(std::function<void(game_layer_change)> cb) { m_on_game_layer_change = std::move(cb); }

		// Replaces the contents of the set "level" with the worlds of the level desc and watches the
		// desc for changes. False (and the current level kept) when the desc cannot be loaded.
		bool load_level(const res::tag& level_tag);

		// A change of the level desc that arrives while the set "level" is inactive (Play) is not applied: the newest
		// one waits here until apply_pending_level_reload() (Stop, after the level is active again). A level load or a
		// change of the game layer drops it.
		bool has_pending_level_reload() const { return m_pending_level_reload.is_valid(); }
		void apply_pending_level_reload();

		// scn::request_level (#564): a set whose request is ready is rebuilt from the new level desc at the start of
		// update(), before any set runs; the kind of its worlds stays (Play's worlds stay kind::play). True while a
		// request of any set still loads (a module reload waits for it, HOT_RELOAD_PLAY.md 5).
		bool level_switch_pending() const;
		// False: request_level refuses the set "level" at once (the editor: that set is the edited level). Kept across
		// set_game_layer.
		void set_level_set_switchable(bool switchable);
		// After a set was rebuilt by a request (the editor rebinds Play's camera and input to the new worlds).
		void set_on_level_switched(std::function<void(scn::context_set&)> cb) { m_on_level_switched = std::move(cb); }

		// Called after every successful hot reload of the level (world list or only the systems
		// changed), so owners of per-world data (the editor) can resync. Contexts that are not
		// `kind::level` survive the reload.
		void set_on_reload(std::function<void()> cb) { m_on_reload = std::move(cb); }

		// Called for every hot reload that read the level file, also one that failed, with the handle it read (its
		// diagnostics: the editor's Problems, #596). Unset by default: a host without it reloads as before.
		using reload_result_callback = std::function<void(const res::tag& level, const res::res_handle<scn::level_desc>& handle)>;
		void set_on_reload_result(reload_result_callback cb) { m_on_reload_result = std::move(cb); }

		// Returns the existing set when the name is taken (logged; its options are kept, a mismatch is an error too).
		// `layer` null: the base services.
		scn::context_set& create_set(std::string_view name, scn::scene_layer* layer = nullptr, scn::set_options options = {});
		// The set "level" cannot be destroyed.
		void destroy_set(std::string_view name);
		scn::context_set* find_set(std::string_view name) const;
		scn::context_set& get_set(std::string_view name) const;

		// Creates a context in the set with the smallest free id (its stream) and builds its plan
		// from `systems`. The address stays valid until destroy_context.
		scn::world_context& create_context(std::string_view set, std::string_view name, context_kind kind,
										   const std::vector<std::string>& systems = {});
		void destroy_context(std::string_view set, std::string_view name);
		// Null when the set or the context does not exist.
		scn::world_context* find(std::string_view set, std::string_view name) const;

		// In id order inside a set; `for_each(fn)` walks all sets in creation order.
		template<typename Fn>
		void for_each(std::string_view set, Fn&& fn) { get_set(set).for_each(fn); }
		template<typename Fn>
		void for_each(Fn&& fn) {
			for (const auto& s : m_sets) s->for_each(fn);
		}

		// Walks the sets in creation order.
		template<typename Fn>
		void for_each_set(Fn&& fn) {
			for (const auto& s : m_sets) fn(*s);
		}

		// Preview of an asset (prefab, scene, ...): the set "preview:<tag>" of the game layer with one `kind::preview`
		// context. It has its own time, runs in Edit mode and takes part in hot reload, and being a
		// set of its own it is neither part of the level nor affected by a level reload. Creating a
		// preview for a tag that already has one replaces it.
		scn::world_context& create_preview(std::string_view tag, std::string_view name,
										   const std::vector<std::string>& systems = {});
		// Destroys the set; a no-op when the tag has no preview.
		void destroy_preview(std::string_view tag);
		// Null when the tag has no preview.
		scn::world_context* find_preview(std::string_view tag) const;

		// Mode, seed and time scale of every context of the set.
		void set_mode(std::string_view set, ecs::run_mode mode) { get_set(set).set_mode(mode); }
		void set_seed(std::string_view set, uint64_t seed) { get_set(set).set_seed(seed); }
		void step(std::string_view set, uint32_t frames) { get_set(set).step(frames); }
		void set_time_scale(std::string_view set, float scale) { get_set(set).set_time_scale(scale); }

		// Message types the sets can carry; shared by every set (also the ones created later). Modules
		// register theirs at init: `channels().add<my_message>()`.
		ecs::channel_registry& channels() { return *m_channel_types; }

		// Event types of the worlds' buses (WORLD_BUS.md 3): the engine's. A layer takes the base's types as they are
		// when it is created and adds the module's in its own `events()`.
		const ecs::bus_registry& events() const { return *m_event_types; }
		// Registers an engine event type (the input stack: `add_event<inp::action_event>()`, written by systems
		// through ecs::event_writer<inp::action_event>&). Only before the first layer exists (scn_scene_service_init
		// creates the game layer): a later type would never reach a layer's worlds, so it is refused with an error.
		// False also when the type is already registered. The base's sets bind it at their next update.
		template <typename T>
		bool add_event(std::string schema = {})
		{
			if (!ENGINE_VERIFY(m_layers.empty(), "context_manager: event type {} is registered after a layer exists; "
							   "register engine event types before the first create_layer", entt::type_name<T>::value())) {
				return false;
			}
			return m_event_types->add<T>(std::move(schema));
		}

		// The runtime types of the base (the engine's components, scene_init); every layer's catalog lies over it.
		scn::component_catalog& catalog() { return m_catalog; }
		const scn::component_catalog& catalog() const { return m_catalog; }

		// Entry of code outside the phases (editor, game shell) into the channel of a set. The
		// message is delivered at the next exchange of that set, before the contexts' own messages.
		template <typename T>
		void post(std::string_view set, ecs::address to, T payload)
		{
			get_set(set).channel().post_external<T>(std::move(to), std::move(payload));
		}
		// An event from outside the phases (editor, game shell, a test) into the bus of world `context` of `set`: first
		// in that world's next delivery, `from` empty. Main thread only and never while the set updates (a task posts
		// through then_main); asserts when the type is not an event type of the set.
		template <typename T>
		void emit(std::string_view set, std::string_view context, T payload)
		{
			// From a system of one set into another set the order would depend on the order the sets run in.
			ASSERT_MSG(!m_updating, "context_manager::emit into set '{}' during the manager's update", set);
			get_set(set).post_event<T>(context, std::move(payload));
		}

		// True inside update().
		bool updating() const { return m_updating; }
		// True while the manager or any of its sets updates (a system or a bus subscriber is on the stack).
		bool any_updating() const
		{
			if (m_updating) return true;
			for (const auto& s : m_sets) {
				if (s->updating()) return true;
			}
			return false;
		}

		// A subscription of the engine or the editor to the worlds of the base's sets (a module subscribes through
		// gs::module_context, which covers its layer's sets). The connection removes it; T must be an engine event type.
		template <typename T>
		[[nodiscard]] ecs::connection subscribe(scn::event_filter filter, std::function<void(ecs::bus_context&, const ecs::envelope<T>&)> fn)
		{
			ASSERT_MSG(m_event_types->contains<T>(), "context_manager::subscribe: {} is not an engine event type", entt::type_name<T>::value());
			return m_subscriptions.subscribe<T>(std::move(filter), std::move(fn));
		}

		// Writer for values from outside the phases; visible after the next exchange of the set.
		ecs::blackboard_writer& blackboard(std::string_view set) { return get_set(set).channel().external_blackboard(); }

	private:
		// The set "level" again from the changed level desc (only the level contexts), then m_on_reload.
		void reload_level(const res::tag& changed_tag);
		// Watches the level desc of the set "level" for hot reload, when the set takes part in it.
		void watch_level(const res::tag& level_tag);
		// Carries out every request whose level is ready, or marks it failed.
		void apply_level_switches();
		// Asks the resources for every requested level not asked for yet.
		void start_level_loads();
		void switch_level(scn::context_set& set, const res::tag& level_tag, const scn::level_desc& desc);
		std::unique_ptr<scn::context_set> make_set(std::string_view name, scn::scene_layer* layer, scn::set_options options = {});
		// The services that build the worlds of `set`: its layer's, or the base's.
		ecs::system_factory& systems_of(const scn::context_set& set) { return set.layer() ? set.layer()->systems() : m_system_factory; }
		scn::ecs_assembler& assembler_of(const scn::context_set& set) { return set.layer() ? set.layer()->assembler() : m_assembler; }
		res::resource_system& resources_of(const scn::context_set& set) { return set.layer() ? set.layer()->resources() : m_resource; }
		scn::hot_reload_manager* hot_reload_of(const scn::context_set& set)
		{
			if (!set.hot_reload()) return nullptr;
			return set.layer() ? set.layer()->hot_reload().get() : m_hot_reload_manager.get();
		}

		std::shared_ptr<scn::hot_reload_manager> m_hot_reload_manager;
		std::shared_ptr<ecs::channel_registry> m_channel_types = std::make_shared<ecs::channel_registry>();
		std::shared_ptr<ecs::bus_registry> m_event_types = std::make_shared<ecs::bus_registry>();
		bool m_updating = false;
		// Before the layers: theirs lie over it.
		scn::component_catalog m_catalog;
		scn::ecs_assembler& m_assembler;
		ecs::system_factory& m_system_factory;
		res::resource_system& m_resource;
		// Before the sets: the worlds of a layer go before the layer.
		std::vector<std::unique_ptr<scn::scene_layer>> m_layers;
		scn::scene_layer* m_game_layer = nullptr;
		// Stable addresses; creation order is the update order.
		std::vector<std::unique_ptr<scn::context_set>> m_sets;
		scn::context_set* m_level_set = nullptr;
		std::function<void()> m_on_reload;
		res::tag m_pending_level_reload;
		std::function<void(game_layer_change)> m_on_game_layer_change;
		// Kept before every later member, so the offsets a game module built earlier inlines stay (SNK_MODULE_API_VERSION
		// 3.0): new members go only after it. The base's sets point at it: the destructor clears them before any member goes.
		scn::event_subscriptions m_subscriptions;
		reload_result_callback m_on_reload_result;
		bool m_level_set_switchable = true;
		std::function<void(scn::context_set&)> m_on_level_switched;
	};
}
