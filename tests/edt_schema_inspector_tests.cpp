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
	};
	BOOST_DESCRIBE_STRUCT(weights_component, (), (weights, labels, zones, speeds, extra))
}

namespace desc
{
	template<>
	inline constexpr auto field_attrs<weights_component> = attrs(f("zones").ref("edt.limits"), f("speeds").ref("edt_mode"));
}

namespace
{
	desc::type_schema weights_schema()
	{
		return desc::schema_generator<weights_component>::make("edt.weights", 1, desc::schema_kind::component);
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

	// editor_hidden is not drawn; an optional field without a value is shown as not set
	BOOST_CHECK(!was_drawn(drawn, "/secret"));
	BOOST_CHECK(widget_at(drawn, "/extra") == ui::widget::label);
	BOOST_CHECK(desc::json_pointer_get(value, "/extra") == nullptr);
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
	BOOST_CHECK(desc::json_pointer_get(value, "/extra") == nullptr); // optional: no value until set
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
	json::object overrides = parse(R"({ "boost": 3.0 })");
	const json::value base = ui::compose(schema, registry, { }, false);
	const json::value before = ui::compose(schema, registry, { &overrides });
	json::value after = before;
	after.as_object().erase("boost");
	ui::apply_edit(overrides, schema, registry, base, before, after);
	// without the record the read gives the initializer (2.0): the clear must stay written
	BOOST_CHECK(overrides == parse(R"({ "boost": null })"));
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
