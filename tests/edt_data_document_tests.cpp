#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "edt_data_document.h"
#include "edt_data_document_panel.h"
#include "edt_data_document_view.h"
#include <snk/desc/desc_schema.h>
#include <snk/desc/desc_system.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <boost/json.hpp>
#include <imgui.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// data_document (#791): `own` and what the __parent chain gives are kept apart, and a save writes `own` only.
namespace {

namespace fs = std::filesystem;
namespace json = boost::json;

constexpr const char* k_config_schema = R"({
	"name": "test.config", "kind": "desc", "is_document": true,
	"fields": [
		{ "name": "speed", "kind": "f32", "default": 1.0 },
		{ "name": "goals", "kind": "array", "element": "i32", "default": [1, 2] },
		{ "name": "min", "key": "/limits/min", "kind": "f32", "default": 0.0 }
	]
})";

constexpr const char* k_plain_schema = R"({
	"name": "test.plain", "kind": "desc",
	"fields": [ { "name": "speed", "kind": "f32", "default": 1.0 } ]
})";

struct document_fixture : test_util::scene_env
{
	document_fixture()
		: test_util::scene_env(test_util::test_descs::without)
	{
		fs::create_directories(res_root);
		fs::create_directories(lower);
		rs->unregistrate_resolver(res::tag::default_protocol());
		rs->registrate_resolver<res::vfs_resolver>(res::tag::default_protocol(), std::vector<fs::path>{ lower, res_root },
			res::mount_access::read_write, res::write_policy::allow_all());
		descs->register_schema(desc::parse_type_schema(json::parse(k_config_schema).as_object()));
		descs->register_schema(desc::parse_type_schema(json::parse(k_plain_schema).as_object()));
		services.base_descs = descs.get();
		services.base_resources = rs.get();
	}

	~document_fixture() { rs->unregistrate_resolver(res::tag::default_protocol()); }

	edt::desc_file_layout layout()
	{
		edt::desc_file_layout out;
		out.top_layer = [this] { return res_root; };
		out.layers = [this] { return std::vector<fs::path>{ lower, res_root }; };
		out.open_level_files = [this] { return open_level; };
		return out;
	}

	edt::data_document make() { return edt::data_document(services, layout()); }

	res::tag write(const fs::path& root, std::string_view name, std::string_view content) const
	{
		std::ofstream out(root / std::string(name), std::ios::binary);
		out << content;
		return res::tag{ "res://" + std::string(name) };
	}

	res::tag put(std::string_view name, std::string_view content) const { return write(res_root, name, content); }

	std::string text_of(std::string_view name) const
	{
		std::ifstream in(res_root / std::string(name), std::ios::binary);
		return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	}

	json::object parsed(std::string_view name) const { return json::parse(text_of(name)).as_object(); }

	test_util::temp_dir dir{ "edt_data_document" };
	const fs::path res_root = dir.path() / "res";
	const fs::path lower = dir.path() / "lower";
	std::vector<std::string> open_level;
	edt::editor_services services;
};

constexpr const char* k_parent = R"({"__type":"test.config","speed":2.0,"goals":[5,6],"limits":{"min":5.0}})";
constexpr const char* k_child = R"({"__type":"test.config","__guid":"guid-1","__parent":"res://parent.desc","speed":3.0})";

}

namespace edt {

struct data_document_test_access
{
	static void record_change(data_document& document, const boost::json::object& before, const boost::json::object& after)
	{
		document.record_change(before, after);
	}
};

}

BOOST_FIXTURE_TEST_SUITE(EdtDataDocumentTests, document_fixture)

BOOST_AUTO_TEST_CASE(Open_ShowsTheInheritedValuesUnderTheOwnOnes)
{
	put("parent.desc", k_parent);
	const res::tag child = put("child.desc", k_child);
	edt::data_document document = make();
	const edt::open_result opened = document.open(child);
	BOOST_REQUIRE_MESSAGE(opened.opened, opened.error);

	BOOST_TEST(document.kind() == "data");
	BOOST_TEST(document.title() == "child");
	BOOST_TEST(document.type() == "test.config");
	BOOST_TEST(!document.is_dirty());
	const json::value shown = document.shown();
	BOOST_TEST(shown.at("speed").to_number<double>() == 3.0);
	BOOST_TEST(shown.at("limits").at("min").to_number<double>() == 5.0);
	BOOST_TEST(shown.at("goals").as_array().size() == 2u);
	BOOST_TEST(document.base().at("speed").to_number<double>() == 2.0);
}

BOOST_AUTO_TEST_CASE(Save_AfterOneEdit_WritesOnlyThatFieldAndKeepsTheParentAndTheGuid)
{
	put("parent.desc", k_parent);
	const res::tag child = put("child.desc", k_child);
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(child).opened);

	const std::uint64_t before = document.generation();
	BOOST_TEST(document.edit([](json::value& shown) {
		shown.as_object()["limits"].as_object()["min"] = 7.0;
		return true;
	}));
	BOOST_TEST(document.generation() > before);
	BOOST_TEST(document.is_dirty());

	const edt::save_result saved = document.save();
	BOOST_REQUIRE_MESSAGE(saved.saved, saved.error);
	BOOST_TEST(!document.is_dirty());

	const json::object file = parsed("child.desc");
	BOOST_TEST(std::string(file.at("__type").as_string()) == "test.config");
	BOOST_TEST(std::string(file.at("__parent").as_string()) == "res://parent.desc");
	BOOST_TEST(std::string(file.at("__guid").as_string()) == "guid-1");
	BOOST_TEST(file.at("__version").to_number<int>() == 1);
	BOOST_TEST(file.at("speed").to_number<double>() == 3.0);
	BOOST_TEST(file.at("limits").at("min").to_number<double>() == 7.0);
	BOOST_TEST(!file.contains("goals"));
}

BOOST_AUTO_TEST_CASE(Save_WithoutChanges_LeavesTheFileAsItIs)
{
	put("parent.desc", k_parent);
	const res::tag child = put("child.desc", k_child);
	const std::string before = text_of("child.desc");
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(child).opened);

	BOOST_TEST(document.save().saved);
	BOOST_TEST(text_of("child.desc") == before);
}

BOOST_AUTO_TEST_CASE(EditBackToTheInheritedValue_DropsTheFieldFromTheFile)
{
	put("parent.desc", k_parent);
	const res::tag child = put("child.desc", k_child);
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(child).opened);

	BOOST_TEST(document.edit([](json::value& shown) {
		shown.as_object()["speed"] = 2.0;
		return true;
	}));
	BOOST_REQUIRE(document.save().saved);
	BOOST_TEST(!parsed("child.desc").contains("speed"));
}

BOOST_AUTO_TEST_CASE(Save_OfAValueTheSchemaRefuses_WritesNothingAndNamesTheField)
{
	const res::tag file = put("config.desc", R"({"__type":"test.config","speed":2.0})");
	const std::string before = text_of("config.desc");
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(file).opened);

	json::object broken = document.own();
	broken["speed"] = "fast";
	BOOST_TEST(document.restore_own(std::move(broken)));
	BOOST_TEST(document.is_dirty());
	const edt::save_result saved = document.save();
	BOOST_TEST(!saved.saved);
	BOOST_TEST(saved.error.find("speed") != std::string::npos);
	BOOST_TEST(text_of("config.desc") == before);
	BOOST_TEST(document.is_dirty());
}

