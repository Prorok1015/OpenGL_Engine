#pragma once
#include "edt_desc_file_commands.h"
#include <snk/resource/res_tag.h>
#include <snk/scene/level/scn_override_record.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace edt
{
	struct editor_services;
	class prefab_document;

	// The bytes `prefab` is written to the file `tag` as (the adapter's form: header, references, only its own layer when it has a
	// __parent, pretty JSON). Empty when the descriptions are not available.
	std::vector<std::byte> prefab_file_bytes(editor_services& services, const scn::prefab_desc& prefab, const res::tag& tag,
		std::string* why = nullptr);

	// Changes the prefab file `tag` without opening it in the prefab editor: reads the file fresh, lets `change` edit a copy (it returns
	// the reason it cannot, empty when it did), writes the result and tells the levels that use the prefab. Refuses, writing nothing,
	// under the checks of an Apply of the prefab editor (a project that may not be written, a lower layer, the open level's own file,
	// a file too big) and for a file that cannot be read. Returns the reason, empty when it was written.
	std::string edit_prefab_file(editor_services& services, const desc_file_layout& files, const res::tag& tag,
		const std::function<std::string(scn::prefab_desc&)>& change);

	// "Apply to Prefab" of an instance: moves `records` (override records of a node that instantiates `tag`) into the file of `tag`
	// (override_edit::fold_record for each, all or none). `open_dirty`: the document of the prefab editor when it has unapplied edits;
	// the file is not written under it (the edits would be in the way of the apply), the person applies or reverts them first.
	// Returns why it was not done, empty when the file is written.
	std::string apply_override_records(editor_services& services, const desc_file_layout& files, const res::tag& tag,
		const std::vector<scn::override_record>& records, const prefab_document* open_dirty);
}
