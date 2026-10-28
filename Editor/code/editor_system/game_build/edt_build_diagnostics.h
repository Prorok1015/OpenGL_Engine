#pragma once
#include <snk/editor/edt_problem_list.h>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace edt
{
	// Turns the output of a build, fed line by line, into problems. Recognizes MSVC (cl, link), GCC/Clang, GNU ld/lld,
	// CMake, and skips ninja's own lines. Not thread-safe.
	//
	// A "note:" (and an lld ">>>" line) goes to the problem before it; one with no problem before it is dropped. GCC
	// prints the template context ("In instantiation of", "required from") before the error it explains, so those
	// lines are kept and go to the next error or warning.
	class build_output_parser
	{
	public:
		void feed(std::string_view line);
		// Everything recognized so far, in order. A note after take() has no problem to go to and is dropped.
		[[nodiscard]] std::vector<problem> take();

	private:
		void add(problem item);
		void add_note(std::string_view line);
		bool continue_cmake(std::string_view line);
		bool parse_cmake(std::string_view line);

		std::vector<problem> m_items;
		// GCC context lines waiting for the error they belong to.
		std::vector<std::string> m_context;
		// A CMake message whose indented text is still coming (index in m_items).
		std::optional<std::size_t> m_cmake_open;
	};
}
