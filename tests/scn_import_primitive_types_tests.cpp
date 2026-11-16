#include <boost/test/unit_test.hpp>
#if SNK_HAS_ASSIMP
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "edt_async_import_task.h"
#include "edt_model_importer.h"
#include <snk/common/logger/engine_log_sinks.h>
#include <snk/render/geom/rnd_geometry_desc.h>
#include <snk/render/geom/rnd_geometry_files.h>
#include <snk/scene/adapters/scn_model_importer_adapter.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <boost/json.hpp>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// #1092: assimp keeps line and point faces after aiProcess_Triangulate; neither importer may put them into a triangle
// index buffer, and the engine importer must not read past a face of fewer than 3 indices.
namespace {

namespace json = boost::json;

// Two line faces with uvs, nothing else.
constexpr std::string_view OBJ_LINES = R"(v 0 0 0
v 1 0 0
v 1 1 0
vt 0 0
vt 1 0
vt 1 1
o Wire
l 1/1 2/2
l 2/2 3/3
)";

// Three point faces (a point statement takes no uv index).
constexpr std::string_view OBJ_POINTS = R"(v 0 0 0
v 1 0 0
v 1 1 0
vt 0 0
vt 1 0
vt 1 1
o Dots
p 1 2 3
)";

// One mesh (one object, no material change): mPrimitiveTypes = TRIANGLE | LINE, one triangle and two lines.
constexpr std::string_view OBJ_TRIANGLE_AND_LINES = R"(v 0 0 0
v 1 0 0
v 1 1 0
vt 0 0
vt 1 0
vt 1 1
o Mixed
f 1/1 2/2 3/3
l 1/1 2/2
l 2/2 3/3
)";

// One mesh: mPrimitiveTypes = TRIANGLE | POINT, one triangle and two points.
constexpr std::string_view OBJ_TRIANGLE_AND_POINTS = R"(v 0 0 0
v 1 0 0
v 1 1 0
vt 0 0
vt 1 0
vt 1 1
o Mixed
f 1/1 2/2 3/3
p 1 2
)";

// Two meshes: a triangle object and a line object beside it; the line mesh keeps nothing in the triangle geometry.
constexpr std::string_view OBJ_TRIANGLE_BESIDE_LINES = R"(v 0 0 0
v 1 0 0
v 1 1 0
vt 0 0
vt 1 0
vt 1 1
o Tri
f 1/1 2/2 3/3
o Wire
l 1/1 2/2
l 2/2 3/3
)";

bool contains(std::string_view text, std::string_view part)
{
	return text.find(part) != std::string_view::npos;
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

	bool any(const std::function<bool(std::string_view)>& matches) const
	{
		std::lock_guard lock(mutex);
		for (const std::string& line : lines) {
			if (matches(line)) return true;
		}
		return false;
	}

	// The post-fix warning of #1092: the file, the mesh and how many of `kind` ("line"/"point") were dropped.
	bool names_dropped(std::string_view file, std::string_view mesh, std::size_t count, std::string_view kind) const
	{
		const std::string counted = std::to_string(count) + " " + std::string(kind);
		return any([&](std::string_view line) { return contains(line, file) && contains(line, mesh) && contains(line, counted); });
	}

	mutable std::mutex mutex;
	std::vector<std::string> lines;
	engine::log_sink_handle sink; // last: removed first
};

struct stored_geometry
{
	json::object desc;
	rnd::geometry_desc data;
};

struct primitive_types_fixture : test_util::scene_env
{
	primitive_types_fixture()
	{
		rs->registrate_adapter<scn::model_importer_adapter>(scn::model_importer_adapter::INFO, std::ref(*descs), std::ref(*rs));
	}

	// The geometry desc a memory:// tag holds, read as the loader reads it; nothing when no desc was stored.
	std::optional<stored_geometry> stored(const res::tag& tag)
	{
		const auto it = ram->files.find(tag);
		if (it == ram->files.end()) return std::nullopt;
		stored_geometry out;
		out.desc = json::parse(std::string_view(reinterpret_cast<const char*>(it->second.data()), it->second.size())).as_object();
		const std::string error = rnd::read_geometry_file(*rs, tag, out.desc, out.data);
		BOOST_REQUIRE_MESSAGE(error.empty(), error);
		return out;
	}

	// Engine importer: memory://models/<name>; the prefab, or null when the load failed.
	std::shared_ptr<scn::prefab_desc> engine_import(const std::string& name, std::string_view text)
	{
		const res::tag tag{ "memory://models/" + name };
		rs->store(tag, test_util::to_bytes(text));
		return require_loaded<scn::prefab_desc>(tag).get_sync();
	}

