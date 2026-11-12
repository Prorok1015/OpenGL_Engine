#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/desc/desc_system.h>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace json = boost::json;

// Migration steps with a summary and a context (#751, EPIC-58 S1).
namespace {

// scn::scene_layer holds a desc_system by value, before members modules read through inline accessors: desc_system must
// not grow, or those members move under a module built before. The members as they are; a new one breaks this.
struct frozen_desc_system
{
	res::resource_system& resources;
	const desc::desc_system* parent = nullptr;
	std::unordered_map<std::string, std::function<std::shared_ptr<desc::desc_base>()>> factories;
	std::unordered_map<std::string, std::map<int, std::function<void(json::object&)>>> migrations;
	std::vector<std::string> component_type_names;
	desc::schema_registry schemas;
};
static_assert(sizeof(desc::desc_system) == sizeof(frozen_desc_system), "desc_system changed its size: scn::scene_layer moves under old modules");
static_assert(alignof(desc::desc_system) == alignof(frozen_desc_system));

struct migration_context_fixture : test_util::scene_env
{
	json::object migrated(const char* text, std::vector<desc::migration_warning>& warnings, std::string_view file = "res://w.desc")
	{
		auto obj = json::parse(text).as_object();
		descs->migrate_tree(obj, file, warnings);
		return obj;
	}
};

}

BOOST_FIXTURE_TEST_SUITE(DescMigrationContextTests, migration_context_fixture)

BOOST_AUTO_TEST_CASE(AStepWithASummary_AndAnOldStep_MakeOneChain)
{
	descs->register_migration("ctx_test_desc", 1, [](json::object& obj) { obj["a"] = 1; });
	descs->register_migration("ctx_test_desc", 2, "adds b", [](json::object& obj, desc::migration_context& context) {
		BOOST_TEST(context.type() == "ctx_test_desc");
		BOOST_TEST(context.from_version() == 2);
		obj["b"] = obj.at("a").as_int64() + 1;
	});
	BOOST_TEST(descs->current_version("ctx_test_desc") == 3);
	BOOST_TEST(descs->migration_summary("ctx_test_desc", 2) == "adds b");
	BOOST_TEST(descs->migration_summary("ctx_test_desc", 1).empty());
	BOOST_TEST(descs->migration_summary("ctx_test_desc", 7).empty());
	BOOST_TEST(descs->migration_summary("no_such_desc", 1).empty());

	std::vector<desc::migration_warning> warnings;
	const auto obj = migrated(R"({"__type":"ctx_test_desc"})", warnings);
	BOOST_TEST(obj.at("b").as_int64() == 2);
	BOOST_TEST(obj.at("__version").as_int64() == 3);
	BOOST_TEST(warnings.empty());
}

// The path of modules built before the summaries (and of every inline caller): the step runs, its warning goes to the log.
BOOST_AUTO_TEST_CASE(TheInlineMigrateTree_RunsAStepWithASummary)
{
	descs->register_migration("ctx_test_desc", 1, "renames x", [](json::object& obj, desc::migration_context& context) {
		obj["y"] = obj.at("x");
		obj.erase("x");
		context.warn("x was renamed");
	});
	auto obj = json::parse(R"({"__type":"ctx_test_desc","x":5})").as_object();
	descs->migrate_tree(obj, "test");
	BOOST_TEST(obj.at("y").as_int64() == 5);
	BOOST_TEST(!obj.contains("x"));
	BOOST_TEST(obj.at("__version").as_int64() == 2);
}

BOOST_AUTO_TEST_CASE(AWarning_NamesTheTypeTheStepAndTheFile)
{
	descs->register_migration("ctx_test_desc", 1, "drops z", [](json::object& obj, desc::migration_context& context) {
		if (obj.erase("z") != 0) context.warn("z is gone");
	});
	std::vector<desc::migration_warning> warnings;
	migrated(R"({"__type":"ctx_test_desc","z":1,"child":{"__type":"ctx_test_desc"}})", warnings, "res://levels/a.desc");
	BOOST_REQUIRE(warnings.size() == 1u);
	BOOST_TEST(warnings[0].type == "ctx_test_desc");
	BOOST_TEST(warnings[0].from_version == 1);
	BOOST_TEST(warnings[0].file == "res://levels/a.desc");
	BOOST_TEST(warnings[0].message == "z is gone");
}

