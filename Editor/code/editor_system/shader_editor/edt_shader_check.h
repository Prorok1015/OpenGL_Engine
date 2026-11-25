#pragma once
#include <snk/model_codec/res_tag.h>
#include <snk/render/rnd_shader_problem.h>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace edt
{
	inline constexpr std::size_t k_shader_check_max_programs = 32;

	// The `.shader.desc` files under the project's layers, as res:// tags, sorted and without duplicates.
	std::vector<res::tag> shader_descs_in(const std::vector<std::filesystem::path>& layers);

	struct shader_check_report
	{
		// The problems of the programs the source belongs to, each program named "default variant: <stages>".
		std::vector<rnd::shader_problem> problems;
		std::size_t built = 0;
		std::size_t belonging = 0;
		// The descs that were looked at to find those.
		std::size_t examined = 0;
		// More descs than k_shader_check_max_programs were left unbuilt.
		bool truncated = false;
		std::string summary;
	};

	struct shader_check_ports
	{
		// The files the program of a desc is made of, found without building it (shader_config::load, quiet). Empty when it cannot be read.
		std::function<std::vector<res::tag>(const res::tag& desc)> files_of;
		// Builds one desc without a material or the program cache (render_system::check_shader_program): its problems, and the program's
		// name when asked.
		std::function<std::vector<rnd::shader_problem>(const res::tag& desc, std::string* program)> check;
	};

	// Builds the default variant of the shader descs that `source` belongs to (#1386): `source` itself for a `.shader.desc`, else the
	// descs of `descs` whose program is made of it (a stage, or a file a stage includes), found first without building anything; at most
	// k_shader_check_max_programs of those are built.
	shader_check_report check_shader_source(const res::tag& source, const std::vector<res::tag>& descs, const shader_check_ports& ports);
}
