#include <snk/scene/scn_material_desc.h>

void scn::material_desc::deserialize(desc::desc_system& desc_system, const json::object& data)
{
	// The schema reads everything the hand-written reader read but one form, settled here on a copy of the object: a queue
	// that is neither a word nor null was no queue (the generic reader would take a number that names an enumerator).
	json::object rest = data;
	const auto* queue_value = rest.if_contains("queue");
	if (queue_value && !queue_value->is_string() && !queue_value->is_null()) {
		rest["queue"] = json::value();
	}

	desc::schema_desc<material_desc, desc::schema_kind::desc>::deserialize(desc_system, rest);
}
