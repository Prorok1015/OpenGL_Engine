#pragma once
#include "common.h"
#include "ds_fixed_vector.hpp"
#include "res_tag.h"
#include "rnd_shader_interface.h"
#include "rnd_driver_interface.h"

namespace rnd
{
	struct shader_config
	{
		struct shader_program_data
		{
			static shader_program_data build() {
				shader_program_data r{};
				r.shader_tags.push_back(res::tag::null);
				r.shader_tags.push_back(res::tag::null);
				r.shader_tags.push_back(res::tag::null);
				return r;
			}

			shader_program_data& set_vertex_shader(const res::tag& tag) {
				shader_tags[0] = tag;
				return *this;
			}

			shader_program_data& set_fragment_shader(const res::tag& tag) {
				shader_tags[1] = tag;
				return *this;
			}

			shader_program_data& set_geometry_shader(const res::tag& tag) {
				shader_tags[2] = tag;
				return *this;
			}

			const res::tag& get_vertex_shader() const {
				auto it = std::find_if(shader_tags.begin(), shader_tags.end(), 
					[](const auto& tag) { return tag.extension() == "vert"; });
				if (it != shader_tags.end()) {
					return *it;
				}

				return res::tag::null;
			}

			const res::tag& get_fragment_shader() const {
				auto it = std::find_if(shader_tags.begin(), shader_tags.end(),
					[](const auto& tag) { return tag.extension() == "frag"; });
				if (it != shader_tags.end()) {
					return *it;
				}

				return res::tag::null;
			}

			const res::tag& get_geometry_shader() const {
				auto it = std::find_if(shader_tags.begin(), shader_tags.end(),
					[](const auto& tag) { return tag.extension() == "geom"; });
				if (it != shader_tags.end()) {
					return *it;
				}

				return res::tag::null;
			}

			int size() const noexcept {
				return shader_tags.size();
			}

			bool empty() const noexcept {
				return shader_tags.empty();
			}

			auto begin() { return shader_tags.begin(); }
			auto end() { return shader_tags.end(); }
			auto begin() const { return shader_tags.begin(); }
			auto end() const { return shader_tags.end(); }
			auto operator<=>(const shader_program_data&) const noexcept = default;

		private:
			ds::fixed_vector<res::tag, 3> shader_tags;
		};

		struct constant_data
		{
			std::unordered_map<std::string, std::string> constants;
			std::vector<std::string> defines;
			shader_program_data program = shader_program_data::build();
			auto operator<=>(const constant_data&) const noexcept = default;
		};

		struct runtime_data
		{
			std::vector<rnd::driver::texture_interface*> samplers;
			std::unordered_map<std::string, driver::uniform_data> uniforms;
		};

		std::size_t get_hash() const noexcept {
			std::size_t hash = 0;
			for (const auto& [_, value] : cdata.constants) {
				hash ^= std::hash<std::string>{}(value);
			}
			for (const auto& define : cdata.defines) {
				hash ^= std::hash<std::string>{}(define);
			}
			for (const auto& tag : cdata.program) {
				hash ^= std::hash<res::tag>{}(tag);
			}
			return hash;
		}

		auto operator==(const shader_config& lhs) const noexcept {
			return cdata == lhs.cdata;
		}

		std::vector<driver::shader_header> load() const;

		constant_data cdata;
		runtime_data rdata;
	};

	void configure_render_pass(const shader_config& desc, rnd::driver::shader_interface* shader);
}

namespace std {
	template<>
	struct hash<rnd::shader_config> {
		std::size_t operator()(const rnd::shader_config& sdr) const {
			return sdr.get_hash();
		}
	};
}