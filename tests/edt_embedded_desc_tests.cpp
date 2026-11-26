#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "edt_embedded_desc.h"
#include "edt_override_edit.h"
#include <snk/desc/desc_schema.h>
#include <snk/desc/desc_system.h>
#include <boost/json.hpp>
#include <algorithm>
#include <cstdint>
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
		{ "name": "label", "kind": "string", "default": "" },
		{ "name": "texture", "kind": "res_tag", "default": "" },
		{ "name": "detail", "kind": "desc_ref", "ref_type": "test.material" }
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

BOOST_AUTO_TEST_CASE(AnIntegralNumber_IsAnInteger_Never1Point0Or1E0)
{
	const json::object own = json::parse(R"({"shininess": 8.0, "counts": [1.0, 2, 3.5], "scale": {"x": 2.0}})").as_object();

	const json::object value = edt::canonical_embedded_value(registry, *schema, own, {}, 1, "");

	BOOST_TEST(json::serialize(value.at("shininess")) == "8");
	BOOST_TEST(json::serialize(value.at("counts")) == "[1,2,3.5]");
	BOOST_TEST(json::serialize(value.at("scale")) == R"({"x":2})");
}

BOOST_AUTO_TEST_CASE(AFloat32Field_TakesTheShortestTextOfItsFloat)
{
	json::object own;
	own["shininess"] = static_cast<double>(0.1f);

	const json::object value = edt::canonical_embedded_value(registry, *schema, own, {}, 1, "");

	BOOST_TEST(json::serialize(value.at("shininess")) == "0.1");
}

BOOST_AUTO_TEST_CASE(AKeptField_IsDroppedWhateverTheFormOfTheNumber)
{
	const json::object base = json::parse(R"({"shininess": 8, "weights": {"a": 0.5}})").as_object();
	json::object own = json::parse(R"({"shininess": 8.0})").as_object();
	BOOST_TEST(!edt::canonical_embedded_value(registry, *schema, own, base, 1, "").contains("shininess"));

	json::object tenth;
	tenth["shininess"] = 0.1;
	json::object widened;
	widened["shininess"] = static_cast<double>(0.1f);
	BOOST_TEST(!edt::canonical_embedded_value(registry, *schema, widened, tenth, 1, "").contains("shininess"));
}

BOOST_AUTO_TEST_CASE(AnIntegerAbove2To53_IsNotTakenForTheDoubleBesideIt)
{
	json::object base;
	base["big"] = 9007199254740992.0;
	json::object own;
	own["big"] = static_cast<std::int64_t>(9007199254740993);

	const json::object value = edt::canonical_embedded_value(registry, *schema, own, base, 1, "");

	BOOST_REQUIRE(value.contains("big"));
	BOOST_TEST(json::serialize(value.at("big")) == "9007199254740993");
}

BOOST_AUTO_TEST_CASE(AnInlineDesc_IsWrittenWhole_ItsTypeIsNeverLost)
{
	const json::object base = json::parse(R"({"detail": {"__type": "test.material", "queue": "opaque", "label": "a"}})").as_object();
	const json::object own = json::parse(R"({"detail": {"__type": "test.material", "queue": "opaque", "label": "b"}})").as_object();

	const json::object value = edt::canonical_embedded_value(registry, *schema, own, base, 1, "");

	BOOST_REQUIRE(value.contains("detail"));
	BOOST_TEST(value.at("detail").as_object().contains("__type"));
	BOOST_TEST(value.at("detail").as_object().contains("queue"));
	BOOST_TEST(value.at("detail").as_object().at("label").as_string() == "b");
}

