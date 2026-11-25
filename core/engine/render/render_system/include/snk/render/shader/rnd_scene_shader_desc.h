#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <snk/common/common.h>
#include <snk/common/snk_api.h>
#include <snk/common/ds/ds_fixed_vector.hpp>
#include <snk/model_codec/res_tag.h>
#include <snk/driver/rnd_driver_interface.h>
#include <snk/render/rnd_shader_problem.h>
#include <snk/render/texture/rnd_texture_id.h>

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
				return static_cast<int>(shader_tags.size());
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

		// What a material feeds the program with, ready to bind: the textures at the sampler slots of set 2
		// as ids (no id for a slot without a map; an id whose texture is still loading has no handle yet) and the bytes of the MaterialParams block (std140, see
		// rnd_material_params.h). Nothing is set by name.
		struct runtime_data
		{
			std::vector<rnd::texture_id> samplers;
			std::vector<std::byte> material_params;
		};

		std::size_t get_hash() const noexcept {
			return static_cast<std::size_t>(cdata.variant_hash());
		}

		auto operator==(const shader_config& lhs) const noexcept {
			return cdata == lhs.cdata;
		}

		// The source of every stage with includes and defines resolved and the binding macros expanded for `bindings`.
		// Every included file is wrapped in #line directives (`#line 1 <k>` before it, `#line <n> <parent>` after it) and
		// `#line <n> 0` follows the defines after #version, so the compiler reports the lines of the files themselves.
		// A source the engine cannot prepare (an unreadable binding macro, no #version) is not returned: `error`
		// says why and the shader must not reach the compiler.
		struct load_result
		{
			std::vector<driver::shader_header> headers;
			std::string error;
			// Every file the source was made of: the stages and the files they include, however deep. Filled
			// when the load failed as well; whoever caches the result drops it when one of these changes.
			std::vector<res::tag> files;
			// What `error` says, one problem per reason at the file and line it is about (#1201).
			std::vector<shader_problem> problems;
			// Per stage that could be read (in the order of `headers`): the files its #line directives number, the stage
			// itself first. A line of the compiler's log for that stage names its file by this index.
			std::vector<std::vector<res::tag>> sources;
			// The stages and the defines, as a problem names the program.
			std::string program;

			bool ok() const noexcept { return error.empty() && !headers.empty(); }
		};

		SNK_API load_result load(res::resource_system& res_sys, const driver::binding_table& bindings) const;
		SNK_API static load_result load(const constant_data& cdata, res::resource_system& res_sys, const driver::binding_table& bindings);

		constant_data cdata;
		runtime_data rdata;
	};

	// The defines the engine puts in front of every stage. Module code inlines everything below, so a change of
	// the program title's format must keep parse_program_defines reading the titles the old format wrote.
	inline constexpr std::string_view max_light_count_define = "MAX_LIGHT_COUNT";
	inline constexpr std::string_view vertex_stage_define = "SNK_VERTEX_SHADER";
	inline constexpr std::string_view fragment_stage_define = "SNK_FRAGMENT_SHADER";
	inline constexpr std::string_view geometry_stage_define = "SNK_GEOMETRY_SHADER";

	inline constexpr bool is_engine_define(std::string_view name) noexcept
	{
		return name == max_light_count_define || name == vertex_stage_define
			|| name == fragment_stage_define || name == geometry_stage_define;
	}

	// The names a program title lists: the constants' names first, then the defines as written.
	inline std::vector<std::string> program_define_entries(const shader_config::constant_data& cdata)
	{
		std::vector<std::string> entries;
		entries.reserve(cdata.constants.size() + cdata.defines.size());
		for (const auto& [name, value] : cdata.constants) {
			entries.push_back(name);
		}
		entries.insert(entries.end(), cdata.defines.begin(), cdata.defines.end());
		return entries;
	}

	// "{ A | B }", or empty for no entries.
	inline std::string format_program_defines(const std::vector<std::string>& entries)
	{
		if (entries.empty()) {
			return {};
		}
		std::string text = "{ ";
		for (std::size_t i = 0; i < entries.size(); ++i) {
			if (i != 0) {
				text += " | ";
			}
			text += entries[i];
		}
		text += " }";
		return text;
	}

	// The entries of the "{ ... }" that ends a program title (or a stage title before its extension); empty when
	// the title has none. An entry that itself contains " | " or " }" does not come back whole.
	inline std::vector<std::string> parse_program_defines(std::string_view title)
	{
		std::vector<std::string> entries;
		const std::size_t open = title.find("{ ");
		if (open == std::string_view::npos) {
			return entries;
		}
		const std::size_t close = title.find(" }", open + 2);
		if (close == std::string_view::npos) {
			return entries;
		}
		std::string_view body = title.substr(open + 2, close - open - 2);
		while (!body.empty()) {
			const std::size_t bar = body.find(" | ");
			entries.emplace_back(body.substr(0, bar));
			if (bar == std::string_view::npos) {
				break;
			}
			body.remove_prefix(bar + 3);
		}
		return entries;
	}
}

namespace std {
	template<>
	struct hash<rnd::shader_config> {
		std::size_t operator()(const rnd::shader_config& sdr) const {
			return sdr.get_hash();
		}
	};
}