BOOST_AUTO_TEST_CASE(Save_OfAFileThatChangedOnDisk_IsRefused)
{
	const res::tag file = put("config.desc", R"({"__type":"test.config","speed":2.0})");
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(file).opened);
	BOOST_TEST(document.edit([](json::value& shown) {
		shown.as_object()["speed"] = 9.0;
		return true;
	}));

	const std::string elsewhere = R"({"__type":"test.config","speed":4.0})";
	put("config.desc", elsewhere);
	const edt::save_result saved = document.save();
	BOOST_TEST(!saved.saved);
	BOOST_TEST(saved.error.find("changed on disk") != std::string::npos);
	BOOST_TEST(text_of("config.desc") == elsewhere);
	BOOST_TEST(document.is_dirty());
}

BOOST_AUTO_TEST_CASE(TheChainIsMergedFromTheFarthestParentToTheNearest)
{
	put("grand.desc", R"({"__type":"test.config","speed":1.5,"limits":{"min":2.0},"goals":[9]})");
	put("parent.desc", R"({"__type":"test.config","__parent":"res://grand.desc","speed":2.5})");
	const res::tag child = put("child.desc", R"({"__type":"test.config","__parent":"res://parent.desc"})");
	edt::data_document document = make();
	const edt::open_result opened = document.open(child);
	BOOST_REQUIRE_MESSAGE(opened.opened, opened.error);

	const json::value shown = document.shown();
	BOOST_TEST(shown.at("speed").to_number<double>() == 2.5);
	BOOST_TEST(shown.at("limits").at("min").to_number<double>() == 2.0);
	BOOST_TEST(shown.at("goals").as_array().size() == 1u);
	BOOST_TEST(!document.inherited().contains("__type"));
	BOOST_TEST(!document.inherited().contains("__parent"));
}

BOOST_AUTO_TEST_CASE(AParentMayLiveInALowerLayer_TheDocumentItselfMayNot)
{
	write(lower, "engine.desc", R"({"__type":"test.config","speed":8.0})");
	const res::tag child = put("child.desc", R"({"__type":"test.config","__parent":"res://engine.desc"})");
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(child).opened);
	BOOST_TEST(document.shown().at("speed").to_number<double>() == 8.0);

	const res::tag engine_file{ "res://engine.desc" };
	const edt::open_result refused = make().open(engine_file);
	BOOST_TEST(!refused.opened);
	BOOST_TEST(refused.error.find("lower layer") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Open_RefusesWhatIsNoDocumentNoChainOrNoProjectFile)
{
	edt::data_document document = make();

	const auto refused = [&](const res::tag& file, std::string_view fragment) {
		BOOST_TEST_CONTEXT(std::string(fragment)) {
			const edt::open_result opened = document.open(file);
			BOOST_TEST(!opened.opened);
			BOOST_TEST(opened.error.find(fragment) != std::string::npos);
			BOOST_TEST(!document.is_open());
		}
	};
	refused(put("plain.desc", R"({"__type":"test.plain"})"), "not a data document type");
	refused(put("unknown.desc", R"({"__type":"test.nothing"})"), "has no schema");
	refused(put("typeless.desc", R"({"speed":1})"), "/__type");
	refused(put("missing.desc", R"({"__type":"test.config","__parent":"res://nowhere.desc"})"), "/__parent");
	put("b.desc", R"({"__type":"test.config","__parent":"res://a.desc"})");
	refused(put("a.desc", R"({"__type":"test.config","__parent":"res://b.desc"})"), "cycle");
	put("other.desc", R"({"__type":"test.plain"})");
	refused(put("typed.desc", R"({"__type":"test.config","__parent":"res://other.desc"})"), "another type");
	refused(put("user.desc", R"({"__type":"test.config","__parent":"user://p.desc"})"), "res://");
	refused(put("mem.desc", R"({"__type":"test.config","__parent":"memory://t/p.desc"})"), "res://");
	refused(put("up.desc", R"({"__type":"test.config","__parent":"res://../outside.desc"})"), "/__parent");
	refused(res::tag{ "memory://t/doc.desc" }, "only res://");
	refused(res::tag{ "res://nothing.desc" }, "no file");
}

BOOST_AUTO_TEST_CASE(TheOpenLevelsFile_IsNotADataDocument)
{
	const res::tag level = put("level.desc", R"({"__type":"test.config","speed":1.0})");
	open_level = { "level.desc" };
	const edt::open_result opened = make().open(level);
	BOOST_TEST(!opened.opened);
	BOOST_TEST(opened.error.find("open level") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(AFileOverTheLimit_IsRefusedBeforeItIsRead)
{
	std::string big = R"({"__type":"test.config","goals":[)";
	while (big.size() < edt::k_desc_file_limit + 16) big += "1,";
	big += "1]}";
	const res::tag file = put("big.desc", big);
	const edt::open_result opened = make().open(file);
	BOOST_TEST(!opened.opened);
	BOOST_TEST(opened.error.find("bytes") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(AFileOfAnOlderVersion_IsMigratedInMemoryAndWrittenInTheCurrentOne)
{
	descs->register_migration("test.config", 1, [](json::object& object) {
		if (auto velocity = object.find("velocity"); velocity != object.end()) {
			json::value moved = std::move(velocity->value());
			object.erase(velocity);
			object["speed"] = std::move(moved);
		}
	});
	const std::string old_text = R"({"__type":"test.config","velocity":4.0})";
	const res::tag file = put("old.desc", old_text);
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(file).opened);

	BOOST_TEST(document.shown().at("speed").to_number<double>() == 4.0);
	BOOST_TEST(text_of("old.desc") == old_text);
	BOOST_TEST(!document.is_dirty());

	BOOST_TEST(document.edit([](json::value& shown) {
		shown.as_object()["speed"] = 5.0;
		return true;
	}));
	BOOST_REQUIRE(document.save().saved);
	const json::object saved = parsed("old.desc");
	BOOST_TEST(saved.at("__version").to_number<int>() == 2);
	BOOST_TEST(!saved.contains("velocity"));
	BOOST_TEST(saved.at("speed").to_number<double>() == 5.0);
}

BOOST_AUTO_TEST_CASE(RestoreOwn_BringsASnapshotBackAndTracksTheSavedState)
{
	const res::tag file = put("config.desc", R"({"__type":"test.config","speed":2.0})");
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(file).opened);
	const json::object snapshot = document.own();

	BOOST_TEST(document.edit([](json::value& shown) {
		shown.as_object()["speed"] = 6.0;
		return true;
	}));
	BOOST_TEST(document.is_dirty());
	BOOST_TEST(document.restore_own(snapshot));
	BOOST_TEST(!document.is_dirty());
	BOOST_TEST(document.own().at("speed").to_number<double>() == 2.0);
}

BOOST_AUTO_TEST_CASE(RestoreOwn_RefusesASnapshotOfAnotherTypeOrParent)
{
	put("parent.desc", k_parent);
	const res::tag child = put("child.desc", k_child);
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(child).opened);
	const json::object before = document.own();

	json::object other_parent = before;
	other_parent["__parent"] = "res://elsewhere.desc";
	BOOST_TEST(!document.restore_own(other_parent));
	json::object no_parent = before;
	no_parent.erase("__parent");
	BOOST_TEST(!document.restore_own(no_parent));
	json::object other_type = before;
	other_type["__type"] = "test.plain";
	BOOST_TEST(!document.restore_own(other_type));
	BOOST_CHECK(document.own() == before);
}

BOOST_AUTO_TEST_CASE(Save_OfAFileThatBecameTheOpenLevels_IsRefused)
{
	const res::tag file = put("config.desc", R"({"__type":"test.config","speed":2.0})");
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(file).opened);
	BOOST_TEST(document.edit([](json::value& shown) {
		shown.as_object()["speed"] = 9.0;
		return true;
	}));
	open_level = { "config.desc" };
	const edt::save_result saved = document.save();
	BOOST_TEST(!saved.saved);
	BOOST_TEST(saved.error.find("open level") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(AParentThatChanges_GivesANewBaseAndAnAnnouncement)
{
	const res::tag parent = put("parent.desc", k_parent);
	const res::tag child = put("child.desc", k_child);
	edt::editor_events events;
	int announced = 0;
	const edt::connection heard = events.document_changed.subscribe([&](edt::document_id, std::uint64_t) { ++announced; });
	edt::data_document document = make();
	document.bind_events(&events);
	BOOST_REQUIRE(document.open(child).opened);
	const std::uint64_t before = document.generation();

	put("parent.desc", R"({"__type":"test.config","speed":6.0,"limits":{"min":9.0}})");
	rs->signal_changed(parent);
	pump();

	BOOST_TEST(announced == 1);
	BOOST_TEST(document.generation() > before);
	BOOST_TEST(document.base().at("speed").to_number<double>() == 6.0);
	const json::value shown = document.shown();
	BOOST_TEST(shown.at("speed").to_number<double>() == 3.0);
	BOOST_TEST(shown.at("limits").at("min").to_number<double>() == 9.0);
	BOOST_TEST(!document.is_dirty());
}

BOOST_AUTO_TEST_CASE(AParentSignalledWithoutAChange_AnnouncesNothing)
{
	const res::tag parent = put("parent.desc", k_parent);
	const res::tag child = put("child.desc", k_child);
	edt::editor_events events;
	int announced = 0;
	const edt::connection heard = events.document_changed.subscribe([&](edt::document_id, std::uint64_t) { ++announced; });
	edt::data_document document = make();
	document.bind_events(&events);
	BOOST_REQUIRE(document.open(child).opened);
	const std::uint64_t before = document.generation();

	rs->signal_changed(parent);
	pump();
	BOOST_TEST(announced == 0);
	BOOST_TEST(document.generation() == before);
}

BOOST_AUTO_TEST_CASE(AParentThatBreaks_KeepsTheLastBaseAndSaysWhy_AndRecovers)
{
	const res::tag parent = put("parent.desc", k_parent);
	const res::tag child = put("child.desc", k_child);
	edt::editor_events events;
	int announced = 0;
	const edt::connection heard = events.document_changed.subscribe([&](edt::document_id, std::uint64_t) { ++announced; });
	edt::data_document document = make();
	document.bind_events(&events);
	BOOST_REQUIRE(document.open(child).opened);

	put("parent.desc", R"({"__type":"test.plain","speed":6.0})");
	rs->signal_changed(parent);
	pump();
	BOOST_TEST(!document.chain_problem().empty());
	BOOST_TEST(document.chain_problem().find("another type") != std::string::npos);
	BOOST_TEST(document.base().at("speed").to_number<double>() == 2.0);
	BOOST_TEST(announced == 1);

	put("parent.desc", R"({"__type":"test.config","speed":6.0})");
	rs->signal_changed(parent);
	pump();
	BOOST_TEST(document.chain_problem().empty());
	BOOST_TEST(document.base().at("speed").to_number<double>() == 6.0);
	BOOST_TEST(announced == 2);
}

BOOST_AUTO_TEST_CASE(AParentThatGainsAParent_IsWatchedFromThenOn)
{
	const res::tag parent = put("parent.desc", k_parent);
	const res::tag grand = put("grand.desc", R"({"__type":"test.config","speed":1.5})");
	const res::tag child = put("child.desc", R"({"__type":"test.config","__parent":"res://parent.desc"})");
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(child).opened);

	put("parent.desc", R"({"__type":"test.config","__parent":"res://grand.desc"})");
	rs->signal_changed(parent);
	pump();
	BOOST_TEST(document.base().at("speed").to_number<double>() == 1.5);

	put("grand.desc", R"({"__type":"test.config","speed":4.5})");
	rs->signal_changed(grand);
	pump();
	BOOST_TEST(document.base().at("speed").to_number<double>() == 4.5);
}

BOOST_AUTO_TEST_CASE(TheDocumentsOwnSave_IsNoChangeOnDisk)
{
	put("parent.desc", k_parent);
	const res::tag child = put("child.desc", k_child);
	edt::editor_events events;
	int announced = 0;
	const edt::connection heard = events.document_changed.subscribe([&](edt::document_id, std::uint64_t) { ++announced; });
	edt::data_document document = make();
	document.bind_events(&events);
	BOOST_REQUIRE(document.open(child).opened);
	BOOST_TEST(document.edit([](json::value& shown) {
		shown.as_object()["speed"] = 4.0;
		return true;
	}));
	BOOST_REQUIRE(document.save().saved);
	pump();

	BOOST_TEST(announced == 0);
	BOOST_TEST(!document.changed_on_disk());
	BOOST_TEST(!document.is_dirty());
	BOOST_TEST(document.shown().at("speed").to_number<double>() == 4.0);
}

BOOST_AUTO_TEST_CASE(AFileChangedBySomeoneElse_IsReadAgainByACleanDocument)
{
	put("parent.desc", k_parent);
	const res::tag child = put("child.desc", k_child);
	edt::editor_events events;
	int announced = 0;
	const edt::connection heard = events.document_changed.subscribe([&](edt::document_id, std::uint64_t) { ++announced; });
	edt::data_document document = make();
	document.bind_events(&events);
	BOOST_REQUIRE(document.open(child).opened);

	put("child.desc", R"({"__type":"test.config","__guid":"guid-1","__parent":"res://parent.desc","speed":7.0})");
	rs->signal_changed(child);
	pump();

	BOOST_TEST(announced == 1);
	BOOST_TEST(!document.changed_on_disk());
	BOOST_TEST(document.shown().at("speed").to_number<double>() == 7.0);
	BOOST_TEST(!document.is_dirty());
}

BOOST_AUTO_TEST_CASE(AFileChangedUnderUnsavedEdits_IsFlaggedAndNotOverwritten)
{
	const res::tag file = put("config.desc", R"({"__type":"test.config","speed":2.0})");
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(file).opened);
	BOOST_TEST(document.edit([](json::value& shown) {
		shown.as_object()["speed"] = 9.0;
		return true;
	}));

	const std::string elsewhere = R"({"__type":"test.config","speed":4.0})";
	put("config.desc", elsewhere);
	rs->signal_changed(file);
	pump();

	BOOST_TEST(document.changed_on_disk());
	BOOST_TEST(document.is_dirty());
	BOOST_TEST(document.own().at("speed").to_number<double>() == 9.0);
	BOOST_TEST(!document.save().saved);
	BOOST_TEST(text_of("config.desc") == elsewhere);
}

BOOST_AUTO_TEST_CASE(ADestroyedDocument_LeavesNoWatcher)
{
	put("parent.desc", k_parent);
	const res::tag child = put("child.desc", k_child);
	const std::size_t before = rs->watched_tag_count();
	{
		edt::data_document document = make();
		BOOST_REQUIRE(document.open(child).opened);
		BOOST_TEST(rs->watched_tag_count() == before + 2u);
	}
	BOOST_TEST(rs->watched_tag_count() == before);
}

BOOST_AUTO_TEST_CASE(AParentAndTheChildSignalledTogether_BothReachTheDocument)
{
	const res::tag parent = put("parent.desc", k_parent);
	const res::tag child = put("child.desc", k_child);
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(child).opened);

	put("parent.desc", R"({"__type":"test.config","speed":6.0,"limits":{"min":9.0}})");
	put("child.desc", R"({"__type":"test.config","__guid":"guid-1","__parent":"res://parent.desc","speed":7.0})");
	rs->signal_changed(parent);
	rs->signal_changed(child);
	pump();

	BOOST_TEST(!document.changed_on_disk());
	const json::value shown = document.shown();
	BOOST_TEST(shown.at("speed").to_number<double>() == 7.0);
	BOOST_TEST(shown.at("limits").at("min").to_number<double>() == 9.0);
	BOOST_TEST(document.base().at("speed").to_number<double>() == 6.0);
}

BOOST_AUTO_TEST_CASE(AParentThatPointsAtAFileNotThereYet_IsWatchedUntilItAppears)
{
	const res::tag parent = put("parent.desc", k_parent);
	const res::tag child = put("child.desc", R"({"__type":"test.config","__parent":"res://parent.desc"})");
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(child).opened);

	put("parent.desc", R"({"__type":"test.config","__parent":"res://later.desc"})");
	rs->signal_changed(parent);
	pump();
	BOOST_TEST(!document.chain_problem().empty());
	BOOST_TEST(document.base().at("speed").to_number<double>() == 2.0);

	const res::tag later = put("later.desc", R"({"__type":"test.config","speed":4.0})");
	rs->signal_changed(later);
	pump();
	BOOST_TEST(document.chain_problem().empty());
	BOOST_TEST(document.base().at("speed").to_number<double>() == 4.0);
}

BOOST_AUTO_TEST_CASE(AFlaggedChangeOnDisk_BlocksTheSaveEvenIfTheFileIsBackAsItWas)
{
	const std::string original = R"({"__type":"test.config","speed":2.0})";
	const res::tag file = put("config.desc", original);
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(file).opened);
	BOOST_TEST(document.edit([](json::value& shown) {
		shown.as_object()["speed"] = 9.0;
		return true;
	}));

	put("config.desc", R"({"__type":"test.config","speed":4.0})");
	rs->signal_changed(file);
	pump();
	BOOST_REQUIRE(document.changed_on_disk());

	put("config.desc", original);
	const edt::save_result saved = document.save();
	BOOST_TEST(!saved.saved);
	BOOST_TEST(saved.error.find("changed on disk") != std::string::npos);
	BOOST_TEST(text_of("config.desc") == original);
}

BOOST_AUTO_TEST_CASE(TheWindowStatus_NamesTheFileItsParentsAndWhatIsWrongWithIt)
{
	const res::tag parent = put("parent.desc", k_parent);
	const res::tag child = put("child.desc", k_child);
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(child).opened);

	edt::data_document_status status = edt::status_of(document);
	BOOST_TEST(status.label == "child (test.config)");
	BOOST_TEST(status.path == "res://child.desc");
	BOOST_TEST(status.parents == "res://parent.desc");
	BOOST_TEST(!status.dirty);
	BOOST_TEST(status.can_save);
	BOOST_TEST(!status.changed_on_disk);
	BOOST_TEST(status.chain_problem.empty());

	BOOST_TEST(document.edit([](json::value& shown) {
		shown.as_object()["speed"] = 5.0;
		return true;
	}));
	BOOST_TEST(edt::status_of(document).dirty);

	put("parent.desc", R"({"__type":"test.plain"})");
	rs->signal_changed(parent);
	pump();
	status = edt::status_of(document);
	BOOST_TEST(!status.chain_problem.empty());
	BOOST_TEST(status.parents == "res://parent.desc");
}

BOOST_AUTO_TEST_CASE(TheWindowTitle_KeepsItsIdentityWhateverTheLabelSays)
{
	put("parent.desc", k_parent);
	const res::tag child = put("child.desc", k_child);
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(child).opened);

	BOOST_TEST(edt::data_document_panel_id(7) == "data_doc:7");
	BOOST_TEST(edt::data_document_window_title(document, 7) == "child (test.config)###data_doc:7");
}

BOOST_AUTO_TEST_CASE(Reload_TakesTheFileOnDiskAndDropsTheUnsavedEdits)
{
	const res::tag file = put("config.desc", R"({"__type":"test.config","speed":2.0})");
	edt::editor_events events;
	int announced = 0;
	const edt::connection heard = events.document_changed.subscribe([&](edt::document_id, std::uint64_t) { ++announced; });
	edt::data_document document = make();
	document.bind_events(&events);
	BOOST_REQUIRE(document.open(file).opened);
	BOOST_TEST(document.edit([](json::value& shown) {
		shown.as_object()["speed"] = 9.0;
		return true;
	}));
	put("config.desc", R"({"__type":"test.config","speed":4.0})");
	rs->signal_changed(file);
	pump();
	BOOST_REQUIRE(document.changed_on_disk());
	const int before = announced;

	const edt::open_result reloaded = document.reload();
	BOOST_REQUIRE_MESSAGE(reloaded.opened, reloaded.error);
	BOOST_TEST(!document.is_dirty());
	BOOST_TEST(!document.changed_on_disk());
	BOOST_TEST(document.shown().at("speed").as_double() == 4.0);
	BOOST_TEST(announced == before + 1);
}

BOOST_AUTO_TEST_CASE(Reload_OfAFileThatIsGone_LeavesTheDocumentAsItWas)
{
	const res::tag file = put("config.desc", R"({"__type":"test.config","speed":2.0})");
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(file).opened);
	BOOST_TEST(document.edit([](json::value& shown) {
		shown.as_object()["speed"] = 9.0;
		return true;
	}));
	fs::remove(res_root / "config.desc");

	BOOST_TEST(!document.reload().opened);
	BOOST_TEST(document.is_dirty());
	BOOST_TEST(document.shown().at("speed").as_double() == 9.0);
}

BOOST_AUTO_TEST_CASE(SaveOver_WritesOverTheFileThatChangedOnDisk)
{
	const res::tag file = put("config.desc", R"({"__type":"test.config","speed":2.0})");
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(file).opened);
	BOOST_TEST(document.edit([](json::value& shown) {
		shown.as_object()["speed"] = 9.0;
		return true;
	}));
	put("config.desc", R"({"__type":"test.config","speed":4.0})");
	rs->signal_changed(file);
	pump();
	BOOST_REQUIRE(document.changed_on_disk());
	BOOST_TEST(!document.save().saved);

	const edt::save_result saved = document.save_over();
	BOOST_REQUIRE_MESSAGE(saved.saved, saved.error);
	BOOST_TEST(parsed("config.desc").at("speed").as_double() == 9.0);
	BOOST_TEST(!document.is_dirty());
	BOOST_TEST(!document.changed_on_disk());
	BOOST_TEST(document.save().saved);
}

BOOST_AUTO_TEST_CASE(SaveOver_StillRunsEveryCheckOfASave_AndAFailureLeavesTheFlag)
{
	const res::tag file = put("config.desc", R"({"__type":"test.config","speed":2.0})");
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(file).opened);
	BOOST_TEST(document.edit([](json::value& shown) {
		shown.as_object()["speed"] = 9.0;
		return true;
	}));
	put("config.desc", R"({"__type":"test.config","speed":4.0})");
	rs->signal_changed(file);
	pump();
	BOOST_REQUIRE(document.changed_on_disk());
	const std::string on_disk = text_of("config.desc");

	open_level = { "config.desc" };
	BOOST_TEST(!document.save_over().saved);
	BOOST_TEST(text_of("config.desc") == on_disk);
	BOOST_TEST(document.changed_on_disk());
	BOOST_TEST(document.is_dirty());
}

BOOST_AUTO_TEST_CASE(ASaveThatFindsAnotherVersionOnDisk_FlagsTheDocumentBeforeTheWatcherSaysSo)
{
	const res::tag file = put("config.desc", R"({"__type":"test.config","speed":2.0})");
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(file).opened);
	BOOST_TEST(document.edit([](json::value& shown) {
		shown.as_object()["speed"] = 9.0;
		return true;
	}));
	put("config.desc", R"({"__type":"test.config","speed":4.0})");

	BOOST_TEST(!document.save().saved);
	BOOST_TEST(document.changed_on_disk());
}

BOOST_AUTO_TEST_CASE(SaveOver_OfACleanDocument_StillWrites_WhenTheFileBecameSomethingElse)
{
	const res::tag file = put("config.desc", R"({"__type":"test.config","speed":2.0})");
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(file).opened);
	put("config.desc", R"({"__type":"test.plain","speed":4.0})");
	rs->signal_changed(file);
	pump();
	BOOST_REQUIRE(document.changed_on_disk());
	BOOST_TEST(!document.reload().opened);

	const edt::save_result saved = document.save_over();
	BOOST_REQUIRE_MESSAGE(saved.saved, saved.error);
	BOOST_TEST(parsed("config.desc").at("__type").as_string() == "test.config");
	BOOST_TEST(!document.changed_on_disk());
	// the version of what was written is the base now: the next save finds nothing foreign
	BOOST_TEST(document.edit([](json::value& shown) {
		shown.as_object()["speed"] = 5.0;
		return true;
	}));
	BOOST_TEST(document.save().saved);
	BOOST_TEST(parsed("config.desc").at("speed").as_double() == 5.0);
}

BOOST_AUTO_TEST_CASE(SaveOver_CreatesAFileThatIsGone_AndWritesOverOneThatNoLongerParses)
{
	const res::tag file = put("config.desc", R"({"__type":"test.config","speed":2.0})");
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(file).opened);
	BOOST_TEST(document.edit([](json::value& shown) {
		shown.as_object()["speed"] = 9.0;
		return true;
	}));

	fs::remove(res_root / "config.desc");
	BOOST_TEST(!document.save().saved);
	BOOST_REQUIRE_MESSAGE(document.save_over().saved, "a gone file is created again");
	BOOST_TEST(parsed("config.desc").at("speed").as_double() == 9.0);

	BOOST_TEST(document.edit([](json::value& shown) {
		shown.as_object()["speed"] = 8.0;
		return true;
	}));
	put("config.desc", "{ not json");
	BOOST_TEST(!document.save().saved);
	BOOST_REQUIRE_MESSAGE(document.save_over().saved, "a file that does not parse is replaced");
	BOOST_TEST(parsed("config.desc").at("speed").as_double() == 8.0);
}

BOOST_AUTO_TEST_CASE(SaveOver_OfAGoneFile_StillRefusesTheOpenLevelsName)
{
	const res::tag file = put("config.desc", R"({"__type":"test.config","speed":2.0})");
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(file).opened);
	BOOST_TEST(document.edit([](json::value& shown) {
		shown.as_object()["speed"] = 9.0;
		return true;
	}));
	fs::remove(res_root / "config.desc");
	open_level = { "config.desc" };

	BOOST_TEST(!document.save_over().saved);
	BOOST_TEST(!fs::exists(res_root / "config.desc"));
}

BOOST_AUTO_TEST_CASE(TheOwnWriteOfASaveOver_IsNoChangeOnDisk)
{
	const res::tag file = put("config.desc", R"({"__type":"test.config","speed":2.0})");
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(file).opened);
	BOOST_TEST(document.edit([](json::value& shown) {
		shown.as_object()["speed"] = 9.0;
		return true;
	}));
	put("config.desc", R"({"__type":"test.config","speed":4.0})");
	rs->signal_changed(file);
	pump();
	BOOST_REQUIRE(document.changed_on_disk());

	BOOST_REQUIRE(document.save_over().saved);
	rs->signal_changed(file);
	pump();
	BOOST_TEST(!document.changed_on_disk());
}

BOOST_AUTO_TEST_CASE(DiskChangeSerial_MovesForANewOutsideVersionOnly)
{
	const res::tag file = put("config.desc", R"({"__type":"test.config","speed":2.0})");
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(file).opened);
	BOOST_TEST(document.edit([](json::value& shown) {
		shown.as_object()["speed"] = 9.0;
		return true;
	}));
	const std::uint64_t start = document.disk_change_serial();

	put("config.desc", R"({"__type":"test.config","speed":4.0})");
	rs->signal_changed(file);
	pump();
	const std::uint64_t first = document.disk_change_serial();
	BOOST_TEST(first == start + 1);

	rs->signal_changed(file);
	pump();
	BOOST_TEST(document.disk_change_serial() == first);

	put("config.desc", R"({"__type":"test.config","speed":6.0})");
	rs->signal_changed(file);
	pump();
	BOOST_TEST(document.disk_change_serial() == first + 1);
}

BOOST_AUTO_TEST_CASE(SaveOver_OfAGoneWorldOfTheOpenLevel_IsRefused)
{
	fs::create_directories(res_root / "worlds");
	fs::create_directories(res_root / "levels");
	const res::tag file = put("worlds/w.desc", R"({"__type":"test.config","speed":2.0})");
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(file).opened);
	BOOST_TEST(document.edit([](json::value& shown) {
		shown.as_object()["speed"] = 9.0;
		return true;
	}));
	fs::remove(res_root / "worlds" / "w.desc");
	put("levels/a.desc", R"({"__type":"level_desc","worlds":["worlds/w.desc"]})");
	open_level = { "levels/a.desc" };

	BOOST_TEST(!document.save_over().saved);
	BOOST_TEST(!fs::exists(res_root / "worlds" / "w.desc"));
}

BOOST_AUTO_TEST_CASE(DiskChangeSerial_TellsAGoneFileFromOneThatCameBackBroken)
{
	const res::tag file = put("config.desc", R"({"__type":"test.config","speed":2.0})");
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(file).opened);
	BOOST_TEST(document.edit([](json::value& shown) {
		shown.as_object()["speed"] = 9.0;
		return true;
	}));
	const std::uint64_t start = document.disk_change_serial();

	fs::remove(res_root / "config.desc");
	rs->signal_changed(file);
	pump();
	BOOST_TEST(document.disk_change_serial() == start + 1);

	put("config.desc", "{ broken");
	rs->signal_changed(file);
	pump();
	BOOST_TEST(document.disk_change_serial() == start + 2);

	put("config.desc", "{ broken again");
	rs->signal_changed(file);
	pump();
	BOOST_TEST(document.disk_change_serial() == start + 3);
}

BOOST_AUTO_TEST_CASE(TheWindowKeepsItsOwnMapState_UntilTheDocumentChangesUnderIt)
{
	struct imgui_context
	{
		imgui_context() : handle(ImGui::CreateContext()) {}
		~imgui_context() { ImGui::DestroyContext(handle); }
		ImGuiContext* handle;
	} context;
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.DisplaySize = ImVec2(800.f, 600.f);
	unsigned char* pixels = nullptr;
	int width = 0;
	int height = 0;
	io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

	const res::tag file = put("map_state.desc", R"({"__type":"test.config","speed":2.0})");
	edt::data_document document = make();
	const edt::open_result opened = document.open(file);
	BOOST_REQUIRE_MESSAGE(opened.opened, opened.error);
	edt::data_document_panel panel(document, services, 3);
	const auto frame = [&] {
		ImGui::NewFrame();
		panel.render();
		ImGui::EndFrame();
	};

	frame();
	panel.map_state().new_keys[7] = "typed";
	panel.map_state().rename_errors[7] = "refused";
	frame();
	BOOST_TEST(panel.map_state().new_keys.size() == 1u);
	BOOST_TEST(panel.map_state().rename_errors.size() == 1u);

	put("map_state.desc", R"({"__type":"test.config","speed":5.0})");
	BOOST_REQUIRE(document.reload().opened);
	frame();
	BOOST_TEST(panel.map_state().new_keys.empty());
	BOOST_TEST(panel.map_state().rename_errors.empty());
}

// Undo and redo (S56-5): one record per edit of the person, snapshots of `own`.
namespace {

bool set_speed(edt::data_document& document, double value)
{
	return document.edit([value](json::value& shown) {
		shown.as_object()["speed"] = value;
		return true;
	});
}

}

BOOST_AUTO_TEST_CASE(UndoAndRedo_PutOwnBackByteForByte)
{
	put("parent.desc", k_parent);
	const res::tag child = put("child.desc", k_child);
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(child).opened);
	const json::object original = document.own();

	BOOST_REQUIRE(set_speed(document, 7.0));
	document.commit_pending();
	const json::object edited = document.own();
	BOOST_REQUIRE(!(edited == original));

	BOOST_REQUIRE(document.undo());
	BOOST_TEST((document.own() == original));
	BOOST_REQUIRE(document.redo());
	BOOST_TEST((document.own() == edited));
	BOOST_TEST(!document.redo());
}

BOOST_AUTO_TEST_CASE(ADragIsOneRecord_WhateverNumberOfFramesItTook)
{
	put("parent.desc", k_parent);
	const res::tag child = put("child.desc", k_child);
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(child).opened);
	const json::object original = document.own();

	for (int frame = 1; frame <= 20; ++frame) {
		BOOST_REQUIRE(set_speed(document, 3.0 + frame));
	}
	BOOST_TEST(document.has_pending_edit());
	BOOST_TEST(document.history().size() == 0u);
	document.commit_pending();
	BOOST_TEST(!document.has_pending_edit());
	BOOST_TEST(document.history().size() == 1u);
	BOOST_REQUIRE(document.undo());
	BOOST_TEST((document.own() == original));
	BOOST_TEST(!document.undo());
}

BOOST_AUTO_TEST_CASE(OneEditOfSeveralPointers_IsOneRecord_AndAnEmptyParentObjectComesBack)
{
	put("parent.desc", k_parent);
	const res::tag child = put("child.desc", k_child);
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(child).opened);
	const json::object original = document.own();
	BOOST_REQUIRE(!original.contains("limits"));

	BOOST_REQUIRE(document.edit([](json::value& shown) {
		shown.as_object()["speed"] = 9.0;
		shown.as_object()["goals"] = json::array{ 1, 2, 3 };
		shown.at_pointer("/limits").as_object()["min"] = 1.5;
		return true;
	}));
	document.commit_pending();
	BOOST_TEST(document.history().size() == 1u);
	BOOST_TEST(document.own().contains("limits"));

	BOOST_REQUIRE(document.undo());
	BOOST_TEST((document.own() == original));
	BOOST_TEST(!document.own().contains("limits"));
}

BOOST_AUTO_TEST_CASE(TheEntryNamesTheEditedField_OrTheNumberOfFields)
{
	put("parent.desc", k_parent);
	const res::tag child = put("child.desc", k_child);
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(child).opened);

	BOOST_REQUIRE(set_speed(document, 7.0));
	document.commit_pending();
	const auto entry = document.next_undo_entry();
	BOOST_REQUIRE(entry.has_value());
	BOOST_TEST(entry->label == "Edit speed");
	BOOST_TEST((entry->origin.kind == cmd::origin_kind::ui));

	BOOST_REQUIRE(document.edit([](json::value& shown) {
		shown.as_object()["speed"] = 8.0;
		shown.as_object()["goals"] = json::array{ 4 };
		return true;
	}));
	document.commit_pending();
	BOOST_REQUIRE(document.next_undo_entry().has_value());
	BOOST_TEST(document.next_undo_entry()->label == "Edit 2 fields");
}

BOOST_AUTO_TEST_CASE(UndoTakesWhatIsPendingFirst_AndTheMenuSeesIt)
{
	put("parent.desc", k_parent);
	const res::tag child = put("child.desc", k_child);
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(child).opened);
	const json::object original = document.own();

	BOOST_TEST(!document.next_undo_entry().has_value());
	BOOST_REQUIRE(set_speed(document, 7.0));
	BOOST_REQUIRE(document.next_undo_entry().has_value());
	BOOST_TEST(document.next_undo_entry()->label == "Edit speed");
	BOOST_TEST(!document.next_redo_entry().has_value());

	BOOST_REQUIRE(document.undo());
	BOOST_TEST((document.own() == original));
	BOOST_TEST(document.next_redo_entry().has_value());
	BOOST_TEST(!document.has_pending_edit());
}

BOOST_AUTO_TEST_CASE(AnEditAfterAnUndo_DropsTheRedo)
{
	put("parent.desc", k_parent);
	const res::tag child = put("child.desc", k_child);
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(child).opened);

	BOOST_REQUIRE(set_speed(document, 7.0));
	document.commit_pending();
	BOOST_REQUIRE(document.undo());
	BOOST_REQUIRE(set_speed(document, 8.0));
	document.commit_pending();
	BOOST_TEST(!document.redo());
	BOOST_TEST(document.history().size() == 1u);
}

