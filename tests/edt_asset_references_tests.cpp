#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "edt_asset_references.h"
#include <snk/desc/desc_describe.h>
#include <snk/desc/desc_describe_ops.h>
#include <snk/desc/desc_schema.h>
#include <snk/desc/desc_schema_desc.h>
#include <snk/desc/desc_system.h>
#include <snk/resource/res_asset_id.h>
#include <snk/resource/res_tag.h>
#include <boost/describe.hpp>
#include <boost/json.hpp>
#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The direct references of a desc file (#774, EPIC-57 section 6.3): what the asset database reads from it, and what the
// schema says each one is.
namespace {

namespace json = boost::json;

// A map of tags: a map field needs a C++ type.
class reference_table_desc : public desc::schema_desc<reference_table_desc>
{
public:
	static constexpr std::string_view __type = "refs.table";

	std::map<std::string, res::tag> named;

	BOOST_DESCRIBE_CLASS(reference_table_desc, (), (named), (), ())
};

constexpr std::string_view k_guid = "3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f";

// A level as the editor writes it, with every place a reference can stand and some that are no reference.
const char* const k_level = R"({
	"__type": "level_desc",
	"note": "https://example.com/page",
	"plain": "textures/a.png",
	"vertices": [0.0, 1.0, 2.0],
	"worlds": [ {
		"__type": "world_desc", "name": "w",
		"children": [ {
			"__id": "00000000000000a1",
			"components": {
				"sky": "res://sky.desc",
				"mesh": { "__type": "mesh_node_desc", "geometry": "res://g.desc",
				          "material": { "guid": "3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f", "path": "res://m.desc" } },
				"inst": { "__type": "prefab_desc", "__parent": "res://rig.prefab.desc" },
				"table": { "__type": "refs.table", "named": { "a": "res://a.png", "b": { "guid": "3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f" } } },
				"probe": { "__type": "refs.probe", "list": [ "res://l0.png" ], "slot": { "image": "res://s.png" }, "tex": "res://alias.png",
				           "label": "res://label.png", "blob": { "deep": "engine://e.png" } }
			},
			"overrides": [ { "target": [], "component": "mesh", "path": "/material", "value": "res://other.desc" } ]
		} ]
	} ]
})";

