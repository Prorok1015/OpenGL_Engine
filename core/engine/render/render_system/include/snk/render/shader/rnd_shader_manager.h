#pragma once
#include <snk/common/common.h>
#include <cstddef>
#include <snk/resource/res_tag.h>
#include <snk/render/shader/rnd_scene_shader_desc.h>
#include <snk/render/shader/rnd_shader_config_cache.h>

#include <snk/driver/rnd_driver_interface.h>
#include <snk/driver/rnd_uniform_buffer_interface.h>

namespace rnd
{
	struct global_params
	{
		glm::mat4 projection{};
		glm::mat4 view{};
		float time = 0;
		float pad0[3]{};
		glm::vec4 view_position{};
	};

	// matches std140 layout of block Matrices in core/res/shaders/phong_model_lib.frag
	static_assert(offsetof(global_params, projection) == 0);
	static_assert(offsetof(global_params, view) == 64);
	static_assert(offsetof(global_params, time) == 128);
	static_assert(offsetof(global_params, view_position) == 144);
	static_assert(sizeof(global_params) == 160);

	struct lights_params
	{
		static constexpr int MAX_LIGHT_COUNT = 12;

		struct directional_light
		{
			glm::vec4 direction;
			glm::vec4 diffuse;
			glm::vec4 ambient;
			glm::vec4 specular;
		};

		struct point_light
		{
			glm::vec4 position;
			glm::vec4 ambient;
			glm::vec4 diffuse;
			glm::vec4 specular;

			float constant;
			float linear;
			float quadratic;
			float pad0 = 0; // std140 array stride of PointLight is 80
		};

		directional_light directional_lights[MAX_LIGHT_COUNT];
		point_light point_lights[MAX_LIGHT_COUNT];
		int dir_light_count = 0;
		int point_light_count = 0;
		int pad0[2]{};
	};

	// matches std140 layout of block GlobalLights in core/res/shaders/phong_model_lib.frag
	static_assert(sizeof(lights_params::directional_light) == 64);
	static_assert(sizeof(lights_params::point_light) == 80);
	static_assert(offsetof(lights_params::directional_light, direction) == 0);
	static_assert(offsetof(lights_params::directional_light, diffuse) == 16);
	static_assert(offsetof(lights_params::directional_light, ambient) == 32);
	static_assert(offsetof(lights_params::directional_light, specular) == 48);
	static_assert(offsetof(lights_params::point_light, position) == 0);
	static_assert(offsetof(lights_params::point_light, ambient) == 16);
	static_assert(offsetof(lights_params::point_light, diffuse) == 32);
	static_assert(offsetof(lights_params::point_light, specular) == 48);
	static_assert(offsetof(lights_params::point_light, constant) == 64);
	static_assert(offsetof(lights_params::point_light, linear) == 68);
	static_assert(offsetof(lights_params::point_light, quadratic) == 72);
	static_assert(offsetof(lights_params, directional_lights) == 0);
	static_assert(offsetof(lights_params, point_lights) == 768);
	static_assert(offsetof(lights_params, dir_light_count) == 1728);
	static_assert(offsetof(lights_params, point_light_count) == 1732);
	static_assert(sizeof(lights_params) == 1744);

	class ShaderManager
	{
	public:
		ShaderManager(driver::driver_interface* ptr, res::resource_system& res_sys);
		~ShaderManager();

		ShaderManager(const ShaderManager&) = delete;
		ShaderManager(ShaderManager&&) = delete;
		ShaderManager& operator= (const ShaderManager&) = delete;
		ShaderManager& operator= (ShaderManager&&) = delete;

		void clear_cache() const {
			m_programs.clear();
		}

		shader_config_cache& variants() const { return m_variants; }

		// The program of a variant, compiled on its first use; null when it does not compile.
		driver::shader_interface* program(variant_id variant) const { return require_program(variant); }

		// Sets the samplers and uniforms of a variant's program. The variant's pipeline has to be bound first:
		// binding it is what makes the program current.
		void apply(const shader_config& desc) const
		{
			apply(m_variants.intern(desc.cdata), desc.rdata, nullptr);
		}

		void apply(variant_id variant, const shader_config::runtime_data& params, const glm::mat4* world) const
		{
			if (auto* shader = require_program(variant)) {
				rnd::configure_render_pass(params, world, shader);
			}
		}

		void update_global_uniform(const global_params& val) const;
		void update_global_sun(const lights_params& val) const;
	private:
		driver::driver_interface* drv = nullptr;
		res::resource_system& m_res_sys;
		std::shared_ptr<rnd::driver::uniform_buffer_interface> _matrices;
		std::shared_ptr<rnd::driver::uniform_buffer_interface> sun_light;
		rnd::driver::shader_interface* require_program(variant_id variant) const
		{
			if (variant.value >= m_programs.size()) {
				m_programs.resize(variant.value + 1);
			}

			auto& slot = m_programs[variant.value];
			if (!slot) {
				slot = drv->create_shader(shader_config::load(m_variants.resolve(variant), m_res_sys));
			}
			return slot.get();
		}

		mutable shader_config_cache m_variants;
		mutable std::vector<std::unique_ptr<rnd::driver::shader_interface>> m_programs;
	};
}
