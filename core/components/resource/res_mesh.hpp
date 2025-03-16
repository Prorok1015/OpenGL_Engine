#pragma once
#include "res_tag.h"

namespace res
{
	struct Vertex
	{
		glm::vec3 position = glm::vec3{ 0 };  // 0
		glm::vec3 normal = glm::vec3{ 0 };    // 1
		glm::vec2 uv = glm::vec2{ 0 };        // 2
		glm::vec3 tangent = glm::vec4{ 0 };   // 3
		glm::vec3 bitangent = glm::vec4{ 0 }; // 4
		glm::vec4 bones_weight = glm::vec4{0};// 5
		glm::vec4 color = glm::vec4{ 1 };	  // 6

		bool operator== (const Vertex& rhs) const;
	};

	struct Material
	{
		enum STATES
		{
			DIFFUSE_TXM,
			SPECULAR_TXM,
			HEIGHT_TXM,
			NORMALS_TXM,
			AMBIENT_TXM,
			ALBEDO_TXM,
			EMISSIVE_TXM,
			CLEARCOAT_TXM,
			SHININESS_TXM,
			OPACITY_TXM,
			LIGHTMAP_TXM,
			REFLECTION_TXM,
			NORMAL_CAMERA_TXM,
			EMISSION_COLOR_TXM,
			METALNESS_TXM,
			DIFFUSE_ROUGHNESS_TXM,
			AMBIENT_OCCLUSION_TXM,
			SHEEN_TXM,
			TRANSMISSION_TXM,

			ALBEDO_COLOR,
			SPECULAR_COLOR,
			AMBIENT_COLOR,
			EMISSIVE_COLOR,
			TRANSPARENT_COLOR,
			REFLECTIVE_COLOR,
			OPACITY,
			REFLECTIVITY,
			REFRACTI,
			SHININESS,
			SHININESS_STRENGTH,
			BUMP_SCALING,
			COUNT
		};
		std::array<bool, STATES::COUNT> state{};
		constexpr void set_state(STATES flag) { state[flag] = true; }
		constexpr void clear_state(STATES flag) { state[flag] = false; }
		constexpr bool is_state(STATES flag) const { return state[flag]; }
		const res::tag& get_txm(STATES txm) const {
			if (0 < txm && txm < TRANSMISSION_TXM + 1) {
				return txm_list[txm];
			}
			return res::tag::null;
		}

		std::string name;
		std::array<res::tag, TRANSMISSION_TXM + 1> txm_list{};
		
		//COLOR_DIFFUSE
		glm::vec4 diffuse_color { 0 };
		//COLOR_SPECULAR
		glm::vec4 specular_color{ 0 };
		//COLOR_AMBIENT
		glm::vec4 ambient_color{ 0 };
		//COLOR_EMISSIVE
		glm::vec4 emissive_color{ 0 };
		//COLOR_TRANSPARENT
		glm::vec4 transparent_color{ 0 };
		//COLOR_REFLECTIVE
		glm::vec4 reflective_color{ 0 };

		float opacity = 1.0;// 0.0 -> 1.0
		float reflectivity = 0.0;// 0.0 -> 1.0
		float refracti = 0.0;
		float shininess = 0.0;// 0.0 -> 1000
		float shininess_strength = 0.0;// 0.0 -> 1.0
		float bump_scaling = 0.0;// height map scaling
	};

	struct animation
	{
		std::string name;
		float duration = 0.f;
		float ticks_per_second = 0.f;
	};

	template<class T>
	struct animation_key_t
	{
		T value;
		float time;
	};

	using animation_pos_key = animation_key_t<glm::vec3>;
	using animation_rotate_key = animation_key_t<glm::quat>;
	using animation_scale_key = animation_key_t<glm::vec3>;

	struct animation_node
	{
		std::vector<animation_pos_key> pos_keys;
		std::vector<animation_rotate_key> rotate_keys;
		std::vector<animation_scale_key> scale_keys;
	};

	struct bone
	{
		std::unordered_map<std::string, animation_node> anim;
		glm::mat4 offset{ 1.0 };
	};

	struct Mesh
	{
		std::vector<Vertex> vertices;
		std::vector<unsigned int> indices;
		std::vector<bone> bones;
		Material material;
	};
	//===========================================//
	struct bone_view
	{
		std::size_t begin = 0;
		std::size_t end = 0;
		res::tag bones_indeces_txm;

		std::size_t get_offset() const { return begin; }
		std::size_t get_count() const { return end - begin; }
	};

	struct bone_container
	{
		std::vector<int32_t> bones_indeces; // vertex_count * max_bones_count_per_vertex
		//std::vector<bone_view> data;
		res::tag bones_indeces_txm;
		glm::ivec2 original_size{ 0 };
	};

	struct meshes_conteiner
	{
		std::vector<Vertex> vertices;
		std::vector<unsigned int> indices;
		std::vector<bone> bones; //?
		std::vector<glm::mat4> bones_matrices;
		std::vector<Material> materials;
		bone_container bones_data; //?
	};

	struct mesh_view
	{
		std::size_t vx_begin = 0;
		std::size_t vx_end = 0;
		std::size_t ind_begin = 0;
		std::size_t ind_end = 0;
		std::size_t bones_begin = 0;
		std::size_t bones_end = 0;
		std::size_t material_id = 0;

		std::size_t get_vertices_count() const { return vx_end - vx_begin; }
		std::size_t get_indices_count() const { return ind_end - ind_begin; }
		std::size_t get_bones_count() const { return bones_end - bones_begin; }
	};

	struct node_hierarchy_view
	{
		std::vector<mesh_view> meshes;
		std::vector<node_hierarchy_view> children; //?
		std::string name;
		glm::mat4 mt{ 1.0 };//?
		glm::mat4 original{ 1.0 };//?
		std::unordered_map<std::string, res::animation_node> anim;
		int bone_id = -1;//?
	};

	struct model_presintation
	{
		meshes_conteiner data;
		std::vector<animation> animations;
		std::vector<mesh_view> meshes_views;//?
		node_hierarchy_view head;
	};
}