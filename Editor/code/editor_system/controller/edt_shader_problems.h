#pragma once
#include <snk/editor/edt_problem_list.h>
#include <snk/model_codec/res_tag.h>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rnd
{
	class render_system;
	struct shader_problem;
}

namespace edt
{
	// A file of a shader source (asset_type_of says "shader": .glsl .vert .frag .geom .comp .tesc .tese .shader, any case).
	bool is_shader_source(std::string_view path);

	enum class shader_file_open { opened, not_a_shader_source, not_on_disk, failed };

	// Go to on a row of the source "shader": its file goes to `open_file` (the system's app for it) only when both the row's
	// tag and the file on the disk are shader sources. An #include may name any file, and the system's app for a script
	// runs it. `disk_path` turns the tag into the file on the disk (nullopt: none).
	shader_file_open open_shader_problem_file(const problem& row, const std::function<std::optional<std::filesystem::path>(const res::tag&)>& disk_path,
		const std::function<std::string(const std::filesystem::path&)>& open_file);

	// Why the shader programs did not build (a GLSL compile or link error, an include or a binding macro the engine
	// could not expand), at the file and line, as the problems of the source "shader" (#1201). The renderer keeps them per
	// program: a fix that hot reloads and builds drops them. Main thread only.
	class shader_problem_publisher
	{
	public:
		static constexpr const char* source = "shader";

		// The rows of the source: an error each, the file as its res:// tag, the program as its note. Problems that differ only
		// in the program (the variants of one file) are one row with every program in its notes.
		static std::vector<problem> to_problems(const std::vector<rnd::shader_problem>& items);

		// Sets the source when `generation` differs from the last call (`snapshot` is asked only then); clears it when
		// there are none.
		void follow(problem_list& problems, std::uint64_t generation, const std::function<std::vector<rnd::shader_problem>()>& snapshot);
		void follow(problem_list& problems, const rnd::render_system& renderer);

	private:
		// render_system's generation starts at 0 with no problems.
		std::uint64_t m_generation = 0;
		bool m_published = false;
	};
}
