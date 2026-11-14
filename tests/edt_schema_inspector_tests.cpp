#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "edt_inspector_panel.h"
#include "edt_schema_inspector.h"
#include <snk/desc/desc_describe.h>
#include <snk/desc/desc_json_ops.h>
#include <snk/desc/desc_schema_ops.h>
#include <snk/editor/edt_inspector_registry.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <snk/scene/scn_camera_desc.h>
#include <snk/scene/scn_directional_light_desc.h>
#include <glm/glm.hpp>
#include <imgui.h>
#include <algorithm>
#include <boost/describe.hpp>
#include <boost/json.hpp>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The inspector generated from desc schemas (#130): which widget a field gets, what the fallback order is, that an edit
// writes only the pointers it changed (the presence leak of the first edit), and that the camera defaults are the desc's.

namespace json = boost::json;
namespace ui = edt::schema_ui;

namespace
{
	using comp_node = scn::prefab_desc::prefab_comp_node;

	constexpr const char* k_mode_enum = R"({ "name": "edt_mode", "items": { "slow": 0, "fast": 1 }, "fallback": 0 })";

	constexpr const char* k_limits_schema = R"({
		"name": "edt.limits", "kind": "struct",
		"fields": [ { "name": "min", "kind": "f32", "default": 0.0 }, { "name": "max", "kind": "f32", "default": 1.0 } ]
	})";

	constexpr const char* k_widget_schema = R"({
		"name": "edt.widget", "kind": "component",
		"fields": [
			{ "name": "enabled", "kind": "bool", "default": true },
			{ "name": "count", "kind": "i32", "default": 3, "range": [0, 10, 1] },
			{ "name": "gain", "kind": "f32", "default": 0.5, "range": [0, 1, 0.01], "tooltip": "Output gain" },
			{ "name": "tint", "kind": "vec4", "default": [1, 0.5, 0.25, 1], "color": true },
			{ "name": "offset", "kind": "vec3", "default": [0, 0, 0] },
			{ "name": "heading", "kind": "f32", "default": 90, "degrees": true },
			{ "name": "mode", "kind": "enumeration", "ref_type": "edt_mode", "default": "fast" },
			{ "name": "label", "kind": "string", "default": "w" },
			{ "name": "title", "key": "/info/title", "kind": "string", "default": "", "read_aliases": ["/name"] },
			{ "name": "source", "kind": "res_tag", "default": "", "filter": "png" },
			{ "name": "limits", "kind": "structure", "ref_type": "edt.limits" },
			{ "name": "ids", "kind": "array", "element": "i32", "default": [1, 2] },
			{ "name": "serial", "kind": "u32", "default": 7, "read_only": true },
			{ "name": "secret", "kind": "u32", "default": 1, "editor_hidden": true },
			{ "name": "extra", "kind": "f32", "optional": true },
			{ "name": "loose", "kind": "f32", "default": null }
		]
	})";

	// A map has no blob layout, so it comes from a C++ type: the schema is generated, not parsed.
	enum class speed_value { slow, fast };
	BOOST_DESCRIBE_ENUM(speed_value, slow, fast)

	struct limits_value
	{
		float min = 0.0f;
		float max = 1.0f;
	};
	BOOST_DESCRIBE_STRUCT(limits_value, (), (min, max))

	struct weights_component
	{
		std::map<std::string, float> weights;
		std::map<std::string, std::string> labels;
		std::map<std::string, limits_value> zones;
		std::map<std::string, speed_value> speeds;
		std::optional<float> extra;
		std::optional<float> bonus = 2.0f;
	};
	BOOST_DESCRIBE_STRUCT(weights_component, (), (weights, labels, zones, speeds, extra, bonus))

	// #861: a std::optional member reads a null as nullopt; a plain member marked optional reads it as its default.
	struct nullable_inner
	{
		std::optional<float> x = 1.0f;
	};
	BOOST_DESCRIBE_STRUCT(nullable_inner, (), (x))

	struct unset_component
	{
		std::optional<float> bonus = 2.0f;
		float level = 5.0f;
		nullable_inner inner;
	};
	BOOST_DESCRIBE_STRUCT(unset_component, (), (bonus, level, inner))
}

namespace desc
{
	template<>
	inline constexpr auto field_attrs<weights_component> = attrs(f("zones").ref("edt.limits"), f("speeds").ref("edt_mode"));
	template<>
	inline constexpr auto field_attrs<unset_component> = attrs(f("level").optional(), f("inner").ref("edt.nullable_inner"));
}

namespace
{
	desc::type_schema weights_schema()
	{
		return desc::schema_generator<weights_component>::make("edt.weights", 1, desc::schema_kind::component);
	}

	desc::type_schema unset_schema(desc::schema_registry& registry)
	{
		if (!registry.find("edt.nullable_inner")) {
			registry.add(desc::schema_generator<nullable_inner>::make("edt.nullable_inner", 1, desc::schema_kind::structure));
		}
		return desc::schema_generator<unset_component>::make("edt.unset", 1, desc::schema_kind::component);
	}

	json::object parse(const char* text)
	{
		return json::parse(text).as_object();
	}

	// Headless ImGui context: enough to run the widgets without a GPU.
	struct imgui_ctx
	{
		imgui_ctx()
		{
			ctx = ImGui::CreateContext();
			ImGuiIO& io = ImGui::GetIO();
			io.IniFilename = nullptr;
			io.DisplaySize = ImVec2(800.f, 600.f);
			unsigned char* px = nullptr;
			int w = 0, h = 0;
			io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
		}
		~imgui_ctx() { ImGui::DestroyContext(ctx); }

		template<typename F>
		void frame(F&& body)
		{
			ImGui::NewFrame();
			body();
			ImGui::EndFrame();
		}

		ImGuiContext* ctx = nullptr;
	};

	struct schema_fixture : test_util::scene_env
	{
		schema_fixture()
		{
			auto& registry = descs->schemas();
			registry.add_enum(desc::parse_enum_schema(parse(k_mode_enum)));
			descs->register_schema(desc::parse_type_schema(parse(k_limits_schema)));
			descs->register_schema(desc::parse_type_schema(parse(k_widget_schema), &registry));
		}

		const desc::type_schema& widget_schema() const
		{
			const desc::type_schema* schema = descs->find_schema("edt.widget");
			if (!schema) throw std::runtime_error("edt.widget is not registered");
			return *schema;
		}
	};

	const desc::field_schema& field_named(const desc::type_schema& schema, std::string_view name)
	{
		for (const auto& field : schema.fields) {
			if (field.name == name) return field;
		}
		throw std::runtime_error("no such field");
	}

	ui::widget_spec spec_of(const desc::type_schema& schema, std::string_view name, bool enum_known = true, bool nested_known = true)
	{
		return ui::describe_widget(field_named(schema, name), enum_known, nested_known);
	}

	double number_at(const json::value& root, std::string_view pointer)
	{
		const json::value* found = desc::json_pointer_get(root, pointer);
		double out = -1e30;
		if (found) desc::json_to_double(*found, out);
		return out;
	}

	using drawn_list = std::vector<std::pair<std::string, ui::widget>>;

	// Draws the schema for `value` in two frames (a tree node opens on the second at the latest) and returns the second's widgets.
	drawn_list draw_twice(imgui_ctx& imgui, const desc::type_schema& schema, const desc::schema_registry& registry, json::value& value, bool* changed = nullptr)
	{
		drawn_list drawn;
		const ui::context ctx{ registry, {}, [&](std::string_view pointer, ui::widget kind) { drawn.emplace_back(std::string(pointer), kind); } };
		for (int frame = 0; frame < 2; ++frame) {
			drawn.clear();
			imgui.frame([&] {
				ImGui::Begin("schema_test");
				const bool edited = ui::draw_fields(schema, value, ctx);
				if (changed) *changed = edited;
				ImGui::End();
			});
		}
		return drawn;
	}

	ui::widget widget_at(const drawn_list& drawn, std::string_view pointer)
	{
		for (const auto& [name, kind] : drawn) {
			if (name == pointer) return kind;
		}
		return ui::widget::hidden;
	}

	bool was_drawn(const drawn_list& drawn, std::string_view pointer)
	{
		return std::ranges::any_of(drawn, [&](const auto& entry) { return entry.first == pointer; });
	}
}

BOOST_FIXTURE_TEST_SUITE(EdtSchemaInspectorTests, schema_fixture)

// ---------------------------------------------------------------- the widget follows the kind and the hints

