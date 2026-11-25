#pragma once
#include <snk/model_codec/res_tag.h>

#include <string>

namespace rnd
{
	// One reason a shader program did not build, at the file and line it is about: an engine preprocessing error or
	// a line of the compiler's or the linker's log mapped back through the #line directives of the source (#1201).
	struct shader_problem
	{
		// The stage or included file; the vertex stage's for a link error.
		res::tag file;
		// 1-based; 0: none.
		int line = 0;
		// 1-based; 0: none.
		int column = 0;
		// "shader/compile", "shader/link", "shader/include", "shader/macro" or "shader/load".
		std::string code;
		std::string message;
		// The stages of the program and its defines, as the log names it.
		std::string program;

		bool operator==(const shader_problem&) const = default;
	};
}
