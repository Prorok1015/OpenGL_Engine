#include <boost/test/unit_test.hpp>
#include <snk/resource/res_tag.h>
#include <snk/resource/res_tag_json.h>
#include <unordered_map>

namespace json = boost::json;

BOOST_AUTO_TEST_SUITE(ResTagJsonAndPathTests)

// ---------------------------------------------------------------- json

BOOST_AUTO_TEST_CASE(Json_ValidTagSerializesAsItsFullString)
{
	auto jv = json::value_from(res::tag{ "res://textures/brick.png" });
	BOOST_REQUIRE(jv.is_string());
	BOOST_TEST(jv.as_string() == "res://textures/brick.png");
}

BOOST_AUTO_TEST_CASE(Json_InvalidTagSerializesAsNull)
{
	BOOST_TEST(json::value_from(res::tag{}).is_null());
	BOOST_TEST(json::value_from(res::tag::null).is_null());
}

BOOST_AUTO_TEST_CASE(Json_StringParsesToTag_NonStringToNullTag)
{
	auto t = json::value_to<res::tag>(json::value("memory://a/b.desc"));
	BOOST_TEST(t.protocol() == "memory");
	BOOST_TEST(t.name() == "b.desc");

	BOOST_TEST(!json::value_to<res::tag>(json::value(nullptr)).is_valid());
	BOOST_TEST(!json::value_to<res::tag>(json::value(5)).is_valid());
	BOOST_TEST(!json::value_to<res::tag>(json::parse("[]")).is_valid());
}

BOOST_AUTO_TEST_CASE(Json_RoundTripKeepsBackslashNormalization)
{
	auto t = json::value_to<res::tag>(json::value("res://dir\\sub\\file.txt"));
	BOOST_TEST(t.string() == "res://dir/sub/file.txt");
	BOOST_TEST(json::value_to<res::tag>(json::value_from(t)) == t);
}

// ---------------------------------------------------------------- path components

BOOST_AUTO_TEST_CASE(Path_DeepDirectoriesAndMultipleDots)
{
	res::tag t{ "res://a/b/c/file.tar.gz" };
	BOOST_TEST(t.protocol() == "res");
	BOOST_TEST(t.path() == "a/b/c/");
	BOOST_TEST(t.name() == "file.tar.gz");
	BOOST_TEST(t.extension() == "gz");
	BOOST_TEST(t.pure_name() == "file.tar");
	BOOST_TEST(t.relative() == "a/b/c/file.tar.gz");
}

BOOST_AUTO_TEST_CASE(Path_FileAtProtocolRootHasEmptyPath)
{
	res::tag t{ "res://root.txt" };
	BOOST_TEST(t.path() == "");
	BOOST_TEST(t.name() == "root.txt");
	BOOST_TEST(t.relative() == "root.txt");
}

BOOST_AUTO_TEST_CASE(Path_NoExtensionReturnsWholeNameAsExtension_CurrentBehavior)
{
	// Pins current behavior: extension() of "README" is "README" (substr from npos+1 == 0), and
	// pure_name() is the whole name. Callers must not treat extension()==name() as "has extension".
	res::tag t{ "res://docs/README" };
	BOOST_TEST(t.name() == "README");
	BOOST_TEST(t.extension() == "README");
	BOOST_TEST(t.pure_name() == "README");
}

BOOST_AUTO_TEST_CASE(Path_DotFileKeepsLeadingDotAsExtension)
{
	res::tag t{ "res://cfg/.hidden" };
	BOOST_TEST(t.name() == ".hidden");
	BOOST_TEST(t.extension() == "hidden");
	BOOST_TEST(t.pure_name() == "");
}

BOOST_AUTO_TEST_CASE(Make_AndProtocolConstructorBuildTheSameTag)
{
	BOOST_TEST(res::tag::make("a/b.txt") == res::tag("res://a/b.txt"));
	BOOST_TEST(res::tag("memory", "x/y.desc") == res::tag("memory://x/y.desc"));
	BOOST_TEST(res::tag::default_protocol() == "res");
}

BOOST_AUTO_TEST_CASE(AssignFromStringReparses)
{
	res::tag t;
	t = std::string_view("memory://m/n.desc");
	BOOST_TEST(t.protocol() == "memory");
	BOOST_TEST(t.pure_name() == "n");
	BOOST_TEST(t.is_valid());
}

// ---------------------------------------------------------------- operator+

BOOST_AUTO_TEST_CASE(Plus_AppendsRightPathAndNameToLeftPath)
{
	res::tag base{ "res://models/hero.glb" };
	res::tag sub{ "res://parts/arm.desc" };
	auto sum = base + sub;
	BOOST_TEST(sum.protocol() == "res");
	BOOST_TEST(sum.string() == "res://models/parts/arm.desc");
}

BOOST_AUTO_TEST_CASE(Plus_WithEqualTagsReturnsLeftUnchanged)
{
	res::tag t{ "res://a/b.txt" };
	BOOST_TEST((t + t) == t);
}

// ---------------------------------------------------------------- hashing

BOOST_AUTO_TEST_CASE(Hash_UsableAsUnorderedMapKeyAcrossProtocols)
{
	std::unordered_map<res::tag, int> m;
	m[res::tag{ "res://a.txt" }] = 1;
	m[res::tag{ "memory://a.txt" }] = 2;
	BOOST_TEST(m.size() == 2u);
	BOOST_TEST(m.at(res::tag{ "res://a.txt" }) == 1);
	BOOST_TEST(m.at(res::tag{ "res:\\\\a.txt" }) == 1); // backslash form normalizes to the same key
}

BOOST_AUTO_TEST_SUITE_END()