BOOST_AUTO_TEST_CASE(DescribeWidget_PicksTheWidgetFromTheKind)
{
	const auto& schema = widget_schema();
	BOOST_CHECK(spec_of(schema, "enabled").kind == ui::widget::checkbox);
	BOOST_CHECK(spec_of(schema, "count").kind == ui::widget::integer);
	BOOST_CHECK(spec_of(schema, "gain").kind == ui::widget::real);
	BOOST_CHECK(spec_of(schema, "offset").kind == ui::widget::vector);
	BOOST_CHECK(spec_of(schema, "mode").kind == ui::widget::combo);
	BOOST_CHECK(spec_of(schema, "label").kind == ui::widget::text);
	BOOST_CHECK(spec_of(schema, "source").kind == ui::widget::asset);
	BOOST_CHECK(spec_of(schema, "limits").kind == ui::widget::group);
	BOOST_CHECK(spec_of(schema, "ids").kind == ui::widget::list);
}

BOOST_AUTO_TEST_CASE(DescribeWidget_HintsChangeTheWidget)
{
	const auto& schema = widget_schema();

	// color: a colour picker instead of four drags
	BOOST_CHECK(spec_of(schema, "tint").kind == ui::widget::color);
	desc::field_schema plain = field_named(schema, "tint");
	plain.color = false;
	BOOST_CHECK(ui::describe_widget(plain, true, true).kind == ui::widget::vector);

	// editor_hidden removes the widget, read_only disables it
	BOOST_CHECK(spec_of(schema, "secret").kind == ui::widget::hidden);
	BOOST_CHECK(spec_of(schema, "serial").kind == ui::widget::integer);
	BOOST_CHECK(spec_of(schema, "serial").disabled);
	BOOST_CHECK(!spec_of(schema, "count").disabled);

	// range: bounds and step of the drag
	const ui::widget_spec count = spec_of(schema, "count");
	BOOST_CHECK(count.bounded);
	BOOST_TEST(count.min == 0.0);
	BOOST_TEST(count.max == 10.0);
	BOOST_TEST(count.speed == 1.0);
	BOOST_CHECK(!spec_of(schema, "heading").bounded);

	// step decides the decimals, degrees adds the unit
	BOOST_TEST(spec_of(schema, "gain").format == "%.3f");
	BOOST_TEST(spec_of(schema, "heading").format == "%.3f deg");
	desc::field_schema fine = field_named(schema, "gain");
	fine.range = desc::field_range{ 0.0001, 10.0, 0.001 };
	BOOST_TEST(ui::describe_widget(fine, true, true).format == "%.4f");
	fine.range = desc::field_range{ 1.0, 100000.0, 1.0 };
	BOOST_TEST(ui::describe_widget(fine, true, true).format == "%.0f");
}

BOOST_AUTO_TEST_CASE(DescribeWidget_UnknownEnumOrStructureFallsBackToJson)
{
	const auto& schema = widget_schema();
	BOOST_CHECK(spec_of(schema, "mode", false, true).kind == ui::widget::json);
	BOOST_CHECK(spec_of(schema, "limits", true, false).kind == ui::widget::json);
}

BOOST_AUTO_TEST_CASE(DisplayLabel_IsTheMemberNameWithoutPrefix)
{
	desc::field_schema field;
	field.name = "m_near_distance";
	BOOST_TEST(ui::display_label(field) == "Near distance");
	field.name = "direction";
	BOOST_TEST(ui::display_label(field) == "Direction");
}

// #631: a tag field's text without a protocol names nothing; the field says so and how to write it.
BOOST_AUTO_TEST_CASE(TagTextProblem_OnlyForATextWithoutAProtocol)
{
	BOOST_TEST(ui::tag_text_problem("shaders/a.frag") == "'shaders/a.frag' has no protocol: write res://shaders/a.frag");
	BOOST_TEST(!ui::tag_text_problem("user/xa.png").empty());
	BOOST_TEST(!ui::tag_text_problem("user:/xa.png").empty());
	BOOST_TEST(ui::tag_text_problem("").empty());
	BOOST_TEST(ui::tag_text_problem("res://shaders/a.frag").empty());
	BOOST_TEST(ui::tag_text_problem("memory://A").empty());
}

// ---------------------------------------------------------------- the schema UI draws the fields

BOOST_AUTO_TEST_CASE(Draw_ShowsTheFieldsOfTheSchema)
{
	imgui_ctx imgui;
	const auto& schema = widget_schema();
	json::value value = ui::compose(schema, descs->schemas(), {});

	bool changed = true;
	const drawn_list drawn = draw_twice(imgui, schema, descs->schemas(), value, &changed);

	BOOST_CHECK(!changed); // looking is not editing
	BOOST_CHECK(widget_at(drawn, "/enabled") == ui::widget::checkbox);
	BOOST_CHECK(widget_at(drawn, "/count") == ui::widget::integer);
	BOOST_CHECK(widget_at(drawn, "/gain") == ui::widget::real);
	BOOST_CHECK(widget_at(drawn, "/tint") == ui::widget::color);
	BOOST_CHECK(widget_at(drawn, "/offset") == ui::widget::vector);
	BOOST_CHECK(widget_at(drawn, "/mode") == ui::widget::combo);
	BOOST_CHECK(widget_at(drawn, "/label") == ui::widget::text);
	BOOST_CHECK(widget_at(drawn, "/info/title") == ui::widget::text);
	BOOST_CHECK(widget_at(drawn, "/source") == ui::widget::asset);
	BOOST_CHECK(widget_at(drawn, "/serial") == ui::widget::integer);

	// structures and arrays draw their members
	BOOST_CHECK(widget_at(drawn, "/limits") == ui::widget::group);
	BOOST_CHECK(widget_at(drawn, "/limits/min") == ui::widget::real);
	BOOST_CHECK(widget_at(drawn, "/limits/max") == ui::widget::real);
	BOOST_CHECK(widget_at(drawn, "/ids") == ui::widget::list);
	BOOST_CHECK(widget_at(drawn, "/ids/0") == ui::widget::integer);
	BOOST_CHECK(widget_at(drawn, "/ids/1") == ui::widget::integer);

	// editor_hidden is not drawn; a data-defined optional without a value shows the zero the read gives it (#861)
	BOOST_CHECK(!was_drawn(drawn, "/secret"));
	BOOST_CHECK(widget_at(drawn, "/extra") == ui::widget::real);
	BOOST_TEST(number_at(value, "/extra") == 0.0);
}

BOOST_AUTO_TEST_CASE(Draw_FollowsTheHintsOfTheSchema)
{
	imgui_ctx imgui;
	desc::type_schema schema = widget_schema();
	json::value value = ui::compose(schema, descs->schemas(), {});

	BOOST_CHECK(widget_at(draw_twice(imgui, schema, descs->schemas(), value), "/tint") == ui::widget::color);
	for (auto& field : schema.fields) {
		if (field.name == "tint") field.color = false;
		if (field.name == "count") field.editor_hidden = true;
	}
	const drawn_list drawn = draw_twice(imgui, schema, descs->schemas(), value);
	BOOST_CHECK(widget_at(drawn, "/tint") == ui::widget::vector);
	BOOST_CHECK(!was_drawn(drawn, "/count"));
}

BOOST_AUTO_TEST_CASE(Draw_AValueOfTheWrongShapeIsLeftToTheJsonEditor)
{
	imgui_ctx imgui;
	const auto& schema = widget_schema();
	json::value value = ui::compose(schema, descs->schemas(), {});
	desc::json_pointer_set(value, "/offset", json::value(json::string("not a vector")));

	bool changed = true;
	const drawn_list drawn = draw_twice(imgui, schema, descs->schemas(), value, &changed);
	BOOST_CHECK(widget_at(drawn, "/offset") == ui::widget::json);
	BOOST_CHECK(!changed);
	BOOST_CHECK(desc::json_pointer_get(value, "/offset")->is_string()); // not overwritten
}

