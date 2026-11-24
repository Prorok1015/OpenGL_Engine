#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "edt_mesh_document.h"
#include "edt_mesh_documents.h"
#include "edt_mesh_shape_asset.h"
#include "edt_save_all.h"
#include "edt_scene_document.h"
#include <snk/editor/edt_asset_kind_registry.h>
#include <snk/editor/edt_document_manager.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/resource/res_system.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_hot_reload_manager.h>
#include <snk/scene/scn_camera_component.hpp>
#include <snk/scene/scn_camera_controller_component.hpp>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <boost/json.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The mesh document (#1011): the working copy of a poly file, the preview geometry and the save of the two files.
namespace {

namespace fs = std::filesystem;
using namespace edt::mesh;

struct mesh_document_fixture : test_util::scene_env
{
	mesh_document_fixture()
	{
		fs::create_directories(res_root / "shapes");
		fs::create_directories(lower / "shapes");
		rs->unregistrate_resolver(res::tag::default_protocol());
		rs->registrate_resolver<res::vfs_resolver>(res::tag::default_protocol(), std::vector<fs::path>{ lower, res_root },
			res::mount_access::read_write, res::write_policy::allow_all());
		services.base_descs = descs.get();
		services.base_resources = rs.get();
		services.base_assembler = assembler.get();
		services.base_sfactory = &sf;
		const shape_asset_result box = build_shape_asset(box_params{}, res::tag{ "res://shapes/crate.geom.desc" });
		BOOST_REQUIRE_MESSAGE(box.files.has_value(), box.error);
		box_text = box.files->poly;
		write(res_root, "shapes/crate.poly.desc", box_text);
		document = std::make_unique<edt::mesh_document>(services);
		document->assign_id(9);
		document->set_files(layout());
		document->bind_events(&events);
	}

	~mesh_document_fixture()
	{
		document.reset();
		rs->unregistrate_resolver(res::tag::default_protocol());
	}

	edt::desc_file_layout layout()
	{
		edt::desc_file_layout out;
		out.top_layer = [this] { return res_root; };
		out.layers = [this] { return std::vector<fs::path>{ lower, res_root }; };
		out.open_level_files = [this] { return open_files; };
		out.write_blocker = [this] { return blocked; };
		return out;
	}

	static void write(const fs::path& root, std::string_view name, std::string_view content)
	{
		std::ofstream out(root / std::string(name), std::ios::binary);
		out << content;
	}

	static std::string text_of(const fs::path& path)
	{
		std::ifstream in(path, std::ios::binary);
		return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	}

	void open_crate() { document->open(res::tag{ "res://shapes/crate.poly.desc" }); }

	bool move_first_vertex()
	{
		return document->edit([](poly_mesh& mesh) {
			mesh.set_position(mesh.vertices().begin()->first, glm::vec3{ 5.f, 5.f, 5.f });
			return true;
		});
	}

	test_util::temp_dir dir{ "edt_mesh_document" };
	const fs::path res_root = dir.path() / "res";
	const fs::path lower = dir.path() / "lower";
	ecs::system_factory sf;
	edt::editor_services services;
	edt::editor_events events;
	std::vector<std::pair<edt::document_id, std::uint64_t>> changed;
	edt::connection seen = events.document_changed.subscribe([this](edt::document_id id, std::uint64_t generation) { changed.push_back({ id, generation }); });
	std::unique_ptr<edt::mesh_document> document;
	std::string box_text;
	std::optional<std::string> blocked;
	std::vector<std::string> open_files;
};


constexpr const char* PREVIEW_TEMPLATE = R"({
	"__type": "world_desc", "__version": 4, "name": "__prefab_preview", "systems": [],
	"components": { "anchor": { "__type": "anchor_desc", "__version": 1 } },
	"children": []
})";

// The document with a context manager and the preview template: it makes its preview world.
struct mesh_world_fixture : mesh_document_fixture
{
	mesh_world_fixture()
	{
		auto& edt_ram = rs->registrate_resolver<test_util::ram_resolver>("edt");
		edt_ram.store(res::tag("edt://templates/prefab_preview.desc"), test_util::to_bytes(PREVIEW_TEMPLATE));
		hot = std::make_shared<scn::hot_reload_manager>(*assembler, *rs);
		mgr = std::make_shared<scn::context_manager>(*rs, *assembler, sf, hot);
		services.ctx_manager = mgr;
		document = std::make_unique<edt::mesh_document>(services);
		document->assign_id(9);
		document->set_files(layout());
		document->bind_events(&events);
	}

