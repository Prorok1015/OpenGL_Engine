#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef SNK_EDITOR_DATA_DIR
#error "SNK_EDITOR_DATA_DIR must point at Editor/data"
#endif

// The editor API a game module (game_editor) builds against (#587): the public headers of Editor/code/editor_system/
// include/snk/editor. This keeps their shape: no private header behind them, no template in the registries, every class
// that has code in the editor carries the export macro. Text scans of the sources, as the layout tests do.
namespace {

namespace fs = std::filesystem;

fs::path api_dir()
{
	return fs::path{ SNK_EDITOR_DATA_DIR }.parent_path() / "code" / "editor_system" / "include" / "snk" / "editor";
}

std::vector<fs::path> api_headers()
{
	std::vector<fs::path> out;
	for (const auto& entry : fs::directory_iterator(api_dir())) {
		if (entry.path().extension() == ".h") out.push_back(entry.path());
	}
	std::ranges::sort(out);
	return out;
}

std::string read_all(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	std::ostringstream text;
	text << in.rdbuf();
	// A Windows checkout has CRLF line ends: the scans read LF text.
	std::string out = text.str();
	std::erase(out, '\r');
	return out;
}

// The headers of the API: a new one is added here on purpose (and one that is only plumbing stays out of the directory).
const std::set<std::string> k_headers = {
	"edt_action_registry.h", "edt_api.h", "edt_asset_kind_registry.h", "edt_dialog_service.h", "edt_document.h", "edt_document_history.h",
	"edt_document_manager.h", "edt_editor_context.h", "edt_editor_events.h", "edt_editor_services.h", "edt_inspector_registry.h",
	"edt_job_service.h", "edt_panel_base.h", "edt_panel_registry.h", "edt_problem_list.h", "edt_registration.h", "edt_selection_service.h", "editor_module.h",
};

// Classes and structs that are whole in their header (no member is defined in a source file), so there is nothing to
// export; every other class, and the structs listed in k_structs_with_code, carries the macro. editor_module is the
// host's own module (only Editor/main.cpp creates it) and stays in the editor, so it is not exported either.
const std::set<std::string> k_header_only = { "connection", "history_change", "editor_events", "editor_module" };
const std::set<std::string> k_structs_with_code = { "shortcut", "editor_services" };

// The registries and the context: what a module registers into. No template across them.
const std::vector<std::string> k_template_free = {
	"edt_panel_registry.h", "edt_action_registry.h", "edt_inspector_registry.h", "edt_asset_kind_registry.h", "edt_editor_context.h", "editor_module.h",
};

}

BOOST_AUTO_TEST_SUITE(EdtEditorApiBoundaryTests)

BOOST_AUTO_TEST_CASE(TheApiHeadersAreExactlyTheListedOnes)
{
	BOOST_REQUIRE(fs::is_directory(api_dir()));
	std::set<std::string> found;
	for (const fs::path& header : api_headers()) found.insert(header.filename().string());
	BOOST_TEST((found == k_headers));
}

BOOST_AUTO_TEST_CASE(ApiHeadersIncludeOnlyEachOtherAndTheEngineNeverAnInternalEditorHeaderOrTheMcpServer)
{
	const std::regex quoted(R"re(^\s*#\s*include\s+"([^"]+)")re");
	const std::regex angled(R"(^\s*#\s*include\s+<(snk/[^>]+)>)");
	const std::set<std::string> forbidden_prefixes = { "snk/mcp/" };
	for (const fs::path& header : api_headers()) {
		std::istringstream lines(read_all(header));
		std::string line;
		while (std::getline(lines, line)) {
			std::smatch match;
			if (std::regex_search(line, match, quoted)) {
				BOOST_CHECK_MESSAGE(fs::exists(api_dir() / match[1].str()), header.filename().string() << " includes \"" << match[1].str() << "\", which is not an API header");
			} else if (std::regex_search(line, match, angled)) {
				for (const std::string& prefix : forbidden_prefixes) {
					BOOST_CHECK_MESSAGE(!match[1].str().starts_with(prefix), header.filename().string() << " includes <" << match[1].str() << ">");
				}
			}
		}
	}
}

BOOST_AUTO_TEST_CASE(TheRegistriesAndTheContextHaveNoTemplateAtTheirBoundary)
{
	for (const std::string& name : k_template_free) {
		const std::string text = read_all(api_dir() / name);
		BOOST_REQUIRE_MESSAGE(!text.empty(), name);
		BOOST_CHECK_MESSAGE(text.find("template<") == std::string::npos, name << " declares a template");
		BOOST_CHECK_MESSAGE(text.find("template <") == std::string::npos, name << " declares a template");
	}
}

BOOST_AUTO_TEST_CASE(EveryClassWithCodeInTheEditorCarriesTheExportMacro)
{
	// A declaration with a body at namespace level, whatever its indent, `alignas` or attribute.
	const std::regex declaration(R"(^\s*(class|struct)\s+(?:alignas\([^)]*\)\s+|\[\[[^\]]*\]\]\s+)*(SNK_EDITOR_API\s+)?(\w+)\b([^;]*)$)");
	std::set<std::string> seen_structs;
	for (const fs::path& header : api_headers()) {
		std::istringstream lines(read_all(header));
		std::string line;
		std::string previous;
		int depth = 0;
		while (std::getline(lines, line)) {
			const std::string code = line.substr(0, line.find("//"));
			std::smatch match;
			const bool templated = previous.find("template") != std::string::npos;
			if (depth == 1 && !templated && std::regex_match(code, match, declaration)) {
				const std::string kind = match[1];
				const std::string name = match[3];
				const bool needs = kind == "class" ? !k_header_only.contains(name) : k_structs_with_code.contains(name);
				if (kind == "struct") seen_structs.insert(name);
				if (needs) {
					BOOST_CHECK_MESSAGE(!match[2].str().empty(), header.filename().string() << ": " << kind << " " << name << " has no SNK_EDITOR_API");
				}
			}
			depth += static_cast<int>(std::ranges::count(code, '{')) - static_cast<int>(std::ranges::count(code, '}'));
			if (!code.empty() && code.find_first_not_of(" \t\r") != std::string::npos) previous = code;
		}
	}
	for (const std::string& name : k_structs_with_code) BOOST_CHECK_MESSAGE(seen_structs.contains(name), "struct " << name << " is not in the API headers any more");
}

BOOST_AUTO_TEST_CASE(TheExportMacroIsEmptyForAStaticBuild)
{
	const std::string text = read_all(api_dir() / "edt_api.h");
	// The last definition is the empty one (a Windows checkout has CRLF line ends).
	const std::string empty_define = "#define SNK_EDITOR_API";
	const std::size_t at = text.rfind(empty_define);
	BOOST_REQUIRE(at != std::string::npos);
	const std::size_t end = at + empty_define.size();
	BOOST_TEST((end >= text.size() || text[end] == '\r' || text[end] == '\n'));
	BOOST_TEST(text.find("SNK_EDITOR_SHARED") != std::string::npos);
	BOOST_TEST(text.find("SNK_EDITOR_BUILD") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
