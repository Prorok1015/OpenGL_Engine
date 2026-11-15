#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <iterator>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef SNK_REPO_ROOT_DIR
#error "SNK_REPO_ROOT_DIR must point at the repository root"
#endif

// Game modules compile the inline code of the engine headers they include (#922 PR 3): a header whose inline code
// reads an engine object's members bakes the engine's layout into the module. In the headers listed here only the
// member declarations may name an `m_` member; everything else reaches them through exported functions.
namespace {

namespace fs = std::filesystem;

// Each header with the exported class whose special members it must not define inline.
const std::vector<std::pair<std::string, std::string>> k_headers = {
	{ "core/engine/scene/include/snk/scene/scn_scene_layer.h", "scene_layer" },
	{ "core/engine/game_system/include/snk/game/gs_input_actions.h", "input_actions" },
	{ "core/core/common/include/snk/common/ds/ds_store.hpp", "app_data_storage" },
};

std::string read_all(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	std::ostringstream text;
	text << in.rdbuf();
	// A Windows checkout has CRLF line ends: the scan reads LF text.
	std::string out = text.str();
	std::erase(out, '\r');
	return out;
}

// Drops `//` and `/* */` comments, keeping string and char literals and every line break (line numbers stay).
std::string strip_comments(std::string_view text)
{
	std::string out;
	out.reserve(text.size());
	std::size_t i = 0;
	while (i < text.size()) {
		const char c = text[i];
		const char next = i + 1 < text.size() ? text[i + 1] : '\0';
		if (c == '/' && next == '/') {
			while (i < text.size() && text[i] != '\n') ++i;
		} else if (c == '/' && next == '*') {
			i += 2;
			while (i < text.size() && !(text[i] == '*' && i + 1 < text.size() && text[i + 1] == '/')) {
				if (text[i] == '\n') out += '\n';
				++i;
			}
			i = i < text.size() ? i + 2 : i;
		} else if (c == '"' || c == '\'') {
			out += c;
			++i;
			while (i < text.size() && text[i] != c && text[i] != '\n') {
				if (text[i] == '\\' && i + 1 < text.size()) out += text[i++];
				out += text[i++];
			}
			if (i < text.size() && text[i] == c) out += text[i++];
		} else {
			out += c;
			++i;
		}
	}
	return out;
}

std::size_t line_of(std::string_view text, std::size_t offset)
{
	return 1 + static_cast<std::size_t>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(offset), '\n'));
}

std::string at(std::string_view header, std::string_view text, std::size_t offset)
{
	return std::string{ header } + ":" + std::to_string(line_of(text, offset)) + ": ";
}

// One violation per `m_` name on a line that is not a member declaration (`type m_name;`, with an optional `{...}`
// or `= value` initializer that names no other member).
std::vector<std::string> inline_member_uses(std::string_view header, std::string_view text)
{
	static const std::regex member{ R"(\bm_\w+)" };
	static const std::regex declaration{
		R"(^(?!\s*(?:return|throw|delete|co_return|co_yield|co_await|case|goto|else|not|sizeof|typename|and|or)\b)\s*[\w:](?:[\w:<>,*&\s]*[\w>*&])?\s+m_\w+\s*(\{[^}]*\}|=\s*[^;]+)?\s*;\s*$)" };
	std::vector<std::string> out;
	std::istringstream lines{ strip_comments(text) };
	std::string line;
	for (std::size_t number = 1; std::getline(lines, line); ++number) {
		const auto first = std::sregex_iterator(line.begin(), line.end(), member);
		const auto count = std::distance(first, std::sregex_iterator());
		if (count == 0 || (count == 1 && std::regex_match(line, declaration))) continue;
		for (auto it = first; it != std::sregex_iterator(); ++it) {
			out.push_back(std::string{ header } + ":" + std::to_string(number) + ": '" + it->str()
				+ "' is used in inline code; module code must reach it through an exported function (#922)");
		}
	}
	return out;
}

// The body of `class name {...}` in comment-free text, with its offset; npos when the class is not defined there.
std::pair<std::size_t, std::string> class_body(const std::string& text, const std::string& name)
{
	const std::regex head{ R"(\b(?:class|struct)\s+(?:SNK_API\s+)?)" + name + R"(\s*(?:final\s*)?(?::[^{;]*)?\{)" };
	std::smatch match;
	if (!std::regex_search(text, match, head)) return { std::string::npos, {} };
	const std::size_t begin = static_cast<std::size_t>(match.position(0) + match.length(0));
	int depth = 1;
	for (std::size_t i = begin; i < text.size(); ++i) {
		if (text[i] == '{') ++depth;
		if (text[i] == '}' && --depth == 0) return { begin, text.substr(begin, i - begin) };
	}
	return { std::string::npos, {} };
}

