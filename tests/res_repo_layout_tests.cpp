#include <boost/test/unit_test.hpp>
#include <boost/json.hpp>
#include "test_temp_dir.hpp"
#include <snk/resource/res_asset_id.h>
#include <snk/resource/res_asset_meta.h>
#include <snk/resource/res_path_sanitize.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// The layout of the shipped resources (#163): res:// holds game resources only (no stray test files; the fixtures that
// stay are listed one by one), and every res:// / engine:// / edt:// path that shipped descs and engine sources
// mention resolves to a file. Every file of the repository's resource trees has its .resource (#768).

#ifndef SNK_TEST_PROJECT_DIR
#error "SNK_TEST_PROJECT_DIR must point at projects/sandbox"
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
const fs::path project_res = fs::path{ SNK_TEST_PROJECT_DIR } / "res";
const fs::path editor_data{ SNK_EDITOR_DATA_DIR };
const fs::path repo_root = core_res.parent_path().parent_path();
const fs::path static_game_res = repo_root / "tests" / "static_game" / "project" / "res";
// res::is_ignored_asset_name per name; tools/ci/asset_ids.py --self-test checks its port against the same table.
const fs::path ignored_names_table = repo_root / "tests" / "data" / "asset_ignored_names.txt";

resource_layout shipped_layout()
{
	return { { core_res, project_res }, { core_res }, { editor_data } };
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

bool is_sidecar_name(std::string_view name)
{
	return name.size() > res::asset_meta_suffix.size() && res::is_asset_meta_name(name);
}

// Files below `root` (the samples submodule aside) with a test-like name in the path that are not allowed fixtures.
std::vector<std::string> stray_test_files(const fs::path& root, const std::set<std::string>& allowed)
{
	std::vector<std::string> found;
	for (const auto& entry : fs::recursive_directory_iterator(root)) {
		if (!entry.is_regular_file()) continue;
		const fs::path rel = entry.path().lexically_relative(root);
		if (*rel.begin() == "samples") continue;
		const std::string generic = rel.generic_string();
		// The .resource of an allowed fixture is its ID (#768), allowed with it.
		const std::string owner = is_sidecar_name(rel.filename().string())
			? generic.substr(0, generic.size() - res::asset_meta_suffix.size())
			: generic;
		const bool test_like = std::ranges::any_of(rel, [](const fs::path& part) { return looks_like_a_test_file(part.string()); });
		if (test_like && !allowed.contains(owner)) {
			found.push_back(generic);
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

// The files of a resource tree as the editor's asset database walks it (tools/ci/asset_ids.py walks the same way): hidden
// and ignored names are neither listed nor entered, links are not followed, a git submodule (a .git inside, or the
// samples submodule, update = none, that may be an empty folder) is left out. Paths relative to the root, sorted.
struct tree_files
{
	std::vector<fs::path> assets;
	std::vector<fs::path> sidecars;
};

tree_files walk_tree(const fs::path& root)
{
	tree_files out;
	for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {
		const std::string name = it->path().filename().string();
		if (it->is_symlink()) {
			continue;
		}
		const fs::path rel = it->path().lexically_relative(root);
		if (it->is_directory()) {
			if (res::is_ignored_asset_name(name) || fs::exists(it->path() / ".git") || rel == "samples") {
				it.disable_recursion_pending();
			}
			continue;
		}
		if (!it->is_regular_file()) {
			continue;
		}
		if (is_sidecar_name(name)) {
			if (!res::is_ignored_asset_name(std::string_view{ name }.substr(0, name.size() - res::asset_meta_suffix.size()))) {
				out.sidecars.push_back(rel);
			}
		} else if (!res::is_ignored_asset_name(name)) {
			out.assets.push_back(rel);
		}
	}
	std::ranges::sort(out.assets);
	std::ranges::sort(out.sidecars);
	return out;
}

// The "__guid" of a desc's root object when it is an asset ID: the ID its .resource holds (EPIC-57 section 8).
std::optional<std::string> desc_root_guid(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	const std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	boost::system::error_code ec;
	const json::value document = json::parse(text, ec);
	if (ec || !document.is_object()) {
		return std::nullopt;
	}
	const json::value* guid = document.as_object().if_contains("__guid");
	if (!guid || !guid->is_string()) {
		return std::nullopt;
	}
	const std::optional<res::asset_id> id = res::asset_id_from_text(guid->as_string());
	if (!id || id->is_nil()) {
		return std::nullopt;
	}
	return res::to_text(*id);
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
	const auto in_project = stray_test_files(project_res, allowed_fixtures);

	BOOST_TEST(in_core.empty(), "stray test files in core/res:" << joined(in_core));
	BOOST_TEST(in_project.empty(), "stray test files in projects/sandbox/res (list a real fixture in allowed_fixtures):" << joined(in_project));
}

BOOST_AUTO_TEST_CASE(EveryAllowedFixture_StillExists)
{
	for (const std::string& fixture : allowed_fixtures) {
		BOOST_TEST(fs::is_regular_file(project_res / fixture), fixture);
	}
}

BOOST_AUTO_TEST_CASE(EveryFileOfTheRepositoryTrees_HasItsResourceInTheEngineForm)
{
	std::map<std::string, std::string> holders; // asset ID -> the .resource that holds it
	for (const fs::path& root : { core_res, project_res, static_game_res }) {
		const tree_files files = walk_tree(root);
		BOOST_TEST(!files.assets.empty(), "no files in " << root.generic_string());

		// A .resource belongs to its file in any letter case, as in the editor.
		std::map<std::string, fs::path> assets;
		for (const fs::path& asset : files.assets) {
			assets.emplace(lower(asset.generic_string()), asset);
		}
		std::set<std::string> owned;
		for (const fs::path& sidecar : files.sidecars) {
			const std::string where = (root / sidecar).generic_string();
			const std::string key = lower(sidecar.generic_string());
			const std::string owner = key.substr(0, key.size() - res::asset_meta_suffix.size());
			owned.insert(owner);
			const auto asset = assets.find(owner);
			BOOST_TEST((asset != assets.end()), where << ": no file next to it");

			const auto bytes = res::read_regular_file_capped(root / sidecar, 64 * 1024);
			if (!bytes) {
				BOOST_ERROR(where << ": not readable, or more than 64 KiB");
				continue;
			}
			const std::string text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
			const res::asset_meta_result meta = res::read_asset_meta(text);
			if (!meta) {
				BOOST_ERROR(where << ": " << meta.error.field_path << ": " << meta.error.message);
				continue;
			}
			BOOST_TEST(res::write_asset_meta(*meta.meta) == text, where << ": not in the form res::write_asset_meta writes");

			const std::string id = res::to_text(meta.meta->guid);
			const auto [holder, fresh] = holders.emplace(id, where);
			BOOST_TEST(fresh, where << ": " << id << " is the ID of " << holder->second << " too");
			if (asset != assets.end() && lower(asset->second.extension().string()) == ".desc") {
				if (const auto guid = desc_root_guid(root / asset->second)) {
					BOOST_TEST(id == *guid, where << ": holds " << id << ", its desc has \"__guid\" " << *guid);
				}
			}
		}
		for (const auto& [key, asset] : assets) {
			BOOST_TEST(owned.contains(key), (root / asset).generic_string() << ": no .resource (tools/ci/asset_ids.py --write makes it)");
		}
	}
	BOOST_TEST(holders.size() >= 40u);
}

BOOST_AUTO_TEST_CASE(IgnoredAssetNames_MatchTheTableTheScriptChecks)
{
	std::ifstream in(ignored_names_table, std::ios::binary);
	BOOST_TEST_REQUIRE(in.is_open(), "cannot open " << ignored_names_table.generic_string());
	std::size_t rows = 0;
	for (std::string line; std::getline(in, line);) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		if (line.empty() || line.front() == '#') {
			continue;
		}
		const std::size_t tab = line.rfind('\t');
		BOOST_TEST_REQUIRE(tab != std::string::npos, "expected \"<name>\\t<0|1>\": " << line);
		const std::string name = line.substr(0, tab);
		const std::string flag = line.substr(tab + 1);
		BOOST_TEST_REQUIRE((flag == "0" || flag == "1"), "expected 0 or 1: " << line);
		BOOST_TEST(res::is_ignored_asset_name(name) == (flag == "1"), "res::is_ignored_asset_name(\"" << name << "\")");
		++rows;
	}
	BOOST_TEST(rows >= 20u);
}

BOOST_AUTO_TEST_CASE(EveryResEngineAndEdtPathInShippedDescs_ResolvesToAFile)
{
	const auto problems = dangling_desc_references(shipped_layout(), { core_res, project_res, editor_data });

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
	dir.write("levels/smoke_level.desc.resource", "{}");
	dir.write("levels/smoke_extra.desc.RESOURCE", "{}");
	dir.write("latest.png", "x");
	dir.write("shaders/templates/normal.frag", "x");
	dir.write("samples/test_model.glb", "x");

	const auto found = stray_test_files(dir.path(), { "levels/smoke_level.desc" });

	BOOST_TEST((found == std::vector<std::string>{ "Bench_Mesh.desc", "foo_test.desc", "levels/smoke_extra.desc", "levels/smoke_extra.desc.RESOURCE", "milla.bmp", "test3.png", "tests/data.txt" }), joined(found));
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