BOOST_AUTO_TEST_CASE(ASaveIsTheSavePoint_AnUndoAfterItMakesTheDocumentDirty_ARedoClean)
{
	put("parent.desc", k_parent);
	const res::tag child = put("child.desc", k_child);
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(child).opened);

	BOOST_REQUIRE(set_speed(document, 7.0));
	BOOST_TEST(document.is_dirty());
	// What is pending is recorded by the save itself.
	BOOST_REQUIRE(document.save().saved);
	BOOST_TEST(!document.is_dirty());
	BOOST_TEST(document.history().is_at_save_point());
	BOOST_TEST(document.history().size() == 1u);

	BOOST_REQUIRE(document.undo());
	BOOST_TEST(document.is_dirty());
	BOOST_TEST(!document.history().is_at_save_point());
	BOOST_REQUIRE(document.redo());
	BOOST_TEST(!document.is_dirty());
	BOOST_TEST(document.history().is_at_save_point());
}

BOOST_AUTO_TEST_CASE(AReload_IsABoundary_TheHistoryStartsAgain)
{
	put("parent.desc", k_parent);
	const res::tag child = put("child.desc", k_child);
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(child).opened);

	BOOST_REQUIRE(set_speed(document, 7.0));
	document.commit_pending();
	BOOST_REQUIRE(document.history().size() == 1u);
	BOOST_REQUIRE(document.reload().opened);
	BOOST_TEST(document.history().size() == 0u);
	BOOST_TEST(!document.undo());
	BOOST_TEST(!document.is_dirty());
	BOOST_TEST(!document.next_undo_entry().has_value());
}

