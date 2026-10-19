#include <boost/test/unit_test.hpp>
#include <boost/json.hpp>
#include <array>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <snk/desc/desc_describe.h>
#include <snk/desc/desc_system.h>
#include <snk/resource/res_system.h>
#include "test_manual_tasks.hpp"

namespace json = boost::json;

namespace schema_test {

	enum class blend { opaque, alpha, additive };
	BOOST_DESCRIBE_ENUM(blend, opaque, alpha, additive)

	struct inner
	{
		float scale = 2.0f;
		bool flag = true;
	};
	BOOST_DESCRIBE_STRUCT(inner, (), (scale, flag))

	class probe : public desc::desc_base
	{
	public:
		void copy_to(desc::desc_base&) const override {}
		void deserialize(desc::desc_system&, const json::object&) override {}
		void serialize(json::object&) const override {}

		glm::vec3 position{ 0.0f, 1.0f, 2.0f };
		std::string label = "probe";
		blend mode = blend::alpha;
		std::vector<int> ids{ 1, 2 };
		inner nested;

	private:
		float m_fov = 90.0f;
		glm::vec4 m_tint{ 1.0f, 0.5f, 0.25f, 1.0f };
		std::uint32_t m_count = 3;
		res::tag m_texture;

		BOOST_DESCRIBE_CLASS(probe, (), (position, label, mode, ids, nested), (), (m_fov, m_tint, m_count, m_texture))
	};

	class typo_probe
	{
	public:
		float fov = 1.0f;
		BOOST_DESCRIBE_CLASS(typo_probe, (), (fov), (), ())
	};

}

namespace desc {

	template<>
	struct enum_names<schema_test::blend>
	{
		static constexpr std::string_view name = "blend_mode";
		static constexpr std::array<std::pair<schema_test::blend, std::string_view>, 1> overrides{ { { schema_test::blend::alpha, "ALPHA_BLEND" } } };
		static constexpr schema_test::blend fallback = schema_test::blend::opaque;
	};

	template<>
	inline constexpr auto field_attrs<schema_test::probe> = attrs(
		f("m_fov").key("/fov").range(1, 179).degrees(),
		f("m_tint").key("/tint").color().tooltip("Surface tint"),
		f("m_texture").key("/texture").alias("/tex").write_if_valid().filter("png"),
		f("mode").ref("blend_mode"),
		f("nested").ref("schema_test.inner"),
		f("m_count").hidden());

	template<>
	inline constexpr auto field_attrs<schema_test::typo_probe> = attrs(f("fov").range(0, 1), f("fvo").hidden());

}

namespace {

