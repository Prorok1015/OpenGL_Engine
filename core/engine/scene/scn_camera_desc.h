#pragma once
#include "desc_base.hpp"
#include "scn_ecs_assembler.h"

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
		res::tag texture() const { return m_texture; }

	private:
		float m_fov = 90.f;
		float m_near_distance = 0.0001f;
		float m_far_distance = 1000.f;
		glm::vec3 m_rotation{ 0.f };
		glm::vec3 m_position{ 0.f };
		res::tag m_texture;
	};

	void assemble_camera(entt::registry& reg, entt::entity e, const camera_desc& desc, const std::string_view name);
}