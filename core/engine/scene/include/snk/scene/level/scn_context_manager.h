#pragma once
#include <snk/scene/level/scn_context_set.h>
#include <snk/scene/level/scn_level.h>
#include <snk/scene/level/scn_world_context.h>
#include <snk/scene/level/scn_hot_reload_manager.h>
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/resource/res_system.h>
#include <snk/resource/res_tag.h>
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
	// system factory, assembler, hot reload) are injected once and used by all sets.
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

		// Facade over the set "level".
		scn::level& get_level() { return m_level; }

		// Replaces the contents of the set "level" with the worlds of the level desc and watches the
		// desc for changes. False (and the current level kept) when the desc cannot be loaded.
		bool load_level(const res::tag& level_tag);

		// Called after every successful hot reload of the level (world list or only the systems
		// changed), so owners of per-world data (the editor) can resync. Contexts that are not
		// `kind::level` survive the reload.
		void set_on_reload(std::function<void()> cb) { m_on_reload = std::move(cb); }

		// Returns the existing set when the name is taken.
		scn::context_set& create_set(std::string_view name);
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

		// Preview of an asset (prefab, scene, ...): the set "preview:<tag>" with one `kind::preview`
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

	private:
		scn::context_set& add_level_set();

		std::shared_ptr<scn::hot_reload_manager> m_hot_reload_manager;
		scn::ecs_assembler& m_assembler;
		ecs::system_factory& m_system_factory;
		res::resource_system& m_resource;
		// Stable addresses; creation order is the update order.
		std::vector<std::unique_ptr<scn::context_set>> m_sets;
		scn::level m_level;
		std::function<void()> m_on_reload;
	};
}
