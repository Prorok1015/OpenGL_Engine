#include "scn_engine_components_detail.h"
#include <snk/scene/scn_component_schema.h>
#include <snk/scene/scn_model.h>
#include <snk/scene/scn_camera_component.hpp>
#include <snk/scene/scn_camera_desc.h>
#include <boost/json/array.hpp>
#include <cstddef>
#include <cstdint>

// The render components of the engine catalog (see scn_engine_components.cpp).

namespace scn
{
	BOOST_DESCRIBE_STRUCT(mesh_view, (), (vx_begin, vx_end, ind_begin, ind_end))
	BOOST_DESCRIBE_STRUCT(mesh_component, (), (mesh))
	BOOST_DESCRIBE_STRUCT(geometry_component, (), (geom_tag))
	// `dirty` is left out: a restored material is compiled again.
	BOOST_DESCRIBE_STRUCT(material_desc_component, (), (mlt_desc))
	BOOST_DESCRIBE_STRUCT(skeleton_component, (), (skinning_tag, bone_count, show_skeleton))
	BOOST_DESCRIBE_STRUCT(skinning_component, (), (skinning_tag, skeleton_entity))
	BOOST_DESCRIBE_STRUCT(bone_component, (), (offset, index))
	BOOST_DESCRIBE_STRUCT(sky_component, (), (cube_map))
	// The viewport is saved: a camera drawing into its own target keeps a rect set once (the Game view hands out the
	// viewport of the backbuffer cameras every frame anyway). `wireframe` is a debug view of the editor and is not.
	BOOST_DESCRIBE_STRUCT(camera_component, (), (fov, near_distance, far_distance, m_viewport, layer, clear, projection,
		ortho_size, color_targets, depth_target, tp_accum_target, tp_reveal_target))
}

namespace desc
{
	// [x, y, width, height].
	template<>
	struct json_codec<scn::viewport>
	{
		static json::value to_json(const scn::viewport& rect)
		{
			return json::value(json::array{ rect.lefttop.x, rect.lefttop.y, rect.size.x, rect.size.y });
		}

		static bool from_json(scn::viewport& rect, const json::value& in)
		{
			if (!in.is_array() || in.as_array().size() != 4) {
				return false;
			}
			std::int64_t numbers[4] = {};
			for (std::size_t i = 0; i < 4; ++i) {
				if (!json_to_int64(in.as_array()[i], numbers[i])) {
					return false;
				}
			}
			rect = scn::viewport{ glm::ivec4{ static_cast<int>(numbers[0]), static_cast<int>(numbers[1]), static_cast<int>(numbers[2]), static_cast<int>(numbers[3]) } };
			return true;
		}
	};

	// A material tag, or an inline material written back inline (desc_ref_traits).
	template<>
	inline constexpr auto field_attrs<scn::material_desc_component> = attrs(
		f("mlt_desc").kind(field_kind::desc_ref).ref("material_desc"));
}

void scn::detail::register_render_components(component_catalog& catalog)
{
	add_engine_component<mesh_component>(catalog, "scn.mesh");
	add_engine_component<geometry_component>(catalog, "scn.geometry");
	add_engine_component<material_desc_component>(catalog, "scn.material_desc");
	add_engine_component<skeleton_component>(catalog, "scn.skeleton");
	add_engine_component<skinning_component>(catalog, "scn.skinning");
	add_engine_component<bone_component>(catalog, "scn.bone");
	add_engine_component<sky_component>(catalog, "scn.sky");
	add_engine_component<camera_component>(catalog, "scn.camera");
}
