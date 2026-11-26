#pragma once
#include <snk/model_codec/res_tag.h>

#include <string>
#include <string_view>

namespace rnd
{
	// One reason a shader program did not build, at the file and line it is about: an engine preprocessing error or
	// a line of the compiler's or the linker's log mapped back through the #line directives of the source (#1201).
	// A material of the transparent or mix queue whose shader desc has no transparent path (#1440).
	inline constexpr std::string_view k_no_transparent_path_code = "shader/no_transparent_path";

	struct shader_problem
	{
		// The stage or included file; the vertex stage's for a link error.
		res::tag file;
		// 1-based; 0: none.
		int line = 0;
		// 1-based; 0: none.
		int column = 0;
		// "shader/compile", "shader/link", "shader/include", "shader/macro", "shader/load" or k_no_transparent_path_code.
		// The last two are a material's: `program` is the material's tag, `file` its shader desc.
		std::string code;
		std::string message;
		// The stages of the program and its defines, as the log names it.
		std::string program;

		bool operator==(const shader_problem&) const = default;
	};
}
