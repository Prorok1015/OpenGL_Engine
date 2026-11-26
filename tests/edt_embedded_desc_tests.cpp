#include <boost/test/unit_test.hpp>
#include "edt_embedded_desc.h"
#include "edt_override_edit.h"
#include <snk/desc/desc_schema.h>
#include <boost/json.hpp>
#include <string>
#include <vector>

// The value an instance's record holds for an embedded material (#1403, EPIC-82 8.1): one canonical form, one record.

namespace {

namespace json = boost::json;

constexpr const char* k_material_schema = R"({
	"name": "test.material", "kind": "desc", "is_document": true,
	"fields": [
		{ "name": "shininess", "kind": "f32", "default": 32.0 },
		{ "name": "queue", "kind": "string", "default": "opaque" },
		{ "name": "label", "kind": "string", "default": "" }
	]
})";

struct embedded_fixture
{
	embedded_fixture()
	{
		schema = registry.add(desc::parse_type_schema(json::parse(k_material_schema).as_object()));
		BOOST_REQUIRE(schema != nullptr);
	}

	std::vector<std::string> keys_of(const json::object& object) const
	{
		std::vector<std::string> keys;
		for (const auto& member : object) keys.emplace_back(member.key());
		return keys;
	}

	desc::schema_registry registry;
	const desc::type_schema* schema = nullptr;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtEmbeddedDesc, embedded_fixture)

BOOST_AUTO_TEST_CASE(TheValueHasItsHeader_ThenTheFieldsInTheOrderOfTheSchema)
{
	const json::object own = json::parse(R"({"label": "x", "queue": "blend", "shininess": 8.0})").as_object();

	const json::object value = edt::canonical_embedded_value(registry, *schema, own, {}, 3, "res://mat/face.mat.desc");

	BOOST_TEST((keys_of(value) == std::vector<std::string>{ "__type", "__version", "__parent", "shininess", "queue", "label" }), boost::test_tools::per_element());
	BOOST_TEST(value.at("__type").as_string() == "test.material");
	BOOST_TEST(value.at("__version").as_int64() == 3);
	BOOST_TEST(value.at("__parent").as_string() == "res://mat/face.mat.desc");
}

BOOST_AUTO_TEST_CASE(NoParent_WritesNoParentKey)
{
	const json::object value = edt::canonical_embedded_value(registry, *schema, json::parse(R"({"queue": "blend"})").as_object(), {}, 1, "");
	BOOST_TEST(!value.contains("__parent"));
}

BOOST_AUTO_TEST_CASE(AFieldThatEqualsTheBase_SaysNothing_AFieldThatDiffersStays)
{
	const json::object base = json::parse(R"({"shininess": 32, "queue": "opaque"})").as_object();
	const json::object own = json::parse(R"({"shininess": 32.0, "queue": "blend"})").as_object();

	const json::object value = edt::canonical_embedded_value(registry, *schema, own, base, 1, "res://a.mat.desc");

	BOOST_TEST(!value.contains("shininess"));
	BOOST_TEST(value.at("queue").as_string() == "blend");
}

BOOST_AUTO_TEST_CASE(TwoEditsOfOneMeaning_GiveTheSameBytes)
{
	const json::object base = json::parse(R"({"shininess": 32})").as_object();
	const json::object one = json::parse(R"({"queue": "blend", "shininess": 32.0, "label": "x", "extra": {"b": 2, "a": 1}})").as_object();
	const json::object other = json::parse(R"({"extra": {"a": 1.0, "b": 2}, "label": "x", "queue": "blend"})").as_object();

	const std::string first = json::serialize(edt::canonical_embedded_value(registry, *schema, one, base, 1, "res://a.mat.desc"));
	const std::string second = json::serialize(edt::canonical_embedded_value(registry, *schema, other, base, 1, "res://a.mat.desc"));

	BOOST_TEST(first == second);
}

