#include <boost/test/unit_test.hpp>
#include <boost/json.hpp>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <snk/desc/desc_system.h>
#include <snk/resource/res_system.h>

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

		res::resource_system rs;
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

BOOST_AUTO_TEST_SUITE_END()
