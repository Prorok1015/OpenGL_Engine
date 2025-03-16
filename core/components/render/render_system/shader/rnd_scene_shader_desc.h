#pragma once
#include "common.h"
#include "res_tag.h"
#include "rnd_shader_interface.h"
#include "rnd_driver_interface.h"

namespace rnd
{
	template <typename T, size_t N1, size_t N2>
	consteval auto concat_arrays(const std::array<T, N1>& a1, const std::array<T, N2>& a2) {
		std::array<T, N1 + N2> result{};
		size_t index = 0;

		for (size_t i = 0; i < N1; ++i) {
			result[index] = a1[i];
			++index;
		}

		for (size_t i = 0; i < N2; ++i) {
			result[index] = a2[i];
			++index;
		}

		return result;
	}

	struct shader_desc
	{
		enum
		{
			LAST,
		};

		static consteval auto get_all_define_names() {
			return std::array<std::string_view, 0>{};
		}

		struct shader_desc_hash {
			struct hasher {
				std::size_t operator() (const shader_desc_hash& desc) const noexcept {
					return desc.value;
				}
			};
			auto operator<=>(const shader_desc_hash& rhs) const noexcept = default;

			std::size_t value;
			std::vector<bool> defines;
			std::vector<std::string> values;
		};

		shader_desc(std::string_view name_, std::size_t defines_count)
			: name(name_)
			, defines(defines_count, false)
			, defines_values(defines_count, "") {}

		static shader_desc_hash get_hash(const shader_desc& desc) { 
			return  shader_desc_hash{ 
				std::hash<std::string_view>{}(desc.name), desc.defines, desc.defines_values 
			}; 
		}

		bool operator== (const shader_desc& desc) const noexcept {
			return name == desc.name && defines == desc.defines && defines_values == desc.defines_values;
		}

		void preprocess_shader_code(std::vector<res::tag> tags, std::vector<rnd::driver::shader_header>& headers, const auto& define_names) const;

		rnd::driver::texture_interface* tex0 = nullptr;
		rnd::driver::texture_interface* tex1 = nullptr;
		rnd::driver::texture_interface* tex2 = nullptr;
		rnd::driver::texture_interface* tex3 = nullptr;
		std::string_view name;
		mutable std::vector<bool> defines;
		std::vector<std::string> defines_values;
	};
	
	struct shader_scene_desc : public shader_desc
	{
		enum
		{
			USE_ANIMATION = LAST,
			USE_NORMAL_MAP,
			USE_SPECULAR_MAP,
			USE_TXM_AS_DIFFUSE,
			MAX_BONE_MATRICES_COUNT,
			DIRECTION_LIGHT_COUNT,
			POINT_LIGHT_COUNT,
			LIGHTS_ENABLED,
			LAST,
		};

		static consteval auto get_all_define_names() {
			auto arr1 = shader_desc::get_all_define_names();
			constexpr auto arr2 = std::array{
				"USE_ANIMATION"sv,
				"USE_NORMAL_MAP"sv,
				"USE_SPECULAR_MAP"sv,
				"USE_TXM_AS_DIFFUSE"sv,
				"MAX_BONE_MATRICES_COUNT"sv,
				"DIRECTION_LIGHT_COUNT"sv,
				"POINT_LIGHT_COUNT"sv,
				"LIGHTS_ENABLED"sv,
			};

			return concat_arrays(arr1, arr2);
		}

		shader_scene_desc(std::string_view name = "scene")
			: shader_desc(name, LAST) {}

		glm::mat4 uWorldModel = glm::mat4{ 1.0 };
		glm::mat4 uWorldMeshMatr = glm::mat4{ 1.0 };
		glm::vec4 diffuseColor = glm::vec4(0);
		glm::vec4 emissiveColor = glm::vec4(0);
		float shininess = 32.f;
		std::vector<driver::shader_header> load() const;
	};

	struct pass_transparent_desc : shader_scene_desc
	{
		pass_transparent_desc(std::string_view name = "transparent")
			: shader_scene_desc(name) {}
	};

	struct pass_z_prepass_desc : shader_scene_desc
	{
		pass_z_prepass_desc(std::string_view name = "z_prepass")
			: shader_scene_desc(name) {}
	};

	struct shader_sky_desc : public shader_desc
	{
		shader_sky_desc()
			: shader_desc("sky", LAST)
		{}

		std::vector<driver::shader_header> load() const;
	};

	struct pass_composition_desc : public shader_desc
	{
		pass_composition_desc()
			: shader_desc("composition", LAST)
		{}

		std::vector<driver::shader_header> load() const;
	};

	struct shader_scene_instance_desc : public shader_desc
	{
		shader_scene_instance_desc() 
			: shader_desc("scene_inst", LAST)
		{}

		std::vector<driver::shader_header> load() const;
	};

	void configure_render_pass(const shader_desc& decs, rnd::driver::shader_interface* shader);
	void configure_render_pass(const shader_scene_desc& decs, rnd::driver::shader_interface* shader);
}