	std::optional<stored_geometry> engine_geometry(const std::string& stem)
	{
		return stored(res::tag{ "memory://models/" + stem + ".geom.desc" });
	}

	// Editor importer: the model written as a file in a scratch folder, imported in this process.
	edt::import_intermediate editor_import(const std::string& file_name, std::string_view text)
	{
		edt::model_importer importer(*descs, *rs);
		edt::import_source source;
		source.path = dir.write(file_name, text);
		return importer.import_background(source);
	}

	test_util::temp_dir dir{ "scn_import_primitive_types" };
};

// What #1092 asks of a stored geometry: a triangle geometry holds the indices of the triangle faces only; lines and
// points are dropped or kept in a geometry of their own topology, never in a mixed one.
void check_no_stray_primitives(const std::optional<stored_geometry>& geometry, std::size_t triangle_faces)
{
	if (!geometry) {
		BOOST_TEST(triangle_faces == 0u);
		return;
	}
	const rnd::geometry_topology topology = rnd::topology_of(geometry->desc);
	BOOST_TEST_INFO("topology: " << rnd::topology_name(topology) << ", indices: " << geometry->data.indices.size());
	BOOST_TEST((topology != rnd::geometry_topology::mixed));
	if (topology == rnd::geometry_topology::triangles) {
		BOOST_TEST(geometry->data.indices.size() == 3u * triangle_faces);
	} else if (topology == rnd::geometry_topology::lines) {
		BOOST_TEST(triangle_faces == 0u);
		BOOST_TEST((geometry->data.indices.size() % 2u == 0u));
	} else if (topology == rnd::geometry_topology::points) {
		BOOST_TEST(triangle_faces == 0u);
	}
}

// The index range of the first mesh node the engine importer made, or -1 without one.
int first_mesh_index_count(const json::value& node)
{
	if (const auto* components = node.as_object().if_contains("components"); components && components->is_object()) {
		if (const auto* mesh = components->as_object().if_contains("mesh_node_desc")) {
			return mesh->at("ind_end").to_number<int>() - mesh->at("ind_begin").to_number<int>();
		}
	}
	if (const auto* children = node.as_object().if_contains("children"); children && children->is_array()) {
		for (const auto& child : children->as_array()) {
			if (const int found = first_mesh_index_count(child); found >= 0) return found;
		}
	}
	return -1;
}

}

// How many mesh nodes the engine importer made.
int mesh_node_count(const json::value& node)
{
	int count = 0;
	if (const auto* components = node.as_object().if_contains("components"); components && components->is_object()) {
		if (components->as_object().contains("mesh_node_desc")) ++count;
	}
	if (const auto* children = node.as_object().if_contains("children"); children && children->is_array()) {
		for (const auto& child : children->as_array()) count += mesh_node_count(child);
	}
	return count;
}

BOOST_FIXTURE_TEST_SUITE(ScnImportPrimitiveTypesTests, primitive_types_fixture)

// ---- engine importer (scn::model_importer_adapter) ----

// Red before the fix under ASan (CI "Linux ASan+UBSan"): the UV loop reads face.mIndices[j + 2] of a 2-index face.
BOOST_AUTO_TEST_CASE(ALinesOnlyObj_DoesNotOverflow_InTheEngineImporter)
{
	const auto prefab = engine_import("wire.obj", OBJ_LINES);
	BOOST_REQUIRE(prefab);
	check_no_stray_primitives(engine_geometry("wire"), 0u);
}

// Red before the fix under ASan: the same loop reads face.mIndices[1] and [2] of a 1-index face.
BOOST_AUTO_TEST_CASE(APointsOnlyObj_DoesNotOverflow_InTheEngineImporter)
{
	const auto prefab = engine_import("dots.obj", OBJ_POINTS);
	BOOST_REQUIRE(prefab);
	check_no_stray_primitives(engine_geometry("dots"), 0u);
}

BOOST_AUTO_TEST_CASE(AMixedMeshOfTrianglesAndLines_KeepsOnlyItsTriangles_InTheEngineImporter)
{
	const warn_capture warnings;
	const auto prefab = engine_import("mixed_line.obj", OBJ_TRIANGLE_AND_LINES);
	BOOST_REQUIRE(prefab);
	const auto geometry = engine_geometry("mixed_line");
	BOOST_REQUIRE(geometry);
	BOOST_TEST((rnd::topology_of(geometry->desc) == rnd::geometry_topology::triangles));
	check_no_stray_primitives(geometry, 1u);
	json::object out;
	prefab->serialize(out);
	BOOST_TEST(first_mesh_index_count(out) == 3);
	BOOST_TEST(warnings.names_dropped("mixed_line.obj", "Mixed", 2u, "line"));
}