// #773: a tag field holding a {guid, path} reference keeps the tag widget (it shows the path); an object that is not a
// good reference is left to the JSON editor.
BOOST_AUTO_TEST_CASE(Draw_AReferenceObjectInATagFieldKeepsTheTagWidget)
{
	imgui_ctx imgui;
	const auto& schema = widget_schema();
	json::value value = ui::compose(schema, descs->schemas(), {});
	const json::value reference = json::parse(R"({"guid":"3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f","path":"res://textures/a.png"})");
	desc::json_pointer_set(value, "/source", reference);

	bool changed = true;
	const drawn_list drawn = draw_twice(imgui, schema, descs->schemas(), value, &changed);
	BOOST_CHECK(widget_at(drawn, "/source") == ui::widget::asset);
	BOOST_CHECK(!changed);
	BOOST_CHECK(*desc::json_pointer_get(value, "/source") == reference);

	desc::json_pointer_set(value, "/source", json::parse(R"({"guid":"3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f","file":"a.png"})"));
	const drawn_list bad = draw_twice(imgui, schema, descs->schemas(), value, &changed);
	BOOST_CHECK(widget_at(bad, "/source") == ui::widget::json);
}

// ---------------------------------------------------------------- what the component has

BOOST_AUTO_TEST_CASE(Compose_LayersTheDefaultsParentAndOverrides)
{
	const auto& schema = widget_schema();
	const json::object parent = parse(R"({ "count": 4, "limits": { "max": 9.0 } })");
	const json::object overrides = parse(R"({ "count": 8, "label": "own" })");
	const json::value value = ui::compose(schema, descs->schemas(), { &parent, &overrides });

	BOOST_TEST(number_at(value, "/count") == 8.0);
	BOOST_TEST(number_at(value, "/limits/max") == 9.0);
	BOOST_TEST(number_at(value, "/limits/min") == 0.0); // a default under a partly set structure
	BOOST_TEST(number_at(value, "/gain") == 0.5);       // a default nobody set
	BOOST_TEST(std::string(desc::json_pointer_get(value, "/label")->as_string()) == "own");
	BOOST_TEST(number_at(value, "/extra") == 0.0); // a data-defined optional nobody set: what the read gives (#861)
}

BOOST_AUTO_TEST_CASE(Compose_ReadsALegacyKeyThroughTheAlias)
{
	const auto& schema = widget_schema();
	const json::object old = parse(R"({ "name": "legacy" })");
	const json::value value = ui::compose(schema, descs->schemas(), { &old });
	BOOST_TEST(std::string(desc::json_pointer_get(value, "/info/title")->as_string()) == "legacy");
}

// ---------------------------------------------------------------- an edit writes the pointers it changed

BOOST_AUTO_TEST_CASE(ApplyEdit_WritesOnlyTheChangedPointers)
{
	const auto& schema = widget_schema();
	const auto& registry = descs->schemas();
	const json::object parent = parse(R"({
		"enabled": false, "count": 4, "gain": 0.25, "label": "parent", "offset": [1, 2, 3], "limits": { "min": 1.0, "max": 9.0 }
	})");
	json::object overrides;
	const json::value base = ui::compose(schema, registry, { &parent }, false);

	// first edit of a component that only inherits: one pointer, not a copy of the parent
	json::value before = ui::compose(schema, registry, { &parent, &overrides });
	json::value after = before;
	desc::json_pointer_set(after, "/count", json::value(7));
	BOOST_CHECK_EQUAL(ui::apply_edit(overrides, schema, registry, base, before, after), 1u);
	BOOST_CHECK(overrides == parse(R"({ "count": 7 })"));

	// a field inside a structure is a pointer of its own
	before = ui::compose(schema, registry, { &parent, &overrides });
	after = before;
	desc::json_pointer_set(after, "/limits/max", json::value(12.5));
	BOOST_CHECK_EQUAL(ui::apply_edit(overrides, schema, registry, base, before, after), 1u);
	BOOST_CHECK(overrides == parse(R"({ "count": 7, "limits": { "max": 12.5 } })"));

	// setting a field back to what it inherits drops its pointer again
	before = ui::compose(schema, registry, { &parent, &overrides });
	after = before;
	desc::json_pointer_set(after, "/count", json::value(4));
	BOOST_CHECK_EQUAL(ui::apply_edit(overrides, schema, registry, base, before, after), 1u);
	BOOST_CHECK(overrides == parse(R"({ "limits": { "max": 12.5 } })"));

	before = ui::compose(schema, registry, { &parent, &overrides });
	after = before;
	desc::json_pointer_set(after, "/limits/max", json::value(9.0));
	ui::apply_edit(overrides, schema, registry, base, before, after);
	BOOST_CHECK(overrides.empty()); // no empty "limits" left behind
}

BOOST_AUTO_TEST_CASE(ApplyEdit_KeepsThePointersTheEditDidNotTouch)
{
	const auto& schema = widget_schema();
	const auto& registry = descs->schemas();
	const json::object parent = parse(R"({ "enabled": false, "gain": 0.25 })");
	// the node spells out a value it also inherits: that is presence the editor must not throw away
	json::object overrides = parse(R"({ "enabled": false, "mystery": 1 })");
	const json::value base = ui::compose(schema, registry, { &parent }, false);
	const json::value before = ui::compose(schema, registry, { &parent, &overrides });
	json::value after = before;
	desc::json_pointer_set(after, "/gain", json::value(0.75));
	BOOST_CHECK_EQUAL(ui::apply_edit(overrides, schema, registry, base, before, after), 1u);
	BOOST_CHECK(overrides == parse(R"({ "enabled": false, "mystery": 1, "gain": 0.75 })"));
}

BOOST_AUTO_TEST_CASE(ApplyEdit_SmallValuesAreComparedRelatively)
{
	const desc::type_schema* schema = descs->find_schema(scn::camera_desc::__type);
	BOOST_REQUIRE(schema);
	const auto& registry = descs->schemas();
	const json::object parent = parse(R"({ "near_distance": 0.0001 })");
	json::object overrides;
	const json::value base = ui::compose(*schema, registry, { &parent }, false);

	// 0.00005 is half of what the parent has, not "about equal" to it
	json::value before = ui::compose(*schema, registry, { &parent, &overrides });
	json::value after = before;
	desc::json_pointer_set(after, "/near_distance", json::value(0.00005));
	BOOST_CHECK_EQUAL(ui::apply_edit(overrides, *schema, registry, base, before, after), 1u);
	BOOST_REQUIRE(overrides.if_contains("near_distance"));
	BOOST_TEST(overrides.at("near_distance").to_number<double>() == 0.00005);

	// back to the parent's value (a float32 round trip of it) drops the pointer
	before = ui::compose(*schema, registry, { &parent, &overrides });
	after = before;
	desc::json_pointer_set(after, "/near_distance", json::value(static_cast<double>(0.0001f)));
	ui::apply_edit(overrides, *schema, registry, base, before, after);
	BOOST_CHECK(overrides.empty());
}

