#pragma once
#include "desc_base.hpp"
#include "scn_ecs_assembler.h"
#include "ds/ds_fixed_vector.hpp"

#include <glm/glm.hpp>

namespace scn {
	class camera_desc : public desc::desc_base {
	public:
		static constexpr std::string_view __type = "camera_desc";
		using base_type = desc::desc_base;
		camera_desc() = default;
		~camera_desc() = default;
		camera_desc(const camera_desc&) = default;
		camera_desc(camera_desc&&) = default;
		camera_desc& operator=(const camera_desc&) = default;
		camera_desc& operator=(camera_desc&&) = default;
		virtual void copy_to(desc_base& other) const override {
			camera_desc& other_desc = static_cast<camera_desc&>(other);
			other_desc = *this;
		}
		virtual void deserialize(desc::desc_system& desc_system, const json::object&) override;
		virtual void serialize(json::object&) const override;

		float fov() const { return m_fov; }
		float near_distance() const { return m_near_distance; }
		float far_distance() const { return m_far_distance; }
		glm::vec3 rotation() const { return glm::radians(m_rotation); }
		glm::vec3 position() const { return m_position; }
		const ds::fixed_vector<res::tag, 4>& color_targets() const { return m_color_targets; }
		res::tag depth_target() const { return m_depth_target; }
		res::tag tp_accum_target() const { return m_tp_accum_target; }
		res::tag tp_reveal_target() const { return m_tp_reveal_target; }

	private:
		float m_fov = 90.f;
		float m_near_distance = 0.0001f;
		float m_far_distance = 1000.f;
		glm::vec3 m_rotation{ 0.f };
		glm::vec3 m_position{ 0.f };
		ds::fixed_vector<res::tag, 4> m_color_targets;
		res::tag m_depth_target;
		res::tag m_tp_accum_target;
		res::tag m_tp_reveal_target;
	};

	void assemble_camera(entt::registry& reg, entt::entity e, const camera_desc& desc, const std::string_view name);
}