	constexpr const char* k_probe_schema_json = R"({
		"name": "test.probe", "version": 1, "kind": "component",
		"fields": [
			{ "name": "position", "kind": "vec3", "default": [0, 1, 2] },
			{ "name": "label", "kind": "string", "default": "probe" },
			{ "name": "mode", "kind": "enumeration", "ref_type": "blend_mode", "default": "ALPHA_BLEND" },
			{ "name": "ids", "kind": "array", "element": "i32", "default": [1, 2] },
			{ "name": "nested", "kind": "structure", "ref_type": "schema_test.inner", "default": { "scale": 2.0, "flag": true } },
			{ "name": "m_fov", "key": "/fov", "kind": "f32", "default": 90, "range": [1, 179], "degrees": true },
			{ "name": "m_tint", "key": "/tint", "kind": "vec4", "default": [1, 0.5, 0.25, 1], "color": true, "tooltip": "Surface tint" },
			{ "name": "m_count", "kind": "u32", "default": 3, "editor_hidden": true },
			{ "name": "m_texture", "key": "/texture", "kind": "res_tag", "default": "", "write": "if_valid", "read_aliases": ["/tex"], "filter": "png" }
		]
	})";

	json::object parse(const char* text)
	{
		return json::parse(text).as_object();
	}

	// The probe has a structure field, so its layout needs the nested schema.
	desc::type_schema parse_probe()
	{
		desc::schema_registry registry;
		registry.add(desc::parse_type_schema(parse(R"({
			"name": "schema_test.inner", "kind": "struct",
			"fields": [ { "name": "scale", "kind": "f32", "default": 2.0 }, { "name": "flag", "kind": "bool", "default": true } ]
		})")));
		return desc::parse_type_schema(parse(k_probe_schema_json), &registry);
	}

	const desc::field_schema* field_named(const desc::type_schema& schema, std::string_view name)
	{
		for (const auto& field : schema.fields) {
			if (field.name == name) {
				return &field;
			}
		}
		return nullptr;
	}

	constexpr auto k_table = desc::attrs(desc::f("a").range(0, 1, 0.1).alias("/x").alias("/y"), desc::f("b"));
	static_assert(k_table.find("a") != nullptr && k_table.find("a")->has_range && k_table.find("a")->alias_count == 2);
	static_assert(k_table.find("b") != nullptr && !k_table.find("b")->has_range);
	static_assert(k_table.find("c") == nullptr);
	static_assert(desc::attrs_match_members<schema_test::probe>());
	static_assert(!desc::attrs_match_members<schema_test::typo_probe>(), "a misspelled attr name must be detected");
	static_assert(desc::kind_of<float> == desc::field_kind::f32);
	static_assert(desc::kind_of<const std::int32_t&> == desc::field_kind::i32);
	static_assert(desc::kind_of<std::uint64_t> == desc::field_kind::u64);
	static_assert(desc::kind_of<glm::quat> == desc::field_kind::quat);
	static_assert(desc::kind_of<std::vector<float>> == desc::field_kind::array);
	static_assert(desc::element_kind_of<std::vector<float>>() == desc::field_kind::f32);
	static_assert(desc::kind_of<schema_test::blend> == desc::field_kind::enumeration);
	static_assert(desc::kind_of<schema_test::inner> == desc::field_kind::structure);
	static_assert(desc::kind_of<std::int16_t> == desc::field_kind::custom);

	// resource_system asserts that its resources directory exists; base classes are built before members.
	struct res_dir_guard
	{
		res_dir_guard() { std::filesystem::create_directories("./res"); }
	};

	struct schema_fixture : res_dir_guard
	{
		schema_fixture() : ds(rs) {}

		ds::task_system tasks{ test_util::manual_tasks() };
		res::resource_system rs{ tasks };
		desc::desc_system ds;
	};

}

BOOST_AUTO_TEST_SUITE(DescSchemaTests)

BOOST_AUTO_TEST_CASE(CppSchemaHasDefaultKeysKindsAndAttributes)
{
	const auto schema = desc::schema_generator<schema_test::probe>::make("test.probe", 1, desc::schema_kind::component);

	BOOST_TEST(schema.name == "test.probe");
	BOOST_TEST(schema.id == desc::schema_id("test.probe"));
	BOOST_TEST(schema.is_component);
	BOOST_REQUIRE_EQUAL(schema.fields.size(), 9u);

	const auto* position = field_named(schema, "position");
	BOOST_REQUIRE(position);
	BOOST_TEST(position->key == "/position");
	BOOST_TEST((position->kind == desc::field_kind::vec3));
	BOOST_TEST((position->default_value == json::value({ 0.0, 1.0, 2.0 })));

	const auto* fov = field_named(schema, "m_fov");
	BOOST_REQUIRE(fov);
	BOOST_TEST(fov->key == "/fov");
	BOOST_TEST((fov->kind == desc::field_kind::f32));
	BOOST_TEST(fov->degrees);
	BOOST_REQUIRE(fov->range.has_value());
	BOOST_TEST(fov->range->min == 1.0);
	BOOST_TEST(fov->range->max == 179.0);
	BOOST_TEST((fov->default_value == json::value(90.0)));

	const auto* tint = field_named(schema, "m_tint");
	BOOST_REQUIRE(tint);
	BOOST_TEST(tint->color);
	BOOST_TEST(tint->tooltip == "Surface tint");

	const auto* texture = field_named(schema, "m_texture");
	BOOST_REQUIRE(texture);
	BOOST_TEST((texture->kind == desc::field_kind::res_tag));
	BOOST_TEST((texture->write == desc::write_policy::if_valid));
	BOOST_REQUIRE_EQUAL(texture->read_aliases.size(), 1u);
	BOOST_TEST(texture->read_aliases.front() == "/tex");

	const auto* count = field_named(schema, "m_count");
	BOOST_REQUIRE(count);
	BOOST_TEST(count->key == "/m_count");
	BOOST_TEST(count->editor_hidden);

	const auto* ids = field_named(schema, "ids");
	BOOST_REQUIRE(ids);
	BOOST_TEST((ids->kind == desc::field_kind::array));
	BOOST_TEST((ids->element == desc::field_kind::i32));
}

