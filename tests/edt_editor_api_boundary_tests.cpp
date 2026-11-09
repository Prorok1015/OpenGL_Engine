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
	return text.str();
}

// Classes that are whole in their header (no member is defined in a source file), so there is nothing to export.
const std::set<std::string> k_header_only = { "connection", "history_change", "editor_events" };

// The registries and the context: what a module registers into. No template across them.
const std::vector<std::string> k_template_free = {
	"edt_panel_registry.h", "edt_action_registry.h", "edt_inspector_registry.h", "edt_asset_kind_registry.h", "edt_editor_context.h", "editor_module.h",
};

}

BOOST_AUTO_TEST_SUITE(EdtEditorApiBoundaryTests)

BOOST_AUTO_TEST_CASE(TheApiHeadersAreFound)
{
	BOOST_REQUIRE(fs::is_directory(api_dir()));
	BOOST_TEST(api_headers().size() >= 18u);
}

BOOST_AUTO_TEST_CASE(ApiHeadersIncludeOnlyEachOtherAndTheEngineNeverAnInternalEditorHeaderOrTheMcpServer)
{
	const std::regex quoted(R"(^\s*#\s*include\s+"([^"]+)")");
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
	const std::regex declaration(R"(^\t(class|struct) (SNK_EDITOR_API )?(\w+)\b([^;]*)$)");
	for (const fs::path& header : api_headers()) {
		std::istringstream lines(read_all(header));
		std::string line;
		std::string previous;
		while (std::getline(lines, line)) {
			std::smatch match;
			const bool templated = previous.find("template") != std::string::npos;
			if (!templated && std::regex_match(line, match, declaration) && match[1] == "class") {
				const std::string name = match[3];
				if (!k_header_only.contains(name)) {
					BOOST_CHECK_MESSAGE(!match[2].str().empty(), header.filename().string() << ": class " << name << " has no SNK_EDITOR_API");
				}
			}
			previous = line;
		}
	}
}

BOOST_AUTO_TEST_CASE(TheExportMacroIsEmptyForAStaticBuild)
{
	const std::string text = read_all(api_dir() / "edt_api.h");
	BOOST_TEST(text.find("#define SNK_EDITOR_API\n") != std::string::npos);
	BOOST_TEST(text.find("SNK_EDITOR_SHARED") != std::string::npos);
	BOOST_TEST(text.find("SNK_EDITOR_BUILD") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