BOOST_AUTO_TEST_CASE(SaveOver_KeepsTheHistory_AndMarksTheSavePoint)
{
	put("parent.desc", k_parent);
	const res::tag child = put("child.desc", k_child);
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(child).opened);
	const json::object original = document.own();

	BOOST_REQUIRE(set_speed(document, 7.0));
	document.commit_pending();
	put("child.desc", R"({"__type":"test.config","__guid":"guid-1","__parent":"res://parent.desc","speed":4.0})");
	BOOST_REQUIRE(document.save_over().saved);
	BOOST_TEST(document.history().size() == 1u);
	BOOST_TEST(document.history().is_at_save_point());
	BOOST_TEST(!document.is_dirty());

	BOOST_REQUIRE(document.undo());
	BOOST_TEST((document.own() == original));
	BOOST_TEST(document.is_dirty());
}

namespace
{
	constexpr const char* k_old_file = R"({"__type":"test.config","velocity":4.0})";

	// test.config gets a second version: "velocity" became "speed" (and the step says so).
	void register_rename(desc::desc_system& descs)
	{
		descs.register_migration("test.config", 1, "velocity is now speed", [](json::object& object, desc::migration_context& context) {
			if (const auto found = object.find("velocity"); found != object.end()) {
				object["speed"] = found->value();
				object.erase("velocity");
				context.warn("velocity was renamed to speed");
			}
		});
	}
}

