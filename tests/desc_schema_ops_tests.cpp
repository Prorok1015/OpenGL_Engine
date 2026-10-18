#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <boost/json.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <snk/desc/desc_describe_ops.h>
#include <snk/desc/desc_dynamic.h>
#include <snk/desc/desc_json_ops.h>
#include <snk/desc/desc_schema_desc.h>
#include <snk/desc/desc_schema_ops.h>

namespace json = boost::json;

namespace ops_test {

	enum class mode { off, on, automatic };
	BOOST_DESCRIBE_ENUM(mode, off, on, automatic)

	struct inner
	{
		float scale = 1.0f;
		bool flag = false;
	};
	BOOST_DESCRIBE_STRUCT(inner, (), (scale, flag))

	class widget : public desc::schema_desc<widget, desc::schema_kind::component>
	{
	public:
		static constexpr std::string_view __type = "ops.widget";

		glm::vec3 position{ 0.0f, 1.0f, 2.0f };
		std::string label = "widget";
		mode state = mode::on;
		std::vector<int> ids{ 1, 2 };
		inner nested;
		float fov = 90.0f;
		res::tag texture;
		std::uint32_t count = 3;
		glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };

		BOOST_DESCRIBE_CLASS(widget, (), (position, label, state, ids, nested, fov, texture, count, rotation), (), ())
	};

	class sibling : public desc::schema_desc<sibling>
	{
	public:
		static constexpr std::string_view __type = "ops.sibling";

		std::string label = "sibling";
		int extra = 7;

		BOOST_DESCRIBE_CLASS(sibling, (), (label, extra), (), ())
	};

}

namespace desc {

	template<>
	struct enum_names<ops_test::mode>
	{
		static constexpr std::string_view name = "ops_mode";
		static constexpr std::array<std::pair<ops_test::mode, std::string_view>, 1> overrides{ { { ops_test::mode::on, "ON" } } };
		static constexpr ops_test::mode fallback = ops_test::mode::off;
	};

	template<>
	inline constexpr auto field_attrs<ops_test::widget> = attrs(
		f("fov").key("/camera/fov").range(1, 179),
		f("texture").alias("/tex").write_if_valid(),
		f("count").write_if_not_default(),
		f("state").ref("ops_mode"),
		f("nested").ref("ops.inner"));

}

namespace {

	json::object parse(const char* text)
	{
		return json::parse(text).as_object();
	}

	json::object serialized(const desc::desc_base& value)
	{
		json::object out;
		value.serialize(out);
		return out;
	}

	struct ops_fixture : test_util::ram_res_env
	{
		ops_fixture() : ds(*rs) {}

		std::shared_ptr<desc::dynamic_desc> spawn(std::string_view type)
		{
			return std::dynamic_pointer_cast<desc::dynamic_desc>(ds.create_instance(type, res::tag{}));
		}

		desc::desc_system ds;
	};

	constexpr const char* k_inner_schema = R"({
		"name": "ops.inner", "kind": "struct",
		"fields": [ { "name": "scale", "kind": "f32", "default": 1.0 }, { "name": "flag", "kind": "bool", "default": false } ]
	})";

	constexpr const char* k_mode_enum = R"({ "name": "ops_mode", "items": { "off": 0, "ON": 1, "automatic": 2 }, "fallback": 0 })";

	constexpr const char* k_health_schema = R"({
		"name": "mymod.health", "kind": "component",
		"fields": [
			{ "name": "hp", "kind": "f32", "default": 100, "range": [0, 1000] },
			{ "name": "label", "key": "/info/label", "kind": "string", "default": "unit", "read_aliases": ["/name"] },
			{ "name": "state", "kind": "enumeration", "ref_type": "ops_mode", "default": "ON" },
			{ "name": "ids", "kind": "array", "element": "i32", "default": [1, 2] },
			{ "name": "nested", "kind": "structure", "ref_type": "ops.inner" },
			{ "name": "pos", "kind": "vec3", "default": [0, 1, 2] },
			{ "name": "level", "kind": "u32", "default": 3, "write": "if_not_default" },
			{ "name": "owner", "kind": "res_tag", "default": "", "write": "if_valid" }
		]
	})";

	void register_health(desc::desc_system& ds)
	{
		ds.schemas().add_enum(desc::parse_enum_schema(parse(k_mode_enum)));
		ds.register_schema(desc::parse_type_schema(parse(k_inner_schema)));
		ds.register_schema(desc::parse_type_schema(parse(k_health_schema), &ds.schemas()));
	}

}

