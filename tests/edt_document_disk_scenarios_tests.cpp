#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "edt_data_document.h"
#include "edt_mesh_document.h"
#include "edt_mesh_shape_asset.h"
#include "edt_prefab_document.h"
#include "edt_scene_document.h"
#include "edt_shader_source_document.h"
#include <snk/desc/desc_schema.h>
#include <snk/desc/desc_system.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_hot_reload_manager.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <boost/json.hpp>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// #1400 AC 2: the file documents answer one scenario alike. The scene, data, prefab, mesh and shader source documents share disk_state and
// write_gate (controller/edt_document_parts.h); each row of the table below runs the same steps through its own document and expects the same
// reasons and the same flag and announcement behaviour. Where a document differs on purpose, the row says so.

namespace {

namespace fs = std::filesystem;
namespace json = boost::json;
using namespace edt::mesh;

constexpr const char* k_config_schema = R"({
	"name": "test.config", "kind": "desc", "is_document": true,
	"fields": [ { "name": "speed", "kind": "f32", "default": 1.0 } ]
})";

constexpr const char* k_preview_template = R"({
	"__type": "world_desc", "__version": 5, "name": "__prefab_preview", "systems": [],
	"components": { "anchor": { "__type": "anchor_desc", "__version": 1 } },
	"children": [
		{ "__id": "09705f78a49797f0", "name": "PrefabLight",
		  "components": { "directional_light_desc": { "__type": "directional_light_desc", "__version": 1 } } }
	]
})";

constexpr const char* k_prefab = R"({
	"__type": "prefab_desc", "__version": 2,
	"components": { "stat": { "__type": "stat_desc", "hp": 10 } },
	"children": [
		{ "__id": "00000000000000a1", "name": "arm", "components": { "stat": { "__type": "stat_desc", "hp": 3 } } }
	]
})";

constexpr const char* k_frag = "#version 450 core\nvoid main() {}\n";

// One document seen through the calls every file document has in common.
struct subject
{
	std::string name;
	edt::document* document = nullptr;
	// The project's file, relative to res/, and what somebody else writes over it.
	std::string file;
	std::string outside;
	res::tag tag;
	std::function<void()> open;
	std::function<bool()> edit;
	std::function<bool()> flagged;
	// Takes the file as it is: the error text, empty when it worked.
	std::function<std::string()> reload;
	std::function<edt::save_result()> overwrite;
	// A file of a lower layer: the text the document refuses it with (at open, or at save).
	std::function<std::string()> lower_layer_refusal;
	// The data document tells its window again for every signal of the file, even for a version it flagged already; the others tell it once.
	bool announces_a_repeat = false;
};

struct disk_scenarios_fixture : test_util::scene_env
{
	disk_scenarios_fixture()
		: test_util::scene_env(test_util::test_descs::with)
	{
		for (const char* folder : { "prefabs", "shapes", "shaders" }) {
			fs::create_directories(res_root / folder);
			fs::create_directories(lower / folder);
		}
		rs->unregistrate_resolver(res::tag::default_protocol());
		rs->registrate_resolver<res::vfs_resolver>(res::tag::default_protocol(), std::vector<fs::path>{ lower, res_root },
			res::mount_access::read_write, res::write_policy::allow_all());
		auto& edt_ram = rs->registrate_resolver<test_util::ram_resolver>("edt");
		edt_ram.store(res::tag("edt://templates/prefab_preview.desc"), test_util::to_bytes(k_preview_template));
		descs->register_schema(desc::parse_type_schema(json::parse(k_config_schema).as_object()));

		hot = std::make_shared<scn::hot_reload_manager>(*assembler, *rs);
		mgr = std::make_shared<scn::context_manager>(*rs, *assembler, sf, hot);
		services.ctx_manager = mgr;
		services.base_descs = descs.get();
		services.base_resources = rs.get();
		services.base_assembler = assembler.get();
		services.base_sfactory = &sf;

		const shape_asset_result box = build_shape_asset(box_params{}, res::tag{ "res://shapes/crate.geom.desc" });
		BOOST_REQUIRE_MESSAGE(box.files.has_value(), box.error);
		box_text = box.files->poly;
		write(res_root, "config.desc", R"({"__type":"test.config","speed":2.0})");
		write(res_root, "prefabs/a.prefab.desc", k_prefab);
		write(res_root, "shapes/crate.poly.desc", box_text);
		write(res_root, "shaders/scene.frag", k_frag);

		data = std::make_unique<edt::data_document>(services, layout());
		prefab = std::make_unique<edt::prefab_document>(services);
		mesh = std::make_unique<edt::mesh_document>(services);
		shader = std::make_unique<edt::shader_source_document>(services);
		prefab->set_files(layout());
		mesh->set_files(layout());
		shader->set_files(layout());
		edt::document_id next = 20;
		for (edt::document* each : std::vector<edt::document*>{ data.get(), prefab.get(), mesh.get(), shader.get() }) {
			each->assign_id(next++);
			each->bind_events(&events);
		}
	}