BOOST_AUTO_TEST_CASE(EveryEngineStep_HasASummary)
{
	for (const auto& [type, last] : { std::pair{ "prefab_desc", 3 }, std::pair{ "world_desc", 3 }, std::pair{ "camera_desc", 1 },
			std::pair{ "level_desc", 1 } }) {
		for (int from = 1; from <= last; ++from) {
			BOOST_TEST_INFO(type << " from " << from);
			BOOST_TEST(!descs->migration_summary(type, from).empty());
		}
	}
}

// What the 3 -> 4 step used to say only in the log: an override that sets part of a legacy camera pose is dropped.
BOOST_AUTO_TEST_CASE(TheCameraStepOfAWorld_WarnsAboutAPartialPoseItDrops)
{
	std::vector<desc::migration_warning> warnings;
	const auto world = migrated(R"({"__type":"world_desc","__version":3,"name":"w","children":[
		{"__id":"00000000000000a1","name":"inst",
		 "components":{"prefab_desc":{"__type":"prefab_desc","__version":4,"__parent":"memory://t/base.prefab.desc"}},
		 "overrides":[
			{"target":["00000000000000c1"],"component":"camera","value":{"fov":50,"transform":{"position":[0,1,3],"rotation":[-5,0,0]}}},
			{"target":["00000000000000c2"],"component":"camera","value":{"transform":{"position":[4,5,6]}}},
			{"target":["00000000000000c3"],"component":"camera","path":"/transform/position","value":[1,2,3]}]}]})", warnings);
	BOOST_TEST(world.at("__version").as_int64() == 4);
	BOOST_REQUIRE(warnings.size() == 2u);
	for (const auto& warning : warnings) {
		BOOST_TEST(warning.type == "world_desc");
		BOOST_TEST(warning.from_version == 3);
		BOOST_TEST(warning.file == "res://w.desc");
	}
	BOOST_TEST(warnings[0].message.find("sets no whole legacy camera_desc.transform") != std::string::npos);
	BOOST_TEST(warnings[1].message.find("/transform/position") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ACameraOutsideANode_WarnsThatItsPoseIsDropped)
{
	std::vector<desc::migration_warning> warnings;
	const auto camera = migrated(R"({"__type":"camera_desc","fov":50,"transform":{"position":[1,2,3],"rotation":[0,90,0]}})", warnings);
	BOOST_TEST(!camera.contains("transform"));
	BOOST_REQUIRE(warnings.size() == 1u);
	BOOST_TEST(warnings[0].type == "camera_desc");
	BOOST_TEST(warnings[0].from_version == 1);
}

// As a module registers its steps: in its own layer, the old API, removed with the layer's scope.
BOOST_AUTO_TEST_CASE(AModuleLayer_RegistersStepsWithTheOldApi_AsBefore)
{
	desc::desc_system layer(*rs, descs.get());
	layer.register_migration("ctx_module_desc", 1, [](json::object& obj) { obj["migrated"] = true; });
	BOOST_TEST(layer.current_version("ctx_module_desc") == 2);
	BOOST_TEST(layer.migration_summary("ctx_module_desc", 1).empty());

	std::vector<desc::migration_warning> warnings;
	auto obj = json::parse(R"({"__type":"ctx_module_desc","inner":{"__type":"world_desc","__version":4}})").as_object();
	layer.migrate_tree(obj, "res://m.desc", warnings);
	BOOST_TEST(obj.at("migrated").as_bool());
	BOOST_TEST(obj.at("__version").as_int64() == 2);
	BOOST_TEST(warnings.empty());

	layer.unregister_migrations("ctx_module_desc");
	BOOST_TEST(layer.current_version("ctx_module_desc") == 1);
	// The engine's described steps are seen through the parent.
	BOOST_TEST(!layer.migration_summary("world_desc", 3).empty());
}

BOOST_AUTO_TEST_SUITE_END()
