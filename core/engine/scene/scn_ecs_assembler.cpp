#include "scn_ecs_assembler.h"
#include "res_system.h"
#include "logger/engine_log.h"

namespace scn {

	void ecs_assembler::assemble_and_apply(
		entt::registry& reg,
		entt::entity e,
		std::string actual_type,
		const res::res_handle<desc::desc_base>& parent_desc,
		const boost::json::object& overrides,
		const std::string_view name,
		bool is_hot_reload) const
	{
		if (actual_type.empty() && parent_desc.is_valid()) {
			actual_type = parent_desc->get_type();
		}

		if (actual_type.empty()) return;

		auto it = m_spawners.find(actual_type);
		if (it == m_spawners.end()) {
			egLOG("assembler/warning", "No ECS spawner for desc type: {0}", actual_type);
			return;
		}

		bool is_structural = (it->second.category == spawner_category::structural);

		if (parent_desc.is_valid() && !is_hot_reload && !is_structural) {
			auto& tracker = reg.get_or_emplace<scn::component_tracker>(e);
			tracker.tracked_components[std::string(name)] = scn::tracked_component_info{
				actual_type,
				parent_desc.get_tag(),
				overrides
			};
		}

		if (overrides.empty() && parent_desc.is_valid()) {
			if (is_hot_reload && it->second.patch) {
				it->second.patch(reg, e, *parent_desc);
			} else if (it->second.spawn) {
				it->second.spawn(reg, e, *parent_desc, name);
			}
			return;
		}

		auto temp_desc = m_desc_sys.create_instance(actual_type, res::tag{});
		if (!temp_desc) {
			egLOG("assembler/error", "Failed to create temporary desc of type: {0}", actual_type);
			return;
		}

		if (parent_desc.is_valid()) {
			parent_desc->copy_to(*temp_desc);
		}

		if (!overrides.empty()) {
			temp_desc->deserialize(m_desc_sys, overrides);
		}

		if (is_hot_reload && it->second.patch) {
			it->second.patch(reg, e, *temp_desc);
		} else if (it->second.spawn) {
			it->second.spawn(reg, e, *temp_desc, name);
		}
	}

	void ecs_assembler::trigger_hot_reload_for_component(entt::registry& reg, const res::res_handle<desc::desc_base>& changed_desc) const
	{
		auto view = reg.view<scn::component_tracker>();
		for (auto entity : view) {
			auto& tracker = view.get<scn::component_tracker>(entity);

			for (const auto& [comp_name, info] : tracker.tracked_components) {
				if (info.source_desc == changed_desc.get_tag()) {
					assemble_and_apply(
						reg,
						entity,
						info.type_name,
						changed_desc,
						info.local_overrides,
						comp_name,
						true
					);
				}
			}
		}
	}
}