BOOST_AUTO_TEST_CASE(AMixedMeshOfTrianglesAndPoints_KeepsOnlyItsTriangles_InTheEngineImporter)
{
	const warn_capture warnings;
	const auto prefab = engine_import("mixed_point.obj", OBJ_TRIANGLE_AND_POINTS);
	BOOST_REQUIRE(prefab);
	const auto geometry = engine_geometry("mixed_point");
	BOOST_REQUIRE(geometry);
	BOOST_TEST((rnd::topology_of(geometry->desc) == rnd::geometry_topology::triangles));
	check_no_stray_primitives(geometry, 1u);
	json::object out;
	prefab->serialize(out);
	BOOST_TEST(first_mesh_index_count(out) == 3);
	BOOST_TEST(warnings.names_dropped("mixed_point.obj", "Mixed", 2u, "point"));
}

// A mesh that keeps no face gets no mesh node: it would draw nothing.
BOOST_AUTO_TEST_CASE(ALineMeshBesideATriangleMesh_IsDropped_InTheEngineImporter)
{
	const warn_capture warnings;
	const auto prefab = engine_import("beside.obj", OBJ_TRIANGLE_BESIDE_LINES);
	BOOST_REQUIRE(prefab);
	check_no_stray_primitives(engine_geometry("beside"), 1u);
	json::object out;
	prefab->serialize(out);
	BOOST_TEST(mesh_node_count(out) == 1);
	BOOST_TEST(first_mesh_index_count(out) == 3);
	BOOST_TEST(warnings.names_dropped("beside.obj", "Wire", 2u, "line"));
}

// ---- editor importer (edt::model_importer) ----

BOOST_AUTO_TEST_CASE(ALinesOnlyObj_HasNoLineInATriangleGeometry_InTheEditorImporter)
{
	const edt::import_intermediate imported = editor_import("wire.obj", OBJ_LINES);
	BOOST_REQUIRE_MESSAGE(imported.error.empty(), imported.error);
	check_no_stray_primitives(stored(imported.geom_tag), 0u);
}

BOOST_AUTO_TEST_CASE(APointsOnlyObj_HasNoPointInATriangleGeometry_InTheEditorImporter)
{
	const edt::import_intermediate imported = editor_import("dots.obj", OBJ_POINTS);
	BOOST_REQUIRE_MESSAGE(imported.error.empty(), imported.error);
	check_no_stray_primitives(stored(imported.geom_tag), 0u);
}

BOOST_AUTO_TEST_CASE(AMixedMeshOfTrianglesAndLines_KeepsOnlyItsTriangles_InTheEditorImporter)
{
	const warn_capture warnings;
	const edt::import_intermediate imported = editor_import("mixed_line.obj", OBJ_TRIANGLE_AND_LINES);
	BOOST_REQUIRE_MESSAGE(imported.error.empty(), imported.error);
	const auto geometry = stored(imported.geom_tag);
	BOOST_REQUIRE(geometry);
	BOOST_TEST((rnd::topology_of(geometry->desc) == rnd::geometry_topology::triangles));
	check_no_stray_primitives(geometry, 1u);
	BOOST_TEST(warnings.names_dropped("mixed_line.obj", "Mixed", 2u, "line"));
}

BOOST_AUTO_TEST_CASE(AMixedMeshOfTrianglesAndPoints_KeepsOnlyItsTriangles_InTheEditorImporter)
{
	const warn_capture warnings;
	const edt::import_intermediate imported = editor_import("mixed_point.obj", OBJ_TRIANGLE_AND_POINTS);
	BOOST_REQUIRE_MESSAGE(imported.error.empty(), imported.error);
	const auto geometry = stored(imported.geom_tag);
	BOOST_REQUIRE(geometry);
	BOOST_TEST((rnd::topology_of(geometry->desc) == rnd::geometry_topology::triangles));
	check_no_stray_primitives(geometry, 1u);
	BOOST_TEST(warnings.names_dropped("mixed_point.obj", "Mixed", 2u, "point"));
}

BOOST_AUTO_TEST_CASE(ALineMeshBesideATriangleMesh_IsDropped_InTheEditorImporter)
{
	const warn_capture warnings;
	const edt::import_intermediate imported = editor_import("beside.obj", OBJ_TRIANGLE_BESIDE_LINES);
	BOOST_REQUIRE_MESSAGE(imported.error.empty(), imported.error);
	check_no_stray_primitives(stored(imported.geom_tag), 1u);
	BOOST_TEST(mesh_node_count(imported.prefab_json) == 1);
	BOOST_TEST(warnings.names_dropped("beside.obj", "Wire", 2u, "line"));
}

BOOST_AUTO_TEST_SUITE_END()

#endif