BOOST_AUTO_TEST_CASE(KeysTheSchemaDoesNotKnow_FollowAlphabetically_AndObjectsBelowAreSortedToo)
{
	const json::object own = json::parse(R"({"zeta": 1, "alpha": {"y": 1, "x": 2}, "shininess": 4})").as_object();

	const json::object value = edt::canonical_embedded_value(registry, *schema, own, {}, 1, "");

	BOOST_TEST((keys_of(value) == std::vector<std::string>{ "__type", "__version", "shininess", "alpha", "zeta" }), boost::test_tools::per_element());
	BOOST_TEST((keys_of(value.at("alpha").as_object()) == std::vector<std::string>{ "x", "y" }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(AnObjectIsComparedKeyByKey_OnlyWhatDiffersIsKept)
{
	const json::object base = json::parse(R"({"extra": {"x": 1}})").as_object();
	const json::object own = json::parse(R"({"extra": {"y": 2, "x": 1}})").as_object();

	const json::object value = edt::canonical_embedded_value(registry, *schema, own, base, 1, "");

	BOOST_REQUIRE(value.contains("extra"));
	BOOST_TEST((keys_of(value.at("extra").as_object()) == std::vector<std::string>{ "y" }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(TheHeaderKeysOfTheOwnFields_AreIgnored_NoSecondParent)
{
	const json::object own = json::parse(R"({"__parent": "res://other.mat.desc", "__version": 9, "__guid": "g", "queue": "blend"})").as_object();

	const json::object value = edt::canonical_embedded_value(registry, *schema, own, {}, 2, "res://a.mat.desc");

	BOOST_TEST(value.at("__parent").as_string() == "res://a.mat.desc");
	BOOST_TEST(value.at("__version").as_int64() == 2);
	BOOST_TEST(!value.contains("__guid"));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(EdtSetWholeValue)

namespace {

scn::override_record record(std::vector<uint64_t> target, std::string component, std::string path, json::value value)
{
	scn::override_record out;
	out.target = std::move(target);
	out.component = std::move(component);
	out.path = std::move(path);
	out.value = std::move(value);
	return out;
}

}

BOOST_AUTO_TEST_CASE(ItWritesOneSetRecord_WithThePointerTheValueAndTheVersion)
{
	scn::prefab_desc::prefab_node owner;
	edt::override_edit::set_whole_value(owner, {}, "mesh_node_desc", "/material", json::object{ { "__type", "material_desc" } }, 4);

	BOOST_REQUIRE(owner.overrides.size() == 1);
	const scn::override_record& written = owner.overrides.front();
	BOOST_TEST((written.op == scn::override_op::set));
	BOOST_TEST(written.component == "mesh_node_desc");
	BOOST_TEST(written.path == "/material");
	BOOST_TEST(written.type_version == 4);
	BOOST_TEST(written.value.as_object().at("__type").as_string() == "material_desc");
}

BOOST_AUTO_TEST_CASE(ASecondEdit_ReplacesTheFirst_NeverAddsASubpathRecord)
{
	scn::prefab_desc::prefab_node owner;
	edt::override_edit::set_whole_value(owner, {}, "mesh_node_desc", "/material", json::object{ { "queue", "a" } }, 4);
	edt::override_edit::set_whole_value(owner, {}, "mesh_node_desc", "/material", json::object{ { "queue", "b" } }, 4);

	BOOST_REQUIRE(owner.overrides.size() == 1);
	BOOST_TEST(owner.overrides.front().value.as_object().at("queue").as_string() == "b");
	const json::value* held = edt::override_edit::whole_value(owner, {}, "mesh_node_desc", "/material");
	BOOST_REQUIRE(held != nullptr);
	BOOST_TEST(held->as_object().at("queue").as_string() == "b");
}

BOOST_AUTO_TEST_CASE(ItTakesOutTheRecordsBelowItsPointer_AndKeepsTheOthers)
{
	scn::prefab_desc::prefab_node owner;
	owner.overrides.push_back(record({}, "mesh_node_desc", "/material/defines", json::array{ "A" }));
	owner.overrides.push_back(record({}, "mesh_node_desc", "/visible", true));
	owner.overrides.push_back(record({ 7 }, "mesh_node_desc", "/material", "res://other.mat.desc"));
	owner.overrides.push_back(record({}, "stat", "/material", "res://stat.mat.desc"));

	edt::override_edit::set_whole_value(owner, {}, "mesh_node_desc", "/material", json::object{ { "queue", "b" } }, 4);

	BOOST_REQUIRE(owner.overrides.size() == 4);
	BOOST_TEST(owner.overrides[0].path == "/visible");
	BOOST_TEST(owner.overrides[1].target.size() == 1u);
	BOOST_TEST(owner.overrides[2].component == "stat");
	BOOST_TEST(owner.overrides[3].path == "/material");
}

BOOST_AUTO_TEST_CASE(AStringValue_GoesBackToTheFile_AsTheSameOneRecord)
{
	scn::prefab_desc::prefab_node owner;
	edt::override_edit::set_whole_value(owner, {}, "mesh_node_desc", "/material", json::object{ { "queue", "a" } }, 4);
	edt::override_edit::set_whole_value(owner, {}, "mesh_node_desc", "/material", json::value("res://mat/face.mat.desc"), 4);

	BOOST_REQUIRE(owner.overrides.size() == 1);
	BOOST_TEST(owner.overrides.front().value.as_string() == "res://mat/face.mat.desc");
}

BOOST_AUTO_TEST_CASE(WholeValue_FindsNothingForAnotherPointerTargetOrComponent)
{
	scn::prefab_desc::prefab_node owner;
	const std::vector<uint64_t> three{ 3 };
	edt::override_edit::set_whole_value(owner, three, "mesh_node_desc", "/material", "res://a.mat.desc", 4);
	BOOST_TEST(edt::override_edit::whole_value(owner, {}, "mesh_node_desc", "/material") == nullptr);
	BOOST_TEST(edt::override_edit::whole_value(owner, three, "mesh_node_desc", "/visible") == nullptr);
	BOOST_TEST(edt::override_edit::whole_value(owner, three, "stat", "/material") == nullptr);
	BOOST_TEST(edt::override_edit::whole_value(owner, three, "mesh_node_desc", "/material") != nullptr);
}

BOOST_AUTO_TEST_SUITE_END()
