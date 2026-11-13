#pragma once
#include <snk/desc/desc_schema.h>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>

// The inspector UI generated from a desc::type_schema (DESC_SCHEMA.md 2.5): every field gets the widget its kind and
// hints ask for, and what the user changes is written back as the JSON pointers that differ, nothing else.
namespace edt::schema_ui
{
	enum class widget : std::uint8_t
	{
		hidden,   // editor_hidden, or a read-only key (write = never)
		checkbox,
		integer,  // i32 u32 i64 u64
		real,     // f32 f64
		vector,   // vec2 vec3 vec4 quat: one drag per component
		color,    // colour picker (3 or 4 components)
		combo,    // enumeration with a known enum_schema
		text,     // string
		asset,    // res_tag, desc_ref given as a tag: text field that takes a dropped asset
		group,    // structure with a known nested schema
		list,     // array
		map,      // map: a row per key (rename, remove), "+ Add" asks for the key
		label,    // node_ref, entity: shown, not edited
		json,     // custom, or a value the schema cannot draw: the generic JSON editor
	};

	struct widget_spec
	{
		widget kind = widget::json;
		bool disabled = false; // read_only
		bool bounded = false;  // range
		double min = 0.0;
		double max = 0.0;
		double speed = 0.0;    // range step, 0 = the widget's default
		bool degrees = false;
		std::string format;    // printf format of the number widgets, empty = ImGui's default
	};

	// The widget of one field from its kind and hints only. `enum_known`/`nested_known`: the enum_schema / the nested
	// type_schema the field names is in the registry; without it the field falls back to the JSON editor.
	widget_spec describe_widget(const desc::field_schema& field, bool enum_known, bool nested_known);

	// The longest key a map row accepts.
	inline constexpr std::size_t k_max_map_key = 256;

	// Why `key` cannot be the name of a new or renamed entry of `entries`: it is empty, longer than k_max_map_key, or taken
	// by another entry already. Empty when it can.
	std::string map_key_problem(const boost::json::object& entries, std::string_view key);

	// Adds the entry; false, and nothing changed, when map_key_problem names a problem.
	bool add_map_key(boost::json::object& entries, std::string_view key, boost::json::value value);

	// Gives the entry `from` the name `to`, keeping its place; false, and nothing changed, when there is no `from` or `to`
	// is a problem. The same name is no change (true).
	bool rename_map_key(boost::json::object& entries, std::string_view from, std::string_view to);

	// "m_near_distance" -> "Near distance"
	std::string display_label(const desc::field_schema& field);

	// Why the text of a tag field names nothing (#631): it has no protocol ("materials/a.mat.desc"). Empty for an empty
	// text and for a text with a protocol, mounted or not.
	std::string tag_text_problem(std::string_view text);

	// A value of `kind` that is "nothing yet" (0, "", [0,0,0], first enum item, ...); null for custom and entity.
	boost::json::value zero_value(const desc::field_schema& field, const desc::schema_registry& registry);

	// Every non-optional field at its default, under its key. A field with no default gets a zero to show unless
	// `fill_missing` is false: the base of an edit is built that way, so a value typed into such a field is never
	// taken for "back to what is inherited" and always written.
	boost::json::value defaults_json(const desc::type_schema& schema, const desc::schema_registry& registry, bool fill_missing = true);

	// The values the component really has: the schema defaults with each layer (parent data, own overrides, ...) on
	// top, null layers skipped. A layer that names a field by a read alias fills the field's key.
	boost::json::value compose(const desc::type_schema& schema, const desc::schema_registry& registry,
		std::initializer_list<const boost::json::object*> layers, bool fill_missing = true);

	struct context
	{
		const desc::schema_registry& registry;
		// Draws a value the schema cannot describe (custom fields); returns true when it edited the value.
		std::function<bool(const std::string& label, boost::json::value& value)> draw_json;
		// Called for every field that got a widget (tests): the field's pointer from the root and the widget.
		std::function<void(std::string_view pointer, widget kind)> on_field;
	};

	// Draws the visible fields of `schema` for `object` (the whole component, or a nested structure) and edits it in
	// place. True when the user changed a value. Needs an ImGui frame.
	bool draw_fields(const desc::type_schema& schema, boost::json::value& object, const context& ctx);

	// Writes the difference between `before` and `after` (what one edit did) into `overrides`, one JSON pointer per
	// changed field. A field the edit sets back to its `base` value loses its pointer (it is inherited again); pointers
	// the edit did not touch stay as they are (a revert also drops the field's legacy alias keys). Returns how many
	// fields the edit changed.
	std::size_t apply_edit(boost::json::object& overrides, const desc::type_schema& schema, const desc::schema_registry& registry,
		const boost::json::value& base, const boost::json::value& before, const boost::json::value& after);

	// One edit of a component in the Inspector: `draw` edits what is shown (the `parent` data with the component's own
	// `overrides` on top) and returns true when it changed something; apply_edit then writes the difference against the
	// parent into `overrides`. True when the overrides changed.
	bool edit_component(const desc::type_schema& schema, const desc::schema_registry& registry, const boost::json::object* parent,
		boost::json::object& overrides, const std::function<bool(boost::json::value& shown)>& draw);
}
