#pragma once
#include "desc_base.hpp"
#include "desc_system.h"
#include "resources/res_resource_handle.hpp"
#include <unordered_map>
#include "scn_ecs_assembler.h"

namespace scn {
	class prefab_desc : public desc::desc_base
	{
	public:
		using base_type = desc::desc_base;

		struct prefab_node {
			std::string name;
			std::string type_name;
			res::res_handle<desc::desc_base> parent_desc;

			std::unordered_map<std::string, prefab_node> components;
			std::vector<prefab_node> children;

			json::object overrides;
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

	void assemble_prefab(scn::ecs_assembler& assembler, entt::registry& reg, entt::entity e, const prefab_desc& prefab, const std::string& name = "");
}