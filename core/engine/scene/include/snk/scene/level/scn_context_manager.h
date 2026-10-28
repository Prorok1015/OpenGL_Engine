#pragma once
#include <snk/scene/level/scn_context_set.h>
#include <snk/scene/level/scn_world_context.h>
#include <snk/scene/level/scn_hot_reload_manager.h>
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/scene/scn_scene_layer.h>
#include <snk/resource/res_system.h>
#include <snk/resource/res_tag.h>
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

		// A layer over the manager's services; lives until destroy_layer() or the manager.
		scn::scene_layer& create_layer();
		// Destroys the sets of the layer, then the layer. The game layer's set "level" goes back to the base, empty.
		void destroy_layer(scn::scene_layer& layer);
		// The layer the set "level" and the previews belong to; null: the base services.
		scn::scene_layer* game_layer() const { return m_game_layer; }
		// Replaces the set "level" with an empty one on `layer` (null: the base) and makes it the game layer.
		void set_game_layer(scn::scene_layer* layer);

		// Replaces the contents of the set "level" with the worlds of the level desc and watches the
		// desc for changes. False (and the current level kept) when the desc cannot be loaded.
		bool load_level(const res::tag& level_tag);

		// Called after every successful hot reload of the level (world list or only the systems
		// changed), so owners of per-world data (the editor) can resync. Contexts that are not
		// `kind::level` survive the reload.
		void set_on_reload(std::function<void()> cb) { m_on_reload = std::move(cb); }

		// Returns the existing set when the name is taken. `layer` null: the base services.
		scn::context_set& create_set(std::string_view name, scn::scene_layer* layer = nullptr);
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

		// Mode and seed of every context of the set.
		void set_mode(std::string_view set, ecs::run_mode mode) { get_set(set).set_mode(mode); }
		void set_seed(std::string_view set, uint64_t seed) { get_set(set).set_seed(seed); }

		// Message types the sets can carry; shared by every set (also the ones created later). Modules
		// register theirs at init: `channels().add<my_message>()`.
		ecs::channel_registry& channels() { return *m_channel_types; }

		// Entry of code outside the phases (editor, game shell) into the channel of a set. The
		// message is delivered at the next exchange of that set, before the contexts' own messages.
		template <typename T>
		void post(std::string_view set, ecs::address to, T payload)
		{
			get_set(set).channel().post_external<T>(std::move(to), std::move(payload));
		}
		// Writer for values from outside the phases; visible after the next exchange of the set.
		ecs::blackboard_writer& blackboard(std::string_view set) { return get_set(set).channel().external_blackboard(); }

	private:
		std::unique_ptr<scn::context_set> make_set(std::string_view name, scn::scene_layer* layer);
		// The services that build the worlds of `set`: its layer's, or the base's.
		ecs::system_factory& systems_of(const scn::context_set& set) { return set.layer() ? set.layer()->systems() : m_system_factory; }
		scn::ecs_assembler& assembler_of(const scn::context_set& set) { return set.layer() ? set.layer()->assembler() : m_assembler; }
		res::resource_system& resources_of(const scn::context_set& set) { return set.layer() ? set.layer()->resources() : m_resource; }
		scn::hot_reload_manager* hot_reload_of(const scn::context_set& set) { return set.layer() ? set.layer()->hot_reload().get() : m_hot_reload_manager.get(); }

		std::shared_ptr<scn::hot_reload_manager> m_hot_reload_manager;
		std::shared_ptr<ecs::channel_registry> m_channel_types = std::make_shared<ecs::channel_registry>();
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
	};
}
