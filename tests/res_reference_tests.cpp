#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/resource/res_reference.h>
#include <snk/resource/res_system.h>
#include <snk/resource/res_tag_json.h>
#include <boost/json/parse.hpp>
#include <string_view>

namespace {

std::optional<res::reference> read(std::string_view text)
{
	return res::reference_from_json(json::parse(text));
}

constexpr std::string_view guid_text = "3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f";

}

BOOST_AUTO_TEST_SUITE(ResReference)

BOOST_AUTO_TEST_CASE(a_string_and_an_object_give_the_same_path)
{
	const auto as_string = read(R"("res://a/b.png")");
	const auto as_object = read(R"({ "path": "res://a/b.png" })");
	const auto with_guid = read(R"({ "guid": "3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f", "path": "res://a/b.png" })");
	BOOST_REQUIRE(as_string);
	BOOST_REQUIRE(as_object);
	BOOST_REQUIRE(with_guid);
	BOOST_TEST((as_string->path == res::tag{ "res://a/b.png" }));
	BOOST_TEST((as_object->path == as_string->path));
	BOOST_TEST((with_guid->path == as_string->path));
	BOOST_TEST(as_string->guid.is_nil());
	BOOST_TEST(res::to_text(with_guid->guid) == guid_text);
}

BOOST_AUTO_TEST_CASE(either_half_may_be_missing)
{
	const auto empty = read(R"("")");
	BOOST_REQUIRE(empty);
	BOOST_TEST(!empty->path.is_valid());
	BOOST_TEST(empty->guid.is_nil());

	const auto guid_only = read(R"({ "guid": "3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f" })");
	BOOST_REQUIRE(guid_only);
	BOOST_TEST(!guid_only->path.is_valid());
	BOOST_TEST(!guid_only->guid.is_nil());
}

BOOST_AUTO_TEST_CASE(anything_else_is_not_a_reference)
{
	BOOST_TEST(!read("{}"));
	BOOST_TEST(!read("5"));
	BOOST_TEST(!read("null"));
	BOOST_TEST(!read(R"(["res://a.png"])"));
	BOOST_TEST(!read(R"({ "__type": "material", "path": "res://a.png" })"));
	BOOST_TEST(!read(R"({ "path": 3 })"));
	BOOST_TEST(!read(R"({ "guid": "not-a-guid", "path": "res://a.png" })"));
	BOOST_TEST(!read(R"({ "guid": "00000000-0000-0000-0000-000000000000", "path": "res://a.png" })"));
}

// The exported value_to<res::tag> (__parent of a desc, skins) reads the object form as its path.
BOOST_AUTO_TEST_CASE(value_to_a_tag_reads_the_object_form_as_its_path)
{
	BOOST_TEST((json::value_to<res::tag>(json::parse(R"({ "guid": "3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f", "path": "res://a.desc" })"))
		== res::tag{ "res://a.desc" }));
	BOOST_TEST((json::value_to<res::tag>(json::parse(R"("res://a.desc")")) == res::tag{ "res://a.desc" }));
	BOOST_TEST(!json::value_to<res::tag>(json::parse("5")).is_valid());
}

BOOST_FIXTURE_TEST_CASE(resolve_takes_the_path_until_the_asset_index, test_util::ram_res_env)
{
	BOOST_TEST((rs->resolve_reference(*read(R"({ "guid": "3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f", "path": "memory://t/a.desc" })"))
		== res::tag{ "memory://t/a.desc" }));
	BOOST_TEST(!rs->resolve_reference(*read(R"({ "guid": "3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f" })")).is_valid());
	BOOST_TEST(!rs->resolve_reference(*read(R"("")")).is_valid());
}

BOOST_AUTO_TEST_SUITE_END()