BOOST_AUTO_TEST_CASE(AnOldFile_IsOpenedMigratedInMemory_AndTheFileIsNotTouched)
{
	register_rename(*descs);
	const res::tag file = put("old.desc", k_old_file);
	edt::data_document document = make();
	BOOST_REQUIRE_MESSAGE(document.open(file).opened, "an old file must open");

	BOOST_TEST(document.file_version() == 1);
	BOOST_TEST(document.current_version() == 2);
	BOOST_TEST(document.migrated());
	BOOST_TEST(document.needs_version_confirmation());
	BOOST_TEST(!document.is_dirty());
	BOOST_TEST(document.own().at("speed").to_number<double>() == 4.0);
	BOOST_TEST(!document.own().contains("velocity"));
	BOOST_REQUIRE(document.migration_notes().size() == 1u);
	BOOST_TEST(document.migration_notes().front().find("velocity was renamed") != std::string::npos);
	BOOST_TEST(text_of("old.desc") == k_old_file);

	const edt::data_document_status status = edt::status_of(document);
	BOOST_TEST(status.migrated);
	BOOST_TEST(status.migration_note.find("v1 -> v2") != std::string::npos);
	BOOST_TEST(status.migration_note.find("Migrate Project") == std::string::npos);
	BOOST_TEST(edt::status_of(document, true).migration_note.find("Project > Migrate Project") != std::string::npos);
	BOOST_TEST(status.migration_details.find("velocity was renamed") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ASaveOfAMigratedFile_NeedsAYes_EvenWhenClean_ThenWritesTheNewVersionOnce)
{
	register_rename(*descs);
	const res::tag file = put("old.desc", k_old_file);
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(file).opened);

	const edt::save_result refused = document.save();
	BOOST_TEST(!refused.saved);
	BOOST_TEST(refused.error.find("version 1") != std::string::npos);
	BOOST_TEST(refused.error.find("version 2") != std::string::npos);
	BOOST_TEST(text_of("old.desc") == k_old_file);

	document.confirm_version_write();
	BOOST_REQUIRE_MESSAGE(document.save().saved, "the confirmed save of a clean old file writes it");
	const json::object written = parsed("old.desc");
	BOOST_TEST(written.at("__version").to_number<int>() == 2);
	BOOST_TEST(written.at("speed").to_number<double>() == 4.0);
	BOOST_TEST(!written.contains("velocity"));
	BOOST_TEST(!document.migrated());
	BOOST_TEST(!document.needs_version_confirmation());
	BOOST_TEST(document.file_version() == 2);

	// the next edit and save go through without asking again
	BOOST_REQUIRE(set_speed(document, 6.0));
	BOOST_TEST(document.save().saved);
}

