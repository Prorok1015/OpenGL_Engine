#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "edt_data_document.h"
#include <snk/desc/desc_schema.h>
#include <snk/desc/desc_system.h>
#include <snk/editor/edt_editor_services.h>
#include <boost/json.hpp>
#include <string>
#include <string_view>

// data_document (#791): `own` and what the __parent chain gives are kept apart, and a save writes `own` only.
namespace {

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
		descs->register_schema(desc::parse_type_schema(json::parse(k_config_schema).as_object()));
		descs->register_schema(desc::parse_type_schema(json::parse(k_plain_schema).as_object()));
		services.base_descs = descs.get();
		services.base_resources = rs.get();
	}

	std::string text_of(const res::tag& tag) const
	{
		const auto& bytes = ram->files.at(tag);
		return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
	}

	json::object parsed(const res::tag& tag) const { return json::parse(text_of(tag)).as_object(); }

	edt::editor_services services;
};

constexpr const char* k_parent = R"({"__type":"test.config","speed":2.0,"goals":[5,6],"limits":{"min":5.0}})";
constexpr const char* k_child = R"({"__type":"test.config","__guid":"guid-1","__parent":"memory://t/parent.desc","speed":3.0})";

}

BOOST_FIXTURE_TEST_SUITE(EdtDataDocumentTests, document_fixture)

BOOST_AUTO_TEST_CASE(Open_ShowsTheInheritedValuesUnderTheOwnOnes)
{
	put("parent.desc", k_parent);
	const res::tag child = put("child.desc", k_child);
	edt::data_document document(services);
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
	edt::data_document document(services);
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

	const json::object file = parsed(child);
	BOOST_TEST(std::string(file.at("__type").as_string()) == "test.config");
	BOOST_TEST(std::string(file.at("__parent").as_string()) == "memory://t/parent.desc");
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
	const std::string before = text_of(child);
	edt::data_document document(services);
	BOOST_REQUIRE(document.open(child).opened);

	BOOST_TEST(document.save().saved);
	BOOST_TEST(text_of(child) == before);
}

BOOST_AUTO_TEST_CASE(EditBackToTheInheritedValue_DropsTheFieldFromTheFile)
{
	put("parent.desc", k_parent);
	const res::tag child = put("child.desc", k_child);
	edt::data_document document(services);
	BOOST_REQUIRE(document.open(child).opened);

	BOOST_TEST(document.edit([](json::value& shown) {
		shown.as_object()["speed"] = 2.0;
		return true;
	}));
	BOOST_REQUIRE(document.save().saved);
	BOOST_TEST(!parsed(child).contains("speed"));
}

BOOST_AUTO_TEST_CASE(Save_OfAValueTheSchemaRefuses_WritesNothingAndNamesTheField)
{
	const res::tag file = put("config.desc", R"({"__type":"test.config","speed":2.0})");
	const std::string before = text_of(file);
	edt::data_document document(services);
	BOOST_REQUIRE(document.open(file).opened);

	json::object broken = document.own();
	broken["speed"] = "fast";
	document.restore_own(std::move(broken));
	BOOST_TEST(document.is_dirty());
	const edt::save_result saved = document.save();
	BOOST_TEST(!saved.saved);
	BOOST_TEST(saved.error.find("speed") != std::string::npos);
	BOOST_TEST(text_of(file) == before);
	BOOST_TEST(document.is_dirty());
}

BOOST_AUTO_TEST_CASE(Save_OfAFileThatChangedOnDisk_IsRefused)
{
	const res::tag file = put("config.desc", R"({"__type":"test.config","speed":2.0})");
	edt::data_document document(services);
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
	BOOST_TEST(text_of(file) == elsewhere);
	BOOST_TEST(document.is_dirty());
}

BOOST_AUTO_TEST_CASE(TheChainIsMergedFromTheFarthestParentToTheNearest)
{
	put("grand.desc", R"({"__type":"test.config","speed":1.5,"limits":{"min":2.0},"goals":[9]})");
	put("parent.desc", R"({"__type":"test.config","__parent":"memory://t/grand.desc","speed":2.5})");
	const res::tag child = put("child.desc", R"({"__type":"test.config","__parent":"memory://t/parent.desc"})");
	edt::data_document document(services);
	const edt::open_result opened = document.open(child);
	BOOST_REQUIRE_MESSAGE(opened.opened, opened.error);

	const json::value shown = document.shown();
	BOOST_TEST(shown.at("speed").to_number<double>() == 2.5);
	BOOST_TEST(shown.at("limits").at("min").to_number<double>() == 2.0);
	BOOST_TEST(shown.at("goals").as_array().size() == 1u);
	BOOST_TEST(!document.inherited().contains("__type"));
	BOOST_TEST(!document.inherited().contains("__parent"));
}

BOOST_AUTO_TEST_CASE(Open_RefusesWhatIsNoDocumentOrNoChain)
{
	edt::data_document document(services);

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
	refused(put("missing.desc", R"({"__type":"test.config","__parent":"memory://t/nowhere.desc"})"), "/__parent");
	put("b.desc", R"({"__type":"test.config","__parent":"memory://t/a.desc"})");
	refused(put("a.desc", R"({"__type":"test.config","__parent":"memory://t/b.desc"})"), "cycle");
	put("other.desc", R"({"__type":"test.plain"})");
	refused(put("typed.desc", R"({"__type":"test.config","__parent":"memory://t/other.desc"})"), "another type");
	refused(put("user.desc", R"({"__type":"test.config","__parent":"user://p.desc"})"), "/__parent");
}

BOOST_AUTO_TEST_CASE(AFileOfAnOlderVersion_IsMigratedInMemoryAndWrittenInTheCurrentOne)
{
	descs->register_migration("test.config", 1, [](json::object& object) {
		if (auto velocity = object.find("velocity"); velocity != object.end()) {
			object["speed"] = velocity->value();
			object.erase("velocity");
		}
	});
	const std::string old_text = R"({"__type":"test.config","velocity":4.0})";
	const res::tag file = put("old.desc", old_text);
	edt::data_document document(services);
	BOOST_REQUIRE(document.open(file).opened);

	BOOST_TEST(document.shown().at("speed").to_number<double>() == 4.0);
	BOOST_TEST(text_of(file) == old_text);
	BOOST_TEST(!document.is_dirty());

	BOOST_TEST(document.edit([](json::value& shown) {
		shown.as_object()["speed"] = 5.0;
		return true;
	}));
	BOOST_REQUIRE(document.save().saved);
	const json::object saved = parsed(file);
	BOOST_TEST(saved.at("__version").to_number<int>() == 2);
	BOOST_TEST(!saved.contains("velocity"));
	BOOST_TEST(saved.at("speed").to_number<double>() == 5.0);
}

BOOST_AUTO_TEST_CASE(RestoreOwn_BringsASnapshotBackAndTracksTheSavedState)
{
	const res::tag file = put("config.desc", R"({"__type":"test.config","speed":2.0})");
	edt::data_document document(services);
	BOOST_REQUIRE(document.open(file).opened);
	const json::object snapshot = document.own();

	BOOST_TEST(document.edit([](json::value& shown) {
		shown.as_object()["speed"] = 6.0;
		return true;
	}));
	BOOST_TEST(document.is_dirty());
	document.restore_own(snapshot);
	BOOST_TEST(!document.is_dirty());
	BOOST_TEST(document.own().at("speed").to_number<double>() == 2.0);
}

BOOST_AUTO_TEST_SUITE_END()
