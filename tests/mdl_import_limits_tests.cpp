#include <boost/test/unit_test.hpp>
#if SNK_HAS_ASSIMP
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "edt_async_import_task.h"
#include "edt_import_problems.h"
#include "edt_model_importer.h"
#include <snk/common/logger/engine_log_sinks.h>
#include <snk/model_import/mdl_import.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <utility>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// #1119: a model past the scene limits is refused before anything is written, names from the model file reach the log
// escaped, and a failed import replaces the Problems rows of the last one.
namespace {

bool contains(std::string_view text, std::string_view part)
{
	return text.find(part) != std::string_view::npos;
}

// The model and nothing beside it, from memory.
class memory_files final : public mdl::file_source
{
public:
	memory_files(std::string name, std::string_view text) : m_name(std::move(name))
	{
		m_bytes.resize(text.size());
		std::memcpy(m_bytes.data(), text.data(), text.size());
	}

	mdl::opened_file open(std::string_view name) override
	{
		if (name != m_name) return {};
		return { &m_bytes, {} };
	}

	bool exists(std::string_view name) override { return name == m_name; }

private:
	std::string m_name;
	std::vector<std::byte> m_bytes;
};

struct recording_sink final : mdl::import_sink
{
	bool exists(const res::tag&) override { return false; }
	void store(const res::tag& tag, const std::vector<std::byte>&) override { stored.push_back(tag); }

	std::vector<res::tag> stored;
};

// A glTF whose node `i` has the children children_of(i); node 0 holds the one triangle, the only root.
std::string gltf_of(std::size_t node_count, const std::function<std::vector<std::size_t>(std::size_t)>& children_of)
{
	std::string nodes;
	nodes.reserve(node_count * 16);
	for (std::size_t i = 0; i < node_count; ++i) {
		if (i > 0) nodes += ',';
		nodes += '{';
		if (i == 0) nodes += "\"mesh\":0";
		const std::vector<std::size_t> children = children_of(i);
		if (!children.empty()) {
			if (i == 0) nodes += ',';
			nodes += "\"children\":[";
			for (std::size_t c = 0; c < children.size(); ++c) {
				if (c > 0) nodes += ',';
				nodes += std::to_string(children[c]);
			}
			nodes += ']';
		}
		nodes += '}';
	}
	// Three vec3 floats: (0, 0, 0), (1, 0, 0), (0, 1, 0).
	return std::format(R"({{"asset":{{"version":"2.0"}},"scene":0,"scenes":[{{"nodes":[0]}}],"nodes":[{}],)"
		R"("meshes":[{{"primitives":[{{"attributes":{{"POSITION":0}}}}]}}],)"
		R"("buffers":[{{"byteLength":36,"uri":"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA"}}],)"
		R"("bufferViews":[{{"buffer":0,"byteOffset":0,"byteLength":36}}],)"
		R"("accessors":[{{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}}]}})", nodes);
}

mdl::import_result import_text(const std::string& name, std::string_view text, recording_sink& sink)
{
	memory_files files(name, text);
	mdl::import_options options;
	options.display_name = name;
	options.geometry_tag = res::tag{ "memory://limits/" + name + ".geom.desc" };
	options.prefab_tag = res::tag{ "memory://limits/" + name + ".prefab.desc" };
	return mdl::import_model(name, files, sink, options);
}

// Every WARN, whatever its category; the sink may be called from another thread.
struct warn_capture
{
	warn_capture()
	{
		sink = engine::add_log_sink([this](std::string_view, engine::log_level level, std::string_view message) {
			if (level != engine::log_level::warn) return;
			std::lock_guard lock(mutex);
			lines.emplace_back(message);
		});
	}

	std::vector<std::string> matching(std::string_view part) const
	{
		std::lock_guard lock(mutex);
		std::vector<std::string> out;
		for (const std::string& line : lines) {
			if (contains(line, part)) out.push_back(line);
		}
		return out;
	}