BOOST_AUTO_TEST_CASE(CppSchemaAndJsonSchemaOfTheSameTypeAreEqual)
{
	const auto generated = desc::schema_generator<schema_test::probe>::make("test.probe", 1, desc::schema_kind::component);
	const auto parsed = parse_probe();

	BOOST_REQUIRE_EQUAL(generated.fields.size(), parsed.fields.size());
	for (std::size_t i = 0; i < generated.fields.size(); ++i) {
		BOOST_CHECK_MESSAGE(generated.fields[i] == parsed.fields[i], "field '" << generated.fields[i].name << "' differs");
	}
	BOOST_CHECK(generated == parsed);
}

BOOST_AUTO_TEST_CASE(SchemasWithDifferentAttributesAreNotEqual)
{
	auto generated = desc::schema_generator<schema_test::probe>::make("test.probe", 1, desc::schema_kind::component);
	auto parsed = parse_probe();
	parsed.fields[5].range->max = 180.0;
	BOOST_CHECK(!(generated == parsed));

	parsed = parse_probe();
	parsed.version = 2;
	BOOST_CHECK(!(generated == parsed));
}

BOOST_AUTO_TEST_CASE(EnumNamesOverrideDescribeNames)
{
	BOOST_CHECK(desc::enum_item_name(schema_test::blend::opaque) == "opaque");
	BOOST_CHECK(desc::enum_item_name(schema_test::blend::alpha) == "ALPHA_BLEND");
	BOOST_CHECK(desc::enum_item_name(schema_test::blend::additive) == "additive");

	const auto generated = desc::make_enum_schema<schema_test::blend>();
	const auto parsed = desc::parse_enum_schema(parse(R"({
		"name": "blend_mode",
		"items": { "opaque": 0, "ALPHA_BLEND": 1, "additive": 2 },
		"fallback": 0
	})"));
	BOOST_CHECK(generated == parsed);
	BOOST_TEST(generated.name == "blend_mode");
	BOOST_REQUIRE(generated.fallback.has_value());
	BOOST_TEST(*generated.fallback == 0);

	const auto from_array = desc::parse_enum_schema(parse(R"({
		"name": "blend_mode",
		"items": [ { "name": "opaque", "value": 0 }, { "name": "ALPHA_BLEND", "value": 1 }, { "name": "additive", "value": 2 } ],
		"fallback": 0
	})"));
	BOOST_CHECK(generated == from_array);

	BOOST_REQUIRE(generated.find("ALPHA_BLEND"));
	BOOST_TEST(generated.find("ALPHA_BLEND")->value == 1);
	BOOST_CHECK(generated.find("alpha") == nullptr);
	BOOST_REQUIRE(generated.find_value(2));
	BOOST_TEST(generated.find_value(2)->name == "additive");

	BOOST_TEST(desc::schema_generator<schema_test::probe>::enums().size() == 1u);
	BOOST_CHECK(desc::schema_generator<schema_test::probe>::enums().front() == generated);
}

BOOST_AUTO_TEST_CASE(ValueToJsonFollowsKind)
{
	BOOST_CHECK(desc::value_to_json(true) == json::value(true));
	BOOST_CHECK(desc::value_to_json(-3) == json::value(std::int64_t{ -3 }));
	BOOST_CHECK(desc::value_to_json(std::uint32_t{ 7 }) == json::value(std::uint64_t{ 7 }));
	BOOST_CHECK(desc::value_to_json(0.5f) == json::value(0.5));
	BOOST_CHECK(desc::value_to_json(std::string{ "abc" }) == json::value("abc"));
	BOOST_CHECK(desc::value_to_json(glm::quat(1.0f, 0.0f, 0.0f, 0.0f)) == json::value({ 0.0, 0.0, 0.0, 1.0 }));
	BOOST_CHECK(desc::value_to_json(schema_test::blend::additive) == json::value("additive"));
	BOOST_CHECK(desc::value_to_json(std::vector<float>{ 1.0f, 2.0f }) == json::value({ 1.0, 2.0 }));
	BOOST_CHECK(desc::value_to_json(std::int16_t{ 1 }).is_null());

	const auto nested = desc::value_to_json(schema_test::inner{});
	BOOST_REQUIRE(nested.is_object());
	BOOST_CHECK(nested.at("scale") == json::value(2.0));
	BOOST_CHECK(nested.at("flag") == json::value(true));
}

