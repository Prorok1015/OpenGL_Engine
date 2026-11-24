#include <boost/test/unit_test.hpp>
#include <boost/json.hpp>
#include <filesystem>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <snk/desc/desc_schema.h>
#include <snk/desc/desc_system.h>
#include <snk/resource/res_system.h>
#include <snk/scene/level/scn_override_record.h>
#include "test_manual_tasks.hpp"

namespace json = boost::json;

namespace {

	constexpr const char* k_type = "test_record_migr_desc";

	struct res_dir_guard
	{
		res_dir_guard() { std::filesystem::create_directories("./res"); }
	};

	struct record_fixture : res_dir_guard
	{
		record_fixture() : descs(resources) {}

		void step(int from, std::function<void(json::object&)> fn) { descs.register_migration(k_type, from, std::move(fn)); }

		scn::override_record record(std::string path, json::value value, int version)
		{
			scn::override_record made;
			made.component = "stat";
			made.path = std::move(path);
			made.value = std::move(value);
			made.type_version = version;
			return made;
		}

		static void rename(json::object& o, const char* from, const char* to)
		{
			if (const auto it = o.find(from); it != o.end()) {
				json::value moved = it->value();
				o.erase(it);
				o[to] = std::move(moved);
			}
		}

		ds::task_system tasks{ test_util::manual_tasks() };
		res::resource_system resources{ tasks };
		desc::desc_system descs;
	};

}

BOOST_FIXTURE_TEST_SUITE(ScnOverrideRecordMigrationTests, record_fixture)

BOOST_AUTO_TEST_CASE(TheVersion_IsReadAndWritten_OnlyForASetWithOne)
{
	const auto read = scn::parse_override_record(json::parse(R"({"target":[],"component":"stat","path":"/hp","value":5,"version":3})"));
	BOOST_REQUIRE(read.has_value());
	BOOST_TEST(read->type_version == 3);
	BOOST_TEST(scn::override_record_to_json(*read).at("version").as_int64() == 3);

	const auto old = scn::parse_override_record(json::parse(R"({"target":[],"component":"stat","path":"/hp","value":5})"));
	BOOST_REQUIRE(old.has_value());
	BOOST_TEST(old->type_version == 0);
	BOOST_TEST(!scn::override_record_to_json(*old).contains("version"));

	const auto bad = scn::parse_override_record(json::parse(R"({"target":[],"component":"stat","path":"/hp","value":5,"version":-2})"));
	BOOST_REQUIRE(bad.has_value());
	BOOST_TEST(bad->type_version == 0);
}

BOOST_AUTO_TEST_CASE(ADeclarativeRename_MovesThePathAndStampsTheVersion)
{
	const json::object text = json::parse(R"({ "name": "test_record_migr_desc", "version": 2,
		"fields": [ { "name": "health", "kind": "f32" } ],
		"migrations": [ { "from": 1, "rename": { "/hp": "/health" } } ] })").as_object();
	BOOST_REQUIRE((descs.register_schema(desc::parse_type_schema(text), text.at("migrations").as_array()) != nullptr));

	auto written = record("/hp", 5, 1);
	const auto result = scn::migrate_override_record(descs, k_type, written);
	BOOST_TEST((result.outcome == scn::record_migration::moved));
	BOOST_TEST(written.path == "/health");
	BOOST_TEST(written.value.as_int64() == 5);
	BOOST_TEST(written.type_version == 2);
}

// A default the step fills where the value went is the value itself: the record is still found there, not taken for dropped.
BOOST_AUTO_TEST_CASE(AValueEqualToTheDefaultAStepFills_IsStillFound)
{
	step(1, [](json::object& o) {
		rename(o, "hp", "health");
		if (!o.contains("health")) o["health"] = 5;
	});
	auto written = record("/hp", 5, 1);
	const auto result = scn::migrate_override_record(descs, k_type, written);
	BOOST_TEST((result.outcome == scn::record_migration::moved));
	BOOST_TEST(written.path == "/health");
	BOOST_TEST(written.value.as_int64() == 5);
}