	mutable std::mutex mutex;
	std::vector<std::string> lines;
	engine::log_sink_handle sink; // last: removed first
};

// ESC and U+202E (right-to-left override), as a model file may hold them in a name.
constexpr std::string_view k_hostile = "\x1b[31m\xE2\x80\xAE";
constexpr std::string_view k_hostile_escaped = "\\u{001B}[31m\\u{202E}";

bool raw_hostile(std::string_view line)
{
	return contains(line, "\x1b") || contains(line, "\xE2\x80\xAE");
}

}

BOOST_AUTO_TEST_SUITE(MdlImportLimitsTests)

BOOST_AUTO_TEST_CASE(EachLimit_TakesItsCount_AndRefusesOneMore_NamingTheLimitAndTheCount)
{
	const mdl::scene_size at_limits{ mdl::k_max_scene_nodes, mdl::k_max_scene_depth, mdl::k_max_scene_meshes,
		mdl::k_max_scene_vertices, mdl::k_max_scene_indices };
	BOOST_TEST(mdl::scene_size_problem(at_limits).empty());

	mdl::scene_size nodes = at_limits;
	nodes.nodes = 70112;
	BOOST_TEST(mdl::scene_size_problem(nodes) == "too many nodes: 70112 (limit 65536)");

	mdl::scene_size depth = at_limits;
	depth.depth = 49;
	BOOST_TEST(mdl::scene_size_problem(depth) == "the node tree is too deep: 49 levels (limit 48)");

	mdl::scene_size meshes = at_limits;
	meshes.meshes = 65537;
	BOOST_TEST(mdl::scene_size_problem(meshes) == "too many meshes: 65537 (limit 65536)");

	mdl::scene_size vertices = at_limits;
	vertices.vertices = 16'777'217;
	BOOST_TEST(mdl::scene_size_problem(vertices) == "too many vertices: 16777217 (limit 16777216)");

	mdl::scene_size indices = at_limits;
	indices.indices = 48ull * 1024 * 1024 + 3;
	BOOST_TEST(mdl::scene_size_problem(indices) == "too many indices: 50331651 (limit 50331648)");
}

BOOST_AUTO_TEST_CASE(ASceneTooDeep_IsRefused_AndNothingIsWritten)
{
	constexpr std::size_t chain = 60;
	const std::string text = gltf_of(chain, [](std::size_t i) {
		return i + 1 < chain ? std::vector<std::size_t>{ i + 1 } : std::vector<std::size_t>{};
	});
	recording_sink sink;
	const mdl::import_result result = import_text("deep.gltf", text, sink);
	BOOST_TEST_INFO("error: " << result.error);
	BOOST_TEST((result.failed == mdl::failure::too_large));
	BOOST_TEST(result.error.starts_with("the node tree is too deep: "));
	BOOST_TEST(contains(result.error, "(limit 48)"));
	BOOST_TEST(sink.stored.empty());
	BOOST_TEST(result.tags.empty());
	BOOST_TEST(result.prefab.empty());
}

BOOST_AUTO_TEST_CASE(ASceneOfTooManyNodes_IsRefused_AndNothingIsWritten)
{
	constexpr std::size_t count = static_cast<std::size_t>(mdl::k_max_scene_nodes) + 1;
	const std::string text = gltf_of(count, [](std::size_t i) {
		std::vector<std::size_t> children;
		if (i == 0) {
			children.reserve(count - 1);
			for (std::size_t c = 1; c < count; ++c) children.push_back(c);
		}
		return children;
	});
	recording_sink sink;
	const mdl::import_result result = import_text("wide.gltf", text, sink);
	BOOST_TEST_INFO("error: " << result.error);
	BOOST_TEST((result.failed == mdl::failure::too_large));
	BOOST_TEST(result.error.starts_with("too many nodes: "));
	BOOST_TEST(contains(result.error, "(limit 65536)"));
	BOOST_TEST(sink.stored.empty());
	BOOST_TEST(result.prefab.empty());
}

