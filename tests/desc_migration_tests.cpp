#include <boost/test/unit_test.hpp>
#include <boost/json.hpp>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <snk/desc/desc_migrations.h>
#include <snk/desc/desc_schema.h>
#include <snk/desc/desc_system.h>
#include <snk/resource/res_system.h>
#include "test_manual_tasks.hpp"

namespace json = boost::json;

namespace {

	constexpr const char* k_type = "test_migr_desc";

	// resource_system asserts that its resources directory exists; base classes are built before members.
	struct res_dir_guard
	{
		res_dir_guard() { std::filesystem::create_directories("./res"); }
	};

	struct migr_fixture : res_dir_guard
	{
		migr_fixture() : ds(rs) {}

		json::object parse(const char* text) { return json::parse(text).as_object(); }

		// v1 -> v2: field 'a' becomes 'b'. v2 -> v3: 'b' is multiplied by 10.
		void register_two_steps()
		{
			ds.register_migration(k_type, 1, [](json::object& o) {
				o["m1"] = true;
				if (o.contains("a")) {
					json::value moved = o.at("a");
					o.erase("a");
					o["b"] = std::move(moved);
				}
			});
			ds.register_migration(k_type, 2, [](json::object& o) {
				o["m2"] = true;
				if (o.contains("b")) {
					o["b"] = o.at("b").as_int64() * 10;
				}
			});
		}

		ds::task_system tasks{ test_util::manual_tasks() };
		res::resource_system rs{ tasks };
		desc::desc_system ds;
	};

}

BOOST_FIXTURE_TEST_SUITE(DescMigrationTests, migr_fixture)

BOOST_AUTO_TEST_CASE(NoMigrations_CurrentVersionIsOne)
{
	BOOST_TEST(ds.current_version(k_type) == 1);

	auto o = parse(R"({"__type":"test_migr_desc"})");
	ds.migrate_tree(o, "ctx");
	BOOST_TEST(o.at("__version").as_int64() == 1);
}

BOOST_AUTO_TEST_CASE(TwoSteps_ObjectWithoutVersionGoesThroughBoth)
{
	register_two_steps();
	BOOST_TEST(ds.current_version(k_type) == 3);

	auto o = parse(R"({"__type":"test_migr_desc","a":4})");
	ds.migrate_tree(o, "ctx");
	BOOST_TEST(o.at("__version").as_int64() == 3);
	BOOST_TEST(o.contains("m1"));
	BOOST_TEST(o.contains("m2"));
	BOOST_TEST(!o.contains("a"));
	BOOST_TEST(o.at("b").as_int64() == 40);
}

BOOST_AUTO_TEST_CASE(TwoSteps_Version2ObjectGetsOnlySecondStep)
{
	register_two_steps();

	auto o = parse(R"({"__type":"test_migr_desc","__version":2,"b":5})");
	ds.migrate_tree(o, "ctx");
	BOOST_TEST(o.at("__version").as_int64() == 3);
	BOOST_TEST(!o.contains("m1"));
	BOOST_TEST(o.contains("m2"));
	BOOST_TEST(o.at("b").as_int64() == 50);
}

BOOST_AUTO_TEST_CASE(CurrentVersionObjectIsNotMigrated)
{
	register_two_steps();

	auto o = parse(R"({"__type":"test_migr_desc","__version":3,"b":7})");
	ds.migrate_tree(o, "ctx");
	BOOST_TEST(!o.contains("m1"));
	BOOST_TEST(!o.contains("m2"));
	BOOST_TEST(o.at("b").as_int64() == 7);
}

BOOST_AUTO_TEST_CASE(NestedTypedObjectInsideArrayIsMigrated)
{
	register_two_steps();

	auto o = parse(R"({"items":[
		{"__type":"test_migr_desc","a":1},
		{"x":{"__type":"test_migr_desc","__version":3,"b":2}}]})");
	ds.migrate_tree(o, "ctx");

	const auto& items = o.at("items").as_array();
	BOOST_TEST(items[0].as_object().at("__version").as_int64() == 3);
	BOOST_TEST(items[0].as_object().at("b").as_int64() == 10);
	const auto& nested = items[1].as_object().at("x").as_object();
	BOOST_TEST(nested.at("__version").as_int64() == 3);
	BOOST_TEST(nested.at("b").as_int64() == 2);
	BOOST_TEST(!o.contains("__version"));
}

