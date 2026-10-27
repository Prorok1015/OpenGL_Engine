#pragma once
#pragma once
#include <snk/desc/desc_system.h>
#include <snk/desc/desc_base.hpp>
#include <entt/fwd.hpp>
#include <snk/resource/resources/res_resource_handle.hpp>
#include <snk/resource/res_tag.h>
#include <boost/json.hpp>
#include <string>
#include <functional>
#include <unordered_map>
#include <snk/common/snk_api.h>

namespace res {
	class resource_system;
}

namespace scn {
	struct tracked_component_info {
		std::string type_name;
		res::tag source_desc;
		boost::json::object local_overrides;
	};

	struct component_tracker {
		std::unordered_map<std::string, tracked_component_info> tracked_components;
	};

	enum class spawner_category {
		component,
		structural 
	};

	class SNK_API ecs_assembler {
	public:
		using desc_spawner_fn = std::function<void(
			entt::registry&,
			entt::entity,
			const desc::desc_base&,
			const std::string_view
			)>;

		using desc_patcher_fn = std::function<void(
			entt::registry&,
			entt::entity,
			const desc::desc_base&
			)>;

		// A spawner that builds through the assembler running it, not the one it was registered in: a prefab or a world
		// spawned in a layer (MODULE_LAYER.md) assembles its components with the layer's assembler and desc system.
		using desc_structural_fn = std::function<void(
			const ecs_assembler&,
			entt::registry&,
			entt::entity,
			const desc::desc_base&,
			const std::string_view
			)>;

		struct spawner_pair {
			desc_spawner_fn spawn;
			desc_patcher_fn patch;
			spawner_category category = spawner_category::component;
			desc_structural_fn structural;
		};

		ecs_assembler(desc::desc_system& desc_sys, res::resource_system& res_sys)
			: m_desc_sys(desc_sys)
			, m_res_sys(res_sys)
		{}

		// A layer over `parent` (MODULE_LAYER.md): spawners fall through to it, a desc type it has a spawner for
		// cannot get one here. `desc_sys` is the layer's desc system.
		ecs_assembler(desc::desc_system& desc_sys, res::resource_system& res_sys, const ecs_assembler* parent)
			: m_desc_sys(desc_sys)
			, m_res_sys(res_sys)
			, m_parent(parent)
		{}

		~ecs_assembler() = default;
		ecs_assembler(const ecs_assembler&) = delete;
		ecs_assembler& operator=(const ecs_assembler&) = delete;
		ecs_assembler(ecs_assembler&&) = default;
		ecs_assembler& operator=(ecs_assembler&&) = delete;

		// False (logged, nothing registered) when the parent has a spawner for the type.
		bool register_desc_spawner(const std::string_view desc_type, desc_spawner_fn spawner, desc_patcher_fn patcher = nullptr, spawner_category category = spawner_category::component) {
			if (m_parent && m_parent->has_spawner(desc_type)) {
				egLOG_ERROR("assembler/error", "Spawner for '{}' not registered: the type belongs to the parent assembler", desc_type);
				return false;
			}
			m_spawners[std::string{ desc_type }] = { std::move(spawner), std::move(patcher), category };
			return true;
		}

		// A structural type (spawner_category::structural); false as register_desc_spawner.
		bool register_structural_spawner(const std::string_view desc_type, desc_structural_fn spawner) {
			if (m_parent && m_parent->has_spawner(desc_type)) {
				egLOG_ERROR("assembler/error", "Spawner for '{}' not registered: the type belongs to the parent assembler", desc_type);
				return false;
			}
			m_spawners[std::string{ desc_type }] = { nullptr, nullptr, spawner_category::structural, std::move(spawner) };
			return true;
		}

		void unregister_desc_spawner(const std::string_view desc_type) {
			m_spawners.erase(std::string{ desc_type });
		}

		bool has_spawner(const std::string_view desc_type) const {
			return find_spawner(desc_type) != nullptr;
		}

		// The spawner for `desc_type` here or in the parent; null when there is none.
		const spawner_pair* find_spawner(const std::string_view desc_type) const {
			auto it = m_spawners.find(std::string{ desc_type });
			if (it != m_spawners.end()) {
				return &it->second;
			}
			return m_parent ? m_parent->find_spawner(desc_type) : nullptr;
		}

		desc::desc_system& get_desc_system() const { return m_desc_sys; }
		res::resource_system& get_resource_system() const { return m_res_sys; }

		void spawn_from_desc(entt::registry& reg, entt::entity e, const desc::desc_base& desc, const std::string_view name = "") const
		{
			const spawner_pair* found = find_spawner(desc.get_type());
			if (found && (found->spawn || found->structural)) {
				run_spawner(*found, reg, e, desc, name);
			} else {
				egLOG_WARN("assembler/warning", "No spawner registered for desc type: {0}", desc.get_type());
			}
		}

		void assemble_and_apply(entt::registry& reg,
								entt::entity e,
								std::string actual_type,
								const res::res_handle<desc::desc_base>& parent_desc,
								const boost::json::object& overrides,
								const std::string_view name,
								bool is_hot_reload = false) const;

		void trigger_hot_reload_for_component(entt::registry& reg, const res::res_handle<desc::desc_base>& changed_desc_tag) const;

	private:
		void run_spawner(const spawner_pair& spawner, entt::registry& reg, entt::entity e, const desc::desc_base& desc, const std::string_view name) const
		{
			if (spawner.structural) {
				spawner.structural(*this, reg, e, desc, name);
			} else if (spawner.spawn) {
				spawner.spawn(reg, e, desc, name);
			}
		}

		desc::desc_system& m_desc_sys;
		res::resource_system& m_res_sys;
		const ecs_assembler* m_parent = nullptr;
		std::unordered_map<std::string, spawner_pair> m_spawners;
	};
}