BOOST_AUTO_TEST_CASE(ASceneWithinTheLimits_IsImported)
{
	const std::string text = gltf_of(3, [](std::size_t i) {
		return i == 0 ? std::vector<std::size_t>{ 1, 2 } : std::vector<std::size_t>{};
	});
	recording_sink sink;
	const mdl::import_result result = import_text("small.gltf", text, sink);
	BOOST_TEST_INFO("error: " << result.error);
	BOOST_TEST((result.failed == mdl::failure::none));
	BOOST_TEST(!sink.stored.empty());
}

// The mesh name (dropped faces) and the texture name (not loaded) come from the model file: the log shows them escaped.
BOOST_AUTO_TEST_CASE(NamesFromTheModelFile_AreLoggedEscaped)
{
	test_util::scene_env env;
	test_util::temp_dir dir{ "mdl_import_log_escape" };
	dir.write("hostile.mtl", std::format("newmtl skin\nKd 1 1 1\nmap_Kd tex{}.png\n", k_hostile));
	const std::string obj = std::format("mtllib hostile.mtl\nv 0 0 0\nv 1 0 0\nv 1 1 0\nvt 0 0\nvt 1 0\nvt 1 1\n"
		"o Mixed{}\nusemtl skin\nf 1/1 2/2 3/3\nl 1/1 2/2\n", k_hostile);
	edt::model_importer importer(*env.descs, *env.rs);
	edt::import_source source;
	source.path = dir.write("hostile.obj", obj);

	const warn_capture warnings;
	const edt::import_intermediate result = importer.import_background(source);
	BOOST_TEST_REQUIRE(result.error.empty(), result.error);

	const std::vector<std::string> dropped = warnings.matching("line(s) and");
	BOOST_REQUIRE(!dropped.empty());
	for (const std::string& line : dropped) {
		BOOST_TEST_INFO("line: " << line);
		BOOST_TEST(contains(line, std::string{ "Mixed" } + std::string{ k_hostile_escaped }));
		BOOST_TEST(!raw_hostile(line));
	}

	const std::vector<std::string> missing = warnings.matching("no sampler for it");
	BOOST_REQUIRE(!missing.empty());
	for (const std::string& line : missing) {
		BOOST_TEST_INFO("line: " << line);
		BOOST_TEST(contains(line, std::string{ "tex" } + std::string{ k_hostile_escaped } + ".png"));
		BOOST_TEST(!raw_hostile(line));
	}
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(EdtImportProblemsTests)

// A failed import is one error row naming the model and the reason, which is shown escaped.
BOOST_AUTO_TEST_CASE(AFailedImport_IsOneErrorRow_NamingTheModelAndTheReason)
{
	const std::vector<edt::problem> rows = edt::import_failure_problems("big.glb",
		std::format("the model is too large: too many nodes: 70112 (limit 65536){}", k_hostile));
	BOOST_REQUIRE(rows.size() == 1u);
	BOOST_TEST((rows.front().severity == edt::problem_severity::error));
	BOOST_TEST(contains(rows.front().message, "'big.glb'"));
	BOOST_TEST(contains(rows.front().message, "too many nodes: 70112 (limit 65536)"));
	BOOST_TEST(contains(rows.front().message, k_hostile_escaped));
	BOOST_TEST(!raw_hostile(rows.front().message));
}

// Problems keeps one set per source: the failure's row replaces the warnings of the import before it.
BOOST_AUTO_TEST_CASE(AFailedImport_ReplacesTheRowsOfTheLastImport)
{
	edt::problem_list problems;
	edt::import_intermediate earlier;
	earlier.missing_textures = { "a.png", "b.png" };
	problems.set(edt::k_import_problem_source, edt::import_problems(earlier, "first.obj"));
	problems.set(edt::k_import_problem_source, edt::import_failure_problems("second.obj", "ASSIMP error: bad file"));
	const std::vector<edt::sourced_problem> rows = problems.list();
	BOOST_REQUIRE(rows.size() == 1u);
	BOOST_TEST(rows.front().source == edt::k_import_problem_source);
	BOOST_TEST(contains(rows.front().item.message, "second.obj"));

	problems.clear(edt::k_import_problem_source);
	BOOST_TEST(problems.size() == 0u);
}

BOOST_AUTO_TEST_SUITE_END()
#endif
