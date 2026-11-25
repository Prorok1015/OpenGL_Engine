#pragma once
#include <snk/model_codec/res_tag.h>
#include <boost/json/array.hpp>
#include <boost/json/value.hpp>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace edt
{
	class data_document;

	// A sampler slot of a material (#1205): its place in `/samplers` (positional) and the name the window shows.
	struct texture_slot
	{
		std::size_t index = 0;
		std::string_view name;
	};

	// Diffuse, Specular, Normal: /samplers/0..2, the slots the scene shaders sample.
	std::span<const texture_slot> material_texture_slots();

	// What a slot holds as the material has it (its parents merged).
	struct texture_slot_state
	{
		// The entry of `/samplers`: a res:// string, a {guid, path} reference, or null for an empty slot.
		boost::json::value value;
		// The path the entry names; empty for an empty slot.
		std::string text;
		// The file has no `/samplers` of its own: the slot is its parent's.
		bool inherited = false;
	};

	// `samplers` with `value` at `index` (null fills a gap before it), and without the empty slots at its end; a hole in the
	// middle stays null. Anything but an array counts as an empty list.
	boost::json::array with_slot(const boost::json::value& samplers, std::size_t index, boost::json::value value);

	// The texture slots of a material_desc data document without ImGui (#1205): each change writes the whole `/samplers` list
	// (the field is one custom value) through data_document::edit, one record of the history labelled with the slot
	// ("Set Diffuse texture", "Clear Normal texture"). A list the parents have already is no own list: the edit drops it.
	class texture_slot_model
	{
	public:
		explicit texture_slot_model(data_document& document) : m_document(document) {}

		texture_slot_state state(const texture_slot& slot) const;
		// Writes `value` (picker::reference_value of the texture desc) into the slot. False when nothing changed.
		bool set(const texture_slot& slot, boost::json::value value);
		// Empties the slot (null, or shorter when it is the last). False when nothing changed.
		bool clear(const texture_slot& slot);

	private:
		bool write(const texture_slot& slot, boost::json::value value, std::string label);

		data_document& m_document;
	};
}