// Where a member's declaration ends after the `(` at `open`: the offset of the `{` or `:` that starts an inline body
// or an initializer list, or npos when a `;` ends it first. Parentheses nest and `::` is not an initializer list.
std::size_t inline_body_start(const std::string& body, std::size_t open)
{
	int depth = 1;
	std::size_t i = open;
	for (; i < body.size() && depth > 0; ++i) {
		if (body[i] == '(') ++depth;
		if (body[i] == ')') --depth;
	}
	for (; i < body.size(); ++i) {
		if (body[i] == ';') return std::string::npos;
		if (body[i] == '{') return i;
		if (body[i] == ':') {
			if (i + 1 < body.size() && body[i + 1] == ':') {
				++i;
				continue;
			}
			return i;
		}
	}
	return std::string::npos;
}

// Code the compiler would write inline over the members: no `= default` in the class, a declared destructor, every
// constructor, the destructor and every `operator=` declared without a body (`= delete` is fine), and no definition of
// a constructor or the destructor after the class in the header.
std::vector<std::string> inline_special_members(std::string_view header, std::string_view source, const std::string& name)
{
	const std::string text = strip_comments(source);
	const auto [offset, body] = class_body(text, name);
	if (offset == std::string::npos) return { std::string{ header } + ": class " + name + " is not defined here" };
	std::vector<std::string> out;
	static const std::regex defaulted{ R"(=\s*default\b)" };
	for (auto it = std::sregex_iterator(body.begin(), body.end(), defaulted); it != std::sregex_iterator(); ++it) {
		out.push_back(at(header, text, offset + static_cast<std::size_t>(it->position(0))) + name
			+ ": '= default' in the class is inline code over the members; define it in the .cpp (#922)");
	}
	// Group 1 is the member's name; the character before it keeps out `other.name(`, `p->name(` and `ns::name(`.
	const std::regex special{ R"((?:^|[^\w:.>~]))" "(~?" + name + R"(|operator\s*=)\s*\()" };
	bool has_destructor = false;
	for (auto it = std::sregex_iterator(body.begin(), body.end(), special); it != std::sregex_iterator(); ++it) {
		const std::string member = (*it)[1].str();
		has_destructor = has_destructor || member.front() == '~';
		const std::size_t open = static_cast<std::size_t>(it->position(0) + it->length(0));
		if (inline_body_start(body, open) != std::string::npos) {
			out.push_back(at(header, text, offset + static_cast<std::size_t>(it->position(1))) + name + ": '" + member
				+ "(' has an inline body; declare it and define it in the .cpp (#922)");
		}
	}
	if (!has_destructor) {
		out.push_back(std::string{ header } + ": " + name
			+ " declares no destructor, so the compiler writes an inline one; declare it and define it in the .cpp (#922)");
	}
	const std::regex outside{ R"(\b)" + name + R"(\s*::\s*(?:~\s*)?)" + name + R"(\s*\()" };
	for (auto it = std::sregex_iterator(text.begin(), text.end(), outside); it != std::sregex_iterator(); ++it) {
		out.push_back(at(header, text, static_cast<std::size_t>(it->position(0))) + "'" + it->str()
			+ "' is defined in the header; define it in the .cpp (#922)");
	}
	return out;
}

// sizeof/alignof/offsetof of the class, of `*this` or of the store's entry in a header bakes a layout into module code.
std::vector<std::string> layout_queries(std::string_view header, std::string_view source)
{
	static const std::regex query{ R"(\b(?:sizeof|alignof|offsetof)\s*\(?\s*(?:(?:const|volatile)\s+)*(?:decltype\s*\(\s*)?)"
		R"((?:\*\s*this\b|(?:[\w:]*::)?(?:scene_layer|input_actions|app_data_storage|entry)\b))" };
	const std::string text = strip_comments(source);
	std::vector<std::string> out;
	for (auto it = std::sregex_iterator(text.begin(), text.end(), query); it != std::sregex_iterator(); ++it) {
		out.push_back(at(header, text, static_cast<std::size_t>(it->position(0))) + "'" + it->str()
			+ "' bakes the class layout into module code (#922)");
	}
	return out;
}

}

BOOST_AUTO_TEST_SUITE(AbiInlineBoundaryTests)

