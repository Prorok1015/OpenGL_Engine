#pragma once
#include <snk/desc/desc_base.hpp>
#include <snk/desc/desc_hints.hpp>
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/common/ds/ds_fixed_vector.hpp>

#include <boost/describe.hpp>
#include <glm/glm.hpp>
#include <snk/common/snk_api.h>

namespace scn {
	class SNK_API camera_desc : public desc::desc_base {
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
			copy_to_same(*this, other);
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

		BOOST_DESCRIBE_CLASS(camera_desc, (), (), (), (m_fov, m_near_distance, m_far_distance, m_rotation, m_position,
			m_color_targets, m_depth_target, m_tp_accum_target, m_tp_reveal_target))
	};

	void assemble_camera(entt::registry& reg, entt::entity e, const camera_desc& desc, const std::string_view name);
}

// The schema of the camera: keys and hints of the fields; the defaults come from the members above.
namespace desc {
	template<>
	inline constexpr auto field_attrs<scn::camera_desc> = attrs(
		f("m_fov").key("/fov").range(1, 179, 0.5).degrees().tooltip("Vertical field of view"),
		f("m_near_distance").key("/near_distance").range(0.0001, 10, 0.001),
		f("m_far_distance").key("/far_distance").range(1, 100000, 1),
		f("m_rotation").key("/transform/rotation").range(0, 0, 0.5).degrees(), // no bounds (min == max), a drag step of half a degree
		f("m_position").key("/transform/position"),
		f("m_color_targets").key("/color_targets").alias("/texture").write_if_valid().hidden(),
		f("m_depth_target").key("/depth_target").write_if_valid().hidden(),
		f("m_tp_accum_target").key("/tp_accum_target").write_if_valid().hidden(),
		f("m_tp_reveal_target").key("/tp_reveal_target").write_if_valid().hidden());
}