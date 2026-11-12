#pragma once
#include <snk/desc/desc_hints.hpp>
#include <snk/desc/desc_schema_desc.h>
#include <snk/input/inp_actions.h>
#include <snk/common/snk_api.h>
#include <boost/describe.hpp>
#include <string>
#include <string_view>
#include <vector>

// The game actions of a project (WORLD_BUS.md 4.1): res/input/actions.desc, named by "input_actions" of project.snkproj.
//   { "__type": "input_actions_desc", "__version": 1,
//     "actions": [ { "name": "MoveLeft", "bindings": [ { "key": "A" }, { "key": "LEFT" }, { "axis": "LeftX-" } ] },
//                  { "name": "Fire", "bindings": [ { "mouse": "LEFT" }, { "gamepad": "A" } ] } ],
//     "gamepad": { "deadzone": 0.25, "press_threshold": 0.5 } }
// Key names are those of inp::KEYBOARD_BUTTONS (physical US positions), mouse names those of inp::MOUSE_BUTTONS, "gamepad"
// names those of inp::GAMEPAD_BUTTONS and "axis" names a directed axis (inp::gamepad_axis_from_name). A binding names exactly
// one of the four. "gamepad" of the file (both numbers optional) is the one setting of the device: below `deadzone` an axis
// reads 0, a directed axis is pressed past `press_threshold`. The desc keeps the file as written; build_action_map checks it.
namespace scn
{
	struct input_binding_entry
	{
		std::string key{};
		std::string mouse{};
		std::string gamepad{};
		std::string axis{};
		// The field is there but is not a string: build_action_map reports it.
		bool key_not_text = false;
		bool mouse_not_text = false;
		bool gamepad_not_text = false;
		bool axis_not_text = false;
		bool operator==(const input_binding_entry&) const = default;
	};

	struct input_action_entry
	{
		std::string name{};
		std::vector<input_binding_entry> bindings{};
		bool operator==(const input_action_entry&) const = default;
	};

	// Not a std::vector: the field is read and written only by its codec, never as a schema array.
	struct input_action_list
	{
		std::vector<input_action_entry> entries{};
		// As the file had it: written back unchanged while `entries` still read the same, so saving a file with
		// mistakes in it keeps them for the person to fix instead of dropping them.
		json::value source;
		bool operator==(const input_action_list& other) const { return entries == other.entries; }
	};

	// The setting of the device. A field of the wrong type or out of 0..1 reads as the default and is reported by
	// build_gamepad_config; `source` is written back unchanged while the numbers read the same.
	struct input_gamepad_settings
	{
		inp::gamepad_config config{};
		bool has_deadzone = false;
		bool has_press_threshold = false;
		bool deadzone_not_number = false;
		bool press_threshold_not_number = false;
		bool not_object = false;
		json::value source;
		bool operator==(const input_gamepad_settings& other) const { return config == other.config && has_deadzone == other.has_deadzone && has_press_threshold == other.has_press_threshold; }
	};

	class input_actions_desc;
}

namespace desc
{
	// The list is read leniently (WORLD_BUS.md, question 4): a field of the wrong type reads as empty and is reported by
	// build_action_map with its path, the rest of the file still loads.
	template<>
	struct SNK_API json_codec<scn::input_action_list>
	{
		static json::value to_json(const scn::input_action_list& actions);
		static bool from_json(scn::input_action_list& actions, const json::value& in);
	};

	template<>
	struct SNK_API json_codec<scn::input_gamepad_settings>
	{
		static json::value to_json(const scn::input_gamepad_settings& settings);
		static bool from_json(scn::input_gamepad_settings& settings, const json::value& in);
	};

	template<>
	inline constexpr auto field_attrs<scn::input_actions_desc> = attrs(
		f("actions").kind(field_kind::custom).optional().tooltip("Named actions with their bindings: {\"key\": \"A\"}, {\"mouse\": \"LEFT\"}, {\"gamepad\": \"A\"} or {\"axis\": \"LeftX-\"}"),
		f("gamepad").kind(field_kind::custom).optional().tooltip("Gamepad setting: {\"deadzone\": 0.25, \"press_threshold\": 0.5}"));
}

namespace scn
{
	class SNK_API input_actions_desc : public desc::schema_desc<input_actions_desc, desc::schema_kind::desc>
	{
	public:
		static constexpr std::string_view __type = "input_actions_desc";
		using base_type = desc::desc_base;

		input_action_list actions;
		input_gamepad_settings gamepad;

		BOOST_DESCRIBE_CLASS(input_actions_desc, (), (actions, gamepad), (), ())
	};

	// The actions of the desc. A problem in the file (an empty or repeated action name, an unknown key, button or axis, a
	// binding with none or more than one of "key", "mouse", "gamepad" and "axis") is warned about with its JSON path and skipped.
	SNK_API inp::action_map build_action_map(const input_actions_desc& desc, std::string_view source = {});
	// The gamepad setting of the desc; a number out of 0..1 or of the wrong type is warned about and the default stays.
	SNK_API inp::gamepad_config build_gamepad_config(const input_actions_desc& desc, std::string_view source = {});
}