BOOST_AUTO_TEST_CASE(ApplyEdit_ARevertAlsoDropsTheLegacyAliasKey)
{
	const desc::type_schema* schema = descs->find_schema(scn::camera_desc::__type);
	BOOST_REQUIRE(schema);
	const auto& registry = descs->schemas();
	const json::object parent = parse(R"({ "color_targets": ["memory://B"] })");
	json::object overrides = parse(R"({ "texture": "memory://A" })"); // the old single-target key
	const json::value base = ui::compose(*schema, registry, { &parent }, false);

	const json::value before = ui::compose(*schema, registry, { &parent, &overrides });
	BOOST_CHECK(std::string(desc::json_pointer_get(before, "/color_targets")->as_string()) == "memory://A");
	json::value after = before;
	desc::json_pointer_set(after, "/color_targets", parent.at("color_targets"));
	ui::apply_edit(overrides, *schema, registry, base, before, after);
	BOOST_CHECK(overrides.empty()); // not { "texture": "A" }, which would still win on load
}

BOOST_AUTO_TEST_CASE(ApplyEdit_AFieldWithoutADefaultIsAlwaysWritten)
{
	const auto& schema = widget_schema();
	const auto& registry = descs->schemas();
	const json::object parent;
	json::object overrides = parse(R"({ "loose": 3 })");
	// the zero shown for "loose" is not something it inherits, so typing 0 must not look like a revert
	const json::value base = ui::compose(schema, registry, { &parent }, false);
	BOOST_CHECK(desc::json_pointer_get(base, "/loose") == nullptr);
	const json::value before = ui::compose(schema, registry, { &parent, &overrides });
	json::value after = before;
	desc::json_pointer_set(after, "/loose", json::value(0.0));
	BOOST_CHECK_EQUAL(ui::apply_edit(overrides, schema, registry, base, before, after), 1u);
	BOOST_REQUIRE(overrides.if_contains("loose"));
	BOOST_TEST(overrides.at("loose").to_number<double>() == 0.0);
}

BOOST_AUTO_TEST_CASE(ApplyEdit_ALightEditLoadsThroughTheDescAndKeepsW)
{
	const desc::type_schema* schema = descs->find_schema(scn::directional_light_desc::__type);
	BOOST_REQUIRE(schema);
	const auto& registry = descs->schemas();
	const scn::directional_light_desc defaults;
	json::object parent;
	defaults.serialize(parent);
	json::object overrides;
	const json::value base = ui::compose(*schema, registry, { &parent }, false);

	// what dragging the x of the direction does: one vec4, w as it was
	const json::value before = ui::compose(*schema, registry, { &parent, &overrides });
	json::value after = before;
	json::array direction = desc::json_pointer_get(after, "/direction")->as_array();
	direction[0] = 0.5;
	desc::json_pointer_set(after, "/direction", json::value(direction));
	BOOST_CHECK_EQUAL(ui::apply_edit(overrides, *schema, registry, base, before, after), 1u);
	BOOST_REQUIRE(overrides.if_contains("direction"));
	BOOST_CHECK_EQUAL(overrides.size(), 1u);
	BOOST_CHECK_EQUAL(overrides.at("direction").as_array().size(), 4u);

	scn::directional_light_desc loaded;
	loaded.deserialize(*descs, overrides);
	BOOST_CHECK((loaded.direction == glm::vec4{ 0.5f, defaults.direction.y, defaults.direction.z, defaults.direction.w }));
	BOOST_CHECK((loaded.diffuse == defaults.diffuse));
}

BOOST_AUTO_TEST_CASE(ApplyEdit_NoDifferenceWritesNothing)
{
	const auto& schema = widget_schema();
	const auto& registry = descs->schemas();
	json::object overrides;
	const json::value base = ui::compose(schema, registry, {});
	const json::value before = base;
	BOOST_CHECK_EQUAL(ui::apply_edit(overrides, schema, registry, base, before, before), 0u);
	BOOST_CHECK(overrides.empty());
}

// ---------------------------------------------------------------- the panel: fallback order and the presence leak

BOOST_AUTO_TEST_CASE(Panel_FallbackOrderIsCustomThenSchemaThenGeneric)
{
	imgui_ctx imgui;
	scn::prefab_desc::prefab_node node;
	node.name = "n";
	for (const char* type : { "edt.widget", "plain_desc" }) {
		comp_node comp;
		comp.type_name = type;
		node.components[type] = comp;
	}

	edt::inspector_registry registry;
	edt::inspector_panel panel;
	int commits = 0;
	panel.set_selected_node(&node);
	panel.set_desc_system(descs.get());
	panel.set_inspector_registry(&registry);
	panel.set_on_node_changed([&] { ++commits; });

	imgui.frame([&] { panel.render(); });
	BOOST_CHECK(panel.ui_of("edt.widget") == edt::inspector_panel::component_ui::schema);
	BOOST_CHECK(panel.ui_of("plain_desc") == edt::inspector_panel::component_ui::generic);
	BOOST_CHECK_EQUAL(commits, 0);
	BOOST_CHECK(node.components.at("edt.widget").overrides.empty()); // drawing the defaults writes nothing

	auto reg = registry.add(std::string("edt.widget"), [](comp_node&) { return false; });
	imgui.frame([&] { panel.render(); });
	BOOST_CHECK(panel.ui_of("edt.widget") == edt::inspector_panel::component_ui::custom);
	BOOST_CHECK(panel.ui_of("plain_desc") == edt::inspector_panel::component_ui::generic);
}

BOOST_AUTO_TEST_CASE(Panel_TheFirstEditOfACustomRendererDoesNotCopyTheParent)
{
	imgui_ctx imgui;
	const res::tag parent_tag = put("p.desc", R"({"__type":"stat_desc","hp":5,"label":"parent"})");

	comp_node comp;
	comp.type_name = "stat_desc";
	comp.parent_desc = require_loaded<desc::desc_base>(parent_tag);
	BOOST_REQUIRE(comp.parent_desc.is_valid());
	BOOST_REQUIRE(comp.parent_desc.is_ready());
	scn::prefab_desc::prefab_node node;
	node.name = "n";
	node.components["stat"] = comp;

	edt::inspector_registry registry;
	auto reg = registry.add(std::string("stat_desc"), [](comp_node& c) {
		c.overrides["hp"] = 9; // the renderer edits one field of the effective data it was given
		return true;
	});
	edt::inspector_panel panel;
	int commits = 0;
	panel.set_selected_node(&node);
	panel.set_inspector_registry(&registry);
	panel.set_on_node_changed([&] { ++commits; });

	imgui.frame([&] { panel.render(); });
	BOOST_CHECK_EQUAL(commits, 1);
	const json::object& written = node.components.at("stat").overrides;
	BOOST_CHECK_EQUAL(written.size(), 1u); // hp only; label and limits stay inherited
	BOOST_REQUIRE(written.if_contains("hp"));
	BOOST_TEST(written.at("hp").to_number<int>() == 9);
}

// ---------------------------------------------------------------- the defaults are the desc's

BOOST_AUTO_TEST_CASE(CameraDefaultsComeFromTheSchema)
{
	const desc::type_schema* schema = descs->find_schema(scn::camera_desc::__type);
	BOOST_REQUIRE(schema);
	const json::value defaults = ui::defaults_json(*schema, descs->schemas());

	const scn::camera_desc camera;
	BOOST_TEST(number_at(defaults, "/fov") == static_cast<double>(camera.fov()));
	BOOST_TEST(number_at(defaults, "/near_distance") == static_cast<double>(camera.near_distance()));
	BOOST_TEST(number_at(defaults, "/far_distance") == static_cast<double>(camera.far_distance()));
	// not the 60 / 0.01 the old camera renderer showed
	BOOST_TEST(number_at(defaults, "/fov") == 90.0);
	BOOST_TEST(number_at(defaults, "/near_distance") < 0.001);
	BOOST_TEST(number_at(defaults, "/far_distance") == 1000.0);

	// what the inspector shows for a camera without data is what the desc serializes
	const json::value shown = ui::compose(*schema, descs->schemas(), {});
	json::object serialized;
	camera.serialize(serialized);
	const json::value written_json(serialized);
	for (const char* pointer : { "/fov", "/near_distance", "/far_distance" }) {
		const json::value* written = desc::json_pointer_get(written_json, pointer);
		const json::value* seen = desc::json_pointer_get(shown, pointer);
		BOOST_REQUIRE_MESSAGE(written && seen, pointer);
		BOOST_CHECK_MESSAGE(desc::json_equal(*written, *seen, 1e-6), pointer);
	}
}

BOOST_AUTO_TEST_CASE(LightDefaultsComeFromTheSchema)
{
	const desc::type_schema* schema = descs->find_schema(scn::directional_light_desc::__type);
	BOOST_REQUIRE(schema);
	const json::value shown = ui::compose(*schema, descs->schemas(), {});

	const scn::directional_light_desc light;
	json::object serialized;
	light.serialize(serialized);
	const json::value written(serialized);
	for (const char* pointer : { "/direction", "/diffuse", "/ambient", "/specular" }) {
		const json::value* a = desc::json_pointer_get(written, pointer);
		const json::value* b = desc::json_pointer_get(shown, pointer);
		BOOST_REQUIRE_MESSAGE(a && b, pointer);
		BOOST_CHECK_MESSAGE(desc::json_equal(*a, *b, 1e-6), pointer);
	}
}

// ---------------------------------------------------------------- the material (#131-c)

BOOST_AUTO_TEST_CASE(MaterialInspectorFieldsComeFromTheSchema)
{
	const desc::type_schema* schema = descs->find_schema(scn::material_desc::__type);
	BOOST_REQUIRE(schema);

	BOOST_CHECK(spec_of(*schema, "albedo").kind == ui::widget::color);
	BOOST_CHECK(spec_of(*schema, "emissive").kind == ui::widget::color);
	BOOST_CHECK(spec_of(*schema, "queue").kind == ui::widget::combo);
	BOOST_CHECK(spec_of(*schema, "render_mode").kind == ui::widget::combo);
	BOOST_CHECK(spec_of(*schema, "vertex_shader").kind == ui::widget::asset);
	BOOST_CHECK(spec_of(*schema, "geometry_shader").kind == ui::widget::asset);
	BOOST_CHECK(spec_of(*schema, "defines").kind == ui::widget::list);
	BOOST_CHECK(spec_of(*schema, "samplers_textures").kind == ui::widget::json);

	// shininess is an ordinary number field now that it is saved: bounded by the range of the header
	const ui::widget_spec shininess = spec_of(*schema, "shininess");
	BOOST_CHECK(shininess.kind == ui::widget::real);
	BOOST_CHECK(shininess.bounded);
	BOOST_TEST(shininess.min == 0.0);
	BOOST_TEST(shininess.max == 1000.0);

	// what the inspector shows for a material without data is what the desc holds
	const json::value shown = ui::compose(*schema, descs->schemas(), {});
	BOOST_TEST(number_at(shown, "/uniforms/shininess") == 32.0);
	BOOST_TEST(number_at(shown, "/uniforms/albedo/0") == 1.0);
	BOOST_TEST(number_at(shown, "/uniforms/emissive/3") == 0.0);
	BOOST_REQUIRE(desc::json_pointer_get(shown, "/queue"));
	BOOST_TEST(desc::json_pointer_get(shown, "/queue")->as_string() == "opaque");
	BOOST_REQUIRE(desc::json_pointer_get(shown, "/render_mode"));
	BOOST_TEST(desc::json_pointer_get(shown, "/render_mode")->as_string() == "TRIANGLE");

	imgui_ctx imgui;
	json::value value = shown;
	const drawn_list drawn = draw_twice(imgui, *schema, descs->schemas(), value);
	BOOST_CHECK(widget_at(drawn, "/uniforms/shininess") == ui::widget::real);
	BOOST_CHECK(widget_at(drawn, "/uniforms/albedo") == ui::widget::color);
	BOOST_CHECK(widget_at(drawn, "/queue") == ui::widget::combo);
	BOOST_CHECK(widget_at(drawn, "/shader_fragment") == ui::widget::asset);
	BOOST_CHECK(!was_drawn(drawn, "/uniforms/ambient")); // not a field of the file, so not of the inspector
}

// ---------------------------------------------------------------- the skybox (#131-d)

BOOST_AUTO_TEST_CASE(SkyboxInspectorFieldComesFromTheSchema)
{
	const desc::type_schema* schema = descs->find_schema(scn::skybox_desc::__type);
	BOOST_REQUIRE(schema);

	// the material is a desc reference: an asset field, not the text box the hand-written renderer drew
	const ui::widget_spec material = spec_of(*schema, "material");
	BOOST_CHECK(material.kind == ui::widget::asset);

	// without data the inspector shows no material, as the desc writes none
	const json::value shown = ui::compose(*schema, descs->schemas(), {});
	const json::value* shown_material = desc::json_pointer_get(shown, "/material");
	BOOST_REQUIRE(shown_material);
	BOOST_REQUIRE(shown_material->is_string());
	BOOST_TEST(shown_material->as_string().empty());

	imgui_ctx imgui;
	json::value value = shown;
	bool changed = false;
	const drawn_list drawn = draw_twice(imgui, *schema, descs->schemas(), value, &changed);
	BOOST_CHECK(widget_at(drawn, "/material") == ui::widget::asset);
	BOOST_CHECK(!changed);
}

BOOST_AUTO_TEST_CASE(Map_GetsTheMapWidgetAndAListedRowPerKey)
{
	const desc::type_schema schema = weights_schema();
	BOOST_CHECK(ui::describe_widget(field_named(schema, "weights"), true, true).kind == ui::widget::map);

	imgui_ctx imgui;
	json::value value = ui::compose(schema, descs->schemas(), {});
	value.as_object()["weights"] = parse(R"({ "a": 1.5, "b": 2.0 })");
	const drawn_list drawn = draw_twice(imgui, schema, descs->schemas(), value);
	BOOST_CHECK(widget_at(drawn, "/weights") == ui::widget::map);
	BOOST_CHECK(widget_at(drawn, "/weights/a") == ui::widget::real);
	BOOST_CHECK(widget_at(drawn, "/weights/b") == ui::widget::real);
}

BOOST_AUTO_TEST_CASE(MapKeys_AreAddedAndRenamedInPlaceAndRefusedWhenEmptyOrTaken)
{
	json::object entries = parse(R"({ "a": 1, "b": 2, "c": 3 })");
	BOOST_TEST(ui::map_key_problem(entries, "").size() > 0u);
	BOOST_TEST(ui::map_key_problem(entries, "a").size() > 0u);
	BOOST_TEST(ui::map_key_problem(entries, std::string(ui::k_max_map_key + 1, 'k')).size() > 0u);
	BOOST_TEST(ui::map_key_problem(entries, "d").empty());

	BOOST_TEST(!ui::add_map_key(entries, "a", json::value(9)));
	BOOST_TEST(ui::add_map_key(entries, "d", json::value(4)));
	BOOST_CHECK(entries.size() == 4u);

	BOOST_TEST(!ui::rename_map_key(entries, "b", "c"));
	BOOST_TEST(!ui::rename_map_key(entries, "missing", "z"));
	BOOST_TEST(ui::rename_map_key(entries, "b", "x"));
	std::vector<std::string> order;
	for (const auto& entry : entries) order.emplace_back(entry.key());
	BOOST_CHECK((order == std::vector<std::string>{ "a", "x", "c", "d" }));
	BOOST_TEST(entries.at("x").as_int64() == 2);
}

BOOST_AUTO_TEST_CASE(Compose_DropsTheKeysAnOverrideNulled)
{
	const desc::type_schema schema = weights_schema();
	const json::object parent = parse(R"({ "weights": { "a": 1.0, "b": 2.0 } })");
	const json::object overrides = parse(R"({ "weights": { "a": null, "c": 3.0 } })");
	const json::value shown = ui::compose(schema, descs->schemas(), { &parent, &overrides });
	BOOST_CHECK(shown.at("weights").as_object() == parse(R"({ "b": 2.0, "c": 3.0 })"));
}

BOOST_AUTO_TEST_CASE(ApplyEdit_OfAMap_RemovesAnInheritedKeyWithANullAndAnOwnKeyWithNothing)
{
	const desc::type_schema schema = weights_schema();
	const auto& registry = descs->schemas();
	const json::object parent = parse(R"({ "weights": { "a": 1.0, "b": 2.0 } })");
	json::object overrides;
	const json::value base = ui::compose(schema, registry, { &parent }, false);

	json::value before = ui::compose(schema, registry, { &parent, &overrides });
	json::value after = before;
	after.at_pointer("/weights").as_object().erase("a");
	after.at_pointer("/weights").as_object().emplace("c", 3.0);
	BOOST_CHECK_EQUAL(ui::apply_edit(overrides, schema, registry, base, before, after), 2u);
	BOOST_CHECK(overrides == parse(R"({ "weights": { "a": null, "c": 3.0 } })"));

	// the own key goes again: no record, no empty object left behind; the inherited key stays removed
	before = ui::compose(schema, registry, { &parent, &overrides });
	after = before;
	after.at_pointer("/weights").as_object().erase("c");
	ui::apply_edit(overrides, schema, registry, base, before, after);
	BOOST_CHECK(overrides == parse(R"({ "weights": { "a": null } })"));

	// the inherited key is back: the record goes
	before = ui::compose(schema, registry, { &parent, &overrides });
	after = before;
	after.at_pointer("/weights").as_object().emplace("a", 1.0);
	ui::apply_edit(overrides, schema, registry, base, before, after);
	BOOST_CHECK(overrides.empty());
}

BOOST_AUTO_TEST_CASE(ApplyEdit_OfAMap_RenamingAnInheritedKeyRemovesItAndAddsTheNewName)
{
	const desc::type_schema schema = weights_schema();
	const auto& registry = descs->schemas();
	const json::object parent = parse(R"({ "weights": { "a": 1.0, "b": 2.0 } })");
	json::object overrides;
	const json::value base = ui::compose(schema, registry, { &parent }, false);

	const json::value before = ui::compose(schema, registry, { &parent, &overrides });
	json::value after = before;
	BOOST_REQUIRE(ui::rename_map_key(after.at_pointer("/weights").as_object(), "a", "x"));
	ui::apply_edit(overrides, schema, registry, base, before, after);
	BOOST_CHECK(overrides == parse(R"({ "weights": { "a": null, "x": 1.0 } })"));
}

BOOST_AUTO_TEST_CASE(ApplyEdit_OfAMap_ARemovedKeyComesBackWithAnotherValueAsARecord)
{
	const desc::type_schema schema = weights_schema();
	const auto& registry = descs->schemas();
	const json::object parent = parse(R"({ "weights": { "a": 1.0, "b": 2.0 } })");
	json::object overrides = parse(R"({ "weights": { "a": null } })");
	const json::value base = ui::compose(schema, registry, { &parent }, false);

	const json::value before = ui::compose(schema, registry, { &parent, &overrides });
	BOOST_CHECK(!before.at("weights").as_object().contains("a"));
	json::value after = before;
	after.at_pointer("/weights").as_object().emplace("a", 5.0);
	ui::apply_edit(overrides, schema, registry, base, before, after);
	BOOST_CHECK(overrides == parse(R"({ "weights": { "a": 5.0 } })"));
}

BOOST_AUTO_TEST_CASE(ApplyEdit_OfAMap_KeysWithASlashOrATildeStayOneKey)
{
	const desc::type_schema schema = weights_schema();
	const auto& registry = descs->schemas();
	json::object overrides;
	const json::value base = ui::compose(schema, registry, { }, false);
	const json::value before = ui::compose(schema, registry, { &overrides });
	json::value after = before;
	BOOST_REQUIRE(ui::add_map_key(after.at_pointer("/weights").as_object(), "a/b", json::value(1.5)));
	BOOST_REQUIRE(ui::add_map_key(after.at_pointer("/weights").as_object(), "x~y", json::value(2.5)));
	ui::apply_edit(overrides, schema, registry, base, before, after);
	BOOST_CHECK(overrides == parse(R"({ "weights": { "a/b": 1.5, "x~y": 2.5 } })"));
}

BOOST_AUTO_TEST_CASE(ApplyEdit_OfAnOptionalNobodyInherits_RemovesItsRecordWhenCleared)
{
	const desc::type_schema schema = weights_schema();
	const auto& registry = descs->schemas();
	json::object overrides = parse(R"({ "extra": 1.0 })");
	const json::value base = ui::compose(schema, registry, { }, false);
	const json::value before = ui::compose(schema, registry, { &overrides });
	json::value after = before;
	after.as_object().erase("extra");
	ui::apply_edit(overrides, schema, registry, base, before, after);
	BOOST_CHECK(overrides.empty());
}

BOOST_AUTO_TEST_CASE(Map_OfStructuresAndEnums_DrawsAGroupAndAComboPerKey)
{
	const desc::type_schema schema = weights_schema();
	imgui_ctx imgui;
	json::value value = ui::compose(schema, descs->schemas(), {});
	value.as_object()["zones"] = parse(R"({ "z": { "min": 0.0, "max": 2.0 } })");
	value.as_object()["speeds"] = parse(R"({ "m": "fast" })");
	const drawn_list drawn = draw_twice(imgui, schema, descs->schemas(), value);
	BOOST_CHECK(widget_at(drawn, "/zones") == ui::widget::map);
	BOOST_CHECK(widget_at(drawn, "/zones/z") == ui::widget::group);
	BOOST_CHECK(widget_at(drawn, "/zones/z/max") == ui::widget::real);
	BOOST_CHECK(widget_at(drawn, "/speeds/m") == ui::widget::combo);
}

BOOST_AUTO_TEST_CASE(Compose_ShowsAnOptionalFieldAnOverrideNulledAsNotSet)
{
	const desc::type_schema schema = weights_schema();
	const json::object parent = parse(R"({ "extra": 2.0 })");
	const json::object overrides = parse(R"({ "extra": null })");
	const json::value shown = ui::compose(schema, descs->schemas(), { &parent, &overrides });
	BOOST_CHECK(!shown.as_object().contains("extra"));
}

BOOST_AUTO_TEST_CASE(ApplyEdit_ClearingAnOptionalThatHasAnInitializer_KeepsTheNullRecord)
{
	const desc::type_schema schema = weights_schema();
	const auto& registry = descs->schemas();
	json::object overrides = parse(R"({ "bonus": 3.0 })");
	const json::value base = ui::compose(schema, registry, { }, false);
	const json::value before = ui::compose(schema, registry, { &overrides });
	json::value after = before;
	after.as_object().erase("bonus");
	ui::apply_edit(overrides, schema, registry, base, before, after);
	// without the record the read gives the initializer (2.0): the clear must stay written
	BOOST_CHECK(overrides == parse(R"({ "bonus": null })"));
}

// ---------------------------------------------------------------- where a value comes from (S56-7)

namespace
{
	ui::field_origin origin_of(const json::object& parent, const json::object& own, std::string inherited_from = "res://p.desc")
	{
		ui::field_origin origin;
		origin.own = own;
		origin.parent = parent;
		origin.has_parent = true;
		origin.inherited_from = std::move(inherited_from);
		return origin;
	}
}

BOOST_AUTO_TEST_CASE(SetHere_IsThePointerOrAnythingUnderItInTheOwnData)
{
	const ui::field_origin origin = origin_of({}, parse(R"({ "zones": { "z": { "max": 2.0 } }, "extra": null })"));
	BOOST_CHECK(ui::set_here(origin, "/zones"));
	BOOST_CHECK(ui::set_here(origin, "/zones/z"));
	BOOST_CHECK(ui::set_here(origin, "/zones/z/max"));
	BOOST_CHECK(!ui::set_here(origin, "/zones/z/min"));
	BOOST_CHECK(!ui::set_here(origin, "/weights"));
	BOOST_CHECK(ui::set_here(origin, "/extra")); // a null record is a record: "not set here" over an inherited value
}

BOOST_AUTO_TEST_CASE(SourceText_NamesWhereTheValueComesFrom)
{
	const ui::field_origin origin = origin_of(parse(R"({ "weights": { "a": 1.0 } })"), parse(R"({ "extra": 1.0 })"));
	BOOST_CHECK_EQUAL(ui::source_text(origin, "/extra"), "set here");
	BOOST_CHECK_EQUAL(ui::source_text(origin, "/weights/a"), "inherited from res://p.desc");
	BOOST_CHECK_EQUAL(ui::source_text(origin, "/labels"), "schema default");
	BOOST_CHECK(ui::source_of(origin_of({}, {}, ""), "/weights") == ui::value_source::schema_default);
	BOOST_CHECK_EQUAL(ui::source_text(origin_of(parse(R"({ "extra": 1.0 })"), {}, ""), "/extra"), "inherited");
}

BOOST_AUTO_TEST_CASE(Reset_TakesTheRecordOut_WhateverTheFieldsDefaultIs_AndTheEmptyObjectsWithIt)
{
	const desc::type_schema schema = weights_schema();
	const auto& registry = descs->schemas();
	const json::object parent = parse(R"({ "weights": { "a": 1.0 } })");
	json::object overrides = parse(R"({ "weights": { "a": 5.0, "b": 6.0 }, "bonus": 3.0, "zones": { "z": { "max": 2.0 } } })");

	// a key of a map, then an optional with an initializer (the record goes, no null is left), then a group's only record
	BOOST_CHECK(ui::edit_component_marked(schema, registry, &parent, overrides, "res://p.desc", {},
		[](json::value&, const ui::field_origin& origin) {
			origin.requests.resets = { "/weights/a", "/bonus", "/zones/z/max" };
			return false;
		}));
	BOOST_CHECK(overrides == parse(R"({ "weights": { "b": 6.0 } })"));

	// nothing to take out: no change
	BOOST_CHECK(!ui::edit_component_marked(schema, registry, &parent, overrides, {}, {},
		[](json::value&, const ui::field_origin& origin) {
			origin.requests.resets = { "/weights/a" };
			return false;
		}));
}

BOOST_AUTO_TEST_CASE(Unset_OfAnInheritedOptional_WritesANull_OfAnOwnOne_TakesTheRecordOut_OfOneWithAnInitializer_KeepsANull)
{
	const desc::type_schema schema = weights_schema();
	const auto& registry = descs->schemas();
	const auto unset = [](std::string pointer) {
		return [pointer](json::value& shown, const ui::field_origin&) { return ui::apply_unsets(shown, { pointer }); };
	};

	const json::object parent = parse(R"({ "extra": 2.0 })");
	json::object inheriting;
	BOOST_CHECK(ui::edit_component_marked(schema, registry, &parent, inheriting, {}, {}, unset("/extra")));
	BOOST_CHECK(inheriting == parse(R"({ "extra": null })"));

	json::object own = parse(R"({ "extra": 1.0 })");
	BOOST_CHECK(ui::edit_component_marked(schema, registry, nullptr, own, {}, {}, unset("/extra")));
	BOOST_CHECK(own.empty());

	json::object initialised = parse(R"({ "bonus": 3.0 })");
	BOOST_CHECK(ui::edit_component_marked(schema, registry, nullptr, initialised, {}, {}, unset("/bonus")));
	BOOST_CHECK(initialised == parse(R"({ "bonus": null })"));
}

namespace
{
	// An edit of a component over a headless ImGui window with the marks drawn, driven by a real mouse.
	struct mark_driver
	{
		mark_driver(const desc::type_schema& schema, const desc::schema_registry& registry, json::object parent, json::object overrides)
			: schema(schema), registry(registry), parent(std::move(parent)), overrides(std::move(overrides)) {}

		void frames(int count)
		{
			for (int i = 0; i < count; ++i) {
				imgui.frame([&] {
					ImGui::Begin("schema_test");
					ui::context ctx{ registry, {}, {} };
					ctx.on_target = [this](std::string_view target, float x, float y) { targets[std::string(target)] = ImVec2(x, y); };
					ui::edit_component_marked(schema, registry, &parent, overrides, "res://p.desc", {},
						[&](json::value& shown, const ui::field_origin& origin) {
							ctx.origin = &origin;
							return ui::draw_fields(schema, shown, ctx);
						});
					ImGui::End();
				});
			}
		}

		void click(const std::string& target)
		{
			const ImVec2 at = targets.at(target);
			ImGuiIO& io = ImGui::GetIO();
			io.AddMousePosEvent(at.x, at.y);
			frames(2);
			io.AddMouseButtonEvent(0, true);
			frames(2);
			io.AddMouseButtonEvent(0, false);
			frames(2);
		}

		imgui_ctx imgui;
		const desc::type_schema& schema;
		const desc::schema_registry& registry;
		json::object parent;
		json::object overrides;
		std::map<std::string, ImVec2> targets;
	};
}

BOOST_AUTO_TEST_CASE(TheMarksMenu_Reset_TakesTheRecordOutOfTheOwnData)
{
	const desc::type_schema schema = weights_schema();
	mark_driver driver(schema, descs->schemas(), parse(R"({ "weights": { "a": 1.0 }, "extra": 2.0 })"),
		parse(R"({ "weights": { "a": 5.0 }, "extra": 1.5 })"));
	driver.frames(3);
	driver.click("/weights/a#mark");
	driver.frames(2);
	driver.click("/weights/a#reset");
	BOOST_CHECK(driver.overrides == parse(R"({ "extra": 1.5 })"));
}

BOOST_AUTO_TEST_CASE(TheMarksMenu_Unset_WritesANullOverAnInheritedValue)
{
	const desc::type_schema schema = weights_schema();
	mark_driver driver(schema, descs->schemas(), parse(R"({ "weights": { "a": 1.0 }, "extra": 2.0 })"),
		parse(R"({ "weights": { "a": 5.0 }, "extra": 1.5 })"));
	driver.frames(3);
	driver.click("/extra#mark");
	driver.frames(2);
	driver.click("/extra#unset");
	BOOST_CHECK(driver.overrides == parse(R"({ "weights": { "a": 5.0 }, "extra": null })"));
}

BOOST_AUTO_TEST_CASE(TheMarkOfAReadOnlyField_OffersNeitherResetNorUnset)
{
	mark_driver driver(widget_schema(), descs->schemas(), {}, parse(R"({ "serial": 9 })"));
	driver.frames(3);
	driver.click("/serial#mark");
	driver.frames(2);
	BOOST_CHECK(!driver.targets.contains("/serial#reset"));
	BOOST_CHECK(!driver.targets.contains("/serial#unset"));
	BOOST_CHECK(driver.overrides == parse(R"({ "serial": 9 })"));
}

BOOST_AUTO_TEST_CASE(Reset_OfMapKeysWithASlashOrATilde_TakesOnlyThatKeyOut)
{
	const desc::type_schema schema = weights_schema();
	json::object overrides = parse(R"({ "weights": { "a/b": 1.0, "c~d": 2.0, "e": 3.0 } })");
	const ui::field_origin origin = origin_of({}, overrides);
	BOOST_CHECK(ui::set_here(origin, "/weights/a~1b"));
	BOOST_CHECK(ui::set_here(origin, "/weights/c~0d"));
	BOOST_CHECK(!ui::set_here(origin, "/weights/a"));
	BOOST_CHECK_EQUAL(ui::apply_resets(overrides, schema, { "/weights/a~1b", "/weights/c~0d" }), 2u);
	BOOST_CHECK(overrides == parse(R"({ "weights": { "e": 3.0 } })"));
}

BOOST_AUTO_TEST_CASE(Unset_OfADataDefinedOptionalWithADefaultInsideAStructure_LeavesNoNull)
{
	auto& registry = descs->schemas();
	descs->register_schema(desc::parse_type_schema(parse(R"({
		"name": "edt.inner_opt", "kind": "struct",
		"fields": [ { "name": "x", "kind": "f32", "default": 1.0, "optional": true } ]
	})")));
	descs->register_schema(desc::parse_type_schema(parse(R"({
		"name": "edt.outer", "kind": "component",
		"fields": [ { "name": "s", "kind": "structure", "ref_type": "edt.inner_opt" } ]
	})"), &registry));
	const desc::type_schema* outer = descs->find_schema("edt.outer");
	BOOST_REQUIRE(outer != nullptr);

	// a data-defined field reads a null as its default (#861): without the record the read gives the same, so none is left
	json::object overrides = parse(R"({ "s": { "x": 3.0 } })");
	BOOST_CHECK(ui::edit_component_marked(*outer, registry, nullptr, overrides, {}, {},
		[](json::value& shown, const ui::field_origin&) { return ui::apply_unsets(shown, { "/s/x" }); }));
	BOOST_CHECK(overrides.empty());
}

// #861: only a std::optional member reads a null as a value of its own.
BOOST_AUTO_TEST_CASE(Nullable_IsSetForAStdOptionalMemberOnly)
{
	const desc::type_schema schema = unset_schema(descs->schemas());
	BOOST_REQUIRE(schema.find_field("/bonus") && schema.find_field("/level"));
	BOOST_CHECK(schema.find_field("/bonus")->nullable);
	BOOST_CHECK(schema.find_field("/level")->optional);
	BOOST_CHECK(!schema.find_field("/level")->nullable);

	const desc::type_schema data = desc::parse_type_schema(parse(R"({
		"name": "edt.dd_unset", "kind": "component",
		"fields": [ { "name": "hp", "kind": "f32", "default": 100, "optional": true } ]
	})"));
	BOOST_REQUIRE(data.find_field("/hp"));
	BOOST_CHECK(data.find_field("/hp")->optional);
	BOOST_CHECK(!data.find_field("/hp")->nullable);
}

// #861: a null is written only where it changes what the game reads.
BOOST_AUTO_TEST_CASE(Unset_WritesANullOnlyWhereItChangesWhatIsRead)
{
	auto& registry = descs->schemas();
	const desc::type_schema schema = unset_schema(registry);
	const auto unset = [](std::string pointer) {
		return [pointer](json::value& shown, const ui::field_origin&) { return ui::apply_unsets(shown, { pointer }); };
	};

	// a std::optional with an initializer: without the record the read gives 2, so the null stays
	json::object bonus = parse(R"({ "bonus": 3.0 })");
	BOOST_CHECK(ui::edit_component_marked(schema, registry, nullptr, bonus, {}, {}, unset("/bonus")));
	BOOST_CHECK(bonus == parse(R"({ "bonus": null })"));

	// a plain member marked optional reads a null as its default: the record goes, no null is left
	json::object level = parse(R"({ "level": 7.0 })");
	BOOST_CHECK(ui::edit_component_marked(schema, registry, nullptr, level, {}, {}, unset("/level")));
	BOOST_CHECK(level.empty());

	// over a parent's value the null takes it away: written
	const json::object parent = parse(R"({ "level": 7.0 })");
	json::object inheriting;
	BOOST_CHECK(ui::edit_component_marked(schema, registry, &parent, inheriting, {}, {}, unset("/level")));
	BOOST_CHECK(inheriting == parse(R"({ "level": null })"));

	// a std::optional with an initializer inside a structure: the null stays
	json::object nested = parse(R"({ "inner": { "x": 3.0 } })");
	BOOST_CHECK(ui::edit_component_marked(schema, registry, nullptr, nested, {}, {}, unset("/inner/x")));
	BOOST_CHECK(nested == parse(R"({ "inner": { "x": null } })"));
}

// #861: Unset is offered where it changes the value; a null of a non-nullable field shows what the game reads.
BOOST_AUTO_TEST_CASE(Unset_ChangesTheValueOnlyAwayFromWhatANullReads_AndANullShowsThatValue)
{
	auto& registry = descs->schemas();
	const desc::type_schema schema = unset_schema(registry);
	const desc::field_schema& level = *schema.find_field("/level");
	const desc::field_schema& bonus = *schema.find_field("/bonus");

	BOOST_CHECK(ui::null_reads_as(bonus, registry).is_null());
	BOOST_CHECK(ui::null_reads_as(level, registry).to_number<double>() == 5.0);
	BOOST_CHECK(!ui::unset_changes_value(level, json::value(5.0), registry));
	BOOST_CHECK(ui::unset_changes_value(level, json::value(7.0), registry));
	BOOST_CHECK(ui::unset_changes_value(bonus, json::value(2.0), registry));

	const json::object own = parse(R"({ "level": null, "bonus": null })");
	const json::value shown = ui::compose(schema, registry, { &own });
	BOOST_REQUIRE(desc::json_pointer_get(shown, "/level") != nullptr);
	BOOST_CHECK(desc::json_pointer_get(shown, "/level")->to_number<double>() == 5.0);
	BOOST_CHECK(desc::json_pointer_get(shown, "/bonus") == nullptr);

	// a missing value shows what the read gives: the default, the initializer of a std::optional too
	const json::value plain = ui::compose(schema, registry, {});
	BOOST_REQUIRE(desc::json_pointer_get(plain, "/level") != nullptr);
	BOOST_CHECK(desc::json_pointer_get(plain, "/level")->to_number<double>() == 5.0);
	BOOST_REQUIRE(desc::json_pointer_get(plain, "/bonus") != nullptr);
	BOOST_CHECK(desc::json_pointer_get(plain, "/bonus")->to_number<double>() == 2.0);
}

// #861: a field that writes null for empty reads a null as its empty value and an entity as none, not as a default.
BOOST_AUTO_TEST_CASE(NullReadsAs_TheEmptyValueOfANullIfEmptyField_AndNoEntity_AndAMissingValueShowsAsANullDoes)
{
	auto& registry = descs->schemas();
	const desc::type_schema schema = desc::parse_type_schema(parse(R"({
		"name": "edt.dd_null", "kind": "component",
		"fields": [
			{ "name": "icon", "kind": "res_tag", "default": "res://a.png", "optional": true, "write": "null_if_empty" },
			{ "name": "target", "kind": "entity", "optional": true },
			{ "name": "hp", "kind": "f32", "optional": true }
		]
	})"));
	BOOST_REQUIRE(schema.find_field("/icon") && schema.find_field("/target") && schema.find_field("/hp"));
	const desc::field_schema& icon = *schema.find_field("/icon");
	BOOST_CHECK(ui::null_reads_as(icon, registry) == json::value(json::string()));
	BOOST_CHECK(ui::null_reads_as(*schema.find_field("/target"), registry).is_null());
	BOOST_CHECK(ui::unset_changes_value(icon, json::value("res://a.png"), registry));

	// Unset of the default tag: the null reads as the empty tag, not the default, so it is written and shown empty
	json::object own;
	BOOST_CHECK(ui::edit_component_marked(schema, registry, nullptr, own, {}, {},
		[](json::value& shown, const ui::field_origin&) { return ui::apply_unsets(shown, { "/icon" }); }));
	BOOST_CHECK(own == parse(R"({ "icon": null })"));
	const json::value shown = ui::compose(schema, registry, { &own });
	BOOST_REQUIRE(desc::json_pointer_get(shown, "/icon") != nullptr);
	BOOST_CHECK(desc::json_pointer_get(shown, "/icon")->as_string().empty());

	// a data-defined optional without a default: a null and a missing value both show the zero the read gives
	const json::object nulled = parse(R"({ "hp": null })");
	BOOST_TEST(number_at(ui::compose(schema, registry, { &nulled }), "/hp") == 0.0);
	BOOST_TEST(number_at(ui::compose(schema, registry, {}), "/hp") == 0.0);
}

BOOST_AUTO_TEST_CASE(TheMarksMenu_UnsetOfAValueANullWouldReadAnyway_ChangesNothing)
{
	const desc::type_schema schema = unset_schema(descs->schemas());
	mark_driver driver(schema, descs->schemas(), {}, parse(R"({ "level": 5.0 })"));
	driver.frames(3);
	driver.click("/level#mark");
	driver.frames(2);
	BOOST_REQUIRE(driver.targets.contains("/level#unset"));
	driver.click("/level#unset");
	BOOST_CHECK(driver.overrides == parse(R"({ "level": 5.0 })"));
}

BOOST_AUTO_TEST_CASE(Draw_WithAnOrigin_DrawsEveryFieldAndAnUnsetAsksForNothingByItself)
{
	const desc::type_schema schema = weights_schema();
	imgui_ctx imgui;
	const json::object parent = parse(R"({ "weights": { "a": 1.0 }, "extra": 2.0 })");
	json::object overrides = parse(R"({ "weights": { "a": 5.0 }, "bonus": 3.0, "extra": null })");
	json::value shown = ui::compose(schema, descs->schemas(), { &parent, &overrides });
	const ui::field_origin origin = origin_of(parent, overrides);
	drawn_list drawn;
	ui::context ctx{ descs->schemas(), {}, [&](std::string_view pointer, ui::widget kind) { drawn.emplace_back(std::string(pointer), kind); } };
	ctx.origin = &origin;
	bool changed = true;
	for (int frame = 0; frame < 2; ++frame) {
		drawn.clear();
		imgui.frame([&] {
			ImGui::Begin("schema_test");
			changed = ui::draw_fields(schema, shown, ctx);
			ImGui::End();
		});
	}
	BOOST_CHECK(!changed);
	BOOST_CHECK(widget_at(drawn, "/weights") == ui::widget::map);
	BOOST_CHECK(widget_at(drawn, "/weights/a") == ui::widget::real);
	BOOST_CHECK(widget_at(drawn, "/bonus") == ui::widget::real);
	BOOST_CHECK(origin.requests.resets.empty());
	BOOST_CHECK(origin.requests.unsets.empty());
}

BOOST_AUTO_TEST_CASE(ApplyEdit_OfAMapWithAnEmptyKey_WritesNothing)
{
	const desc::type_schema schema = weights_schema();
	const auto& registry = descs->schemas();
	json::object overrides = parse(R"({ "weights": { "a": 1.0 } })");
	const json::value base = ui::compose(schema, registry, { }, false);
	const json::value before = ui::compose(schema, registry, { &overrides });
	json::value after = before;
	after.at_pointer("/weights").as_object().emplace("", 2.0);
	after.at_pointer("/weights").as_object().emplace("fine", 3.0);
	BOOST_CHECK_EQUAL(ui::apply_edit(overrides, schema, registry, base, before, after), 0u);
	BOOST_CHECK(overrides == parse(R"({ "weights": { "a": 1.0 } })"));
}

// Every kind of edit of a map, written through apply_edit and read back as the editor reads it (parent + overrides).
BOOST_AUTO_TEST_CASE(ApplyEdit_OfAMap_RoundTripsThroughTheOverrides)
{
	const desc::type_schema schema = weights_schema();
	const auto& registry = descs->schemas();
	const json::object parent = parse(R"({ "weights": { "a": 1.0, "b": 2.0 }, "zones": { "z": { "min": 0.0, "max": 2.0 } } })");
	const json::value base = ui::compose(schema, registry, { &parent }, false);

	const auto without_null_keys = [](json::value shown) {
		for (const char* name : { "weights", "zones" }) {
			json::object& entries = shown.at(name).as_object();
			for (auto it = entries.begin(); it != entries.end();) {
				it = it->value().is_null() ? entries.erase(it) : std::next(it);
			}
		}
		return shown;
	};
	const auto edit_and_check = [&](const char* what, const auto& change) {
		BOOST_TEST_CONTEXT(what) {
			json::object overrides;
			const json::value before = ui::compose(schema, registry, { &parent, &overrides });
			json::value after = before;
			change(after);
			BOOST_CHECK(ui::apply_edit(overrides, schema, registry, base, before, after) > 0u);
			BOOST_CHECK(without_null_keys(ui::compose(schema, registry, { &parent, &overrides })) == after);
		}
	};
	edit_and_check("add a key", [](json::value& v) { v.at_pointer("/weights").as_object().emplace("c", 3.0); });
	edit_and_check("change a key", [](json::value& v) { v.at_pointer("/weights/a") = 9.0; });
	edit_and_check("remove an inherited key", [](json::value& v) { v.at_pointer("/weights").as_object().erase("b"); });
	edit_and_check("keys with a slash and a tilde", [](json::value& v) {
		v.at_pointer("/weights").as_object().emplace("a/b", 1.5);
		v.at_pointer("/weights").as_object().emplace("x~y", 2.5);
	});
	edit_and_check("a map of structures", [](json::value& v) {
		v.at_pointer("/zones/z/max") = 7.0;
		v.at_pointer("/zones").as_object().emplace("w", parse(R"({ "min": 1.0, "max": 5.0 })"));
	});
}

BOOST_AUTO_TEST_SUITE_END()
