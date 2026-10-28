#include <boost/test/unit_test.hpp>
#include "edt_build_diagnostics.h"
#include <cstddef>
#include <string>
#include <vector>

// build_output_parser over lines as MSVC, GCC/Clang, the linkers, CMake and ninja print them (#434).

namespace {

using edt::problem_severity;

struct expected
{
	problem_severity severity = problem_severity::error;
	std::string file;
	int line = 0;
	int column = 0;
	std::string code;
	std::string message;
	std::vector<std::string> notes;
};

struct parse_case
{
	const char* name;
	std::vector<std::string> lines;
	std::vector<expected> problems;
};

const std::vector<parse_case>& cases()
{
	static const std::vector<parse_case> all = {
		{ "msvc error",
			{ R"(C:\src\a.cpp(12): error C2065: 'x': undeclared identifier)" },
			{ { problem_severity::error, R"(C:\src\a.cpp)", 12, 0, "C2065", "'x': undeclared identifier", {} } } },
		{ "msvc warning with a column",
			{ "a.cpp(12,5): warning C4100: 'p': unreferenced formal parameter" },
			{ { problem_severity::warning, "a.cpp", 12, 5, "C4100", "'p': unreferenced formal parameter", {} } } },
		{ "msvc fatal error",
			{ R"(C:\src\a.cpp(3): fatal error C1083: Cannot open include file: 'x.h': No such file or directory)" },
			{ { problem_severity::error, R"(C:\src\a.cpp)", 3, 0, "C1083", "Cannot open include file: 'x.h': No such file or directory", {} } } },
		{ "msvc path with parentheses",
			{ R"(C:\Program Files (x86)\inc\b.h(7,1): error C2143: syntax error: missing ';' before '}')" },
			{ { problem_severity::error, R"(C:\Program Files (x86)\inc\b.h)", 7, 1, "C2143", "syntax error: missing ';' before '}'", {} } } },
		{ "msvc note goes to the error before it",
			{ R"(C:\src\a.cpp(12): error C2065: 'x': undeclared identifier)", R"(C:\src\a.h(5): note: see declaration of 'x')" },
			{ { problem_severity::error, R"(C:\src\a.cpp)", 12, 0, "C2065", "'x': undeclared identifier", { R"(C:\src\a.h(5): note: see declaration of 'x')" } } } },
		{ "clang-cl without a code",
			{ "a.cpp(4,9): error: unknown type name 'foo'" },
			{ { problem_severity::error, "a.cpp", 4, 9, "", "unknown type name 'foo'", {} } } },
		{ "msvc unresolved external",
			{ R"(a.obj : error LNK2019: unresolved external symbol "void __cdecl g(void)" (?g@@YAXXZ) referenced in function main)" },
			{ { problem_severity::error, "a.obj", 0, 0, "LNK2019", R"(unresolved external symbol "void __cdecl g(void)" (?g@@YAXXZ) referenced in function main)", {} } } },
		{ "msvc linker without a file",
			{ "LINK : fatal error LNK1104: cannot open file 'x.lib'" },
			{ { problem_severity::error, "", 0, 0, "LNK1104", "cannot open file 'x.lib'", {} } } },
		{ "msvc command line warning",
			{ "cl : command line warning D9002: ignoring unknown option '/foo'" },
			{ { problem_severity::warning, "", 0, 0, "D9002", "ignoring unknown option '/foo'", {} } } },
		{ "gcc error with a column",
			{ "/p/a.cpp:12:5: error: 'x' was not declared in this scope" },
			{ { problem_severity::error, "/p/a.cpp", 12, 5, "", "'x' was not declared in this scope", {} } } },
		{ "gcc error without a column",
			{ "a.cpp:12: error: expected ';' before '}' token" },
			{ { problem_severity::error, "a.cpp", 12, 0, "", "expected ';' before '}' token", {} } } },
		{ "gcc warning",
			{ "/p/a.cpp:3:9: warning: unused variable 'y' [-Wunused-variable]" },
			{ { problem_severity::warning, "/p/a.cpp", 3, 9, "", "unused variable 'y' [-Wunused-variable]", {} } } },
		{ "gcc fatal error",
			{ "/p/a.cpp:1:10: fatal error: missing.h: No such file or directory" },
			{ { problem_severity::error, "/p/a.cpp", 1, 10, "", "missing.h: No such file or directory", {} } } },
		{ "clang error and its note",
			{ "/p/b.cpp:7:3: error: no matching function for call to 'f'", "/p/b.h:2:6: note: candidate function not viable" },
			{ { problem_severity::error, "/p/b.cpp", 7, 3, "", "no matching function for call to 'f'", { "/p/b.h:2:6: note: candidate function not viable" } } } },
		{ "gcc style windows paths",
			{ "C:/p/a.cpp:12:5: error: boom", R"(C:\p\b.cpp:3:1: warning: bang)" },
			{ { problem_severity::error, "C:/p/a.cpp", 12, 5, "", "boom", {} },
				{ problem_severity::warning, R"(C:\p\b.cpp)", 3, 1, "", "bang", {} } } },
		{ "gcc template context goes to the next error",
			{ "/p/a.cpp: In instantiation of 'void f(T) [with T = int]':", "/p/a.cpp:9:6:   required from here",
				"/p/a.cpp:3:5: error: 'g' was not declared in this scope" },
			{ { problem_severity::error, "/p/a.cpp", 3, 5, "", "'g' was not declared in this scope",
				{ "/p/a.cpp: In instantiation of 'void f(T) [with T = int]':", "/p/a.cpp:9:6:   required from here" } } } },
		{ "ld undefined reference",
			{ "/usr/bin/ld: a.o: in function `f()':", "/usr/bin/ld: a.cpp:(.text+0x1a): undefined reference to `g()'" },
			{ { problem_severity::error, "a.cpp", 0, 0, "", "undefined reference to `g()'", {} } } },
		{ "lld undefined symbol and its references",
			{ "ld.lld: error: undefined symbol: g()", ">>> referenced by a.cpp:12" },
			{ { problem_severity::error, "", 0, 0, "", "undefined symbol: g()", { ">>> referenced by a.cpp:12" } } } },
		{ "collect2",
			{ "collect2: error: ld returned 1 exit status" },
			{ { problem_severity::error, "", 0, 0, "", "ld returned 1 exit status", {} } } },
		{ "clang driver",
			{ "clang: error: linker command failed with exit code 1 (use -v to see invocation)" },
			{ { problem_severity::error, "", 0, 0, "", "linker command failed with exit code 1 (use -v to see invocation)", {} } } },
		{ "cmake error with an indented message",
			{ "CMake Error at CMakeLists.txt:12 (find_package):",
				"  By not providing \"FindFoo.cmake\" in CMAKE_MODULE_PATH this project has",
				"  asked CMake to find a package configuration file provided by \"Foo\".",
				"",
				"  Could not find a package configuration file provided by \"Foo\"" },
			{ { problem_severity::error, "CMakeLists.txt", 12, 0, "",
				"By not providing \"FindFoo.cmake\" in CMAKE_MODULE_PATH this project has asked CMake to find a package configuration file provided by \"Foo\".", {} } } },
		{ "cmake error ended by a non-indented line",
			{ "CMake Error at C:/p/CMakeLists.txt:3 (include):", "  include could not find requested file:", "", "    foo.cmake",
				"Call Stack (most recent call first):", "  CMakeLists.txt:1 (project)" },
			{ { problem_severity::error, "C:/p/CMakeLists.txt", 3, 0, "", "include could not find requested file:", {} } } },
		{ "cmake warning",
			{ "CMake Warning (dev) at src/CMakeLists.txt:5 (add_library):", "  Policy CMP0000 is not set.", "This warning is for project developers." },
			{ { problem_severity::warning, "src/CMakeLists.txt", 5, 0, "", "Policy CMP0000 is not set.", {} } } },
		{ "cmake error without a location",
			{ "CMake Error: The source directory \"/x\" does not exist." },
			{ { problem_severity::error, "", 0, 0, "", "The source directory \"/x\" does not exist.", {} } } },
		{ "ninja and progress lines are not problems",
			{ "[3/10] Building CXX object Editor/CMakeFiles/editor.dir/a.cpp.o", "FAILED: Editor/CMakeFiles/editor.dir/a.cpp.o",
				"/usr/bin/c++ -I/p/include -c /p/a.cpp -o a.cpp.o", "ninja: build stopped: subcommand failed.",
				"-- Configuring done", "In file included from /p/a.h:3,", "                 from /p/a.cpp:1:",
				"/p/a.cpp: In function 'int main()':", "   12 |     x = 1;", "      |     ^" },
			{} },
		{ "a note with no problem before it is dropped",
			{ "/p/a.h:2:6: note: declared here", "/p/a.cpp:4:1: error: boom" },
			{ { problem_severity::error, "/p/a.cpp", 4, 1, "", "boom", {} } } },
		{ "carriage returns are stripped",
			{ "/p/a.cpp:1:2: error: boom\r", "a.cpp(1): error C2059: syntax error: ')'\r" },
			{ { problem_severity::error, "/p/a.cpp", 1, 2, "", "boom", {} },
				{ problem_severity::error, "a.cpp", 1, 0, "C2059", "syntax error: ')'", {} } } },
	};
	return all;
}

}