BOOST_AUTO_TEST_CASE(LayoutUsesCAlignmentAndHandleIndices)
{
	const auto schema = desc::parse_type_schema(parse(R"({
		"name": "test.layout",
		"fields": [
			{ "name": "flag", "kind": "bool" },
			{ "name": "weight", "kind": "f64" },
			{ "name": "pos", "kind": "vec3" },
			{ "name": "label", "kind": "string" },
			{ "name": "count", "kind": "i32" }
		]
	})"));
	BOOST_REQUIRE_EQUAL(schema.fields.size(), 5u);
	BOOST_TEST(schema.fields[0].offset == 0u);
	BOOST_TEST(schema.fields[1].offset == 8u);
	BOOST_TEST(schema.fields[2].offset == 16u);
	BOOST_TEST(schema.fields[3].offset == 28u);
	BOOST_TEST(schema.fields[4].offset == 32u);
	BOOST_TEST(schema.layout.alignment == 8u);
	BOOST_TEST(schema.layout.size == 40u);

	BOOST_TEST(desc::layout_of(desc::field_kind::boolean).size == 1u);
	BOOST_TEST(desc::layout_of(desc::field_kind::res_tag).size == 4u);
	BOOST_TEST(desc::layout_of(desc::field_kind::array).size == 4u);
	BOOST_TEST(desc::layout_of(desc::field_kind::quat).size == 16u);
	BOOST_TEST(desc::layout_of(desc::field_kind::node_ref).alignment == 8u);
}

BOOST_AUTO_TEST_CASE(StructureLayoutNeedsTheNestedSchema)
{
	desc::schema_registry registry;
	registry.add(desc::parse_type_schema(parse(R"({
		"name": "test.point", "kind": "struct",
		"fields": [ { "name": "x", "kind": "f32" }, { "name": "y", "kind": "f32" } ]
	})")));
	const auto outer_json = parse(R"({
		"name": "test.outer",
		"fields": [
			{ "name": "enabled", "kind": "bool" },
			{ "name": "at", "kind": "structure", "ref_type": "test.point" }
		]
	})");

	BOOST_CHECK_THROW(desc::parse_type_schema(outer_json), std::runtime_error);

	const auto outer = desc::parse_type_schema(outer_json, &registry);
	BOOST_TEST(outer.fields[1].offset == 4u);
	BOOST_TEST(outer.layout.size == 12u);
	BOOST_TEST(outer.layout.alignment == 4u);
}

BOOST_AUTO_TEST_CASE(JsonDefaultsAreNormalizedToTheCppRepresentation)
{
	const auto schema = desc::parse_type_schema(parse(R"({
		"name": "test.defaults",
		"fields": [
			{ "name": "a", "kind": "f32", "default": 100 },
			{ "name": "b", "kind": "u32", "default": 5 },
			{ "name": "c", "kind": "i64", "default": 5 },
			{ "name": "d", "kind": "vec2", "default": [1, 2] },
			{ "name": "e", "kind": "f32", "default": 0.1 },
			{ "name": "f", "kind": "vec4" },
			{ "name": "g", "kind": "array", "element": "f32", "default": [1, 2] }
		]
	})"));
	BOOST_CHECK(schema.fields[0].default_value.is_double());
	BOOST_CHECK(schema.fields[1].default_value.is_uint64());
	BOOST_CHECK(schema.fields[2].default_value.is_int64());
	BOOST_CHECK(schema.fields[3].default_value.as_array()[0].is_double());
	BOOST_CHECK(schema.fields[4].default_value == json::value(static_cast<double>(0.1f)));
	BOOST_CHECK(schema.fields[5].default_value == json::value({ 0.0, 0.0, 0.0, 0.0 }));
	BOOST_CHECK(schema.fields[6].default_value.as_array()[1].is_double());
	BOOST_TEST(schema.fields[0].key == "/a");
	BOOST_CHECK(schema.find_field("/b") == &schema.fields[1]);
	BOOST_CHECK(schema.find_field("/zzz") == nullptr);
}

