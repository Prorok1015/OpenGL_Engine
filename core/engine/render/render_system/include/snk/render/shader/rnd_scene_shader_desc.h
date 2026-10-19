#pragma once
#include <cstdint>
#include <string_view>
#include <snk/common/common.h>
#include <snk/common/ds/ds_fixed_vector.hpp>
#include <snk/resource/res_tag.h>
#include <snk/driver/rnd_shader_interface.h>
#include <snk/driver/rnd_driver_interface.h>

namespace res { class resource_system; }

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
			bool operator==(const shader_program_data&) const noexcept = default;

		private:
			ds::fixed_vector<res::tag, 3> shader_tags;
		};

		struct constant_data
		{
			std::unordered_map<std::string, std::string> constants;
			std::vector<std::string> defines;
			shader_program_data program = shader_program_data::build();
			bool operator==(const constant_data&) const noexcept = default;

			// Platform- and iteration-order-independent: the same program, defines and constants always give
			// the same value, whatever the order they were added in. `pass_mask` extends the key with the
			// pass defines the variant cache appends (see shader_config_cache).
			std::uint64_t variant_hash(std::uint32_t pass_mask = 0) const noexcept {
				std::uint64_t hash = fnv1a(std::string_view{ "program" });
				for (const auto& tag : program) {
					hash = (hash ^ fnv1a(tag.view())) * fnv_prime;
				}

				std::uint64_t defines_sum = 0;
				for (const auto& define : defines) {
					defines_sum += mix(fnv1a(define));
				}

				std::uint64_t constants_sum = 0;
				for (const auto& [name, value] : constants) {
					constants_sum += mix((fnv1a(name) * fnv_prime) ^ fnv1a(value));
				}

				return mix(hash ^ mix(defines_sum) ^ (mix(constants_sum) << 1) ^ (std::uint64_t{ pass_mask } << 32));
			}

			static constexpr std::uint64_t fnv_prime = 1099511628211ull;

			static constexpr std::uint64_t fnv1a(std::string_view text) noexcept {
				std::uint64_t hash = 14695981039346656037ull;
				for (const char c : text) {
					hash = (hash ^ static_cast<unsigned char>(c)) * fnv_prime;
				}
				return hash;
			}

			static constexpr std::uint64_t mix(std::uint64_t x) noexcept {
				x += 0x9e3779b97f4a7c15ull;
				x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
				x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
				return x ^ (x >> 31);
			}
		};

		struct runtime_data
		{
			std::vector<rnd::driver::texture_interface*> samplers;
			std::unordered_map<std::string, driver::uniform_data> uniforms;
		};

		std::size_t get_hash() const noexcept {
			return static_cast<std::size_t>(cdata.variant_hash());
		}

		auto operator==(const shader_config& lhs) const noexcept {
			return cdata == lhs.cdata;
		}

		// The source of every stage with includes and defines resolved and the binding macros expanded for `bindings`.
		std::vector<driver::shader_header> load(res::resource_system& res_sys, const driver::binding_table& bindings) const;
		static std::vector<driver::shader_header> load(const constant_data& cdata, res::resource_system& res_sys, const driver::binding_table& bindings);

		constant_data cdata;
		runtime_data rdata;
	};

	// Binds the samplers and sets the uniforms of a material on the program that is current.
	void configure_render_pass(const shader_config::runtime_data& rdata, rnd::driver::shader_interface* shader);
	void configure_uniforms(const shader_config::runtime_data& rdata, rnd::driver::shader_interface* shader);
}

namespace std {
	template<>
	struct hash<rnd::shader_config> {
		std::size_t operator()(const rnd::shader_config& sdr) const {
			return sdr.get_hash();
		}
	};
}