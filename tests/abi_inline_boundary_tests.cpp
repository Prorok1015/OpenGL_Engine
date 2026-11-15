#include <boost/test/unit_test.hpp>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef SNK_REPO_ROOT_DIR
#error "SNK_REPO_ROOT_DIR must point at the repository root"
#endif

// Game modules compile the inline code of the engine headers they include (#922 PR 3): a header whose inline code
// reads an engine object's members bakes the engine's layout into the module. In the headers listed here only the
// member declarations may name an `m_` member; everything else reaches them through exported functions.
namespace {

namespace fs = std::filesystem;

const std::vector<std::string> k_headers = {
	"core/engine/scene/include/snk/scene/scn_scene_layer.h",
	"core/engine/game_system/include/snk/game/gs_input_actions.h",
	"core/core/common/include/snk/common/ds/ds_store.hpp",
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

// One violation per `m_` name on a line that is not a member declaration (`type m_name;`, with an optional `{...}`
// or `= value` initializer).
std::vector<std::string> inline_member_uses(std::string_view header, std::string_view text)
{
	static const std::regex member{ R"(\bm_\w+)" };
	static const std::regex declaration{
		R"(^(?!\s*(?:return|throw|delete|co_return|co_yield|case|goto)\b)\s*[\w:<>,*&\s]*[\w>*&]\s+m_\w+\s*(\{[^}]*\}|=\s*[^;]+)?\s*;\s*$)" };
	std::vector<std::string> out;
	std::istringstream lines{ strip_comments(text) };
	std::string line;
	for (std::size_t number = 1; std::getline(lines, line); ++number) {
		if (!std::regex_search(line, member) || std::regex_match(line, declaration)) continue;
		for (auto it = std::sregex_iterator(line.begin(), line.end(), member); it != std::sregex_iterator(); ++it) {
			out.push_back(std::string{ header } + ":" + std::to_string(number) + ": '" + it->str()
				+ "' is used in inline code; module code must reach it through an exported function (#922)");
		}
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
	BOOST_TEST(inline_member_uses("synthetic.h", "\t\tint m_x = 0;\n\t\tstd::vector<std::shared_ptr<void>> m_kept;\n"
		"\t\tds::lifetime m_tasks{ ds::lifetime_option::cancel_on_destroy };\n\t\tres::resource_system& m_resources;\n"
		"\t\tapp_data_storage* m_parent = nullptr;\n\t\t// return m_x;\n\t\t/* m_y */").empty());
}

BOOST_AUTO_TEST_CASE(EngineHeadersTouchMembersOnlyInDeclarations)
{
	for (const std::string& header : k_headers) {
		const fs::path path = fs::path{ SNK_REPO_ROOT_DIR } / header;
		BOOST_REQUIRE_MESSAGE(fs::is_regular_file(path), path.string() + " is missing");
		for (const std::string& violation : inline_member_uses(header, read_all(path))) {
			BOOST_ERROR(violation);
		}
	}
}

BOOST_AUTO_TEST_SUITE_END()