BOOST_AUTO_TEST_CASE(MalformedSchemasThrow)
{
	const auto fails = [](const char* text) {
		BOOST_CHECK_THROW(desc::parse_type_schema(parse(text)), std::runtime_error);
	};
	fails(R"({ "fields": [] })");
	fails(R"({ "name": "t" })");
	fails(R"({ "name": "t", "fields": [], "typo": 1 })");
	fails(R"({ "name": "t", "fields": [], "migrations": [] })");
	fails(R"({ "name": "t", "version": 0, "fields": [] })");
	fails(R"({ "name": "t", "kind": "widget", "fields": [] })");
	fails(R"({ "name": "t", "fields": [ { "name": "a", "kind": "float" } ] })");
	fails(R"({ "name": "t", "fields": [ { "name": "a" } ] })");
	fails(R"({ "name": "t", "fields": [ { "name": "a", "kind": "f32", "key": "a" } ] })");
	fails(R"({ "name": "t", "fields": [ { "name": "a", "kind": "f32" }, { "name": "b", "kind": "f32", "key": "/a" } ] })");
	fails(R"({ "name": "t", "fields": [ { "name": "a", "kind": "array" } ] })");
	fails(R"({ "name": "t", "fields": [ { "name": "a", "kind": "f32", "element": "f32" } ] })");
	fails(R"({ "name": "t", "fields": [ { "name": "a", "kind": "f32", "range": [5, 1] } ] })");
	fails(R"({ "name": "t", "fields": [ { "name": "a", "kind": "f32", "default": "x" } ] })");
	fails(R"({ "name": "t", "fields": [ { "name": "a", "kind": "u32", "default": -1 } ] })");
	fails(R"({ "name": "t", "fields": [ { "name": "a", "kind": "vec3", "default": [1, 2] } ] })");
	fails(R"({ "name": "t", "fields": [ { "name": "a", "kind": "f32", "write": "sometimes" } ] })");

	BOOST_CHECK_THROW(desc::parse_enum_schema(parse(R"({ "name": "e", "items": 3 })")), std::runtime_error);
	BOOST_CHECK_THROW(desc::parse_enum_schema(parse(R"({ "name": "e", "items": { "a": "x" } })")), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(MigrationsKeyIsReportedAsNotSupportedYet)
{
	try {
		desc::parse_type_schema(parse(R"({ "name": "t", "fields": [], "migrations": [] })"));
		BOOST_FAIL("expected an exception");
	} catch (const std::runtime_error& error) {
		BOOST_CHECK(std::string_view{ error.what() }.find("not supported yet") != std::string_view::npos);
	}
}

BOOST_AUTO_TEST_CASE(KindNamesRoundTrip)
{
	for (int i = 0; i <= static_cast<int>(desc::field_kind::custom); ++i) {
		const auto kind = static_cast<desc::field_kind>(i);
		const auto parsed = desc::field_kind_from_string(desc::to_string(kind));
		BOOST_REQUIRE(parsed.has_value());
		BOOST_CHECK(*parsed == kind);
	}
	BOOST_CHECK(!desc::field_kind_from_string("bool_").has_value());
	BOOST_CHECK(desc::write_policy_from_string("if_not_default") == desc::write_policy::if_not_default);
	BOOST_CHECK(desc::schema_kind_from_string("struct") == desc::schema_kind::structure);
}

BOOST_AUTO_TEST_CASE(RegistryFindsByNameAndIdAndKeepsPointersStable)
{
	desc::schema_registry registry;
	const auto* first = registry.add(desc::parse_type_schema(parse(R"({ "name": "a.one", "fields": [] })")));
	BOOST_REQUIRE(first);
	for (int i = 0; i < 64; ++i) {
		BOOST_REQUIRE(registry.add(desc::parse_type_schema(parse(("{\"name\": \"b.t" + std::to_string(i) + "\", \"fields\": []}").c_str()))));
	}

	BOOST_TEST(registry.size() == 65u);
	BOOST_CHECK(registry.find("a.one") == first);
	BOOST_CHECK(registry.find(desc::schema_id("a.one")) == first);
	BOOST_CHECK(registry.find("a.two") == nullptr);
	BOOST_CHECK(registry.contains("b.t3"));

	BOOST_CHECK(registry.add(desc::parse_type_schema(parse(R"({ "name": "a.one", "fields": [] })"))) == nullptr);
	BOOST_TEST(registry.size() == 65u);

	BOOST_CHECK(registry.remove("a.one"));
	BOOST_CHECK(!registry.remove("a.one"));
	BOOST_CHECK(registry.find(desc::schema_id("a.one")) == nullptr);
	BOOST_TEST(registry.type_names().size() == 64u);

	BOOST_CHECK(registry.add_enum(desc::make_enum_schema<schema_test::blend>()) != nullptr);
	BOOST_CHECK(registry.add_enum(desc::make_enum_schema<schema_test::blend>()) == nullptr);
	BOOST_CHECK(registry.find_enum("blend_mode") != nullptr);
	BOOST_CHECK(registry.remove_enum("blend_mode"));
	BOOST_CHECK(registry.find_enum("blend_mode") == nullptr);
}

BOOST_FIXTURE_TEST_CASE(DescSystemRegistersJsonSchemas, schema_fixture)
{
	const auto* stored = ds.register_schema(parse_probe());
	BOOST_REQUIRE(stored);
	BOOST_CHECK(ds.find_schema("test.probe") == stored);
	BOOST_CHECK(ds.schemas().find(desc::schema_id("test.probe")) == stored);
	BOOST_CHECK(!ds.is_registered("test.probe"));
	BOOST_CHECK(ds.find_schema("test.other") == nullptr);
}

BOOST_FIXTURE_TEST_CASE(SchemaVersionFollowsRegisteredMigrations, schema_fixture)
{
	ds.register_migration("test.versioned", 1, [](json::object&) {});
	BOOST_TEST(ds.current_version("test.versioned") == 2);

	const auto* stored = ds.register_schema(desc::parse_type_schema(parse(R"({ "name": "test.versioned", "version": 2, "fields": [] })")));
	BOOST_REQUIRE(stored);
	BOOST_TEST(stored->version == ds.current_version("test.versioned"));
}

BOOST_FIXTURE_TEST_CASE(MigrationRegisteredAfterTheDescBumpsTheSchemaVersion, schema_fixture)
{
	ds.register_desc<schema_test::probe>("test.late");
	BOOST_REQUIRE(ds.find_schema("test.late"));
	BOOST_TEST(ds.find_schema("test.late")->version == 1);

	ds.register_migration("test.late", 1, [](json::object&) {});
	BOOST_TEST(ds.current_version("test.late") == 2);
	BOOST_TEST(ds.find_schema("test.late")->version == 2);

	ds.unregister_migrations("test.late");
	BOOST_TEST(ds.find_schema("test.late")->version == 1);

	ds.register_migration("test.runtime", 1, [](json::object&) {});
	const auto* stored = ds.register_schema(desc::parse_type_schema(parse(R"({ "name": "test.runtime", "version": 2, "fields": [] })")));
	BOOST_REQUIRE(stored);
	ds.register_migration("test.runtime", 2, [](json::object&) {});
	BOOST_TEST(stored->version == 3);

	BOOST_CHECK_NO_THROW(ds.register_migration("test.none", 1, [](json::object&) {}));
}

BOOST_FIXTURE_TEST_CASE(RegisteringADescribedTypeRegistersItsSchema, schema_fixture)
{
	ds.register_component_desc<schema_test::probe>("test.probe_c");

	BOOST_CHECK(ds.is_registered("test.probe_c"));
	const auto* stored = ds.find_schema("test.probe_c");
	BOOST_REQUIRE(stored);
	BOOST_CHECK(stored->is_component);
	BOOST_CHECK(*stored == desc::schema_generator<schema_test::probe>::make("test.probe_c", 1, desc::schema_kind::component));
	BOOST_CHECK(ds.schemas().find_enum("blend_mode") != nullptr);

	ds.register_desc<schema_test::probe>("test.probe_d");
	BOOST_REQUIRE(ds.find_schema("test.probe_d"));
	BOOST_CHECK(!ds.find_schema("test.probe_d")->is_component);

	ds.unregister_desc("test.probe_c");
	BOOST_CHECK(ds.find_schema("test.probe_c") == nullptr);
	BOOST_CHECK(ds.find_schema("test.probe_d") != nullptr);
}

BOOST_FIXTURE_TEST_CASE(DescsWithoutDescribeStayWithoutSchema, schema_fixture)
{
	struct plain : desc::desc_base
	{
		void copy_to(desc::desc_base&) const override {}
		void deserialize(desc::desc_system&, const json::object&) override {}
		void serialize(json::object&) const override {}
	};
	static_assert(!desc::is_described<plain>);
	static_assert(desc::is_described<schema_test::probe>);

	ds.register_desc<plain>("test.plain");
	BOOST_CHECK(ds.is_registered("test.plain"));
	BOOST_CHECK(ds.find_schema("test.plain") == nullptr);
	BOOST_TEST(ds.schemas().size() == 0u);
}

BOOST_AUTO_TEST_SUITE_END()
