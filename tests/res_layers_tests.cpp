#include <boost/test/unit_test.hpp>
#include <boost/json.hpp>
#include "resolvers/res_vfs_resolver.h"
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

// "Out of the box": the engine base layer (core/res) must be self-sufficient. Everything the base descs and
// the engine's built-in passes refer to has to resolve with ONLY core/res mounted, without the editor layer.

namespace fs = std::filesystem;

#ifndef SNK_CORE_RES_DIR
#error "SNK_CORE_RES_DIR must point at core/res"
#endif

namespace {

std::string read_text(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	std::ostringstream out;
	out << in.rdbuf();
	return out.str();
}

void collect_res_refs(const boost::json::value& v, std::set<std::string>& out)
{
	if (v.is_string()) {
		const std::string s{ v.as_string() };
		if (s.rfind("res://", 0) == 0) out.insert(s);
	} else if (v.is_array()) {
		for (const auto& e : v.as_array()) collect_res_refs(e, out);
	} else if (v.is_object()) {
		for (const auto& kv : v.as_object()) collect_res_refs(kv.value(), out);
	}
}

struct base_layer_fixture
{
	fs::path root = SNK_CORE_RES_DIR;
	res::vfs_resolver base_only{ "res", std::vector<fs::path>{ root } };
};

}

BOOST_AUTO_TEST_SUITE(ResLayersOutOfTheBox)

BOOST_FIXTURE_TEST_CASE(BaseDescsReferenceOnlyFilesOfTheBaseLayer, base_layer_fixture)
{
	std::set<std::string> refs;
	size_t descs = 0;
	for (const auto& entry : fs::recursive_directory_iterator(root)) {
		if (!entry.is_regular_file() || entry.path().extension() != ".desc") continue;
		++descs;
		collect_res_refs(boost::json::parse(read_text(entry.path())), refs);
	}
	BOOST_TEST(descs > 0u);
	BOOST_TEST(!refs.empty());
	for (const auto& ref : refs) {
		BOOST_TEST(base_only.exists(res::tag{ ref }), "missing in core/res: " << ref);
	}
}

BOOST_FIXTURE_TEST_CASE(BaseShaderIncludesResolveInsideTheBaseLayer, base_layer_fixture)
{
	size_t includes = 0;
	for (const auto& entry : fs::directory_iterator(root / "shaders")) {
		if (!entry.is_regular_file()) continue;
		std::istringstream lines(read_text(entry.path()));
		for (std::string line; std::getline(lines, line);) {
			const auto at = line.find("#include \"");
			if (at == std::string::npos) continue;
			const auto begin = at + 10;
			const auto end = line.find('"', begin);
			BOOST_REQUIRE(end != std::string::npos);
			++includes;
			const auto tag = "res://shaders/" + line.substr(begin, end - begin);
			BOOST_TEST(base_only.exists(res::tag{ tag }), entry.path().filename() << " includes missing " << tag);
		}
	}
	BOOST_TEST(includes > 0u);
}

BOOST_FIXTURE_TEST_CASE(ShadersUsedByBuiltInPassesAreInTheBaseLayer, base_layer_fixture)
{
	// Literals from rnd_z_prepass, rnd_composition_pass, rnd_debug_pass and the model importers.
	for (const char* name : { "z_prepass", "composition", "debug_line", "sky", "scene" }) {
		for (const char* ext : { "vert", "frag" }) {
			const auto tag = std::string{ "res://shaders/" } + name + "." + ext;
			BOOST_TEST(base_only.exists(res::tag{ tag }), "missing in core/res: " << tag);
		}
	}
	BOOST_TEST(base_only.exists(res::tag{ "res://shaders/mix_opaque_trans_scene.frag" }));
}

BOOST_FIXTURE_TEST_CASE(BaseLayerContentIsReadableThroughTheResolver, base_layer_fixture)
{
	auto block = base_only.resolve(res::tag{ "res://base_material.desc" });
	block->wait();
	BOOST_TEST(block->is_ready());
	BOOST_TEST(!block->data.empty());
}

BOOST_AUTO_TEST_SUITE_END()