	std::shared_ptr<scn::hot_reload_manager> hot;
	std::shared_ptr<scn::context_manager> mgr;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtMeshDocument, mesh_document_fixture)

BOOST_AUTO_TEST_CASE(Open_APolyFile_HoldsItsMeshAndABakedPreview)
{
	open_crate();

	BOOST_REQUIRE_MESSAGE(document->is_open(), document->last_error());
	BOOST_TEST(document->kind() == "mesh");
	BOOST_TEST(document->title() == "crate");
	BOOST_TEST(!document->is_dirty());
	BOOST_TEST(document->preview_problem().empty());
	BOOST_TEST(document->stats().vertices == 8u);
	BOOST_TEST(document->stats().faces == 6u);
	BOOST_TEST(document->stats().triangles == 12u);
	BOOST_TEST(document->geometry_file().view() == "res://shapes/crate.geom.desc");
	BOOST_TEST(document->preview_geometry().view().starts_with("memory://editor/mesh/"));
	BOOST_TEST(document->can_save());
}

BOOST_AUTO_TEST_CASE(Open_TheGeometryFile_OpensItsPolyFileBeside)
{
	document->open(res::tag{ "res://shapes/crate.geom.desc" });

	BOOST_REQUIRE_MESSAGE(document->is_open(), document->last_error());
	BOOST_TEST(document->source().view() == "res://shapes/crate.poly.desc");
}

BOOST_AUTO_TEST_CASE(Open_AGeometryFileWithoutASource_IsRefusedAndSaysWhy)
{
	document->open(res::tag{ "res://shapes/imported.geom.desc" });

	BOOST_TEST(!document->is_open());
	BOOST_TEST(document->last_error().find("has no source to edit") != std::string::npos);
	document->open(res::tag{ "res://shapes/crate.desc" });
	BOOST_TEST(!document->is_open());
	BOOST_TEST(!document->last_error().empty());
}

