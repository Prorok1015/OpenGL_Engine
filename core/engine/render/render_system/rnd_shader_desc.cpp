#include <snk/render/rnd_shader_desc.h>
#include <snk/common/logger/engine_log.h>
#include <snk/resource/res_diagnostics.h>

#include <algorithm>
#include <format>
#include <iterator>
#include <string>
#include <string_view>

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
	for (const auto& [name, stages] : backends) {
		const std::string pointer = "/backends/" + res::json_pointer_token(name);
		if (std::ranges::find(k_shader_backends, std::string_view{ name }) == std::end(k_shader_backends)) {
			res::report_load_warning(res::diagnostic_kind::schema, pointer, std::format("unknown backend '{}'", name));
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
	const auto found = backends.find(std::string{ backend });
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
