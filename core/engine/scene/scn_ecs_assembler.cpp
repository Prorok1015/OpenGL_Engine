#include "scn_ecs_assembler.h"
#include <entt/entt.hpp>
#include "res_system.h"
#include "desc_json_ops.h"
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
		if (!ENGINE_VERIFY(!parent_desc.has_error(), "component '{}': parent desc '{}' failed to load, skipping", name, parent_desc.get_tag().view())) {
			return;
		}

		if (actual_type.empty() && parent_desc.is_valid()) {
			actual_type = parent_desc->get_type();
		}

		if (actual_type.empty()) return;

		auto it = m_spawners.find(actual_type);
		if (it == m_spawners.end()) {
			egLOG_WARN("assembler/warning", "No ECS spawner for desc type: {0}", actual_type);
			return;
		}

		bool is_structural = (it->second.category == spawner_category::structural);

		if (parent_desc.is_valid() && !is_hot_reload && !is_structural) {
			scn::tracked_component_info info{ actual_type, parent_desc.get_tag(), overrides };
			// Fill before the registry signals, so hot_reload_manager sees the source desc.
			if (reg.all_of<scn::component_tracker>(e)) {
				reg.patch<scn::component_tracker>(e, [&] (scn::component_tracker& tracker) {
					tracker.tracked_components[std::string(name)] = std::move(info);
				});
			} else {
				scn::component_tracker tracker;
				tracker.tracked_components[std::string(name)] = std::move(info);
				reg.emplace<scn::component_tracker>(e, std::move(tracker));
			}
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
			egLOG_ERROR("assembler/error", "Failed to create temporary desc of type: {0}", actual_type);
			return;
		}

		if (parent_desc.is_valid()) {
			parent_desc->copy_to(*temp_desc);
		}

		if (!overrides.empty() && parent_desc.is_valid()) {
			// Effective data is serialize(parent) with the overrides merged in recursively; a plain deserialize of
			// the overrides would replace a nested object whole and lose the sibling fields the parent set.
			boost::json::object effective;
			parent_desc->serialize(effective);
			desc::json_deep_merge(effective, overrides);
			temp_desc->deserialize(m_desc_sys, effective);
		} else if (!overrides.empty()) {
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