BOOST_AUTO_TEST_CASE(NewerVersionThrows)
{
	register_two_steps();

	auto o = parse(R"({"__type":"test_migr_desc","__version":4})");
	BOOST_CHECK_EXCEPTION(ds.migrate_tree(o, "ctx"), std::runtime_error, [](const std::runtime_error& e) {
		return std::string(e.what()).find("newer than supported") != std::string::npos;
	});
}

BOOST_AUTO_TEST_CASE(NonIntegerVersionThrows)
{
	register_two_steps();

	auto o = parse(R"({"__type":"test_migr_desc","__version":"2"})");
	BOOST_CHECK_THROW(ds.migrate_tree(o, "ctx"), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(NumberArraysAreSkipped)
{
	register_two_steps();

	auto o = parse(R"({"nums":[1,2,3],"mixed":[1,{"__type":"test_migr_desc"}]})");
	const auto before = o;
	ds.migrate_tree(o, "ctx");
	BOOST_TEST(json::value(o) == json::value(before));
}

BOOST_AUTO_TEST_CASE(StampHeader_RegisteredTypeGetsCurrentVersion)
{
	register_two_steps();

	auto o = parse(R"({"__type":"test_migr_desc","b":1})");
	ds.stamp_header(o);
	BOOST_TEST(o.at("__version").as_int64() == 3);
}

BOOST_AUTO_TEST_CASE(StampHeader_UnknownTypeGetsOne)
{
	auto o = parse(R"({"__type":"never_registered_desc"})");
	ds.stamp_header(o);
	BOOST_TEST(o.at("__version").as_int64() == 1);
}

BOOST_AUTO_TEST_CASE(StampHeader_WithoutTypeIsUntouched)
{
	auto o = parse(R"({"name":"x"})");
	ds.stamp_header(o);
	BOOST_TEST(!o.contains("__version"));
}

// EPIC-58 S9a (#763): the declarative steps of a runtime schema, and the helpers C++ steps share with them.
BOOST_AUTO_TEST_CASE(SchemaV3WithTwoSteps_MigratesAV1Desc)
{
	const json::object text = parse(R"({ "name": "test_migr_desc", "version": 3,
		"fields": [ { "name": "health", "kind": "f32" }, { "name": "armor", "kind": "i32" } ],
		"migrations": [ { "from": 1, "rename": { "/hp": "/health" } },
		                { "from": 2, "remove": ["/legacy"], "set_default": { "/armor": 0 } } ] })");
	BOOST_REQUIRE((ds.register_schema(desc::parse_type_schema(text), text.at("migrations").as_array()) != nullptr));
	BOOST_TEST(ds.current_version(k_type) == 3);
	BOOST_TEST(ds.migration_summary(k_type, 1) == "rename /hp -> /health");
	BOOST_TEST(ds.migration_summary(k_type, 2) == "remove /legacy; default /armor");

	auto o = parse(R"({"__type":"test_migr_desc","hp":5,"legacy":true})");
	ds.migrate_tree(o, "ctx");
	BOOST_TEST(o.at("__version").as_int64() == 3);
	BOOST_TEST(o.at("health").as_int64() == 5);
	BOOST_TEST(!o.contains("hp"));
	BOOST_TEST(!o.contains("legacy"));
	BOOST_TEST(o.at("armor").as_int64() == 0);

	// A default does not replace a value already there.
	auto kept = parse(R"({"__type":"test_migr_desc","__version":2,"armor":7})");
	ds.migrate_tree(kept, "ctx");
	BOOST_TEST(kept.at("armor").as_int64() == 7);
}

BOOST_AUTO_TEST_CASE(SchemaSteps_ThatCannotLeadToItsVersion_AreRefused)
{
	const auto schema = [](int version) {
		desc::type_schema result;
		result.name = k_type;
		result.version = version;
		return result;
	};
	const auto steps = [](const char* text) { return json::parse(text).as_array(); };
	BOOST_TEST((ds.register_schema(schema(3), steps(R"([{"from":1,"remove":["/a"]}])")) == nullptr));
	BOOST_TEST((ds.register_schema(schema(3), steps(R"([{"from":1,"remove":["/a"]},{"from":3,"remove":["/b"]}])")) == nullptr));
	BOOST_TEST((ds.register_schema(schema(2), steps(R"([{"from":1}])")) == nullptr));
	BOOST_TEST((ds.register_schema(schema(2), steps(R"([{"from":1,"remove":["/__guid"]}])")) == nullptr));
	BOOST_TEST((ds.register_schema(schema(2), steps(R"([{"from":1,"rename":{"/a":"/b","/c":"/b"}}])")) == nullptr));
	// A chain depends on the order of the keys; header keys at any depth; array indexes and malformed pointers.
	BOOST_TEST((ds.register_schema(schema(2), steps(R"([{"from":1,"rename":{"/a":"/b","/b":"/c"}}])")) == nullptr));
	BOOST_TEST((ds.register_schema(schema(2), steps(R"([{"from":1,"rename":{"/a":"/b/c","/b":"/d"}}])")) == nullptr));
	BOOST_TEST((ds.register_schema(schema(2), steps(R"([{"from":1,"set_default":{"/a":1,"/a/b":2}}])")) == nullptr));
	BOOST_TEST((ds.register_schema(schema(2), steps(R"([{"from":1,"remove":["/a/b"],"set_default":{"/a":{}}}])")) == nullptr));
	BOOST_TEST((ds.register_schema(schema(2), steps(R"([{"from":1,"remove":["/weapon/__version"]}])")) == nullptr));
	BOOST_TEST((ds.register_schema(schema(2), steps(R"([{"from":1,"set_default":{"/items/0/hp":1}}])")) == nullptr));
	BOOST_TEST((ds.register_schema(schema(2), steps(R"([{"from":1,"remove":["/items/-"]}])")) == nullptr));
	BOOST_TEST((ds.register_schema(schema(2), steps(R"([{"from":1,"remove":["/a//b"]}])")) == nullptr));
	BOOST_TEST((ds.register_schema(schema(2), steps(R"([{"from":1,"remove":["/a~2"]}])")) == nullptr));
	// Without steps, a version they do not lead to is refused in every build, not only asserted.
	BOOST_TEST((ds.register_schema(schema(3)) == nullptr));
	BOOST_TEST(ds.current_version(k_type) == 1);
	BOOST_TEST((ds.find_schema(k_type) == nullptr));

	// The type has steps in C++ already: the schema does not bring a second set of them.
	ds.register_migration(k_type, 1, [](json::object&) {});
	BOOST_TEST((ds.register_schema(schema(2), steps(R"([{"from":1,"remove":["/a"]}])")) == nullptr));
	BOOST_TEST(ds.current_version(k_type) == 2);
	BOOST_TEST((ds.find_schema(k_type) == nullptr));
}

BOOST_AUTO_TEST_CASE(UnregisteringTheSchema_TakesItsStepsAlong)
{
	const json::object text = parse(R"({ "name": "test_migr_desc", "version": 2, "fields": [],
		"migrations": [ { "from": 1, "remove": ["/old"] } ] })");
	BOOST_REQUIRE((ds.register_schema(desc::parse_type_schema(text), text.at("migrations").as_array()) != nullptr));
	ds.unregister_desc(k_type);
	BOOST_TEST(ds.current_version(k_type) == 1);
	BOOST_TEST(ds.migration_summary(k_type, 1).empty());

	// Registered again, without a duplicate step.
	BOOST_TEST((ds.register_schema(desc::parse_type_schema(text), text.at("migrations").as_array()) != nullptr));
	BOOST_TEST(ds.current_version(k_type) == 2);
}

BOOST_AUTO_TEST_CASE(MigrationHelpers_MoveRemoveAndFillFields)
{
	auto o = parse(R"({"__type":"t","hp":5,"stats":{"old":1},"armor":null})");
	desc::migrations::rename(o, "/hp", "/stats/health");
	BOOST_TEST(!o.contains("hp"));
	BOOST_TEST(o.at("stats").at("health").as_int64() == 5);

	desc::migrations::rename(o, "/missing", "/other");
	BOOST_TEST(!o.contains("other"));
	// A value already at the target is not overwritten, and the source stays.
	BOOST_CHECK_THROW(desc::migrations::rename(o, "/stats/old", "/stats/health"), std::runtime_error);
	BOOST_TEST(o.at("stats").at("old").as_int64() == 1);
	BOOST_TEST(o.at("stats").at("health").as_int64() == 5);

	desc::migrations::remove(o, "/stats/old");
	BOOST_TEST(!o.at("stats").as_object().contains("old"));
	desc::migrations::remove(o, "/stats/old");

	desc::migrations::set_default(o, "/armor", 3);
	BOOST_TEST(o.at("armor").is_null());
	desc::migrations::set_default(o, "/speed", 2);
	BOOST_TEST(o.at("speed").as_int64() == 2);

	BOOST_CHECK_THROW(desc::migrations::remove(o, "hp"), std::invalid_argument);
	BOOST_CHECK_THROW(desc::migrations::set_default(o, "/__version", 1), std::invalid_argument);
	BOOST_CHECK_THROW(desc::migrations::remove(o, "/stats/__guid"), std::invalid_argument);
	BOOST_TEST(o.at("__type").as_string() == "t");

	// A step does not enter an array: it would turn it into an object or shift its elements.
	auto listed = parse(R"({"items":[{"hp":1}]})");
	BOOST_CHECK_THROW(desc::migrations::remove(listed, "/items/hp"), std::runtime_error);
	BOOST_CHECK_THROW(desc::migrations::set_default(listed, "/items/hp", 2), std::runtime_error);
	BOOST_TEST(listed.at("items").as_array().size() == 1u);
}

// #1228: a rename onto the object that holds its source lifts the value a level up, while that object holds nothing
// else on the way down; otherwise it names what would be dropped. A refusal leaves the object as it was.
BOOST_AUTO_TEST_CASE(MigrationHelpers_RenameOntoItsOwnHolder_LiftsTheValue)
{
	auto lifted = parse(R"({"a":{"b":{"c":5}},"keep":1})");
	desc::migrations::rename(lifted, "/a/b/c", "/a");
	BOOST_TEST(lifted.at("a").as_int64() == 5);
	BOOST_TEST(lifted.at("keep").as_int64() == 1);

	const json::object crowded = parse(R"({"a":{"b":5,"c":1}})");
	auto tried = crowded;
	BOOST_CHECK_EXCEPTION(desc::migrations::rename(tried, "/a/b", "/a"), std::runtime_error, [](const std::runtime_error& e) {
		return std::string(e.what()).find("would drop /a/c") != std::string::npos;
	});
	BOOST_TEST((tried == crowded));
}

BOOST_AUTO_TEST_CASE(MigrationHelpers_ARefusedRename_LeavesTheObjectAsItWas)
{
	const json::object before = parse(R"({"hp":5,"stats":3,"taken":{"x":1}})");
	auto o = before;
	BOOST_CHECK_THROW(desc::migrations::rename(o, "/hp", "/stats/health"), std::runtime_error);
	BOOST_TEST((o == before));
	BOOST_CHECK_THROW(desc::migrations::rename(o, "/hp", "/taken/x"), std::runtime_error);
	BOOST_TEST((o == before));
}

// Onto a place inside the source the whole value moves down: an object, and a scalar or an array too, whose way to the
// target runs through the source itself.
BOOST_AUTO_TEST_CASE(MigrationHelpers_RenameInsideItsSource_MovesTheValueDown)
{
	auto o = parse(R"({"pos":{"x":1},"hp":5,"tags":["a"]})");
	desc::migrations::rename(o, "/pos", "/pos/local");
	BOOST_TEST(o.at("pos").at("local").at("x").as_int64() == 1);
	BOOST_TEST(o.at("pos").as_object().size() == 1u);
	desc::migrations::rename(o, "/hp", "/hp/value");
	BOOST_TEST(o.at("hp").at("value").as_int64() == 5);
	desc::migrations::rename(o, "/tags", "/tags/list/items");
	BOOST_TEST(o.at("tags").at("list").at("items").as_array().size() == 1u);

	const auto steps = [](const char* text) { return json::parse(text).as_array(); };
	BOOST_TEST(desc::schema_migrations_problem(steps(R"([{"from":1,"rename":{"/hp":"/hp/value"}}])"), 2).empty());
	BOOST_TEST(!desc::schema_migrations_problem(steps(R"([{"from":1,"rename":{"/hp":"/hp/value"},"set_default":{"/hp/max":9}}])"), 2).empty());
}

BOOST_AUTO_TEST_CASE(SchemaSteps_ALiftParses_ARepeatedRemovalDoesNot)
{
	const auto steps = [](const char* text) { return json::parse(text).as_array(); };
	BOOST_TEST(desc::schema_migrations_problem(steps(R"([{"from":1,"rename":{"/stats/hp":"/stats"}}])"), 2).empty());
	BOOST_TEST(desc::schema_migrations_problem(steps(R"([{"from":1,"remove":["/a","/a"]}])"), 2).find("remove /a twice") != std::string::npos);
	// The lift does not let another operation of the step inside the target.
	BOOST_TEST(!desc::schema_migrations_problem(steps(R"([{"from":1,"rename":{"/stats/hp":"/stats"},"remove":["/stats/mp"]}])"), 2).empty());

	desc::type_schema schema;
	schema.name = k_type;
	schema.version = 2;
	BOOST_REQUIRE(ds.register_schema(schema, steps(R"([{"from":1,"rename":{"/stats/hp":"/stats"}}])")) != nullptr);
	auto o = parse(R"({"__type":"test_migr_desc","stats":{"hp":7}})");
	ds.migrate_tree(o, "ctx");
	BOOST_TEST(o.at("stats").as_int64() == 7);
}

BOOST_AUTO_TEST_CASE(AFailingSchemaStep_NamesItsTypeAndVersion)
{
	const json::object text = parse(R"({ "name": "test_migr_desc", "version": 2, "fields": [],
		"migrations": [ { "from": 1, "rename": { "/a": "/b" } } ] })");
	BOOST_REQUIRE((ds.register_schema(desc::parse_type_schema(text), text.at("migrations").as_array()) != nullptr));
	auto o = parse(R"({"__type":"test_migr_desc","a":1,"b":2})");
	try {
		ds.migrate_tree(o, "ctx");
		BOOST_FAIL("expected an exception");
	} catch (const std::runtime_error& error) {
		const std::string_view what = error.what();
		BOOST_TEST(what.find("'test_migr_desc' step from version 1") != std::string_view::npos, what);
		BOOST_TEST(what.find("rename /a -> /b") != std::string_view::npos, what);
	}
}

// #766 (EPIC-58 section 7.2): a user's document in memory: older brought up, newer and malformed refused with the
// source, its version and the one this engine reads named.
BOOST_AUTO_TEST_CASE(UpgradeInMemory_NamesTheSourceAndBothVersions)
{
	register_two_steps();
	auto newer = parse(R"({"__type":"test_migr_desc","__version":5})");
	const std::string problem = ds.upgrade_in_memory(newer, "user://settings/x.json");
	BOOST_TEST(problem.find("user://settings/x.json") != std::string::npos, problem);
	BOOST_TEST(problem.find("version 5") != std::string::npos, problem);
	BOOST_TEST(problem.find("version 3") != std::string::npos, problem);

	auto older = parse(R"({"__type":"test_migr_desc","a":4})");
	BOOST_TEST(ds.upgrade_in_memory(older, "older.json").empty());
	BOOST_TEST(older.at("__version").as_int64() == 3);
	BOOST_TEST(older.at("b").as_int64() == 40);

	auto malformed = parse(R"({"__type":"test_migr_desc","__version":"x"})");
	BOOST_TEST(!ds.upgrade_in_memory(malformed, "bad.json").empty());
}

// Review of #1231: a root of the current version does not let a typed object inside through unchecked. A newer one is
// refused, an older one brought up.
BOOST_AUTO_TEST_CASE(UpgradeInMemory_LooksInsideARootOfTheCurrentVersion)
{
	register_two_steps();
	auto newer_inside = parse(R"({"__type":"test_migr_desc","__version":3,"inner":{"__type":"test_migr_desc","__version":5}})");
	const std::string problem = ds.upgrade_in_memory(newer_inside, "world 'main'");
	BOOST_TEST(!problem.empty());
	BOOST_TEST(problem.find("5") != std::string::npos, problem);

	auto older_inside = parse(R"({"__type":"test_migr_desc","__version":3,"inner":{"__type":"test_migr_desc","a":4}})");
	BOOST_TEST(ds.upgrade_in_memory(older_inside, "world 'main'").empty());
	BOOST_TEST(older_inside.at("inner").at("__version").as_int64() == 3);
	BOOST_TEST(older_inside.at("inner").at("b").as_int64() == 40);
}

BOOST_AUTO_TEST_SUITE_END()
