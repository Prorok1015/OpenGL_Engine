#pragma once
#include <snk/editor/edt_problem_list.h>
#include <cstdint>
#include <functional>
#include <vector>

namespace rnd
{
	class render_system;
	struct shader_problem;
}

namespace edt
{
	// Why the shader programs did not build (a GLSL compile or link error, an include or a binding macro the engine
	// could not expand), at the file and line, as the problems of the source "shader" (#1201). The renderer keeps them per
	// program: a fix that hot reloads and builds drops them. Main thread only.
	class shader_problem_publisher
	{
	public:
		static constexpr const char* source = "shader";

		// The rows of the source: an error each, the file as its res:// tag, the program as its note.
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