BOOST_AUTO_TEST_CASE(AReference_IsWrittenWhole_NotCutDownToItsPath)
{
	const json::object base = json::parse(R"({"texture": {"guid": "3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f", "path": "res://a.png"}})").as_object();
	const json::object own = json::parse(R"({"texture": {"guid": "3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f", "path": "res://b.png"}})").as_object();

	const json::object value = edt::canonical_embedded_value(registry, *schema, own, base, 1, "");

	BOOST_REQUIRE(value.contains("texture"));
	BOOST_TEST(value.at("texture").as_object().contains("guid"));
	BOOST_TEST(value.at("texture").as_object().at("path").as_string() == "res://b.png");
}

BOOST_AUTO_TEST_CASE(AMapField_IsWrittenWhole_ASomewhereRemovedKeyCanBeSaid)
{
	// A map field cannot be in a JSON schema (it needs a C++ type), so the schema of the type is made by hand, as a described type would give it.
	desc::type_schema maps;
	maps.name = "test.maps";
	desc::field_schema weights;
	weights.name = "weights";
	weights.key = "weights";
	weights.kind = desc::field_kind::map;
	weights.element = desc::field_kind::f32;
	maps.fields.push_back(weights);
	const desc::type_schema* added = registry.add(maps);
	BOOST_REQUIRE(added != nullptr);
	const json::object base = json::parse(R"({"weights": {"a": 1, "b": 2}})").as_object();
	const json::object own = json::parse(R"({"weights": {"a": 1}})").as_object();

	const json::object value = edt::canonical_embedded_value(registry, *added, own, base, 1, "");

	BOOST_REQUIRE(value.contains("weights"));
	BOOST_TEST(json::serialize(value.at("weights")) == R"({"a":1})");
}

BOOST_AUTO_TEST_CASE(TheFormsOfAReference_AreMadeOneByTheNormalizer_SoTheValueBeforeASaveIsTheValueAfterIt)
{
	// What the save does (desc_system::write_references): a path becomes {guid, path}. The editor's value is made the same way.
	const edt::reference_normalizer to_object = [](const json::value& reference) -> json::value {
		if (reference.is_string()) return json::object{ { "guid", "g1" }, { "path", reference.as_string() } };
		return reference;
	};
	const json::object before = json::parse(R"({"texture": "res://t.png"})").as_object();
	const json::object after = json::parse(R"({"texture": {"path": "res://t.png", "guid": "g1"}})").as_object();

	const std::string first = json::serialize(edt::canonical_embedded_value(registry, *schema, before, {}, 1, "res://a.mat.desc", to_object));
	const std::string second = json::serialize(edt::canonical_embedded_value(registry, *schema, after, {}, 1, "res://a.mat.desc", to_object));

	BOOST_TEST(first == second);
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

BOOST_AUTO_TEST_CASE(ARecordOfAnotherPointerWithTheSamePrefix_Survives_AndSoDoesAnotherOp)
{
	scn::prefab_desc::prefab_node owner;
	owner.overrides.push_back(record({}, "mesh_node_desc", "/materials", json::array{ 1 }));
	owner.overrides.push_back(record({}, "mesh_node_desc", "/material~1x", json::value("keep")));
	scn::override_record removal = record({}, "mesh_node_desc", "/material", json::value());
	removal.op = scn::override_op::remove;
	owner.overrides.push_back(removal);

	edt::override_edit::set_whole_value(owner, {}, "mesh_node_desc", "/material", json::object{ { "queue", "b" } }, 4);

	BOOST_REQUIRE(owner.overrides.size() == 4);
	BOOST_TEST(owner.overrides[0].path == "/materials");
	BOOST_TEST(owner.overrides[1].path == "/material~1x");
	BOOST_TEST((owner.overrides[2].op == scn::override_op::remove));
	BOOST_TEST((owner.overrides[3].op == scn::override_op::set));
}

BOOST_AUTO_TEST_SUITE_END()

namespace {

constexpr const char* k_mesh_schema = R"({
	"name": "test.meshish", "kind": "desc",
	"fields": [
		{ "name": "material", "kind": "desc_ref", "ref_type": "test.material" },
		{ "name": "visible", "kind": "boolean", "default": true }
	]
})";

struct write_component_fixture : test_util::scene_env
{
	write_component_fixture()
	{
		material = descs->register_schema(desc::parse_type_schema(json::parse(k_material_schema).as_object()));
		mesh = descs->register_schema(desc::parse_type_schema(json::parse(k_mesh_schema).as_object()));
		BOOST_REQUIRE(material != nullptr && mesh != nullptr);
	}

	std::size_t records_at(const scn::prefab_desc::prefab_node& owner, const std::string& path) const
	{
		return static_cast<std::size_t>(std::ranges::count_if(owner.overrides, [&](const scn::override_record& r) { return r.path == path; }));
	}

	const desc::type_schema* material = nullptr;
	const desc::type_schema* mesh = nullptr;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtWriteComponentKeepsTheMaterialRecord, write_component_fixture)

BOOST_AUTO_TEST_CASE(AnotherEditOfTheComponent_KeepsOneSetOfTheMaterial_AndWritesTheOtherFieldApart)
{
	const json::object base = json::parse(R"({"material": "res://m.mat.desc", "visible": true})").as_object();
	json::object edited = json::parse(R"({"visible": false})").as_object();
	edited["material"] = json::parse(R"({"__type": "test.material", "__version": 1, "__parent": "res://m.mat.desc", "queue": "blend"})");
	scn::prefab_desc::prefab_node owner;

	edt::override_edit::write_component(owner, {}, "test.meshish", base, edited, descs.get(), mesh);

	BOOST_TEST(records_at(owner, "/material") == 1u);
	const json::value* held = edt::override_edit::whole_value(owner, {}, "test.meshish", "/material");
	BOOST_REQUIRE(held != nullptr);
	BOOST_TEST(held->as_object().at("queue").as_string() == "blend");
	BOOST_TEST(held->as_object().at("__parent").as_string() == "res://m.mat.desc");
	const auto whole_component = std::ranges::find_if(owner.overrides, [](const scn::override_record& r) { return r.path.empty(); });
	BOOST_REQUIRE(whole_component != owner.overrides.end());
	BOOST_TEST(!whole_component->value.as_object().contains("material"));
	BOOST_TEST(whole_component->value.as_object().at("visible").as_bool() == false);

	edited["visible"] = true;
	edt::override_edit::write_component(owner, {}, "test.meshish", base, edited, descs.get(), mesh);
	BOOST_TEST(records_at(owner, "/material") == 1u);
	BOOST_TEST(edt::override_edit::whole_value(owner, {}, "test.meshish", "/material") != nullptr);
}

BOOST_AUTO_TEST_SUITE_END()
