#include <boost/test/unit_test.hpp>
#include <boost/json.hpp>
#include "test_temp_dir.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// The layout of the shipped resources (#163): res:// holds game resources only (no stray test files; the fixtures that
// stay are listed one by one), and every res:// / engine:// / edt:// path that shipped descs and engine sources
// mention resolves to a file.

#ifndef SNK_EDITOR_RES_DIR
#error "SNK_EDITOR_RES_DIR must point at Editor/res"
#endif
#ifndef SNK_CORE_RES_DIR
#error "SNK_CORE_RES_DIR must point at core/res"
#endif

namespace fs = std::filesystem;
namespace json = boost::json;

namespace {

// Where each protocol finds its files; a protocol without roots (memory://) is not checked.
struct resource_layout
{
	std::vector<fs::path> res;
	std::vector<fs::path> engine;
	std::vector<fs::path> edt;

	// nullptr: the protocol holds no files to check. Unknown protocols are reported by the callers.
	const std::vector<fs::path>* roots_of(std::string_view protocol) const
	{
		if (protocol == "res") return &res;
		if (protocol == "engine") return &engine;
		if (protocol == "edt") return &edt;
		return nullptr;
	}
};

const fs::path core_res{ SNK_CORE_RES_DIR };
const fs::path editor_res{ SNK_EDITOR_RES_DIR };
const fs::path editor_data = editor_res.parent_path() / "data";
const fs::path repo_root = core_res.parent_path().parent_path();

resource_layout shipped_layout()
{
	return { { core_res, editor_res }, { core_res }, { editor_data } };
}

// What stays in the game resources although its name looks like a test: every entry is a fixture some test or the CI
// smoke run loads through res://. A new one has to be added here, i.e. justified in review.
const std::set<std::string> allowed_fixtures = {
	"levels/test_level.desc",
	"levels/smoke_level.desc",
	"levels/smoke_two_worlds.desc",
	"levels/bench_100_meshes.desc",
	"bench/bench_quad.desc",
};

std::string lower(std::string_view text)
{
	std::string out(text);
	for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return out;
}

// test*, bench*, smoke*, *_test*, and the picture and bitmap leftovers the owner removed in #176.
bool looks_like_a_test_file(const std::string& segment)
{
	const std::string name = lower(segment);
	if (name.starts_with("test") || name.starts_with("bench") || name.starts_with("smoke") || name.find("_test") != std::string::npos) {
		return true;
	}
	return name == "block.png" || name == "milla.bmp" || name == "window.png";
}

// Files below `root` (the samples submodule aside) with a test-like name in the path that are not allowed fixtures.
std::vector<std::string> stray_test_files(const fs::path& root, const std::set<std::string>& allowed)
{
	std::vector<std::string> found;
	for (const auto& entry : fs::recursive_directory_iterator(root)) {
		if (!entry.is_regular_file()) continue;
		const fs::path rel = entry.path().lexically_relative(root);
		if (*rel.begin() == "samples") continue;
		const bool test_like = std::ranges::any_of(rel, [](const fs::path& part) { return looks_like_a_test_file(part.string()); });
		if (test_like && !allowed.contains(rel.generic_string())) {
			found.push_back(rel.generic_string());
		}
	}
	std::ranges::sort(found);
	return found;
}

std::string joined(const std::vector<std::string>& lines)
{
	std::string out;
	for (const std::string& line : lines) out += "\n  " + line;
	return out;
}

bool is_below(const fs::path& path, const fs::path& dir)
{
	const fs::path rel = path.lexically_relative(dir);
	return !rel.empty() && *rel.begin() != "..";
}

bool has_file(const std::vector<fs::path>& roots, std::string_view rel)
{
	return std::ranges::any_of(roots, [&](const fs::path& root) { return fs::is_regular_file(root / rel); });
}

// "protocol://path" -> problem text, or empty when the reference resolves (or is not a file reference).
std::string reference_problem(const resource_layout& layout, const std::string& reference)
{
	const std::size_t split = reference.find("://");
	const std::string_view protocol = std::string_view{ reference }.substr(0, split);
	const std::string_view rel = std::string_view{ reference }.substr(split + 3);
	if (protocol == "memory") return {};
	const auto* roots = layout.roots_of(protocol);
	if (roots == nullptr) return "unknown protocol";
	return has_file(*roots, rel) ? std::string{} : std::string{ "no such file" };
}

void collect_references(const json::value& value, std::vector<std::string>& out)
{
	if (value.is_string()) {
		const std::string_view text = value.as_string();
		if (text.find("://") != std::string_view::npos) out.emplace_back(text);
	} else if (value.is_object()) {
		for (const auto& member : value.as_object()) collect_references(member.value(), out);
	} else if (value.is_array()) {
		for (const auto& element : value.as_array()) collect_references(element, out);
	}
}

json::value read_json(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	const std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	return json::parse(text);
}

// Every reference of the .desc files below `desc_roots` that does not resolve, as "file -> reference (problem)".
std::vector<std::string> dangling_desc_references(const resource_layout& layout, const std::vector<fs::path>& desc_roots)
{
	std::vector<std::string> problems;
	for (const fs::path& root : desc_roots) {
		if (!fs::is_directory(root)) continue;
		for (const auto& entry : fs::recursive_directory_iterator(root)) {
			if (!entry.is_regular_file() || entry.path().extension() != ".desc") continue;
			// The samples submodule is optional (update = none) and has its own content.
			if (*entry.path().lexically_relative(root).begin() == "samples") continue;

			std::vector<std::string> references;
			collect_references(read_json(entry.path()), references);
			for (const std::string& reference : references) {
				if (const std::string problem = reference_problem(layout, reference); !problem.empty()) {
					problems.push_back(entry.path().lexically_relative(root).generic_string() + " -> " + reference + " (" + problem + ")");
				}
			}
		}
	}
	return problems;
}

// Literals "res://x", "engine://x", "edt://x" and res::tag::make("x") of the C++ sources below `source_roots`, outside
// comment lines. A bare prefix ("res://"), a pattern ('#', '{', '%', a glob with '*' or '?') or a path ending in '/' names no file.
std::vector<std::string> dangling_source_references(const resource_layout& layout, const std::vector<fs::path>& source_roots,
	const fs::path& skipped)
{
	static const std::regex protocol_literal{ R"re("((?:res|engine|edt)://[^"\\]*)")re" };
	static const std::regex make_literal{ R"re(tag::make\("([^"\\]+)"\))re" };

	std::vector<std::string> problems;
	for (const fs::path& root : source_roots) {
		for (const auto& entry : fs::recursive_directory_iterator(root)) {
			if (!entry.is_regular_file()) continue;
			const std::string ext = entry.path().extension().string();
			if (ext != ".cpp" && ext != ".h" && ext != ".hpp") continue;
			if (!skipped.empty() && is_below(entry.path(), skipped)) continue;

			std::ifstream in(entry.path());
			std::string line;
			int number = 0;
			while (std::getline(in, line)) {
				++number;
				const auto first = line.find_first_not_of(" \t");
				if (first == std::string::npos || line.compare(first, 2, "//") == 0) continue;
				if (line.find("://") == std::string::npos && line.find("tag::make(") == std::string::npos) continue;

				std::vector<std::string> references;
				for (auto it = std::sregex_iterator(line.begin(), line.end(), protocol_literal); it != std::sregex_iterator(); ++it) {
					references.push_back((*it)[1].str());
				}
				for (auto it = std::sregex_iterator(line.begin(), line.end(), make_literal); it != std::sregex_iterator(); ++it) {
					references.push_back("res://" + (*it)[1].str());
				}
				for (const std::string& reference : references) {
					const std::string path = reference.substr(reference.find("://") + 3);
					if (path.empty() || path.back() == '/' || path.find_first_of("#{%*?") != std::string::npos) continue;
					if (const std::string problem = reference_problem(layout, reference); !problem.empty()) {
						problems.push_back(entry.path().lexically_relative(root).generic_string() + ":" + std::to_string(number) + " " + reference + " (" + problem + ")");
					}
				}
			}
		}
	}
	return problems;
}

}

BOOST_AUTO_TEST_SUITE(ResRepoLayout)

BOOST_AUTO_TEST_CASE(GameResourceRoots_HoldNoStrayTestFiles)
{
	const auto in_core = stray_test_files(core_res, {});
	const auto in_editor = stray_test_files(editor_res, allowed_fixtures);

	BOOST_TEST(in_core.empty(), "stray test files in core/res:" << joined(in_core));
	BOOST_TEST(in_editor.empty(), "stray test files in Editor/res (list a real fixture in allowed_fixtures):" << joined(in_editor));
}

BOOST_AUTO_TEST_CASE(EveryAllowedFixture_StillExists)
{
	for (const std::string& fixture : allowed_fixtures) {
		BOOST_TEST(fs::is_regular_file(editor_res / fixture), fixture);
	}
}

BOOST_AUTO_TEST_CASE(EveryResEngineAndEdtPathInShippedDescs_ResolvesToAFile)
{
	const auto problems = dangling_desc_references(shipped_layout(), { core_res, editor_res, editor_data });

	BOOST_TEST(problems.empty(), "dangling references in descs:" << joined(problems));
}

BOOST_AUTO_TEST_CASE(EveryResEngineAndEdtPathInEngineAndEditorSources_ResolvesToAFile)
{
	const auto problems = dangling_source_references(shipped_layout(), { repo_root / "core", repo_root / "Editor" / "code" }, core_res);

	BOOST_TEST(problems.empty(), "dangling references in sources:" << joined(problems));
}

// ---- the checks themselves, on a scratch directory ----

BOOST_AUTO_TEST_CASE(StrayTestFiles_CatchesNewTestFilesAndFoldersAndSparesTheRest)
{
	test_util::temp_dir dir{ "layout_stray" };
	dir.write("test3.png", "x");
	dir.write("foo_test.desc", "{}");
	dir.write("tests/data.txt", "x");
	dir.write("levels/smoke_extra.desc", "{}");
	dir.write("Bench_Mesh.desc", "{}");
	dir.write("milla.bmp", "x");
	dir.write("levels/smoke_level.desc", "{}");
	dir.write("latest.png", "x");
	dir.write("shaders/templates/normal.frag", "x");
	dir.write("samples/test_model.glb", "x");

	const auto found = stray_test_files(dir.path(), { "levels/smoke_level.desc" });

	BOOST_TEST((found == std::vector<std::string>{ "Bench_Mesh.desc", "foo_test.desc", "levels/smoke_extra.desc", "milla.bmp", "test3.png", "tests/data.txt" }), joined(found));
}

BOOST_AUTO_TEST_CASE(DanglingDescReferences_CatchesMissingFilesAndUnknownProtocols)
{
	test_util::temp_dir dir{ "layout_desc_refs" };
	dir.write("res/ok.png", "x");
	dir.write("res/engine_only.png", "x");
	dir.write("data/icon.png", "x");
	dir.write("res/a.desc", R"({"fine":"res://ok.png","scratch":"memory://anything","edt":"edt://icon.png","engine":"engine://engine_only.png"})");
	dir.write("res/b.desc", R"({"nested":[{"x":"res://missing.png"}],"edt":"edt://missing_icon.png","odd":"foo://y"})");
	const resource_layout layout{ { dir.path() / "res" }, { dir.path() / "res" }, { dir.path() / "data" } };

	const auto problems = dangling_desc_references(layout, { dir.path() / "res" });

	BOOST_TEST((problems == std::vector<std::string>{
		"b.desc -> res://missing.png (no such file)",
		"b.desc -> edt://missing_icon.png (no such file)",
		"b.desc -> foo://y (unknown protocol)" }), joined(problems));
}

BOOST_AUTO_TEST_CASE(DanglingSourceReferences_CatchesMissingFilesAndSkipsCommentsAndPatterns)
{
	test_util::temp_dir dir{ "layout_source_refs" };
	dir.write("res/ok.png", "x");
	dir.write("src/a.cpp",
		"auto a = res::tag{ \"res://ok.png\" };\n"
		"auto b = res::tag{ \"res://missing.png\" };\n"
		"auto c = res::tag::make(\"made_missing.png\");\n"
		"auto d = res::tag::make(\"ok.png\");\n"
		"// auto e = res::tag{ \"res://commented.png\" };\n"
		"ImGui::TreeNodeEx(\"res://##root\");\n"
		"starts_with(\"res://\");\n"
		"auto f = \"memory://scratch/x\";\n"
		"auto g = res::tag{ \"edt://settings/**\" };\n"
		"auto h = res::tag{ \"res://shaders/pass?.frag\" };\n"
		"auto i = res::tag{ \"edt://settings/missing.json\" };\n");
	const resource_layout layout{ { dir.path() / "res" }, { dir.path() / "res" }, {} };

	const auto problems = dangling_source_references(layout, { dir.path() / "src" }, {});

	BOOST_TEST((problems == std::vector<std::string>{
		"a.cpp:2 res://missing.png (no such file)",
		"a.cpp:3 res://made_missing.png (no such file)",
		"a.cpp:11 edt://settings/missing.json (no such file)" }), joined(problems));
}

BOOST_AUTO_TEST_SUITE_END()
