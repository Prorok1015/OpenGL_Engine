#include <boost/test/unit_test.hpp>
#include <snk/resource/res_model_reference.h>
#include <snk/resource/res_tag.h>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <utility>

BOOST_AUTO_TEST_SUITE(ResTagTests)

BOOST_AUTO_TEST_CASE(Make_CreatesWithDefaultProtocol) {
	auto t = res::tag::make("textures/brick.png");
	BOOST_TEST(t.protocol() == "res");
	BOOST_TEST(t.name() == "brick.png");
	BOOST_TEST(t.is_valid());
}

BOOST_AUTO_TEST_CASE(ExplicitProtocol_Memory) {
	res::tag t("memory", "runtime/generated.mesh");
	BOOST_TEST(t.protocol() == "memory");
	BOOST_TEST(t.name() == "generated.mesh");
}

BOOST_AUTO_TEST_CASE(FullStringConstructor) {
	res::tag t("res://models/character/hero.glb");
	BOOST_TEST(t.protocol() == "res");
	BOOST_TEST(t.name() == "hero.glb");
	BOOST_TEST(t.extension() == "glb");
	BOOST_TEST(t.pure_name() == "hero");
}

BOOST_AUTO_TEST_CASE(Extension_Extracted) {
	auto t = res::tag::make("shaders/basic.vert");
	BOOST_TEST(t.extension() == "vert");
}

BOOST_AUTO_TEST_CASE(PureName_WithoutExtension) {
	auto t = res::tag::make("icons/logo.png");
	BOOST_TEST(t.pure_name() == "logo");
}

BOOST_AUTO_TEST_CASE(Relative_PathPlusName) {
	auto t = res::tag::make("models/tree.obj");
	BOOST_TEST(t.relative() == "models/tree.obj");
}

BOOST_AUTO_TEST_CASE(BackslashesNormalized) {
	res::tag t("res://textures\\walls\\brick.png");
	BOOST_TEST(t.name() == "brick.png");
	BOOST_TEST(t.string().find('\\') == std::string::npos);
}

BOOST_AUTO_TEST_CASE(Equality_SameTags) {
	auto a = res::tag::make("models/cube.obj");
	auto b = res::tag::make("models/cube.obj");
	BOOST_TEST(a == b);
}

BOOST_AUTO_TEST_CASE(Equality_DifferentTags) {
	auto a = res::tag::make("models/cube.obj");
	auto b = res::tag::make("models/sphere.obj");
	BOOST_TEST(!(a == b));
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
	BOOST_TEST(!t.is_valid());
}

BOOST_AUTO_TEST_CASE(NullTag_IsInvalid) {
	BOOST_TEST(!res::tag::null.is_valid());
}

BOOST_AUTO_TEST_CASE(CopyAndAssignment) {
	auto original = res::tag::make("data/config.json");
	res::tag copy(original);
	BOOST_TEST(copy == original);

	res::tag assigned;
	assigned = original;
	BOOST_TEST(assigned == original);
}

BOOST_AUTO_TEST_CASE(MoveConstructor) {
	auto t = res::tag::make("data/file.txt");
	auto hash = t.get_hash();
	res::tag moved(std::move(t));
	BOOST_CHECK_EQUAL(moved.get_hash(), hash);
	BOOST_TEST(moved.is_valid());
}

namespace {
	struct parsed_tag
	{
		std::string_view text;
		std::string_view string;
		std::string_view protocol;
		std::string_view relative;
		std::string_view path;
		std::string_view name;
	};
}

