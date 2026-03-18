#include <boost/test/unit_test.hpp>
#include "res_tag.h"
#include <unordered_set>

BOOST_AUTO_TEST_SUITE(ResTagTests)

BOOST_AUTO_TEST_CASE(Make_CreatesWithDefaultProtocol) {
	auto t = res::tag::make("textures/brick.png");
	BOOST_CHECK(t.protocol() == "res");
	BOOST_CHECK(t.name() == "brick.png");
	BOOST_CHECK(t.is_valid());
}

BOOST_AUTO_TEST_CASE(ExplicitProtocol_Memory) {
	res::tag t("memory", "runtime/generated.mesh");
	BOOST_CHECK(t.protocol() == "memory");
	BOOST_CHECK(t.name() == "generated.mesh");
}

BOOST_AUTO_TEST_CASE(FullStringConstructor) {
	res::tag t("res://models/character/hero.glb");
	BOOST_CHECK(t.protocol() == "res");
	BOOST_CHECK(t.name() == "hero.glb");
	BOOST_CHECK(t.extension() == "glb");
	BOOST_CHECK(t.pure_name() == "hero");
}

BOOST_AUTO_TEST_CASE(Extension_Extracted) {
	auto t = res::tag::make("shaders/basic.vert");
	BOOST_CHECK(t.extension() == "vert");
}

BOOST_AUTO_TEST_CASE(PureName_WithoutExtension) {
	auto t = res::tag::make("icons/logo.png");
	BOOST_CHECK(t.pure_name() == "logo");
}

BOOST_AUTO_TEST_CASE(Relative_PathPlusName) {
	auto t = res::tag::make("models/tree.obj");
	BOOST_CHECK(t.relative() == "models/tree.obj");
}

BOOST_AUTO_TEST_CASE(BackslashesNormalized) {
	res::tag t("res://textures\\walls\\brick.png");
	BOOST_CHECK(t.name() == "brick.png");
	BOOST_CHECK(t.string().find('\\') == std::string::npos);
}

BOOST_AUTO_TEST_CASE(Equality_SameTags) {
	auto a = res::tag::make("models/cube.obj");
	auto b = res::tag::make("models/cube.obj");
	BOOST_CHECK(a == b);
}

BOOST_AUTO_TEST_CASE(Equality_DifferentTags) {
	auto a = res::tag::make("models/cube.obj");
	auto b = res::tag::make("models/sphere.obj");
	BOOST_CHECK(!(a == b));
}

BOOST_AUTO_TEST_CASE(Hash_ConsistentForSameTag) {
	auto a = res::tag::make("textures/grass.png");
	auto b = res::tag::make("textures/grass.png");
	BOOST_CHECK_EQUAL(a.get_hash(), b.get_hash());
}

BOOST_AUTO_TEST_CASE(Hash_WorksInUnorderedSet) {
	std::unordered_set<res::tag> tags;
	tags.insert(res::tag::make("a.txt"));
	tags.insert(res::tag::make("b.txt"));
	tags.insert(res::tag::make("a.txt"));
	BOOST_TEST(tags.size() == 2u);
}

BOOST_AUTO_TEST_CASE(DefaultConstructor_IsInvalid) {
	res::tag t;
	BOOST_CHECK(!t.is_valid());
}

BOOST_AUTO_TEST_CASE(NullTag_IsInvalid) {
	BOOST_CHECK(!res::tag::null.is_valid());
}

BOOST_AUTO_TEST_CASE(CopyAndAssignment) {
	auto original = res::tag::make("data/config.json");
	res::tag copy(original);
	BOOST_CHECK(copy == original);

	res::tag assigned;
	assigned = original;
	BOOST_CHECK(assigned == original);
}

BOOST_AUTO_TEST_CASE(MoveConstructor) {
	auto t = res::tag::make("data/file.txt");
	auto hash = t.get_hash();
	res::tag moved(std::move(t));
	BOOST_CHECK_EQUAL(moved.get_hash(), hash);
	BOOST_CHECK(moved.is_valid());
}

BOOST_AUTO_TEST_SUITE_END()