BOOST_AUTO_TEST_CASE(TheMigrationIsNoEdit_NothingIsRecorded_AndASaveLeavesTheHistoryAlone)
{
	register_rename(*descs);
	const res::tag file = put("old.desc", k_old_file);
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(file).opened);
	BOOST_TEST(document.history().size() == 0u);
	BOOST_TEST(!document.undo());

	BOOST_REQUIRE(set_speed(document, 5.0));
	document.commit_pending();
	document.confirm_version_write();
	BOOST_REQUIRE(document.save().saved);
	BOOST_TEST(document.history().size() == 1u);
	BOOST_TEST(!document.is_dirty());

	// an undo after the save works as before: the migrated state comes back, dirty against the file now of version 2
	BOOST_REQUIRE(document.undo());
	BOOST_TEST(document.own().at("speed").to_number<double>() == 4.0);
	BOOST_TEST(document.is_dirty());
	BOOST_TEST(!document.needs_version_confirmation());
	BOOST_TEST(document.save().saved);
}

BOOST_AUTO_TEST_CASE(TheYesOfTheVersionQuestion_IsForOneSave_AFailedSaveAsksAgain)
{
	register_rename(*descs);
	const res::tag file = put("old.desc", k_old_file);
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(file).opened);

	document.confirm_version_write();
	BOOST_TEST(!document.needs_version_confirmation());
	put("old.desc", R"({"__type":"test.config","velocity":9.0})");
	BOOST_TEST(!document.save().saved); // the file changed on disk: nothing is written
	BOOST_TEST(document.needs_version_confirmation());
}