BOOST_AUTO_TEST_CASE(ACppStepThatConvertsTheValue_GivesTheConvertedValueAtItsNewPlace)
{
	step(1, [](json::object& o) {
		if (const auto it = o.find("hp"); it != o.end()) {
			const auto tenfold = it->value().as_int64() * 10;
			o.erase(it);
			o["health"] = tenfold;
		}
	});
	auto written = record("/hp", 5, 1);
	const auto result = scn::migrate_override_record(descs, k_type, written);
	BOOST_TEST((result.outcome == scn::record_migration::moved));
	BOOST_TEST(written.path == "/health");
	BOOST_TEST(written.value.as_int64() == 50);
}

BOOST_AUTO_TEST_CASE(AStepThatRemovesTheField_DropsTheRecord_AndLeavesItAsItWas)
{
	step(1, [](json::object& o) { o.erase("legacy"); });
	auto written = record("/legacy", true, 1);
	const auto result = scn::migrate_override_record(descs, k_type, written);
	BOOST_TEST((result.outcome == scn::record_migration::dropped));
	BOOST_TEST(result.reason.find("/legacy") != std::string::npos);
	BOOST_TEST(written.path == "/legacy");
	BOOST_TEST(written.type_version == 1);
}

BOOST_AUTO_TEST_CASE(AStepThatCopiesTheFieldToTwoPlaces_IsDuplicated)
{
	step(1, [](json::object& o) {
		if (const auto* hp = o.if_contains("hp")) {
			// Copied first: inserting into the object may move its members.
			const json::value value = *hp;
			o.erase("hp");
			o["health"] = value;
			o["max_health"] = value;
		}
	});
	auto written = record("/hp", 5, 1);
	const auto result = scn::migrate_override_record(descs, k_type, written);
	BOOST_TEST((result.outcome == scn::record_migration::duplicated));
	BOOST_TEST(result.reason.find("2 places") != std::string::npos);
	BOOST_TEST(written.path == "/hp");
}

BOOST_AUTO_TEST_CASE(ARecordWithoutAVersion_GetsTheCurrentOne_AndIsNotMoved)
{
	step(1, [](json::object& o) { rename(o, "hp", "health"); });
	auto written = record("/hp", 5, 0);
	const auto result = scn::migrate_override_record(descs, k_type, written);
	BOOST_TEST((result.outcome == scn::record_migration::kept));
	BOOST_TEST(written.path == "/hp");
	BOOST_TEST(written.type_version == 2);
}

BOOST_AUTO_TEST_CASE(AMergeIntoTheWholeComponent_MovesEachFieldItSets)
{
	step(1, [](json::object& o) {
		rename(o, "hp", "health");
		o.erase("legacy");
	});
	auto written = record("", json::parse(R"({"hp":5,"armor":2,"legacy":true})"), 1);
	const auto result = scn::migrate_override_record(descs, k_type, written);
	BOOST_TEST((result.outcome == scn::record_migration::moved));
	BOOST_TEST(written.path.empty());
	BOOST_TEST((written.value == json::parse(R"({"health":5,"armor":2})")));
}

// Review of #1310: a step may change what is inside the elements, which a sparse object cannot hold.
BOOST_AUTO_TEST_CASE(APathThroughAnArray_IsNotFollowed_AndKeepsItsVersion)
{
	step(1, [](json::object& o) {
		if (auto* lights = o.if_contains("lights"); lights && lights->is_array()) {
			for (auto& light : lights->as_array()) {
				if (light.is_object()) rename(light.as_object(), "color", "tint");
			}
		}
	});
	auto written = record("/lights/0/color", json::parse("[1,0,0]"), 1);
	const auto result = scn::migrate_override_record(descs, k_type, written);
	BOOST_TEST((result.outcome == scn::record_migration::failed));
	BOOST_TEST(written.path == "/lights/0/color");
	BOOST_TEST(written.type_version == 1);
}

