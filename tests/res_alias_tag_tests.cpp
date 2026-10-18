#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"

// Bug #181: a model import registers an alias original -> generated memory:// prefab. A desc that
// references the original must save the original tag, however many descs loaded it before.

namespace json = boost::json;

namespace {

struct alias_tag_fixture : test_util::scene_env
{
	alias_tag_fixture()
		: adapter(*rs, *descs)
	{
		original = put("model.desc", R"({"__type":"stat_desc","__version":1,"hp":5})");
		generated = put("model_generated.desc", R"({"__type":"stat_desc","__version":1,"hp":5})");
		put("first.desc", user_desc());
		put("second.desc", user_desc());
	}

	static std::string user_desc()
	{
		return R"({"__type":"prefab_desc","__version":2,"components":{"scene":"memory://t/model.desc"}})";
	}

	// The component tag a desc writes when it is saved.
	std::string saved_scene_tag(std::string_view name)
	{
		auto tag = tag_of(name);
		auto loaded = load<desc::desc_base>(tag);
		BOOST_REQUIRE(loaded);
		auto bytes = adapter.serialize(tag, loaded);
		auto parsed = json::parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
		return std::string{ parsed.at("components").at("scene").as_string() };
	}

	core::desc::desc_adapter_t adapter;
	res::tag original;
	res::tag generated;
};

}

BOOST_FIXTURE_TEST_SUITE(ResAliasTagTests, alias_tag_fixture)

BOOST_AUTO_TEST_CASE(Handle_ReportsRequestedTagAndSharesTheTargetBlock)
{
	rs->register_alias(original, generated);

	auto via_original = rs->require_sync<desc::desc_base>(original);
	auto via_generated = rs->require_sync<desc::desc_base>(generated);

	BOOST_TEST(via_original.get_tag() == original);
	BOOST_TEST(via_generated.get_tag() == generated);
	BOOST_TEST(via_original.get_control_block() == via_generated.get_control_block());
}

BOOST_AUTO_TEST_CASE(Save_KeepsRequestedTag_AliasRegisteredAfterFirstLoad)
{
	// what the importer does: the first user loads the model, then the alias appears
	BOOST_TEST(saved_scene_tag("first.desc") == original.string());
	rs->register_alias(original, generated);
	BOOST_TEST(saved_scene_tag("second.desc") == original.string());
}

BOOST_AUTO_TEST_CASE(Save_KeepsRequestedTag_AliasRegisteredBeforeAnyLoad)
{
	rs->register_alias(original, generated);
	BOOST_TEST(saved_scene_tag("second.desc") == original.string());
	BOOST_TEST(saved_scene_tag("first.desc") == original.string());
}

BOOST_AUTO_TEST_SUITE_END()
