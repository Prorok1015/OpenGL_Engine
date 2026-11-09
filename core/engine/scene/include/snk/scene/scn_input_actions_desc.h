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
//     "actions": [ { "name": "MoveLeft", "bindings": [ { "key": "A" }, { "key": "LEFT" } ] },
//                  { "name": "Fire", "bindings": [ { "mouse": "LEFT" } ] } ] }
// Key names are those of inp::KEYBOARD_BUTTONS (physical US positions), mouse names those of inp::MOUSE_BUTTONS. The desc
// keeps the file as written; build_action_map checks it.
namespace scn
{
	struct input_binding_entry
	{
		std::string key;
		std::string mouse;
		bool operator==(const input_binding_entry&) const = default;
	};

	struct input_action_entry
	{
		std::string name;
		std::vector<input_binding_entry> bindings;
		bool operator==(const input_action_entry&) const = default;
	};

	// Not a std::vector: the field is read and written only by its codec, never as a schema array.
	struct input_action_list
	{
		std::vector<input_action_entry> entries;
		bool operator==(const input_action_list&) const = default;
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
	inline constexpr auto field_attrs<scn::input_actions_desc> = attrs(
		f("actions").kind(field_kind::custom).optional().tooltip("Named actions with their bindings: {\"key\": \"A\"} or {\"mouse\": \"LEFT\"}"));
}

namespace scn
{
	class SNK_API input_actions_desc : public desc::schema_desc<input_actions_desc, desc::schema_kind::desc>
	{
	public:
		static constexpr std::string_view __type = "input_actions_desc";
		using base_type = desc::desc_base;

		input_action_list actions;

		BOOST_DESCRIBE_CLASS(input_actions_desc, (), (actions), (), ())
	};

	// The actions of the desc. A problem in the file (an empty or repeated action name, an unknown key or button, a
	// binding with neither or both of "key" and "mouse") is warned about with its JSON path and skipped.
	SNK_API inp::action_map build_action_map(const input_actions_desc& desc, std::string_view source = {});
}