// #631: the protocol is the scheme before the only "://" of the text, nothing else; a text without one keeps all of it.
BOOST_AUTO_TEST_CASE(Protocol_IsOnlyTheSchemeBeforeTheOnlySeparator) {
	const parsed_tag table[] = {
		{ "res://a/b.png", "res://a/b.png", "res", "a/b.png", "a/", "b.png" },
		{ "materials/a.mat.desc", "materials/a.mat.desc", "", "materials/a.mat.desc", "materials/", "a.mat.desc" },
		{ "user/xsecret.desc", "user/xsecret.desc", "", "user/xsecret.desc", "user/", "xsecret.desc" },
		{ "user/xa.png", "user/xa.png", "", "user/xa.png", "user/", "xa.png" },
		{ "a.png", "a.png", "", "a.png", "", "a.png" },
		{ "res://a/b:c.png", "res://a/b:c.png", "res", "a/b:c.png", "a/", "b:c.png" },
		{ "user:/xa.png", "user:/xa.png", "", "user:/xa.png", "user:/", "xa.png" },
		{ "C:/x.frag", "C:/x.frag", "", "C:/x.frag", "C:/", "x.frag" },
		{ "C:\\x.frag", "C:/x.frag", "", "C:/x.frag", "C:/", "x.frag" },
		{ "c:\\\\path", "c://path", "c", "path", "", "path" },
		{ "\\\\", "//", "", "//", "//", "" },
		{ "res:\\\\a\\b.png", "res://a/b.png", "res", "a/b.png", "a/", "b.png" },
		{ "res:/a.png", "res:/a.png", "", "res:/a.png", "res:/", "a.png" },
		{ "res://res://a.png", "res://res://a.png", "", "res://res://a.png", "res://res://", "a.png" },
		{ "res://user://a.png", "res://user://a.png", "", "res://user://a.png", "res://user://", "a.png" },
		{ "a:b://x", "a:b://x", "", "a:b://x", "a:b://", "x" },
		{ "a/b://x", "a/b://x", "", "a/b://x", "a/b://", "x" },
		{ "x-y.z+1_://a", "x-y.z+1_://a", "x-y.z+1_", "a", "", "a" },
		{ "1x://a", "1x://a", "", "1x://a", "1x://", "a" },
		{ "RES://a.png", "RES://a.png", "RES", "a.png", "", "a.png" },
		{ "", "", "", "", "", "" },
		{ "://a.png", "://a.png", "", "://a.png", "://", "a.png" },
		{ "res://", "res://", "res", "", "", "" },
		{ "res://a/", "res://a/", "res", "a/", "a/", "" },
		{ "res:///abs", "res:///abs", "res", "/abs", "/", "abs" },
	};
	for (const parsed_tag& row : table) {
		BOOST_TEST_CONTEXT(row.text) {
			const res::tag t{ row.text };
			BOOST_TEST(t.string() == row.string);
			BOOST_TEST(t.protocol() == row.protocol);
			BOOST_TEST(res::tag::protocol_of(t.view()) == row.protocol);
			BOOST_TEST(t.relative() == row.relative);
			BOOST_TEST(t.path() == row.path);
			BOOST_TEST(t.name() == row.name);
			BOOST_TEST(t.is_valid() == !row.text.empty());
		}
	}
}

BOOST_AUTO_TEST_CASE(ProtocolOf_IsAConstantExpression) {
	static_assert(res::tag::protocol_of("res://a.png") == "res");
	static_assert(res::tag::protocol_of("user/a.png").empty());
	static_assert(res::tag::protocol_of("a:b://c").empty());
	BOOST_TEST(true);
}

BOOST_AUTO_TEST_CASE(NoProtocolReason_SaysHowToWriteTheTag) {
	BOOST_TEST(res::no_protocol_reason(res::tag{ "materials/a.mat.desc" })
		== "'materials/a.mat.desc' has no protocol: write res://materials/a.mat.desc");
}

// #631: what a model refers to is read next to it, in its protocol, and never above that protocol's root.
BOOST_AUTO_TEST_CASE(ModelReference_IsReadInTheModelsFolderAndProtocol) {
	const res::tag model{ "memory://models/m/tex.obj" };
	const std::pair<std::string_view, std::string_view> resolved[] = {
		{ "textures/a.png", "memory://models/m/textures/a.png" },
		{ "textures\\a.png", "memory://models/m/textures/a.png" },
		{ "a//b/./c.png", "memory://models/m/a/b/c.png" },
		{ "user/a.png", "memory://models/m/user/a.png" },
		{ "../a.png", "memory://models/a.png" },
		{ "../../a.png", "memory://a.png" },
		{ "memory://models/m/tex.mtl", "memory://models/m/tex.mtl" },
		{ "memory://models/m/../n/x.bin", "memory://models/n/x.bin" },
	};
	for (const auto& [reference, expected] : resolved) {
		BOOST_TEST_CONTEXT(reference) {
			const std::optional<res::tag> found = res::model_reference_tag(model, reference);
			BOOST_REQUIRE(found.has_value());
			BOOST_TEST(found->view() == expected);
		}
	}
	for (const std::string_view reference : { "../../../a.png", "memory://../x.png", "memory://models/../../x", "res://a.png",
		"user://a.png", "/abs.png", "C:/a.png", "c:\\a.png", "a:b://c", "", "a\x01.png", "\\\\server\\share\\a.png",
		"MEMORY://models/a.png", "..\\..\\..\\x", "../c:/x", "a.png:stream", "dir:x/a.png", "memory://models/c:/x" }) {
		BOOST_TEST_CONTEXT(reference) {
			BOOST_TEST(!res::model_reference_tag(model, reference).has_value());
		}
	}
	BOOST_TEST(!res::model_reference_tag(res::tag{ "models/m.obj" }, "a.png").has_value());
	BOOST_TEST(!res::model_reference_tag(res::tag{ "res://m/x.obj" }, "res://C:/x").has_value());
	BOOST_TEST(!res::model_reference_tag(res::tag{ "res://m/x.obj" }, "RES://m/a.png").has_value());
}

BOOST_AUTO_TEST_SUITE_END()