// Review of #1310: the string marker survives a step that converts only numbers, while the value itself moves.
BOOST_AUTO_TEST_CASE(AStepThatChecksTheKindOfTheValue_IsFollowedByWhereTheValueWent)
{
	step(1, [](json::object& o) {
		if (const auto it = o.find("hp"); it != o.end() && it->value().is_number()) {
			const auto tenfold = it->value().to_number<std::int64_t>() * 10;
			o.erase(it);
			o["health"] = tenfold;
		}
	});
	auto written = record("/hp", 5, 1);
	const auto result = scn::migrate_override_record(descs, k_type, written);
	BOOST_TEST((result.outcome == scn::record_migration::moved));
	BOOST_TEST(written.path == "/health");
	BOOST_TEST(written.value.as_int64() == 50);
}

// A set replaces the value at its pointer: the leaf the value went to is set, not the whole object that wraps it.
BOOST_AUTO_TEST_CASE(AStepThatWrapsTheValue_MovesTheRecordToTheLeafThatHoldsIt)
{
	step(1, [](json::object& o) {
		if (const auto it = o.find("hp"); it != o.end()) {
			json::object wrapped{ { "value", it->value() }, { "max", 100 } };
			o.erase(it);
			o["hp"] = std::move(wrapped);
		}
	});
	auto written = record("/hp", 5, 1);
	const auto result = scn::migrate_override_record(descs, k_type, written);
	BOOST_TEST((result.outcome == scn::record_migration::moved));
	BOOST_TEST(written.path == "/hp/value");
	BOOST_TEST(written.value.as_int64() == 5);
}

BOOST_AUTO_TEST_CASE(AStepThatThrows_FailsTheRecord_AndLeavesItAsItWas)
{
	step(1, [](json::object&) { throw std::runtime_error("broken step"); });
	auto written = record("/hp", 5, 1);
	const auto result = scn::migrate_override_record(descs, k_type, written);
	BOOST_TEST((result.outcome == scn::record_migration::failed));
	BOOST_TEST(result.reason.find("broken step") != std::string::npos);
	BOOST_TEST(written.path == "/hp");
	BOOST_TEST(written.type_version == 1);
}

// Review of #1310: a type whose module is not loaded must not be given version 1, its steps would run again later.
BOOST_AUTO_TEST_CASE(ATypeNoLayerKnows_LeavesTheRecordAsItIs)
{
	for (const int version : { 0, 3 }) {
		auto written = record("/hp", 5, version);
		const auto result = scn::migrate_override_record(descs, "test_record_unknown_desc", written);
		BOOST_TEST((result.outcome == scn::record_migration::kept));
		BOOST_TEST(written.type_version == version);
	}
}

// Review of #1310: an edit older than versions whose field the type has not got any more is not taken as current.
BOOST_AUTO_TEST_CASE(ARecordWithoutAVersion_WhoseFieldIsGone_KeepsNoVersion)
{
	const json::object text = json::parse(R"({ "name": "test_record_migr_desc", "version": 2,
		"fields": [ { "name": "health", "kind": "f32" } ],
		"migrations": [ { "from": 1, "rename": { "/hp": "/health" } } ] })").as_object();
	BOOST_REQUIRE((descs.register_schema(desc::parse_type_schema(text), text.at("migrations").as_array()) != nullptr));

	auto old = record("/hp", 5, 0);
	BOOST_TEST((scn::migrate_override_record(descs, k_type, old).outcome == scn::record_migration::failed));
	BOOST_TEST(old.type_version == 0);

	auto fine = record("/health", 5, 0);
	BOOST_TEST((scn::migrate_override_record(descs, k_type, fine).outcome == scn::record_migration::kept));
	BOOST_TEST(fine.type_version == 2);
}

BOOST_AUTO_TEST_CASE(ATransformRecord_IsLeftAsItIs)
{
	step(1, [](json::object& o) { rename(o, "position", "pos"); });
	auto written = record("/position", json::parse("[1,2,3]"), 1);
	written.component = std::string(scn::transform_pseudo_component);
	const auto result = scn::migrate_override_record(descs, k_type, written);
	BOOST_TEST((result.outcome == scn::record_migration::kept));
	BOOST_TEST(written.path == "/position");
	BOOST_TEST(written.type_version == 1);
}

BOOST_AUTO_TEST_SUITE_END()
