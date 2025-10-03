#pragma once
#include <common.h>
#include "res_tag.h"
#include "rnd_scene_shader_desc.h"

#include <rnd_driver_interface.h>
#include <rnd_uniform_buffer_interface.h>

namespace rnd
{
	struct global_params
	{
		glm::mat4 projection{};
		glm::mat4 view{};
		float time = 0;
		glm::vec4 view_position{};
	};

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
		};

		directional_light directional_lights[MAX_LIGHT_COUNT];
		point_light point_lights[MAX_LIGHT_COUNT];
	};

	struct bones_matrices
	{
		static constexpr int MAX_BONE_MATRICES_COUNT = 128;
		int row_height;
		int bone_count;
	private:
		glm::ivec2 _std140padding;
	public:
		glm::mat4 bones[MAX_BONE_MATRICES_COUNT];
	};

	class ShaderManager
	{
	public:
		ShaderManager(driver::driver_interface* ptr);
		~ShaderManager();

		ShaderManager(const ShaderManager&) = delete;
		ShaderManager(ShaderManager&&) = delete;
		ShaderManager& operator= (const ShaderManager&) = delete;
		ShaderManager& operator= (ShaderManager&&) = delete;

		void clear_cache() const {
			_cache.clear(); new_cache.clear();
		}

		void unuse() const;

		template<class T>
		void use(const T& desc) const
		{
			auto it = _cache.find(shader_desc::get_hash(desc));
			rnd::driver::shader_interface* shader = nullptr;
			if (it == _cache.end()) {
				auto new_shader = drv->create_shader(desc.load());
				if (new_shader) {
					shader = new_shader.get();
					_cache[shader_desc::get_hash(desc)] = std::move(new_shader);
				}
			} else {
				shader = it->second.get();
			}

			if (shader) {
				shader->use();
				rnd::configure_render_pass(desc, shader);
			}
		}

		void use(const new_shader_desc& desc) const
		{
			auto hash = desc.get_hash();
			auto it = new_cache.find(hash);
			rnd::driver::shader_interface* shader = nullptr;
			if (it == new_cache.end()) {
				auto new_shader = drv->create_shader(desc.load());
				if (new_shader) {
					shader = new_shader.get();
					new_cache[hash] = std::move(new_shader);
				}
			}
			else {
				shader = it->second.get();
			}
			if (shader) {
				shader->use();
				rnd::configure_render_pass(desc, shader);
			}
		}

		void update_global_uniform(const global_params& val) const;
		void update_global_sun(const lights_params& val) const;
		void update_global_bones_matrices(const bones_matrices& val, std::size_t count) const;
	private:
		driver::driver_interface* drv = nullptr;
		std::shared_ptr<rnd::driver::uniform_buffer_interface> _matrices;
		std::shared_ptr<rnd::driver::uniform_buffer_interface> sun_light;
		std::shared_ptr<rnd::driver::uniform_buffer_interface> bones_buffer;
		mutable std::unordered_map<shader_desc::shader_desc_hash, std::unique_ptr<rnd::driver::shader_interface>, shader_desc::shader_desc_hash::hasher> _cache;
		mutable std::unordered_map<std::size_t, std::unique_ptr<rnd::driver::shader_interface>> new_cache;
	};
}