struct references_fixture : test_util::scene_env
{
	references_fixture()
	{
		descs->register_desc<reference_table_desc>("refs.table");
		BOOST_REQUIRE(descs->register_schema(desc::parse_type_schema(json::parse(R"({
			"name": "refs.slot", "kind": "struct", "fields": [ { "name": "image", "kind": "res_tag", "default": "" } ]
		})").as_object())));
		desc::schema_registry nested;
		nested.add(*descs->find_schema("refs.slot"));
		BOOST_REQUIRE(descs->register_schema(desc::parse_type_schema(json::parse(R"({
			"name": "refs.probe", "version": 1,
			"fields": [
				{ "name": "list", "kind": "array", "element": "res_tag" },
				{ "name": "slot", "kind": "structure", "ref_type": "refs.slot", "default": { "image": "" } },
				{ "name": "texture", "kind": "res_tag", "default": "", "read_aliases": ["/tex"] },
				{ "name": "label", "kind": "string", "default": "" },
				{ "name": "blob", "kind": "custom" }
			]
		})").as_object(), &nested)));
		references = edt::collect_raw_references(json::parse(k_level));
	}

	const edt::raw_reference* at(std::string_view pointer) const
	{
		const auto found = std::ranges::find_if(references, [pointer](const edt::raw_reference& r) { return r.json_pointer == pointer; });
		return found == references.end() ? nullptr : &*found;
	}

	std::optional<edt::usage_kind> kind_at(std::string_view pointer) const
	{
		const edt::raw_reference* found = at(pointer);
		BOOST_REQUIRE_MESSAGE(found, std::string(pointer));
		return edt::classify_reference(*found, descs.get());
	}

	std::vector<edt::raw_reference> references;
};

const std::string k_node = "/worlds/0/children/0";

}

BOOST_FIXTURE_TEST_SUITE(EdtAssetReferencesTests, references_fixture)

BOOST_AUTO_TEST_CASE(EveryPlaceOfAReference_IsCollectedWithItsPointerAndOwner)
{
	const edt::raw_reference* sky = at(k_node + "/components/sky");
	BOOST_REQUIRE(sky);
	BOOST_TEST(sky->path == "res://sky.desc");
	BOOST_TEST((sky->place == edt::reference_place::component));
	BOOST_TEST(sky->owner_type == "world_desc");
	BOOST_TEST(sky->owner_pointer == "/worlds/0");

	const edt::raw_reference* material = at(k_node + "/components/mesh/material");
	BOOST_REQUIRE(material);
	BOOST_TEST(material->path == "res://m.desc");
	BOOST_REQUIRE(material->guid.has_value());
	BOOST_TEST(res::to_text(*material->guid) == k_guid);
	BOOST_TEST(material->owner_type == "mesh_node_desc");
	BOOST_TEST(material->owner_pointer == k_node + "/components/mesh");

	const edt::raw_reference* parent = at(k_node + "/components/inst/__parent");
	BOOST_REQUIRE(parent);
	BOOST_TEST((parent->place == edt::reference_place::parent));
	BOOST_TEST(parent->path == "res://rig.prefab.desc");

	const edt::raw_reference* guid_only = at(k_node + "/components/table/named/b");
	BOOST_REQUIRE(guid_only);
	BOOST_TEST(guid_only->path.empty());
	BOOST_TEST(guid_only->guid.has_value());

	BOOST_TEST(at("/note") != nullptr, "a string of another scheme is kept: the schema decides later");
	BOOST_TEST(at("/plain") == nullptr, "a string without a protocol is no reference");
	BOOST_TEST(at("/vertices/0") == nullptr);
	BOOST_TEST(references.size() == 13u);
}

BOOST_AUTO_TEST_CASE(TheSchemaOfTheOwner_SaysWhatEachReferenceIs)
{
	using kind = edt::usage_kind;
	BOOST_TEST((kind_at(k_node + "/components/sky") == kind::component));
	BOOST_TEST((kind_at(k_node + "/components/inst/__parent") == kind::parent));
	BOOST_TEST((kind_at(k_node + "/components/mesh/geometry") == kind::field));
	BOOST_TEST((kind_at(k_node + "/components/mesh/material") == kind::field), "a {guid, path} in a desc_ref field");
	BOOST_TEST((kind_at(k_node + "/components/table/named/a") == kind::field), "a map of tags");
	BOOST_TEST((kind_at(k_node + "/components/table/named/b") == kind::field));
	BOOST_TEST((kind_at(k_node + "/components/probe/list/0") == kind::field), "a list of tags");
	BOOST_TEST((kind_at(k_node + "/components/probe/slot/image") == kind::field), "a structure");
	BOOST_TEST((kind_at(k_node + "/components/probe/tex") == kind::field), "a read alias");
	BOOST_TEST((kind_at(k_node + "/components/probe/label") == kind::loose), "a string field is no reference field");
	BOOST_TEST((kind_at(k_node + "/components/probe/blob/deep") == kind::loose), "the value of a custom field");
	BOOST_TEST((kind_at(k_node + "/overrides/0/value") == kind::loose), "the record of a prefab instance");
	BOOST_TEST(!kind_at("/note").has_value(), "another scheme where no schema field is");
}

BOOST_AUTO_TEST_CASE(WithoutADescSystem_OnlyThePlaceSaysAnything)
{
	BOOST_TEST((edt::classify_reference(*at(k_node + "/components/mesh/geometry"), nullptr) == edt::usage_kind::loose));
	BOOST_TEST((edt::classify_reference(*at(k_node + "/components/sky"), nullptr) == edt::usage_kind::component));
}

BOOST_AUTO_TEST_SUITE_END()
