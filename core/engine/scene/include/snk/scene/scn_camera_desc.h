#pragma once
#include <snk/desc/desc_hints.hpp>
#include <snk/desc/desc_schema_desc.h>
#include <snk/scene/scn_camera_component.hpp>
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/common/ds/ds_fixed_vector.hpp>
#include <snk/resource/res_tag.h>

#include <boost/describe.hpp>
#include <glm/glm.hpp>
#include <snk/common/snk_api.h>

namespace scn {
	BOOST_DESCRIBE_ENUM(camera_clear, all, depth, none)
	BOOST_DESCRIBE_ENUM(camera_projection, perspective, ortho)

	class camera_desc;
}

// The schema of the camera: keys and hints of the fields; the defaults come from the members of the class. Declared before the
// class so that the serializer generated for schema_desc sees the attributes.
namespace desc {
	// The render targets are runtime tags in the memory:// store: an element without a protocol gets "memory://", as before
	// (res::tag(std::string) slices garbage from a string without one). Written as an array of the tag strings. Read from an
	// array, or from a single string: that is the legacy "texture" key, read as color_targets[0]. More tags than the camera
	// has slots do not fit.
	template<>
	struct json_codec<ds::fixed_vector<res::tag, 4>> {
		static json::value to_json(const ds::fixed_vector<res::tag, 4>& targets)
		{
			json::array array;
			array.reserve(targets.size());
			for (const auto& target : targets)
				array.push_back(json::string(target.string()));
			return json::value(std::move(array));
		}

		static bool from_json(ds::fixed_vector<res::tag, 4>& targets, const json::value& in)
		{
			ds::fixed_vector<res::tag, 4> parsed;
			if (in.is_string()) {
				parsed.push_back(render_target(in.as_string()));
			} else if (in.is_array() && in.as_array().size() <= 4) {
				for (const auto& item : in.as_array()) {
					if (!item.is_string())
						return false;
					parsed.push_back(render_target(item.as_string()));
				}
			} else {
				return false;
			}
			targets = std::move(parsed);
			return true;
		}

		static res::tag render_target(const std::string_view str)
		{
			return str.find("://") != std::string_view::npos ? res::tag(str) : res::tag(res::tag::memory, str);
		}
	};

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
		f("m_tp_reveal_target").key("/tp_reveal_target").write_if_valid().hidden(),
		f("m_layer").key("/layer").range(-16, 16, 1).write_if_not_default().tooltip("Render layer: lower layers draw first, a higher one lands on top"),
		f("m_clear").key("/clear").write_if_not_default().tooltip("What the camera clears first: all (colour and depth), depth (keeps the colour of lower layers) or none"),
		f("m_projection").key("/projection").write_if_not_default(),
		f("m_ortho_size").key("/ortho_size").range(0.01, 100000, 0.1).write_if_not_default().tooltip("Orthographic projection: height of the visible volume in world units"));

	// Written as lower-case words in the desc ("clear": "depth", "projection": "ortho"). No fallback: an unknown word keeps
	// the value the camera has.
	template<>
	struct enum_names<scn::camera_clear> {
		static constexpr std::string_view name = "camera_clear";
	};

	template<>
	struct enum_names<scn::camera_projection> {
		static constexpr std::string_view name = "camera_projection";
	};
}

namespace scn {
	class SNK_API camera_desc : public desc::schema_desc<camera_desc, desc::schema_kind::component> {
	public:
		static constexpr std::string_view __type = "camera_desc";
		using base_type = desc::desc_base;
		camera_desc() = default;
		~camera_desc() = default;
		camera_desc(const camera_desc&) = default;
		camera_desc(camera_desc&&) = default;
		camera_desc& operator=(const camera_desc&) = default;
		camera_desc& operator=(camera_desc&&) = default;

		// Member-wise copy as before: the copy of the schema leaves the type name of the instance behind.
		virtual void copy_to(desc_base& other) const override {
			copy_to_same(*this, other);
		}

		// Generic read of the schema, after the few forms it cannot say: see scn_camera_desc.cpp.
		virtual void deserialize(desc::desc_system& desc_system, const json::object& data) override;

		float fov() const { return m_fov; }
		float near_distance() const { return m_near_distance; }
		float far_distance() const { return m_far_distance; }
		glm::vec3 rotation() const { return glm::radians(m_rotation); }
		glm::vec3 position() const { return m_position; }
		const ds::fixed_vector<res::tag, 4>& color_targets() const { return m_color_targets; }
		res::tag depth_target() const { return m_depth_target; }
		res::tag tp_accum_target() const { return m_tp_accum_target; }
		res::tag tp_reveal_target() const { return m_tp_reveal_target; }
		int layer() const { return m_layer; }
		camera_clear clear() const { return m_clear; }
		camera_projection projection() const { return m_projection; }
		float ortho_size() const { return m_ortho_size; }

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
		int m_layer = 0;
		camera_clear m_clear = camera_clear::all;
		camera_projection m_projection = camera_projection::perspective;
		float m_ortho_size = default_camera_ortho_size;

		BOOST_DESCRIBE_CLASS(camera_desc, (), (), (), (m_fov, m_near_distance, m_far_distance, m_rotation, m_position,
			m_color_targets, m_depth_target, m_tp_accum_target, m_tp_reveal_target,
			m_layer, m_clear, m_projection, m_ortho_size))
	};

	SNK_API void assemble_camera(entt::registry& reg, entt::entity e, const camera_desc& desc, const std::string_view name);
}