	~disk_scenarios_fixture()
	{
		shader.reset();
		mesh.reset();
		prefab.reset();
		data.reset();
		rs->unregistrate_resolver(res::tag::default_protocol());
	}

	edt::desc_file_layout layout()
	{
		edt::desc_file_layout out;
		out.top_layer = [this] { return res_root; };
		out.layers = [this] { return std::vector<fs::path>{ lower, res_root }; };
		out.open_level_files = [] { return std::vector<std::string>{}; };
		out.write_blocker = [this] { return blocked; };
		return out;
	}

	static void write(const fs::path& root, std::string_view name, std::string_view content)
	{
		fs::create_directories((root / std::string(name)).parent_path());
		std::ofstream out(root / std::string(name), std::ios::binary);
		out << content;
	}

	std::string text_of(const std::string& file) const
	{
		std::ifstream in(res_root / file, std::ios::binary);
		return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	}

	// Each document is opened on its own file; the same calls then work on all of them.
	std::vector<subject> subjects()
	{
		std::vector<subject> out;

		subject d;
		d.name = "data";
		d.document = data.get();
		d.file = "config.desc";
		d.outside = R"({"__type":"test.config","speed":4.0})";
		d.tag = res::tag{ "res://config.desc" };
		d.open = [this, tag = d.tag] { BOOST_REQUIRE(data->open(tag).opened); };
		d.edit = [this] {
			return data->edit([](json::value& shown) {
				shown.as_object()["speed"] = 9.0;
				return true;
			});
		};
		d.flagged = [this] { return data->changed_on_disk(); };
		d.reload = [this] {
			const edt::open_result opened = data->reload();
			return opened.opened ? std::string{} : opened.error;
		};
		d.overwrite = [this] { return data->save_over(); };
		d.announces_a_repeat = true;
		d.lower_layer_refusal = [this] {
			write(lower, "engine.desc", R"({"__type":"test.config","speed":8.0})");
			return edt::data_document(services, layout()).open(res::tag{ "res://engine.desc" }).error;
		};
		out.push_back(std::move(d));

		subject p;
		p.name = "prefab";
		p.document = prefab.get();
		p.file = "prefabs/a.prefab.desc";
		p.outside = R"({"__type":"prefab_desc","__version":5,"children":[{"__id":"00000000000000a1","name":"other"}]})";
		p.tag = res::tag::make("prefabs/a.prefab.desc");
		p.open = [this, tag = p.tag] {
			prefab->open(tag);
			BOOST_REQUIRE(prefab->is_open());
		};
		p.edit = [this] {
			return prefab->edit([](scn::prefab_desc& copy) {
				copy.get_root().children[0].name = "arm2";
				return true;
			});
		};
		p.flagged = [this] { return prefab->changed_on_disk(); };
		p.reload = [this] { return prefab->reload(); };
		p.overwrite = [this] { return prefab->overwrite(); };
		p.lower_layer_refusal = [this] {
			write(lower, "prefabs/low.prefab.desc", k_prefab);
			prefab->close();
			prefab->open(res::tag::make("prefabs/low.prefab.desc"));
			BOOST_REQUIRE(prefab->is_open());
			BOOST_REQUIRE(prefab->edit([](scn::prefab_desc& copy) {
				copy.get_root().children[0].name = "arm2";
				return true;
			}));
			return prefab->save().error;
		};
		out.push_back(std::move(p));

		subject m;
		m.name = "mesh";
		m.document = mesh.get();
		m.file = "shapes/crate.poly.desc";
		m.outside = box_text + "\n";
		m.tag = res::tag{ "res://shapes/crate.poly.desc" };
		m.open = [this, tag = m.tag] {
			mesh->open(tag);
			BOOST_REQUIRE_MESSAGE(mesh->is_open(), mesh->last_error());
		};
		m.edit = [this] {
			return mesh->edit([](poly_mesh& edited) {
				edited.set_position(edited.vertices().begin()->first, glm::vec3{ 5.f, 5.f, 5.f });
				return true;
			});
		};
		m.flagged = [this] { return mesh->changed_on_disk(); };
		m.reload = [this] { return mesh->reload(); };
		m.overwrite = [this] { return mesh->overwrite(); };
		m.lower_layer_refusal = [this] {
			write(lower, "shapes/deep.poly.desc", box_text);
			mesh->close();
			mesh->open(res::tag{ "res://shapes/deep.poly.desc" });
			BOOST_REQUIRE_MESSAGE(mesh->is_open(), mesh->last_error());
			BOOST_REQUIRE(mesh->edit([](poly_mesh& edited) {
				edited.set_position(edited.vertices().begin()->first, glm::vec3{ 5.f, 5.f, 5.f });
				return true;
			}));
			return mesh->save().error;
		};
		out.push_back(std::move(m));

		subject s;
		s.name = "shader";
		s.document = shader.get();
		s.file = "shaders/scene.frag";
		s.outside = "somebody else\n";
		s.tag = res::tag{ "res://shaders/scene.frag" };
		s.open = [this, tag = s.tag] {
			shader->open(tag);
			BOOST_REQUIRE_MESSAGE(shader->is_open(), shader->last_error());
		};
		s.edit = [this] { return shader->set_text("mine\n"); };
		s.flagged = [this] { return shader->changed_on_disk(); };
		s.reload = [this] { return shader->revert(); };
		s.overwrite = [this] { return shader->overwrite(); };
		// A shader source of a lower layer opens read-only and says why, instead of being refused at save.
		s.lower_layer_refusal = [this] {
			write(lower, "shaders/engine.frag", "lower\n");
			shader->close();
			shader->open(res::tag{ "res://shaders/engine.frag" });
			BOOST_REQUIRE_MESSAGE(shader->is_open(), shader->last_error());
			BOOST_TEST(!shader->save().saved);
			return shader->read_only_reason();
		};
		out.push_back(std::move(s));
		return out;
	}

