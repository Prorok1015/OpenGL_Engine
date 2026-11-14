#include <boost/test/unit_test.hpp>
#include "test_asset_ids_index.hpp"
#include "test_scene_env.hpp"
#include <snk/scene/scn_animation_collection_desc.h>
#include <boost/json.hpp>
#include <memory>
#include <string>
#include <string_view>

// #920: descs that read a reference field with their own code take every form a file writes it in: a path, or
// {guid, path} where the guid wins over a path that is no longer the file's (resolve_reference, as in #897).
namespace {

namespace json = boost::json;

constexpr std::string_view k_moved_guid = "3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f";
constexpr std::string_view k_unknown_guid = "0b6c1d2e-3f40-4a51-8b62-7c83d94ea5f6";

struct manual_reference_fixture : test_util::scene_env
{
	manual_reference_fixture()
	{
		auto index = std::make_shared<test_util::asset_ids_index>();
		index->hold(k_moved_guid, "memory://t/new_place.desc");
		rs->set_asset_index(index);
	}
	~manual_reference_fixture() { rs->set_asset_index(nullptr); }

	// A list of references in every form, the moved one naming its old path.
	static std::string references()
	{
		return "[ \"memory://t/plain.desc\", " + test_util::reference_text(k_moved_guid, "memory://t/old_place.desc") + ", "
			+ test_util::reference_text(k_unknown_guid, "memory://t/kept.desc") + ", "
			+ test_util::reference_text({}, "memory://t/only_path.desc") + " ]";
	}

	template<class Handles>
	static void expect_resolved(const Handles& handles)
	{
		BOOST_REQUIRE(handles.size() == 4u);
		BOOST_TEST(handles[0].get_tag().view() == "memory://t/plain.desc");
		BOOST_TEST(handles[1].get_tag().view() == "memory://t/new_place.desc");
		BOOST_TEST(handles[2].get_tag().view() == "memory://t/kept.desc");
		BOOST_TEST(handles[3].get_tag().view() == "memory://t/only_path.desc");
	}
};

}

BOOST_FIXTURE_TEST_SUITE(DescManualReferenceTests, manual_reference_fixture)

BOOST_AUTO_TEST_CASE(AnimationCollection_ClipsTakeAGuidAndAPath)
{
	scn::animation_collection_desc collection;
	collection.deserialize(*descs, json::parse("{ \"clips\": " + references() + " }").as_object());

	expect_resolved(collection.clips);
}

BOOST_AUTO_TEST_CASE(Level_WorldsTakeAGuidAndAPath)
{
	scn::level_desc level;
	level.deserialize(*descs, json::parse("{ \"name\": \"l\", \"worlds\": " + references() + " }").as_object());

	expect_resolved(level.get_worlds());
}

BOOST_AUTO_TEST_CASE(Prefab_TheParentOfAComponentTakesAGuidAndAPath)
{
	const std::string text = "{ \"components\": {"
		" \"plain\": { \"__type\": \"stat_desc\", \"__parent\": \"memory://t/plain.desc\" },"
		" \"moved\": { \"__type\": \"stat_desc\", \"__parent\": " + test_util::reference_text(k_moved_guid, "memory://t/old_place.desc") + " },"
		" \"kept\": { \"__type\": \"stat_desc\", \"__parent\": " + test_util::reference_text(k_unknown_guid, "memory://t/kept.desc") + " },"
		" \"only_path\": { \"__type\": \"stat_desc\", \"__parent\": " + test_util::reference_text({}, "memory://t/only_path.desc") + ", \"hp\": 3 }"
		" } }";
	scn::prefab_desc prefab;
	prefab.deserialize(*descs, parse_migrated(text));

	const auto& components = prefab.get_root().components;
	BOOST_REQUIRE(components.size() == 4u);
	BOOST_TEST(components.at("plain").parent_desc.get_tag().view() == "memory://t/plain.desc");
	BOOST_TEST(components.at("moved").parent_desc.get_tag().view() == "memory://t/new_place.desc");
	BOOST_TEST(components.at("kept").parent_desc.get_tag().view() == "memory://t/kept.desc");
	BOOST_TEST(components.at("only_path").parent_desc.get_tag().view() == "memory://t/only_path.desc");
	// The reference is the parent, never an override of the component.
	BOOST_TEST(components.at("moved").overrides.empty());
	BOOST_TEST(components.at("only_path").overrides.size() == 1u);
}

BOOST_AUTO_TEST_CASE(Prefab_AComponentWrittenAsOneReference_TakesAGuidAndAPath)
{
	const std::string text = "{ \"components\": {"
		" \"plain\": \"memory://t/plain.desc\","
		" \"moved\": " + test_util::reference_text(k_moved_guid, "memory://t/old_place.desc") + ","
		" \"kept\": " + test_util::reference_text(k_unknown_guid, "memory://t/kept.desc") + ","
		" \"only_path\": " + test_util::reference_text({}, "memory://t/only_path.desc") + ","
		" \"typed\": { \"__type\": \"stat_desc\", \"path\": \"not a reference\" },"
		" \"stat_desc\": { \"path\": \"memory://t/a_field.desc\" }"
		" } }";
	scn::prefab_desc prefab;
	prefab.deserialize(*descs, parse_migrated(text));

	const auto& components = prefab.get_root().components;
	BOOST_REQUIRE(components.size() == 6u);
	BOOST_TEST(components.at("plain").parent_desc.get_tag().view() == "memory://t/plain.desc");
	BOOST_TEST(components.at("moved").parent_desc.get_tag().view() == "memory://t/new_place.desc");
	BOOST_TEST(components.at("kept").parent_desc.get_tag().view() == "memory://t/kept.desc");
	BOOST_TEST(components.at("only_path").parent_desc.get_tag().view() == "memory://t/only_path.desc");
	for (const char* key : { "plain", "moved", "kept", "only_path" }) {
		BOOST_TEST_CONTEXT(key) {
			BOOST_TEST(components.at(key).overrides.empty());
			BOOST_TEST(components.at(key).type_name.empty());
		}
	}
	// An object with a type is a component, whatever its keys are called.
	BOOST_TEST(components.at("typed").type_name == "stat_desc");
	BOOST_TEST(components.at("typed").overrides.contains("path"));
	BOOST_TEST(!components.at("typed").parent_desc.is_valid());
	// So is an object under the key of a registered type: the key names the type.
	BOOST_TEST(components.at("stat_desc").type_name == "stat_desc");
	BOOST_TEST(components.at("stat_desc").overrides.contains("path"));
	BOOST_TEST(!components.at("stat_desc").parent_desc.is_valid());
}

BOOST_AUTO_TEST_SUITE_END()