BOOST_AUTO_TEST_SUITE(DescSchemaOpsTests)

BOOST_FIXTURE_TEST_CASE(RoundTripNeedsNoHandwrittenCode, ops_fixture)
{
	ops_test::widget source;
	source.position = { 3.0f, 4.0f, 5.0f };
	source.label = "roundtrip";
	source.state = ops_test::mode::automatic;
	source.ids = { 9, 8, 7 };
	source.nested.scale = 2.5f;
	source.nested.flag = true;
	source.fov = 60.0f;
	source.texture = res::tag{ "res://a/b.png" };
	source.count = 11;
	source.rotation = glm::quat{ 0.0f, 0.0f, 1.0f, 0.0f };

	const json::object written = serialized(source);

	ops_test::widget copy;
	copy.deserialize(ds, written);

	BOOST_TEST((copy.position == source.position));
	BOOST_TEST(copy.label == "roundtrip");
	BOOST_TEST((copy.state == ops_test::mode::automatic));
	BOOST_TEST((copy.ids == source.ids));
	BOOST_TEST(copy.nested.scale == 2.5f);
	BOOST_TEST(copy.nested.flag);
	BOOST_TEST(copy.fov == 60.0f);
	BOOST_TEST(copy.texture.view() == "res://a/b.png");
	BOOST_TEST(copy.count == 11u);
	BOOST_TEST((copy.rotation == source.rotation));
	BOOST_TEST(desc::equal(ops_test::widget::binding(), &source, &copy));
	BOOST_TEST(json::value(serialized(copy)) == json::value(written));
}

BOOST_AUTO_TEST_CASE(SerializeFollowsSchemaOrderKeysAndEnumNames)
{
	ops_test::widget value;
	value.fov = 45.0f;
	const json::object written = serialized(value);

	std::vector<std::string> keys;
	for (const auto& member : written) {
		keys.emplace_back(member.key());
	}
	const std::vector<std::string> expected{ "position", "label", "state", "ids", "nested", "camera", "rotation" };
	BOOST_TEST(keys == expected, boost::test_tools::per_element());

	BOOST_TEST(written.at("state").as_string() == "ON");
	BOOST_TEST(written.at("camera").at("fov").as_double() == 45.0);
}

BOOST_AUTO_TEST_CASE(WritePoliciesIfValidAndIfNotDefault)
{
	ops_test::widget value;
	json::object written = serialized(value);
	BOOST_TEST(!written.contains("texture"));
	BOOST_TEST(!written.contains("count"));

	value.texture = res::tag{ "res://t.png" };
	value.count = 4;
	written = serialized(value);
	BOOST_TEST(written.at("texture").as_string() == "res://t.png");
	BOOST_TEST(written.at("count").as_uint64() == 4u);

	value.count = 3;
	BOOST_TEST(!serialized(value).contains("count"));
}

BOOST_AUTO_TEST_CASE(AlwaysPolicyWritesTheDefaultValueToo)
{
	ops_test::widget value;
	const json::object written = serialized(value);
	BOOST_TEST(written.at("label").as_string() == "widget");
	BOOST_TEST(written.at("nested").at("scale").as_double() == 1.0);
}

