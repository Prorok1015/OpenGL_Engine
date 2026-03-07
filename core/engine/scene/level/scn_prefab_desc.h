#pragma once
#include "desc_base.hpp"
#include "desc_system.h"
#include "resources/res_resource_handle.hpp"
#include <unordered_map>
#include "scn_ecs_assembler.h"
#include <glm/glm.hpp>

namespace scn {
	class prefab_desc : public desc::desc_base
	{
	public:
		using base_type = desc::desc_base;

		struct prefab_comp_node {
			std::string type_name;
			res::res_handle<desc::desc_base> parent_desc;
			boost::json::object overrides;
		};

		struct prefab_node {
			std::string name;
			glm::vec3 position{ 0.0f };
			glm::vec3 rotation{ 0.0f };
			glm::vec3 scale{ 1.0f };

			std::unordered_map<std::string, prefab_comp_node> components;
			std::vector<prefab_node> children;

			glm::mat4 get_local_transform() const
			{
				glm::mat4 t = glm::translate(glm::mat4(1.0f), position);
				glm::mat4 r = glm::yawPitchRoll(glm::radians(rotation.y), glm::radians(rotation.x), glm::radians(rotation.z));
				glm::mat4 s = glm::scale(glm::mat4(1.0f), scale);
				return t * r * s;
			}
		};

		prefab_desc() = default;
		~prefab_desc() = default;
		prefab_desc(const prefab_desc&) = default;
		prefab_desc(prefab_desc&&) = default;
		prefab_desc& operator=(const prefab_desc&) = default;
		prefab_desc& operator=(prefab_desc&&) = default;

		virtual void copy_to(desc_base& other) const override
		{
			prefab_desc& other_desc = static_cast<prefab_desc&>(other);
			other_desc = *this;
		}
		virtual void deserialize(desc::desc_system& desc_system, const json::object&) override;
		virtual void serialize(json::object&) const override;

		prefab_node& get_root() { return root; }
		const prefab_node& get_root() const { return root; }
	private:
		prefab_node root;
	};

	void trigger_hot_reload(scn::ecs_assembler& assembler, entt::registry& reg, res::res_handle<desc::desc_base> changed_prefab);
	void assemble_prefab(scn::ecs_assembler& assembler, entt::registry& reg, entt::entity e, const prefab_desc& prefab, const std::string_view name, res::tag source_prefab);
}