BOOST_AUTO_TEST_CASE(TheCheckerFlagsInlineMemberUseAndPassesDeclarations)
{
	BOOST_TEST(inline_member_uses("synthetic.h", "int f() { return m_x; }").size() == 1u);
	BOOST_TEST(inline_member_uses("synthetic.h", "\t\treturn m_x;").size() == 1u);
	BOOST_TEST(inline_member_uses("synthetic.h", "\t\tm_list.push_back(std::move(held));").size() == 1u);
	BOOST_TEST(inline_member_uses("synthetic.h", "\t&& m_ok;").size() == 1u);
	BOOST_TEST(inline_member_uses("synthetic.h", "\t\telse m_x;").size() == 1u);
	BOOST_TEST(inline_member_uses("synthetic.h", "\t\tco_await m_x;").size() == 1u);
	BOOST_TEST(inline_member_uses("synthetic.h", "\t\tnot m_x;").size() == 1u);
	BOOST_TEST(inline_member_uses("synthetic.h", "\t\tstd::size_t m_n = m_entries.size();").size() == 2u);
	BOOST_TEST(inline_member_uses("synthetic.h", "\t\tint m_x = 0;\n\t\tstd::vector<std::shared_ptr<void>> m_kept;\n"
		"\t\tds::lifetime m_tasks{ ds::lifetime_option::cancel_on_destroy };\n\t\tres::resource_system& m_resources;\n"
		"\t\tapp_data_storage* m_parent = nullptr;\n\t\tT m_t;\n\t\t// return m_x;\n\t\t/* m_y */").empty());
}

BOOST_AUTO_TEST_CASE(TheCheckerFlagsInlineSpecialMembersAndLayoutQueries)
{
	BOOST_TEST(inline_special_members("synthetic.h", "class X { X() = default; ~X(); };", "X").size() == 1u);
	BOOST_TEST(inline_special_members("synthetic.h", "class X { int f(); };", "X").size() == 1u);
	BOOST_TEST(inline_special_members("synthetic.h", "class X { ~X() {} };", "X").size() == 1u);
	BOOST_TEST(inline_special_members("synthetic.h", "class X { ~X(); X(X&& other) {} };", "X").size() == 1u);
	BOOST_TEST(inline_special_members("synthetic.h", "class X { ~X(); X& operator=(const X&) { return *this; } };", "X").size() == 1u);
	BOOST_TEST(inline_special_members("synthetic.h", "class X { ~X(); X() {} };", "X").size() == 1u);
	BOOST_TEST(inline_special_members("synthetic.h", "class X { ~X(); explicit X(X*) {} };", "X").size() == 1u);
	BOOST_TEST(inline_special_members("synthetic.h", "class X { ~X(); X(int (*f)(int)) noexcept(a::b) : n{ 0 } {} };", "X").size() == 1u);
	BOOST_TEST(inline_special_members("synthetic.h", "class X { ~X(); X(X const&) {} };", "X").size() == 1u);
	BOOST_TEST(inline_special_members("synthetic.h", "class X { ~X(); X& operator=(X) { return *this; } };", "X").size() == 1u);
	BOOST_TEST(inline_special_members("synthetic.h", "class X { ~X(); X& operator =(const ds::X&) { return *this; } };", "X").size() == 1u);
	BOOST_TEST(inline_special_members("synthetic.h", "class X { ~X(); };\ninline X::~X() = default;", "X").size() == 1u);
	BOOST_TEST(inline_special_members("synthetic.h", "class X { ~X(); X(X&&); };\ninline X::X(X&&) {}", "X").size() == 1u);
	BOOST_TEST(inline_special_members("synthetic.h", "class SNK_API X\n{\n\t~X();\n\tX(const X&) = delete;\n\tX& operator=(X&&);\n"
		"\texplicit X(ds::Y* parent);\n\tvoid f(const ds::X& other) { other.X(); }\n\tvoid g(X* p);\n};\n"
		"void h() { throw \"ds::X::f\"; }", "X").empty());
	BOOST_TEST(layout_queries("synthetic.h", "static_assert(sizeof(ds::app_data_storage) > 0);").size() == 1u);
	BOOST_TEST(layout_queries("synthetic.h", "constexpr auto a = alignof( scene_layer );").size() == 1u);
	BOOST_TEST(layout_queries("synthetic.h", "constexpr auto a = alignof(const scene_layer);").size() == 1u);
	BOOST_TEST(layout_queries("synthetic.h", "auto n = sizeof(*this);").size() == 1u);
	BOOST_TEST(layout_queries("synthetic.h", "auto n = sizeof(decltype(*this));").size() == 1u);
	BOOST_TEST(layout_queries("synthetic.h", "auto n = sizeof(entry);").size() == 1u);
	BOOST_TEST(layout_queries("synthetic.h", "constexpr auto s = sizeof(int); // sizeof(input_actions)").empty());
}

BOOST_AUTO_TEST_CASE(EngineHeadersTouchMembersOnlyInDeclarations)
{
	for (const auto& [header, name] : k_headers) {
		const fs::path path = fs::path{ SNK_REPO_ROOT_DIR } / header;
		BOOST_REQUIRE_MESSAGE(fs::is_regular_file(path), path.string() + " is missing");
		const std::string text = read_all(path);
		for (const std::string& violation : inline_member_uses(header, text)) BOOST_ERROR(violation);
		for (const std::string& violation : inline_special_members(header, text, name)) BOOST_ERROR(violation);
		for (const std::string& violation : layout_queries(header, text)) BOOST_ERROR(violation);
	}
}

BOOST_AUTO_TEST_SUITE_END()