BOOST_FIXTURE_TEST_CASE(AliasIsReadWhenTheKeyIsAbsent, ops_fixture)
{
	ops_test::widget value;
	value.deserialize(ds, parse(R"({ "tex": "res://legacy.png" })"));
	BOOST_TEST(value.texture.view() == "res://legacy.png");

	value.deserialize(ds, parse(R"({ "tex": "res://other.png", "texture": "res://current.png" })"));
	BOOST_TEST(value.texture.view() == "res://current.png");
}

BOOST_FIXTURE_TEST_CASE(MissingKeysLeaveFieldsUntouchedAndBadValuesAreKept, ops_fixture)
{
	ops_test::widget value;
	value.label = "kept";
	value.fov = 33.0f;
	value.deserialize(ds, parse(R"({ "camera": { "fov": "wide" }, "ids": [1, "x"], "count": -4, "unknown": 1 })"));

	BOOST_TEST(value.label == "kept");
	BOOST_TEST(value.fov == 33.0f);
	BOOST_TEST((value.ids == std::vector<int>{ 1, 2 }));
	BOOST_TEST(value.count == 3u);
}

BOOST_FIXTURE_TEST_CASE(EnumReadsNamesWithFallbackAndIntegers, ops_fixture)
{
	ops_test::widget value;
	value.deserialize(ds, parse(R"({ "state": "automatic" })"));
	BOOST_TEST((value.state == ops_test::mode::automatic));

	value.deserialize(ds, parse(R"({ "state": "no_such_mode" })"));
	BOOST_TEST((value.state == ops_test::mode::off));

	value.deserialize(ds, parse(R"({ "state": 1 })"));
	BOOST_TEST((value.state == ops_test::mode::on));

	value.state = ops_test::mode::automatic;
	value.deserialize(ds, parse(R"({ "state": true })"));
	BOOST_TEST((value.state == ops_test::mode::automatic));
}

BOOST_FIXTURE_TEST_CASE(ParentCopyPlusOverridesKeepsUntouchedFields, ops_fixture)
{
	ops_test::widget parent;
	parent.position = { 7.0f, 7.0f, 7.0f };
	parent.label = "parent";
	parent.nested.scale = 4.0f;
	parent.nested.flag = true;

	ops_test::widget child;
	parent.copy_to(child);
	child.deserialize(ds, parse(R"({ "label": "child", "nested": { "scale": 5.0 } })"));

	BOOST_TEST((child.position == parent.position));
	BOOST_TEST(child.label == "child");
	BOOST_TEST(child.nested.scale == 5.0f);
	BOOST_TEST(child.nested.flag);
	BOOST_TEST(parent.label == "parent");
}

BOOST_AUTO_TEST_CASE(SchemaOnlyTypeIsSpawnedThroughTheFactoryMap)
{
	ops_fixture fx;
	register_health(fx.ds);

	BOOST_REQUIRE(fx.ds.is_registered("mymod.health"));
	const auto& components = fx.ds.get_component_type_names();
	BOOST_TEST((std::ranges::find(components, std::string{ "mymod.health" }) != components.end()));

	auto health = fx.spawn("mymod.health");
	BOOST_REQUIRE(health);
	BOOST_TEST(health->get_type() == "mymod.health");
	BOOST_TEST(health->get("/hp").as_double() == 100.0);
	BOOST_TEST(health->get("/info/label").as_string() == "unit");
	BOOST_TEST(health->get("/state").as_string() == "ON");
	BOOST_TEST(health->get("/ids").as_array().size() == 2u);
	BOOST_TEST(health->get("/pos").as_array().size() == 3u);
	BOOST_TEST(health->get("/nested").at("scale").as_double() == 1.0);
	BOOST_TEST(health->get("/missing").is_null());

	BOOST_TEST(health->set("/hp", json::value(250)));
	BOOST_TEST(!health->set("/hp", json::value("lots")));
	BOOST_TEST(health->get("/hp").as_double() == 250.0);

	health->deserialize(fx.ds, parse(R"({
		"name": "legacy", "state": "automatic", "ids": [5, 6, 7], "pos": [1, 2, 3],
		"nested": { "scale": 3.0 }, "level": 9, "owner": "res://who.desc"
	})"));
	const json::object written = serialized(*health);
	BOOST_TEST(written.at("info").at("label").as_string() == "legacy");
	BOOST_TEST(written.at("state").as_string() == "automatic");
	BOOST_TEST(written.at("ids").as_array().size() == 3u);
	BOOST_TEST(written.at("nested").at("scale").as_double() == 3.0);
	BOOST_TEST(written.at("nested").at("flag").as_bool() == false);
	BOOST_TEST(written.at("level").as_uint64() == 9u);
	BOOST_TEST(written.at("owner").as_string() == "res://who.desc");
	BOOST_TEST(written.at("hp").as_double() == 250.0);

	auto reloaded = fx.spawn("mymod.health");
	reloaded->deserialize(fx.ds, written);
	BOOST_TEST(json::value(serialized(*reloaded)) == json::value(written));

	fx.ds.unregister_desc("mymod.health");
	BOOST_TEST(!fx.ds.is_registered("mymod.health"));
	BOOST_TEST(fx.ds.create_instance("mymod.health", res::tag{}) == nullptr);
}

BOOST_AUTO_TEST_CASE(SchemaOnlyDefaultsHonorWritePolicies)
{
	ops_fixture fx;
	register_health(fx.ds);
	auto health = fx.spawn("mymod.health");
	const json::object written = serialized(*health);
	BOOST_TEST(!written.contains("level"));
	BOOST_TEST(!written.contains("owner"));
	BOOST_TEST(written.contains("hp"));
}

BOOST_AUTO_TEST_CASE(SchemaOnlyTypeWithAnUnknownNestedSchemaGetsNoFactory)
{
	ops_fixture fx;
	desc::schema_registry resolve;
	resolve.add(desc::parse_type_schema(parse(k_inner_schema)));
	fx.ds.register_schema(desc::parse_type_schema(parse(k_health_schema), &resolve));
	BOOST_TEST(fx.ds.find_schema("mymod.health") != nullptr);
	BOOST_TEST(!fx.ds.is_registered("mymod.health"));
}

BOOST_AUTO_TEST_CASE(DescribedTypeRegisteredThroughTheSystemKeepsItsFactory)
{
	ops_fixture fx;
	fx.ds.register_component_desc<ops_test::widget>("ops.widget");
	BOOST_TEST(fx.ds.is_registered("ops.widget"));
	BOOST_TEST(fx.ds.get_component_type_names().size() == 1u);

	auto instance = fx.ds.create_instance("ops.widget", res::tag{});
	BOOST_REQUIRE(instance);
	BOOST_TEST(dynamic_cast<ops_test::widget*>(instance.get()) != nullptr);
}

BOOST_AUTO_TEST_CASE(CopyBetweenSameSchemaIsMemberWise)
{
	ops_fixture fx;
	register_health(fx.ds);
	auto a = fx.spawn("mymod.health");
	auto b = fx.spawn("mymod.health");
	a->set("/hp", json::value(5));
	a->set("/ids", json::parse("[4, 5, 6]"));
	a->set("/nested", json::parse(R"({ "scale": 8.0, "flag": true })"));
	a->copy_to(*b);
	BOOST_TEST(json::value(serialized(*a)) == json::value(serialized(*b)));
}

BOOST_AUTO_TEST_CASE(CopyBetweenDifferentSchemasGoesThroughJsonOnCommonKeys)
{
	ops_fixture fx;
	fx.ds.register_desc<ops_test::widget>("ops.widget");
	fx.ds.register_desc<ops_test::sibling>("ops.sibling");

	auto widget = std::dynamic_pointer_cast<ops_test::widget>(fx.ds.create_instance("ops.widget", res::tag{}));
	auto sibling = std::dynamic_pointer_cast<ops_test::sibling>(fx.ds.create_instance("ops.sibling", res::tag{}));
	widget->label = "from widget";

	widget->copy_to(*sibling);
	BOOST_TEST(sibling->label == "from widget");
	BOOST_TEST(sibling->extra == 7);

	sibling->label = "from sibling";
	sibling->extra = 70;
	sibling->copy_to(*widget);
	BOOST_TEST(widget->label == "from sibling");
	BOOST_TEST(widget->fov == 90.0f);
}

BOOST_AUTO_TEST_CASE(CopyBetweenDynamicAndCppTypesGoesThroughJson)
{
	ops_fixture fx;
	fx.ds.register_schema(desc::parse_type_schema(parse(R"({
		"name": "ops.mini", "fields": [ { "name": "label", "kind": "string", "default": "mini" }, { "name": "only_here", "kind": "i32", "default": 1 } ]
	})")));
	fx.ds.register_desc<ops_test::sibling>("ops.sibling");
	auto mini = fx.spawn("ops.mini");
	auto sibling = std::dynamic_pointer_cast<ops_test::sibling>(fx.ds.create_instance("ops.sibling", res::tag{}));

	mini->set("/label", json::value("from mini"));
	mini->set("/only_here", json::value(5));
	mini->copy_to(*sibling);
	BOOST_TEST(sibling->label == "from mini");
	BOOST_TEST(sibling->extra == 7);

	sibling->label = "from sibling";
	sibling->extra = 70;
	sibling->copy_to(*mini);
	BOOST_TEST(mini->get("/label").as_string() == "from sibling");
	BOOST_TEST(mini->get("/only_here").as_int64() == 5);
}

BOOST_AUTO_TEST_CASE(DiffUsesFloatToleranceAndRecordsChangedFieldsOnly)
{
	const auto schema = desc::schema_generator<ops_test::widget>::make("ops.widget", 1, desc::schema_kind::component);
	const json::value base = json::parse(R"({ "position": [0, 1, 2], "label": "a", "camera": { "fov": 90.0 }, "ids": [1, 2], "count": 3 })");

	json::value edited = base;
	edited.as_object()["camera"].as_object()["fov"] = 90.000001;
	BOOST_TEST(desc::diff(schema, base, edited).empty());

	edited.as_object()["camera"].as_object()["fov"] = 75.0;
	edited.as_object()["position"] = json::parse("[0, 1, 2.5]");
	edited.as_object()["ids"] = json::parse("[1, 2, 3]");
	const auto changes = desc::diff(schema, base, edited);
	BOOST_REQUIRE_EQUAL(changes.size(), 3u);
	BOOST_TEST(changes[0].pointer == "/position");
	BOOST_TEST(changes[0].value.as_array().size() == 3u);
	BOOST_TEST(changes[1].pointer == "/ids");
	BOOST_TEST(changes[1].value.as_array().size() == 3u);
	BOOST_TEST(changes[2].pointer == "/camera/fov");
	BOOST_TEST(changes[2].value.as_double() == 75.0);
}

BOOST_AUTO_TEST_CASE(DiffRecursesIntoStructuresThroughTheRegistry)
{
	const auto schema = desc::schema_generator<ops_test::widget>::make("ops.widget", 1, desc::schema_kind::component);
	desc::schema_registry registry;
	registry.add(desc::schema_generator<ops_test::inner>::make("ops.inner", 1, desc::schema_kind::structure));

	const json::value base = json::parse(R"({ "nested": { "scale": 1.0, "flag": false } })");
	const json::value edited = json::parse(R"({ "nested": { "scale": 2.0, "flag": false } })");

	const auto with_schema = desc::diff(schema, base, edited, &registry);
	BOOST_REQUIRE_EQUAL(with_schema.size(), 1u);
	BOOST_TEST(with_schema[0].pointer == "/nested/scale");

	const auto without_schema = desc::diff(schema, base, edited);
	BOOST_REQUIRE_EQUAL(without_schema.size(), 1u);
	BOOST_TEST(without_schema[0].pointer == "/nested/scale");
	BOOST_TEST(without_schema[0].value.as_double() == 2.0);

	BOOST_TEST(desc::diff(schema, base, base, &registry).empty());
}

BOOST_AUTO_TEST_CASE(ValidateOverrideAcceptsFieldsAndWarnsAboutStaleOnes)
{
	const auto schema = desc::schema_generator<ops_test::widget>::make("ops.widget", 1, desc::schema_kind::component);
	desc::schema_registry registry;
	registry.add_enum(desc::make_enum_schema<ops_test::mode>());
	registry.add(desc::schema_generator<ops_test::inner>::make("ops.inner", 1, desc::schema_kind::structure));

	using desc::override_status;
	const auto check = [&](const char* pointer, const char* value) {
		return desc::validate_override(schema, desc::json_change{ pointer, json::parse(value) }, &registry);
	};

	BOOST_TEST((check("/label", "\"x\"") == override_status::valid));
	BOOST_TEST((check("/camera/fov", "12") == override_status::valid));
	BOOST_TEST((check("/camera/fov", "\"wide\"") == override_status::bad_value));
	BOOST_TEST((check("/tex", "\"res://a\"") == override_status::valid));
	BOOST_TEST((check("/nested/scale", "2.5") == override_status::valid));
	BOOST_TEST((check("/nested/gone", "1") == override_status::stale));
	BOOST_TEST((check("/ids/1", "5") == override_status::valid));
	BOOST_TEST((check("/ids/1", "\"five\"") == override_status::bad_value));
	BOOST_TEST((check("/position/2", "0.5") == override_status::valid));
	BOOST_TEST((check("/position/3", "0.5") == override_status::stale));
	BOOST_TEST((check("/state", "\"ON\"") == override_status::valid));
	BOOST_TEST((check("/no_such_field", "1") == override_status::stale));
	BOOST_TEST((check("/label/deeper", "1") == override_status::stale));

	desc::json_change record{ "/no_such_field", json::value(1) };
	desc::validate_override(schema, record, &registry);
	BOOST_TEST(record.pointer == "/no_such_field");
	BOOST_TEST(record.value.as_int64() == 1);
}

BOOST_AUTO_TEST_CASE(JsonEqualComparesNumbersByValueWithinTolerance)
{
	BOOST_TEST(desc::json_equal(json::value(1), json::value(1.0)));
	BOOST_TEST(desc::json_equal(json::value(std::uint64_t{ 5 }), json::value(std::int64_t{ 5 })));
	BOOST_TEST(!desc::json_equal(json::value(1.0), json::value(1.001), 1e-5));
	BOOST_TEST(desc::json_equal(json::value(1.0), json::value(1.0000001), 1e-5));
	BOOST_TEST(!desc::json_equal(json::value(std::int64_t{ 4 }), json::value(std::uint64_t{ 5 }), 1.0));
	BOOST_TEST(!desc::json_equal(json::parse("[1, 2]"), json::parse("[1, 2, 3]")));
}

BOOST_AUTO_TEST_CASE(EnumSchemaConvertsBothWaysWithFallback)
{
	const auto schema = desc::make_enum_schema<ops_test::mode>();
	BOOST_TEST(desc::enum_to_json(schema, 1).as_string() == "ON");
	BOOST_TEST(desc::enum_to_json(schema, 99).as_int64() == 99);
	BOOST_TEST(desc::enum_from_json(schema, json::value("automatic")).value() == 2);
	BOOST_TEST(desc::enum_from_json(schema, json::value("nope")).value() == 0);
	BOOST_TEST(!desc::enum_from_json(schema, json::value(99)).has_value());

	desc::enum_schema strict;
	strict.items.push_back({ "a", 1 });
	BOOST_TEST(!desc::enum_from_json(strict, json::value("b")).has_value());
}

BOOST_AUTO_TEST_SUITE_END()
