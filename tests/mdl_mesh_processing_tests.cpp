#include <boost/test/unit_test.hpp>
#if SNK_HAS_ASSIMP
#include <snk/model_import/mdl_import.h>
#include <cstddef>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// How the meshes are made (#1195): flat normals are not the smooth ones, tangents can be left out, and the import
// without mesh_processing is the import as before.

namespace {

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

struct memory_sink final : mdl::import_sink
{
	bool exists(const res::tag& tag) override { return stored.contains(std::string(tag.view())); }
	void store(const res::tag& tag, const std::vector<std::byte>& bytes) override
	{
		stored[std::string(tag.view())] = bytes;
	}

	std::map<std::string, std::vector<std::byte>> stored;
};

// Two triangles bent along their shared edge, with texture coordinates (tangents need them) and no normals.
constexpr std::string_view k_bent_obj =
	"v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\n"
	"vt 0 0\nvt 1 0\nvt 0 1\n"
	"f 1/1 2/2 3/3\nf 1/1 4/3 2/2\n";

// The same two triangles with the model's own normals, one for all four vertices: smooth keeps them.
constexpr std::string_view k_bent_obj_with_normals =
	"v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\n"
	"vt 0 0\nvt 1 0\nvt 0 1\n"
	"vn 0.577 0.577 0.577\n"
	"f 1/1/1 2/2/1 3/3/1\nf 1/1/1 4/3/1 2/2/1\n";

std::map<std::string, std::vector<std::byte>> imported(const mdl::mesh_processing* processing,
	std::string_view text = k_bent_obj)
{
	memory_files files("bent.obj", text);
	memory_sink sink;
	mdl::import_options options;
	options.display_name = "bent.obj";
	options.geometry_tag = res::tag{ "memory://processing/bent.geom.desc" };
	options.prefab_tag = res::tag{ "memory://processing/bent.prefab.desc" };
	const mdl::import_result result = processing ? mdl::import_model("bent.obj", files, sink, options, *processing)
		: mdl::import_model("bent.obj", files, sink, options);
	BOOST_REQUIRE_MESSAGE(result.error.empty(), result.error);
	BOOST_REQUIRE(!sink.stored.empty());
	return std::move(sink.stored);
}

std::size_t total_bytes(const std::map<std::string, std::vector<std::byte>>& stored)
{
	std::size_t out = 0;
	for (const auto& entry : stored) out += entry.second.size();
	return out;
}

}

BOOST_AUTO_TEST_SUITE(MdlMeshProcessing)

BOOST_AUTO_TEST_CASE(TheImportWithoutProcessing_IsTheDefaults)
{
	const mdl::mesh_processing defaults{};
	BOOST_CHECK(defaults.normals == mdl::normals_mode::smooth);
	BOOST_CHECK(defaults.tangents);
	BOOST_CHECK(imported(nullptr) == imported(&defaults));
}

BOOST_AUTO_TEST_CASE(FlatNormals_AreNotTheSmoothOnes)
{
	const mdl::mesh_processing smooth{};
	mdl::mesh_processing flat;
	flat.normals = mdl::normals_mode::flat;
	const auto smooth_import = imported(&smooth);
	const auto flat_import = imported(&flat);
	BOOST_CHECK(smooth_import != flat_import);
}

// GenNormals alone keeps normals a model has: flat must make its own for a model that brings them.
BOOST_AUTO_TEST_CASE(FlatNormals_ReplaceTheModelsOwn)
{
	const mdl::mesh_processing smooth{};
	mdl::mesh_processing flat;
	flat.normals = mdl::normals_mode::flat;
	BOOST_CHECK(imported(&flat, k_bent_obj_with_normals) != imported(&smooth, k_bent_obj_with_normals));
	// The model's own normals are gone: flat gives what it gives a model without them.
	BOOST_CHECK(imported(&flat, k_bent_obj_with_normals) == imported(&flat, k_bent_obj));
}

BOOST_AUTO_TEST_CASE(WithoutTangents_TheVerticesAreSmaller)
{
	const mdl::mesh_processing with{};
	mdl::mesh_processing without;
	without.tangents = false;
	BOOST_CHECK_LT(total_bytes(imported(&without)), total_bytes(imported(&with)));
}

BOOST_AUTO_TEST_SUITE_END()
#endif