BOOST_AUTO_TEST_CASE(ANestedObjectOfAnOlderVersion_MakesTheDocumentMigrated_EvenWhenTheRootIsCurrent)
{
	register_rename(*descs);
	descs->register_migration("test.plain", 1, [](json::object&) {});
	const res::tag outdated = put("nested_old.desc", R"({"__type":"test.config","__version":2,"inner":{"__type":"test.plain"}})");
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(outdated).opened);
	BOOST_TEST(document.file_version() == 2);
	BOOST_TEST(document.migrated());
	BOOST_TEST(document.nested_outdated());
	BOOST_TEST(document.needs_version_confirmation());
	BOOST_TEST(edt::status_of(document).migration_note.find("Outdated data") != std::string::npos);

	const res::tag current = put("nested_current.desc", R"({"__type":"test.config","__version":2,"inner":{"__type":"test.plain","__version":2}})");
	edt::data_document up_to_date = make();
	BOOST_REQUIRE(up_to_date.open(current).opened);
	BOOST_TEST(!up_to_date.migrated());
}

BOOST_AUTO_TEST_CASE(AnOutdatedParent_UnderACurrentChild_IsMigratedForTheViewAndNeverWritten)
{
	register_rename(*descs);
	const res::tag parent = put("old_parent.desc", k_old_file);
	const res::tag child = put("new_child.desc", R"({"__type":"test.config","__version":2,"__parent":"res://old_parent.desc"})");
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(child).opened);
	BOOST_TEST(!document.migrated());
	BOOST_TEST(document.shown().at_pointer("/speed").to_number<double>() == 4.0);

	BOOST_REQUIRE(set_speed(document, 6.0));
	BOOST_TEST(document.save().saved);
	BOOST_TEST(text_of("old_parent.desc") == k_old_file);
	BOOST_TEST(!parsed("new_child.desc").contains("velocity"));
	BOOST_TEST(parsed("new_child.desc").at("speed").to_number<double>() == 6.0);
	(void)parent;
}

