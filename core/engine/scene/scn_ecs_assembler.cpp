#include "scn_ecs_assembler.h"

void scn::ecs_assembler::assemble_and_apply(entt::registry& reg, 
											entt::entity e, 
											std::string actual_type, 
											const res::res_handle<desc::desc_base>& parent_desc,
											const boost::json::object& overrides,
											const std::string& name) const 
{
	if (actual_type.empty() && parent_desc.is_ready()) {
		actual_type = parent_desc.get()->get_type();
	}

	if (actual_type.empty()) return;

	auto it = m_spawners.find(actual_type);
	if (it == m_spawners.end()) {
		egLOG("assembler/warning", "No ECS spawner for desc type: {0}", actual_type);
		return;
	}

	if (overrides.empty() && parent_desc.is_ready()) {
		it->second(reg, e, *parent_desc, name);
		return;
	}

	auto temp_desc = m_desc_sys.create_instance(actual_type, res::tag::null);
	if (!temp_desc) {
		egLOG("assembler/error", "Failed to create temporary desc of type: {0}", actual_type);
		return;
	}

	if (parent_desc.is_ready()) {
		parent_desc->copy_to(*temp_desc);
	}

	if (!overrides.empty()) {
		temp_desc->deserialize(m_desc_sys, overrides);
	}

	it->second(reg, e, *temp_desc, name);
}