#pragma once
#include <common.h>
#include "res_resource_base.h"
#include "res_mesh.hpp"

namespace res
{  
	class Model : public Resource
	{
	public:
		Model(const tag& tag);
		Model(const tag& tag, const res::model_presintation& model);

		const std::vector<Mesh>& get_meshes() const { return meshes; }
		res::model_presintation& get_model_pres() { return model; }

		std::vector<glm::mat4> get_bone_transforms(double TimeInSeconds, std::string_view animation_name);

	private:
		void calculate_bone_transform(res::node_hierarchy_view& node, float time_sec, const res::animation& anim, const glm::mat4& parent, std::vector<glm::mat4>& out_transform);

		void calc_interpolated_scaling(glm::vec3& Out, float AnimationTimeTicks, const res::animation_node& anim);
		void calc_interpolated_position(glm::vec3& Out, float AnimationTimeTicks, const res::animation_node& anim);
		void calc_interpolated_rotation(glm::quat& Out, float AnimationTimeTicks, const res::animation_node& anim);

	private:
		std::vector<Mesh> meshes;
		res::model_presintation model;
	};

	class asset_3d : public Resource
	{
	public:
		asset_3d(const tag& tag);

		res::tag geometry;
		res::tag skelet;
		res::tag model;
		res::tag anim;
	};
}