BOOST_AUTO_TEST_CASE(AFileOfTheCurrentVersion_IsNotMigrated_AndSavesWithoutAQuestion)
{
	const res::tag file = put("current.desc", R"({"__type":"test.config","speed":2.0})");
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(file).opened);
	BOOST_TEST(!document.migrated());
	BOOST_TEST(document.migration_notes().empty());
	BOOST_TEST(edt::status_of(document).migration_note.empty());
	BOOST_TEST(document.save().saved); // clean: nothing to write, nothing to ask
	BOOST_REQUIRE(set_speed(document, 3.0));
	BOOST_TEST(document.save().saved);
}

BOOST_AUTO_TEST_CASE(ANewerFile_IsNotOpened_TheErrorNamesTheFileThePointerAndBothVersions)
{
	const res::tag file = put("newer.desc", R"({"__type":"test.config","__version":5,"speed":2.0})");
	edt::data_document document = make();
	const edt::open_result result = document.open(file);
	BOOST_TEST(!result.opened);
	BOOST_TEST(!result.unsuitable);
	BOOST_TEST(!document.is_open());
	BOOST_TEST(result.error.find("newer.desc") != std::string::npos);
	BOOST_TEST(result.error.find("/__version") != std::string::npos);
	BOOST_TEST(result.error.find("version 5") != std::string::npos);
	BOOST_TEST(result.error.find("knows (1)") != std::string::npos);
	BOOST_TEST(text_of("newer.desc").find("\"speed\"") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ANewerParent_AndANewerNestedObject_RefuseTheOpenToo)
{
	put("parent_new.desc", R"({"__type":"test.config","__version":7})");
	const res::tag child = put("child_of_new.desc", R"({"__type":"test.config","__parent":"res://parent_new.desc"})");
	edt::data_document with_parent = make();
	const edt::open_result parent_result = with_parent.open(child);
	BOOST_TEST(!parent_result.opened);
	BOOST_TEST(parent_result.error.find("parent_new.desc") != std::string::npos);

	const res::tag nested = put("nested_new.desc", R"({"__type":"test.config","extra":{"__type":"test.config","__version":9}})");
	edt::data_document with_nested = make();
	const edt::open_result nested_result = with_nested.open(nested);
	BOOST_TEST(!nested_result.opened);
	BOOST_TEST(nested_result.error.find("newer") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ARecordTheDocumentCannotTakeBack_IsRefused_DroppedAndNothingIsAnnounced)
{
	put("parent.desc", k_parent);
	const res::tag child = put("child.desc", k_child);
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(child).opened);
	const json::object before_edit = document.own();
	BOOST_REQUIRE(set_speed(document, 7.0));
	document.commit_pending();
	const json::object edited = document.own();

	for (const char* key : { "__parent", "__type" }) {
		BOOST_TEST_CONTEXT(key) {
			json::object foreign = before_edit;
			foreign[key] = "res://somewhere/else";
			edt::data_document_test_access::record_change(document, foreign, edited);
			const std::size_t records = document.history().size();
			const std::uint64_t generation = document.generation();

			BOOST_TEST(!document.undo());
			BOOST_TEST((document.own() == edited));
			BOOST_TEST(document.generation() == generation);
			BOOST_TEST(document.history().size() == records - 1u);
		}
	}
	BOOST_REQUIRE(document.undo());
	BOOST_TEST((document.own() == before_edit));

	json::object foreign = edited;
	foreign["__parent"] = "res://somewhere/else";
	edt::data_document_test_access::record_change(document, before_edit, foreign);
	BOOST_REQUIRE(document.undo());
	const std::uint64_t generation = document.generation();
	BOOST_TEST(!document.redo());
	BOOST_TEST((document.own() == before_edit));
	BOOST_TEST(document.generation() == generation);
	BOOST_TEST(!document.history().next_redo());
}

BOOST_AUTO_TEST_CASE(ADataDocumentHasNoAgentChangesAndIsNeverLocked)
{
	put("parent.desc", k_parent);
	const res::tag child = put("child.desc", k_child);
	edt::data_document document = make();
	BOOST_REQUIRE(document.open(child).opened);
	BOOST_TEST(!document.has_agent_changes());
	BOOST_TEST(!document.edits_locked());
}

BOOST_AUTO_TEST_SUITE_END()