BOOST_AUTO_TEST_CASE(Open_AFileThatIsNoMesh_IsRefusedAndNothingIsOpen)
{
	write(res_root, "shapes/bad.poly.desc", "{ this is not json");
	document->open(res::tag{ "res://shapes/bad.poly.desc" });

	BOOST_TEST(!document->is_open());
	BOOST_TEST(document->last_error().find("bad.poly.desc") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Edit_ChangesThePreviewAndTheDirtyMark_AndABackEditCleansIt)
{
	open_crate();
	const std::uint64_t before = document->generation();

	BOOST_REQUIRE(move_first_vertex());
	BOOST_TEST(document->is_dirty());
	BOOST_TEST(document->generation() > before);
	BOOST_TEST(document->display_title() == "crate [*]");
	BOOST_REQUIRE(!changed.empty());
	BOOST_TEST(changed.back().first == 9u);

	BOOST_REQUIRE(document->edit([&](poly_mesh& mesh) {
		const poly_mesh original = [&] {
			poly_mesh_desc source;
			boost::json::object json = boost::json::parse(box_text).as_object();
			poly_mesh_from_json(json, source);
			return source.mesh;
		}();
		mesh.set_position(mesh.vertices().begin()->first, original.vertices().begin()->second.position);
		return true;
	}));
	BOOST_TEST(!document->is_dirty());

	BOOST_TEST(!document->edit([](poly_mesh&) { return false; }));
}

BOOST_AUTO_TEST_CASE(Save_WritesTheSourceAndTheGeometryPair_AndTheDocumentIsClean)
{
	open_crate();
	BOOST_REQUIRE(move_first_vertex());

	const edt::save_result saved = document->save();

	BOOST_REQUIRE_MESSAGE(saved.saved, saved.error);
	BOOST_TEST(!document->is_dirty());
	BOOST_TEST(!document->geometry_stale());
	BOOST_TEST(text_of(res_root / "shapes/crate.poly.desc") != box_text);
	BOOST_TEST(fs::exists(res_root / "shapes/crate.geom.desc"));
	BOOST_TEST(fs::exists(res_root / "shapes/crate.geom.bin"));
	BOOST_TEST(document->save().saved);
}

BOOST_AUTO_TEST_CASE(Save_WhenTheGeometryFails_KeepsTheDocumentDirty_AndSaysTheGameGeometryIsStale)
{
	open_crate();
	std::string failure = "the disk is full";
	document->set_geometry_writer([&failure](res::resource_system& system, const res::tag& file, const rnd::geometry_desc& geometry) {
		return failure.empty() ? write_mesh_geometry(system, file, geometry) : failure;
	});
	BOOST_REQUIRE(move_first_vertex());

	const edt::save_result first = document->save();

	BOOST_TEST(!first.saved);
	BOOST_TEST(document->is_dirty());
	BOOST_TEST(document->geometry_stale());
	BOOST_TEST(text_of(res_root / "shapes/crate.poly.desc") != box_text);
	BOOST_TEST(!fs::exists(res_root / "shapes/crate.geom.desc"));
	BOOST_TEST(document->stale_geometry_message().find("was saved") != std::string::npos);
	BOOST_TEST(document->stale_geometry_message().find("The game still loads the old geometry") != std::string::npos);
	BOOST_TEST(document->stale_geometry_message().find("press Save again") != std::string::npos);
	BOOST_TEST(document->stale_geometry_message().find("the disk is full") != std::string::npos);
	BOOST_TEST(first.error == document->stale_geometry_message());

	failure.clear();
	const edt::save_result second = document->save();
	BOOST_REQUIRE_MESSAGE(second.saved, second.error);
	BOOST_TEST(!document->is_dirty());
	BOOST_TEST(document->stale_geometry_message().empty());
	BOOST_TEST(fs::exists(res_root / "shapes/crate.geom.desc"));
}

BOOST_AUTO_TEST_CASE(Save_IsRefusedWithTheReason_AndNothingIsWritten)
{
	open_crate();
	BOOST_REQUIRE(move_first_vertex());

	blocked = "a migration runs";
	edt::save_result refused = document->save();
	BOOST_TEST(!refused.saved);
	BOOST_TEST(refused.error == "a migration runs");
	blocked.reset();

	document->set_lock_source([] { return true; });
	refused = document->save();
	BOOST_TEST(!refused.saved);
	BOOST_TEST(refused.error.find("Stop Play") != std::string::npos);
	BOOST_TEST(!move_first_vertex());
	document->set_lock_source({});

	BOOST_TEST(text_of(res_root / "shapes/crate.poly.desc") == box_text);
	BOOST_TEST(!fs::exists(res_root / "shapes/crate.geom.desc"));
}

BOOST_AUTO_TEST_CASE(Save_AFileOfALowerLayer_IsRefused)
{
	write(lower, "shapes/deep.poly.desc", box_text);
	document->open(res::tag{ "res://shapes/deep.poly.desc" });
	BOOST_REQUIRE_MESSAGE(document->is_open(), document->last_error());
	BOOST_REQUIRE(move_first_vertex());

	const edt::save_result refused = document->save();

	BOOST_TEST(!refused.saved);
	BOOST_TEST(refused.error.find("lower layer") != std::string::npos);
	BOOST_TEST(!fs::exists(res_root / "shapes/deep.poly.desc"));
}

BOOST_AUTO_TEST_CASE(Save_AFileChangedOnDisk_IsRefused_UntilTheDocumentIsRevertedOrOverwrites)
{
	open_crate();
	write(res_root, "shapes/crate.poly.desc", box_text + "\n");
	BOOST_REQUIRE(move_first_vertex());

	const edt::save_result refused = document->save();

	BOOST_TEST(!refused.saved);
	BOOST_TEST(refused.error.find("changed on disk") != std::string::npos);
	BOOST_TEST(document->changed_on_disk());

	const edt::save_result forced = document->overwrite();
	BOOST_REQUIRE_MESSAGE(forced.saved, forced.error);
	BOOST_TEST(!document->changed_on_disk());
	BOOST_TEST(!document->is_dirty());
}

BOOST_AUTO_TEST_CASE(Revert_TakesTheFileAgain_AndDropsTheEdits)
{
	open_crate();
	BOOST_REQUIRE(move_first_vertex());

	BOOST_TEST(document->revert().empty());

	BOOST_TEST(!document->is_dirty());
	BOOST_TEST(document->stats().triangles == 12u);
}

BOOST_AUTO_TEST_CASE(AMeshWithNoTriangle_HasAPreviewProblem_AndIsNotSaved)
{
	open_crate();
	BOOST_REQUIRE(document->edit([](poly_mesh& mesh) {
		const std::vector<face_id> faces = [&] {
			std::vector<face_id> ids;
			for (const auto& entry : mesh.faces()) ids.push_back(entry.first);
			return ids;
		}();
		for (const face_id id : faces) mesh.remove_face(id);
		return true;
	}));

	BOOST_TEST(!document->preview_problem().empty());
	const edt::save_result refused = document->save();
	BOOST_TEST(!refused.saved);
	BOOST_TEST(refused.error.find("nothing was written") != std::string::npos);
	BOOST_TEST(text_of(res_root / "shapes/crate.poly.desc") == box_text);
}

BOOST_AUTO_TEST_CASE(SaveAll_WritesADirtyMeshDocument_AndListsItWithTheOtherUnsavedDocuments)
{
	edt::scene_document scene;
	edt::document_manager documents{ scene, events };
	open_crate();
	BOOST_REQUIRE(move_first_vertex());
	auto owned = std::move(document);
	edt::document& adopted = documents.adopt(std::move(owned));

	const std::vector<edt::unsaved_item> unsaved = edt::list_unsaved(documents);
	BOOST_REQUIRE_EQUAL(unsaved.size(), 1u);
	BOOST_TEST(unsaved.front().kind == "mesh");
	BOOST_REQUIRE_EQUAL(edt::unsaved_groups(unsaved).size(), 1u);
	BOOST_TEST(edt::unsaved_groups(unsaved).front().heading == "Mesh");

	const edt::save_all_report report = edt::save_all_write(documents, {});

	BOOST_REQUIRE_EQUAL(report.saved.size(), 1u);
	BOOST_TEST(report.saved.front().id == adopted.id());
	BOOST_TEST(report.failed.empty());
	BOOST_TEST(!adopted.is_dirty());
	BOOST_TEST(text_of(res_root / "shapes/crate.poly.desc") != box_text);
}

BOOST_AUTO_TEST_CASE(Overwrite_WhenTheGeometryFails_KeepsTheWrittenSource_SoTheNextSaveIsNotRefused)
{
	open_crate();
	std::string failure = "the disk is full";
	document->set_geometry_writer([&failure](res::resource_system& system, const res::tag& file, const rnd::geometry_desc& geometry) {
		return failure.empty() ? write_mesh_geometry(system, file, geometry) : failure;
	});
	write(res_root, "shapes/crate.poly.desc", box_text + "\n");
	BOOST_REQUIRE(move_first_vertex());
	BOOST_TEST(!document->save().saved);
	BOOST_REQUIRE(document->changed_on_disk());

	const edt::save_result forced = document->overwrite();

	BOOST_TEST(!forced.saved);
	BOOST_TEST(document->geometry_stale());
	BOOST_TEST(!document->changed_on_disk());
	failure.clear();
	const edt::save_result again = document->save();
	BOOST_REQUIRE_MESSAGE(again.saved, again.error);
	BOOST_TEST(!document->is_dirty());
}

BOOST_AUTO_TEST_CASE(Save_OverAGeometryFileThatIsNotMadeFromTheSource_IsRefusedAndNamesTheFile)
{
	const shape_asset_result sphere = build_shape_asset(sphere_params{ 0.5f, 8, 4 }, res::tag{ "res://shapes/crate.geom.desc" });
	BOOST_REQUIRE(sphere.files.has_value());
	BOOST_REQUIRE(write_mesh_geometry(*rs, res::tag{ "res://shapes/crate.geom.desc" }, sphere.files->data).empty());
	const std::string imported = text_of(res_root / "shapes/crate.geom.desc");
	open_crate();
	BOOST_REQUIRE(move_first_vertex());

	const edt::save_result refused = document->save();

	BOOST_TEST(!refused.saved);
	BOOST_TEST(refused.error.find("crate.geom.desc") != std::string::npos);
	BOOST_TEST(refused.error.find("nothing was written") != std::string::npos);
	BOOST_TEST(text_of(res_root / "shapes/crate.poly.desc") == box_text);
	BOOST_TEST(text_of(res_root / "shapes/crate.geom.desc") == imported);
}

BOOST_AUTO_TEST_CASE(Save_OverTheGeometryPairTheSourceMade_IsAllowed)
{
	const shape_asset_result box = build_shape_asset(box_params{}, res::tag{ "res://shapes/crate.geom.desc" });
	BOOST_REQUIRE(box.files.has_value());
	BOOST_REQUIRE(write_mesh_geometry(*rs, res::tag{ "res://shapes/crate.geom.desc" }, box.files->data).empty());
	open_crate();
	BOOST_REQUIRE(move_first_vertex());

	const edt::save_result saved = document->save();

	BOOST_TEST_REQUIRE(saved.saved, saved.error);
}

BOOST_AUTO_TEST_CASE(Save_TheGeometryFileOfALowerLayerOrOfTheOpenLevel_IsRefused)
{
	write(lower, "shapes/crate.geom.desc", "{}");
	open_crate();
	BOOST_REQUIRE(move_first_vertex());
	edt::save_result refused = document->save();
	BOOST_TEST(!refused.saved);
	BOOST_TEST(refused.error.find("lower layer") != std::string::npos);
	fs::remove(lower / "shapes/crate.geom.desc");

	open_files = { "shapes/crate.geom.desc" };
	refused = document->save();
	BOOST_TEST(!refused.saved);
	BOOST_TEST(refused.error.find("open level") != std::string::npos);
	BOOST_TEST(text_of(res_root / "shapes/crate.poly.desc") == box_text);
}

BOOST_AUTO_TEST_CASE(Close_ErasesThePreviewGeometry_AndTheDestructorDoesToo)
{
	open_crate();
	const res::tag preview = document->preview_geometry();
	BOOST_TEST(rs->exists(preview));

	document->close();
	BOOST_TEST(!rs->exists(preview));

	open_crate();
	const res::tag second = document->preview_geometry();
	BOOST_TEST(rs->exists(second));
	document.reset();
	BOOST_TEST(!rs->exists(second));
}

BOOST_AUTO_TEST_CASE(Save_AfterAFailedPair_AllowsATopologyChange_OverTheGeometryFileTheDocumentMade)
{
	open_crate();
	BOOST_REQUIRE(move_first_vertex());
	BOOST_REQUIRE(document->save().saved);
	std::string failure = "the disk is full";
	document->set_geometry_writer([&failure](res::resource_system& system, const res::tag& file, const rnd::geometry_desc& geometry) {
		return failure.empty() ? write_mesh_geometry(system, file, geometry) : failure;
	});
	BOOST_REQUIRE(document->edit([](poly_mesh& mesh) { return mesh.set_position(mesh.vertices().begin()->first, glm::vec3{ 7.f, 7.f, 7.f }); }));
	BOOST_REQUIRE(!document->save().saved);
	BOOST_REQUIRE(document->geometry_stale());
	BOOST_REQUIRE(document->edit([](poly_mesh& mesh) { return mesh.remove_face(mesh.faces().begin()->first); }));
	failure.clear();

	const edt::save_result saved = document->save();

	BOOST_TEST_REQUIRE(saved.saved, saved.error);
	BOOST_TEST(!document->geometry_stale());
}

BOOST_AUTO_TEST_CASE(Save_ASourceWithTwoMaterials_MayBeSavedOverItsOwnGeometry)
{
	poly_mesh_desc source;
	poly_mesh_from_json(boost::json::parse(box_text).as_object(), source);
	const face_id first = source.mesh.faces().begin()->first;
	const face moved = source.mesh.faces().begin()->second;
	BOOST_REQUIRE(source.mesh.remove_face(first));
	BOOST_REQUIRE(source.mesh.restore_face(first, moved.corners, 1));
	write(res_root, "shapes/two.poly.desc", poly_file_text(source));
	BOOST_REQUIRE(write_mesh_geometry(*rs, res::tag{ "res://shapes/two.geom.desc" }, bake(source.mesh).geometry).empty());
	document->open(res::tag{ "res://shapes/two.poly.desc" });
	BOOST_REQUIRE_MESSAGE(document->is_open(), document->last_error());
	BOOST_REQUIRE(move_first_vertex());

	const edt::save_result saved = document->save();

	BOOST_TEST_REQUIRE(saved.saved, saved.error);
}

// #1186: what autosave writes is the source Save writes; taking it changes neither the file nor the document.
BOOST_AUTO_TEST_CASE(AutosaveCapture_IsTheSourceSaveWrites_AndChangesNothing)
{
	open_crate();
	BOOST_REQUIRE(move_first_vertex());
	const std::uint64_t generation = document->generation();

	const edt::autosave_snapshot snapshot = document->autosave_capture();
	BOOST_REQUIRE_MESSAGE(snapshot.error.empty(), snapshot.error);
	const std::vector<std::byte> captured = edt::autosave_bytes(snapshot);
	BOOST_TEST(document->is_dirty());
	BOOST_TEST(document->generation() == generation);
	BOOST_TEST(text_of(res_root / "shapes/crate.poly.desc") == box_text);

	BOOST_REQUIRE(document->save().saved);
	BOOST_TEST(std::string(reinterpret_cast<const char*>(captured.data()), captured.size()) == text_of(res_root / "shapes/crate.poly.desc"));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(EdtMeshDocumentWorld, mesh_world_fixture)

BOOST_AUTO_TEST_CASE(Open_MakesAPreviewWorldWithItsOwnCamera_AndCloseDestroysIt)
{
	open_crate();
	BOOST_REQUIRE_MESSAGE(document->is_open(), document->last_error());

	BOOST_REQUIRE(document->registry() != nullptr);
	BOOST_TEST(!document->set_name().empty());
	BOOST_TEST(document->render_target().starts_with("memory://__mesh_camera_rt/"));
	BOOST_REQUIRE(document->camera() != nullptr);
	BOOST_TEST(mgr->find_preview("res://shapes/crate.poly.desc") != nullptr);

	BOOST_REQUIRE(move_first_vertex());
	BOOST_TEST(document->registry() != nullptr);

	document->close();
	BOOST_TEST(document->registry() == nullptr);
	BOOST_TEST(mgr->find_preview("res://shapes/crate.poly.desc") == nullptr);
}

BOOST_AUTO_TEST_CASE(TwoMeshes_HaveTheirOwnCamerasAndTargets)
{
	write(res_root, "shapes/second.poly.desc", box_text);
	edt::mesh_document other(services);
	other.set_files(layout());
	open_crate();
	other.open(res::tag{ "res://shapes/second.poly.desc" });
	BOOST_REQUIRE_MESSAGE(other.is_open(), other.last_error());

	BOOST_TEST(document->render_target() != other.render_target());
	BOOST_TEST(document->set_name() != other.set_name());
	BOOST_REQUIRE(other.camera() != nullptr);
	other.park_camera();
}

BOOST_AUTO_TEST_CASE(Open_AGeometryFileThatIsNotTheBakeOfTheSource_IsAMismatch_AndOverwriteGeometryReplacesIt)
{
	const shape_asset_result sphere = build_shape_asset(sphere_params{ 0.5f, 8, 4 }, res::tag{ "res://shapes/crate.geom.desc" });
	BOOST_REQUIRE(sphere.files.has_value());
	BOOST_REQUIRE(write_mesh_geometry(*rs, res::tag{ "res://shapes/crate.geom.desc" }, sphere.files->data).empty());

	open_crate();

	BOOST_REQUIRE(document->geometry_mismatch());
	BOOST_TEST(document->geometry_mismatch_message().find("Overwrite geometry") != std::string::npos);
	BOOST_TEST(document->geometry_mismatch_message().find("crate.geom.desc") != std::string::npos);
	BOOST_TEST(!document->is_dirty());
	BOOST_REQUIRE(move_first_vertex());
	const edt::save_result refused = document->save();
	BOOST_TEST(!refused.saved);
	BOOST_TEST(refused.error.find("Overwrite geometry") != std::string::npos);
	BOOST_TEST(text_of(res_root / "shapes/crate.poly.desc") == box_text);

	const edt::save_result forced = document->overwrite_geometry();

	BOOST_TEST_REQUIRE(forced.saved, forced.error);
	BOOST_TEST(!document->geometry_mismatch());
	BOOST_TEST(!document->is_dirty());
	BOOST_TEST(text_of(res_root / "shapes/crate.poly.desc") != box_text);
}

BOOST_AUTO_TEST_CASE(OverwriteGeometry_OfAnUneditedSource_WritesOnlyTheGeometry)
{
	const shape_asset_result sphere = build_shape_asset(sphere_params{ 0.5f, 8, 4 }, res::tag{ "res://shapes/crate.geom.desc" });
	BOOST_REQUIRE(sphere.files.has_value());
	BOOST_REQUIRE(write_mesh_geometry(*rs, res::tag{ "res://shapes/crate.geom.desc" }, sphere.files->data).empty());
	const std::string foreign = text_of(res_root / "shapes/crate.geom.desc");
	open_crate();
	BOOST_REQUIRE(document->geometry_mismatch());

	BOOST_TEST(document->save().saved);
	BOOST_TEST(text_of(res_root / "shapes/crate.geom.desc") == foreign);

	const edt::save_result forced = document->overwrite_geometry();

	BOOST_TEST_REQUIRE(forced.saved, forced.error);
	BOOST_TEST(text_of(res_root / "shapes/crate.geom.desc") != foreign);
	BOOST_TEST(text_of(res_root / "shapes/crate.poly.desc") == box_text);
	BOOST_TEST(!document->geometry_mismatch());
}

BOOST_AUTO_TEST_CASE(Open_TheGeometryTheSourceMade_AndNoGeometryAtAll_AreNoMismatch)
{
	open_crate();
	BOOST_TEST(!document->geometry_mismatch());
	document->close();

	const shape_asset_result box = build_shape_asset(box_params{}, res::tag{ "res://shapes/crate.geom.desc" });
	BOOST_REQUIRE(box.files.has_value());
	BOOST_REQUIRE(write_mesh_geometry(*rs, res::tag{ "res://shapes/crate.geom.desc" }, box.files->data).empty());
	open_crate();
	BOOST_TEST(!document->geometry_mismatch());
	BOOST_TEST(document->geometry_mismatch_message().empty());
}

BOOST_AUTO_TEST_CASE(OneSource_IsOneDocument_WhetherItWasOpenedAsThePolyFileOrTheGeometryFile)
{
	edt::scene_document scene;
	edt::document_manager documents{ scene, events };
	edt::asset_kind_registry kinds;
	const edt::registration made = kinds.add_preview({ .id = "mesh", .suffix = ".poly.desc",
		.make_preview = [this](edt::editor_services& given) -> std::unique_ptr<edt::previewable_document> {
			auto created = std::make_unique<edt::mesh_document>(given);
			created->set_files(layout());
			return created;
		}, .multiple = true });
	documents.set_preview_kinds(kinds, services);
	edt::previewable_document* opened = documents.open_preview("mesh", res::tag{ "res://shapes/crate.poly.desc" });
	BOOST_REQUIRE(opened && opened->is_open());

	BOOST_TEST(edt::mesh_of_source(documents, res::tag{ "res://shapes/crate.poly.desc" }) == opened);
	BOOST_TEST(edt::mesh_of_source(documents, res::tag{ "res://shapes/crate.geom.desc" }) == opened);
	BOOST_TEST(edt::mesh_of_source(documents, res::tag{ "res://shapes/other.geom.desc" }) == nullptr);
	BOOST_TEST(edt::meshes_of(documents).size() == 1u);
}

BOOST_AUTO_TEST_CASE(Close_GivesUpTheRenderTargetsAndTheWorldTemplate)
{
	std::vector<res::tag> released;
	document->set_targets_release([&released](const std::vector<res::tag>& tags) { released = tags; });
	open_crate();
	const res::tag world_template = document->world_template();
	BOOST_TEST(rs->exists(world_template));
	const std::string color = document->render_target();

	document->close();

	BOOST_TEST(!rs->exists(world_template));
	BOOST_REQUIRE_EQUAL(released.size(), 4u);
	BOOST_TEST(released.front().view() == color);
	for (const res::tag& tag : released) BOOST_TEST(tag.view().starts_with("memory://__mesh_"));
}

BOOST_AUTO_TEST_CASE(TheGameLayerChange_DropsThePreviewWorld_AndKeepsTheEditsUntilItIsMadeAgain)
{
	open_crate();
	BOOST_REQUIRE(move_first_vertex());
	BOOST_REQUIRE(document->registry() != nullptr);

	document->release_preview_world();

	BOOST_TEST(document->registry() == nullptr);
	BOOST_TEST(mgr->find_preview("res://shapes/crate.poly.desc") == nullptr);
	BOOST_TEST(document->is_open());
	BOOST_TEST(document->is_dirty());
	BOOST_TEST(document->stats().triangles == 12u);

	document->rebuild_preview_world();

	BOOST_TEST(document->registry() != nullptr);
	BOOST_TEST(document->camera() != nullptr);
	BOOST_TEST(document->is_dirty());
}

BOOST_AUTO_TEST_CASE(TheOverwriteQuestion_NamesBothWritesForAnEditedSource_AndSaysTheFileIsNotOursForAMismatch)
{
	open_crate();
	const std::string clean = document->overwrite_geometry_question();
	BOOST_TEST(clean.find("writes the geometry") != std::string::npos);
	BOOST_TEST(clean.find("crate.geom.desc") != std::string::npos);
	BOOST_TEST(clean.find("not made from this source") == std::string::npos);

	BOOST_REQUIRE(move_first_vertex());
	const std::string edited = document->overwrite_geometry_question();
	BOOST_TEST(edited.find("two files") != std::string::npos);
	BOOST_TEST(edited.find("crate.poly.desc") != std::string::npos);
	BOOST_TEST(edited.find("crate.geom.desc") != std::string::npos);
	document->close();

	const shape_asset_result sphere = build_shape_asset(sphere_params{ 0.5f, 8, 4 }, res::tag{ "res://shapes/crate.geom.desc" });
	BOOST_REQUIRE(sphere.files.has_value());
	BOOST_REQUIRE(write_mesh_geometry(*rs, res::tag{ "res://shapes/crate.geom.desc" }, sphere.files->data).empty());
	open_crate();
	BOOST_REQUIRE(document->geometry_mismatch());
	const std::string foreign = document->overwrite_geometry_question();
	BOOST_TEST(foreign.find("not made from this source") != std::string::npos);
	BOOST_TEST(foreign.find("it is replaced") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ViewProjection_NeedsTheCamerasSize_AndPutsTheMeshInTheMiddleOfTheImage)
{
	open_crate();
	BOOST_REQUIRE_MESSAGE(document->is_open(), document->last_error());
	scn::camera_component* camera = document->camera();
	BOOST_REQUIRE(camera != nullptr);
	camera->m_viewport.size = decltype(camera->m_viewport.size){};
	BOOST_TEST(!document->view_projection().has_value());

	camera->m_viewport.size = decltype(camera->m_viewport.size){ 200, 200 };
	const std::optional<glm::mat4> matrix = document->view_projection();
	BOOST_REQUIRE(matrix.has_value());
	const edt::mesh::screen_view screen(*matrix, { 0.f, 0.f }, { 200.f, 200.f });
	const std::optional<glm::vec2> middle = screen.project({ 0.f, 0.f, 0.f });
	BOOST_REQUIRE(middle.has_value());
	BOOST_TEST(middle->x == 100.f, boost::test_tools::tolerance(1e-2f));
	BOOST_TEST(middle->y == 100.f, boost::test_tools::tolerance(1e-2f));
	const std::optional<glm::vec2> above = screen.project({ 0.f, 0.5f, 0.f });
	BOOST_REQUIRE(above.has_value());
	BOOST_TEST(above->y < middle->y);

	document->park_camera();
	BOOST_TEST(!document->view_projection().has_value());
}

BOOST_AUTO_TEST_CASE(TheCameraController_CanBeTurnedOff_AndBackOn)
{
	open_crate();
	BOOST_REQUIRE_MESSAGE(document->is_open(), document->last_error());
	scn::mouse_controller_component* controller = document->camera_controller();
	BOOST_REQUIRE(controller != nullptr);
	BOOST_TEST(controller->input_enabled);
	controller->input_enabled = false;
	BOOST_TEST(!document->camera_controller()->input_enabled);
	document->camera_controller()->input_enabled = true;
	BOOST_TEST(document->camera_controller()->input_enabled);
}

BOOST_AUTO_TEST_CASE(TheView_IsEditorState_AnEditThatRemovesAFacePrunesIt_AndARevertForgetsTheSelection)
{
	open_crate();
	BOOST_REQUIRE_MESSAGE(document->is_open(), document->last_error());
	const edt::mesh::poly_mesh& working = *document->working();
	edt::mesh::mesh_view& view = document->view();
	view.set_mode(edt::mesh::select_mode::face, working);
	const auto first = working.faces().begin()->first;
	const auto second = std::next(working.faces().begin())->first;
	view.selection().apply_face(first, edt::mesh::select_op::replace);
	view.selection().apply_face(second, edt::mesh::select_op::add);
	BOOST_REQUIRE_EQUAL(view.selection().size(), 2u);
	BOOST_TEST(!document->is_dirty());

	BOOST_REQUIRE(document->edit([first](edt::mesh::poly_mesh& mesh) { return mesh.remove_face(first); }));
	const edt::mesh::mesh_selection& after = view.current_selection(*document->working());
	BOOST_TEST(after.size() == 1u);
	BOOST_TEST(!after.faces().contains(first));
	BOOST_TEST(after.faces().contains(second));

	BOOST_REQUIRE(document->revert().empty());
	BOOST_TEST(view.selection().empty());
	BOOST_TEST((view.mode() == edt::mesh::select_mode::face));
}

BOOST_AUTO_TEST_SUITE_END()