	int announced_for(edt::document_id id) const
	{
		int count = 0;
		for (const auto& entry : changed) {
			if (entry.first == id) ++count;
		}
		return count;
	}

	test_util::temp_dir dir{ "edt_document_disk_scenarios" };
	const fs::path res_root = dir.path() / "res";
	const fs::path lower = dir.path() / "lower";
	ecs::system_factory sf;
	std::shared_ptr<scn::hot_reload_manager> hot;
	std::shared_ptr<scn::context_manager> mgr;
	edt::editor_services services;
	edt::editor_events events;
	std::vector<std::pair<edt::document_id, std::uint64_t>> changed;
	edt::connection seen = events.document_changed.subscribe([this](edt::document_id id, std::uint64_t generation) { changed.push_back({ id, generation }); });
	std::string box_text;
	std::optional<std::string> blocked;
	std::unique_ptr<edt::data_document> data;
	std::unique_ptr<edt::prefab_document> prefab;
	std::unique_ptr<edt::mesh_document> mesh;
	std::unique_ptr<edt::shader_source_document> shader;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtDocumentDiskScenarios, disk_scenarios_fixture)

BOOST_AUTO_TEST_CASE(AFileChangedOutside_RefusesTheSave_WithTheSameReason_AndFlagsTheDocument)
{
	for (subject& one : subjects()) {
		BOOST_TEST_CONTEXT(one.name) {
			one.open();
			BOOST_REQUIRE(one.edit());
			write(res_root, one.file, one.outside);

			const edt::save_result refused = one.document->save();

			BOOST_TEST(!refused.saved);
			BOOST_TEST(refused.error.find("changed on disk") != std::string::npos);
			BOOST_TEST(text_of(one.file) == one.outside);
			BOOST_TEST(one.document->is_dirty());
			BOOST_TEST(one.flagged());
		}
	}
}

BOOST_AUTO_TEST_CASE(Reload_TakesTheFileAndDropsTheEdits_AndClearsTheFlag)
{
	for (subject& one : subjects()) {
		BOOST_TEST_CONTEXT(one.name) {
			one.open();
			BOOST_REQUIRE(one.edit());
			write(res_root, one.file, one.outside);
			BOOST_REQUIRE(!one.document->save().saved);
			BOOST_REQUIRE(one.flagged());

			BOOST_TEST(one.reload().empty());

			BOOST_TEST(!one.document->is_dirty());
			BOOST_TEST(!one.flagged());
			BOOST_TEST(text_of(one.file) == one.outside);
		}
	}
}