BOOST_AUTO_TEST_SUITE(EdtBuildDiagnosticsTests)

BOOST_AUTO_TEST_CASE(EveryFormat_IsParsedAsExpected)
{
	for (const parse_case& test : cases()) {
		BOOST_TEST_CONTEXT(test.name) {
			edt::build_output_parser parser;
			for (const std::string& line : test.lines) parser.feed(line);
			const std::vector<edt::problem> got = parser.take();
			BOOST_TEST_REQUIRE(got.size() == test.problems.size());
			for (std::size_t i = 0; i < got.size(); ++i) {
				const edt::problem& actual = got[i];
				const expected& want = test.problems[i];
				BOOST_TEST_CONTEXT("problem " << i) {
					BOOST_TEST((actual.severity == want.severity));
					BOOST_TEST(actual.file == want.file);
					BOOST_TEST(actual.line == want.line);
					BOOST_TEST(actual.column == want.column);
					BOOST_TEST(actual.code == want.code);
					BOOST_TEST(actual.message == want.message);
					BOOST_TEST(actual.notes == want.notes, boost::test_tools::per_element());
				}
			}
		}
	}
}

BOOST_AUTO_TEST_CASE(Take_EmptiesTheParser_AndANoteAfterItIsDropped)
{
	edt::build_output_parser parser;
	parser.feed("/p/a.cpp:1:2: error: boom");
	BOOST_TEST(parser.take().size() == 1u);
	parser.feed("/p/a.h:1:1: note: declared here");
	BOOST_TEST(parser.take().empty());
}

BOOST_AUTO_TEST_CASE(ProblemsKeepTheOrderOfTheOutput)
{
	edt::build_output_parser parser;
	parser.feed("/p/a.cpp:1:1: warning: first");
	parser.feed("LINK : fatal error LNK1104: cannot open file 'x.lib'");
	parser.feed("CMake Error: third");
	const auto got = parser.take();
	BOOST_TEST_REQUIRE(got.size() == 3u);
	BOOST_TEST(got[0].message == "first");
	BOOST_TEST(got[1].code == "LNK1104");
	BOOST_TEST(got[2].message == "third");
}

BOOST_AUTO_TEST_SUITE_END()
