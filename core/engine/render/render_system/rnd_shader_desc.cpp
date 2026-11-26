#include <snk/render/rnd_shader_desc.h>
#include <snk/common/logger/engine_log.h>
#include <snk/desc/desc_schema_ops.h>
#include <snk/resource/res_diagnostics.h>

#include <algorithm>
#include <cctype>
#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	// The macro an entry defines: its first word ("NAME" or "NAME VALUE").
	std::string_view define_name(std::string_view entry)
	{
		const auto begin = entry.find_first_not_of(" \t");
		if (begin == std::string_view::npos) {
			return {};
		}
		entry.remove_prefix(begin);
		return entry.substr(0, entry.find_first_of(" \t"));
	}

	bool equal_ignoring_case(std::string_view a, std::string_view b)
	{
		return std::ranges::equal(a, b, [](char x, char y) {
			return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
		});
	}

	std::string unknown_backend_reason(std::string_view name)
	{
		const auto similar = std::ranges::find_if(rnd::k_shader_backends, [&](std::string_view known) { return equal_ignoring_case(known, name); });
		if (similar != std::end(rnd::k_shader_backends)) {
			return std::format("unknown backend '{}': did you mean '{}'?", name, *similar);
		}
		return std::format("unknown backend '{}'", name);
	}

	// The schema read drops a key the structure does not have with a debug line only, and a save then loses it: an override
	// with a misspelt stage would otherwise vanish without a word.
	void check_override_keys(const json::object& data)
	{
		const json::value* backends = data.if_contains("backends");
		if (!backends || !backends->is_object()) {
			return;
		}
		static const std::vector<std::string> known = desc::known_keys(
			*desc::bind_described<rnd::shader_stage_sources>("shader_stage_sources", 1, desc::schema_kind::structure));
		for (const auto& entry : backends->as_object()) {
			if (!entry.value().is_object()) {
				continue;
			}
			const std::string_view backend(entry.key().data(), entry.key().size());
			for (const std::string& key : desc::unknown_keys(known, entry.value().as_object())) {
				res::report_load_warning(res::diagnostic_kind::schema,
					"/backends/" + res::json_pointer_token(backend) + "/" + res::json_pointer_token(key),
					std::format("unknown stage '{}' in the override of '{}'", key, backend));
			}
		}
	}

	void check_stage(const res::tag& stage, const std::string& pointer, std::string_view key, std::string_view extension, bool required)
	{
		if (!stage.is_valid()) {
			if (required) {
				res::report_load_warning(res::diagnostic_kind::schema, pointer, std::format("the {} stage is missing", key));
			}
			return;
		}
		if (stage.extension() != extension) {
			res::report_load_warning(res::diagnostic_kind::schema, pointer,
				std::format("'{}' is not a .{} file, which the {} stage needs", stage.view(), extension, key));
		}
	}
}

rnd::shader_config rnd::shader_desc::config(const std::vector<std::string>& extra_defines) const
{
	shader_config out;
	out.cdata.program = shader_config::shader_program_data::build();
	out.cdata.program.set_vertex_shader(vertex);
	out.cdata.program.set_fragment_shader(fragment);
	out.cdata.program.set_geometry_shader(geometry);

	std::vector<std::string> merged;
	merged.reserve(defines.size() + extra_defines.size());
	for (const std::string& own : defines) {
		const std::string_view name = define_name(own);
		const auto conflict = std::ranges::find_if(extra_defines, [&](const std::string& extra) {
			return extra != own && define_name(extra) == name;
		});
		if (conflict != extra_defines.end()) {
			egLOG_WARN("render/shader", "'{}': the define '{}' of the shader and '{}' of the material name the same macro, the material's is used",
				get_tag().view(), own, *conflict);
			continue;
		}
		merged.push_back(own);
	}
	merged.insert(merged.end(), extra_defines.begin(), extra_defines.end());
	std::ranges::sort(merged);
	const auto [first, last] = std::ranges::unique(merged);
	merged.erase(first, last);
	out.cdata.defines = std::move(merged);
	return out;
}

void rnd::shader_desc::deserialize(desc::desc_system& desc_system, const json::object& data)
{
	desc::schema_desc<shader_desc, desc::schema_kind::desc>::deserialize(desc_system, data);
	check_stage(vertex, "/vertex", "vertex", "vert", true);
	check_stage(fragment, "/fragment", "fragment", "frag", true);
	check_stage(geometry, "/geometry", "geometry", "geom", false);
	check_override_keys(data);
	for (const auto& [name, stages] : backends) {
		const std::string pointer = "/backends/" + res::json_pointer_token(name);
		if (std::ranges::find(k_shader_backends, std::string_view{ name }) == std::end(k_shader_backends)) {
			res::report_load_warning(res::diagnostic_kind::schema, pointer, unknown_backend_reason(name));
		}
		if (!stages.vertex.is_valid() && !stages.fragment.is_valid() && !stages.geometry.is_valid()) {
			res::report_load_warning(res::diagnostic_kind::schema, pointer, "the override names no stage");
			continue;
		}
		check_stage(stages.vertex, pointer + "/vertex", "vertex", "vert", false);
		check_stage(stages.fragment, pointer + "/fragment", "fragment", "frag", false);
		check_stage(stages.geometry, pointer + "/geometry", "geometry", "geom", false);
	}
}

rnd::shader_stage_sources rnd::shader_desc::stages_for(std::string_view backend) const
{
	shader_stage_sources out{ .vertex = vertex, .fragment = fragment, .geometry = geometry };
	const auto found = backends.find(backend);
	if (found == backends.end()) {
		return out;
	}
	const shader_stage_sources& over = found->second;
	if (over.vertex.is_valid()) {
		out.vertex = over.vertex;
	}
	if (over.fragment.is_valid()) {
		out.fragment = over.fragment;
	}
	if (over.geometry.is_valid()) {
		out.geometry = over.geometry;
	}
	return out;
}