BOOST_AUTO_TEST_CASE(Overwrite_WritesTheEditsOverTheOutsideVersion_AndClearsTheFlag)
{
	for (subject& one : subjects()) {
		BOOST_TEST_CONTEXT(one.name) {
			one.open();
			BOOST_REQUIRE(one.edit());
			write(res_root, one.file, one.outside);
			BOOST_REQUIRE(!one.document->save().saved);

			const edt::save_result forced = one.overwrite();

			BOOST_REQUIRE_MESSAGE(forced.saved, forced.error);
			BOOST_TEST(!one.document->is_dirty());
			BOOST_TEST(!one.flagged());
			BOOST_TEST(text_of(one.file) != one.outside);
		}
	}
}

BOOST_AUTO_TEST_CASE(ADirtyDocumentWhoseFileIsSignalled_IsFlaggedAndAnnounced_TheSameVersionAgainAnnouncesNothingMoreExceptForData)
{
	for (subject& one : subjects()) {
		BOOST_TEST_CONTEXT(one.name) {
			one.open();
			BOOST_REQUIRE(one.edit());
			write(res_root, one.file, one.outside);
			const int before = announced_for(one.document->id());

			rs->signal_changed(one.tag);
			pump();

			BOOST_TEST(one.flagged());
			BOOST_TEST(announced_for(one.document->id()) == before + 1);
			BOOST_TEST(one.document->is_dirty());

			rs->signal_changed(one.tag);
			pump();

			BOOST_TEST(one.flagged());
			BOOST_TEST(announced_for(one.document->id()) == before + (one.announces_a_repeat ? 2 : 1));
		}
	}
}

BOOST_AUTO_TEST_CASE(ACleanDocumentWhoseFileIsSignalled_TakesTheNewFileAtOnce_WithoutAFlag)
{
	for (subject& one : subjects()) {
		BOOST_TEST_CONTEXT(one.name) {
			one.open();
			write(res_root, one.file, one.outside);

			rs->signal_changed(one.tag);
			pump();

			BOOST_TEST(!one.flagged());
			BOOST_TEST(!one.document->is_dirty());
		}
	}
}

BOOST_AUTO_TEST_CASE(AWriteBlocker_RefusesTheSaveWithItsReason_NothingIsWritten_AndTheSaveGoesThroughOnceItIsLifted)
{
	for (subject& one : subjects()) {
		BOOST_TEST_CONTEXT(one.name) {
			one.open();
			const std::string before = text_of(one.file);
			BOOST_REQUIRE(one.edit());
			blocked = "a project migration is running";

			const edt::save_result refused = one.document->save();

			BOOST_TEST(!refused.saved);
			BOOST_TEST(refused.error.find("a project migration is running") != std::string::npos);
			BOOST_TEST(text_of(one.file) == before);
			BOOST_TEST(!one.flagged());

			blocked.reset();
			const edt::save_result saved = one.document->save();
			BOOST_REQUIRE_MESSAGE(saved.saved, saved.error);
			BOOST_TEST(text_of(one.file) != before);
		}
	}
}

BOOST_AUTO_TEST_CASE(AFileOfALowerLayer_IsNeverWritten_AndTheReasonSaysSo)
{
	for (subject& one : subjects()) {
		BOOST_TEST_CONTEXT(one.name) {
			one.open();
			const std::string reason = one.lower_layer_refusal();
			// The shader source says "layer below" as its read-only reason; the others "lower layer" in the refusal.
			BOOST_TEST((reason.find("lower layer") != std::string::npos || reason.find("layer below") != std::string::npos));
			BOOST_TEST(!fs::exists(res_root / "engine.desc"));
			BOOST_TEST(!fs::exists(res_root / "prefabs/low.prefab.desc"));
			BOOST_TEST(!fs::exists(res_root / "shapes/deep.poly.desc"));
			BOOST_TEST(!fs::exists(res_root / "shaders/engine.frag"));
		}
	}
}

// The scene is edited by the level controller, which re-reads the level file itself: its document only holds the flag and the serial, and its
// serial moves with every flag (data moves only for a version not flagged last; prefab, mesh and shader keep none).
BOOST_AUTO_TEST_CASE(TheScene_FlagsEveryOutsideVersion_AndItsSerialMovesWithEachFlag)
{
	edt::scene_document scene;
	BOOST_TEST(!scene.changed_on_disk());
	const std::uint64_t start = scene.disk_change_serial();

	scene.flag_changed_on_disk();
	BOOST_TEST(scene.changed_on_disk());
	BOOST_TEST(scene.disk_change_serial() == start + 1);

	scene.flag_changed_on_disk();
	BOOST_TEST(scene.disk_change_serial() == start + 2);
}

BOOST_AUTO_TEST_SUITE_END()
