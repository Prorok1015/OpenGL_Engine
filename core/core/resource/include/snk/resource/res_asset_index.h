#pragma once
#include <optional>
#include <snk/resource/res_asset_id.h>
#include <snk/resource/res_tag.h>

namespace res
{
	// Path <-> ID of the assets of res:// (EPIC-57 section 5). resource_system keys what it caches, pins, watches and
	// reloads by the ID of a file that has one, so a renamed file keeps its loaded resource. The editor implements it
	// over its asset database; Shipping has none until the cook manifest (#46).
	// Called from any thread (pool threads request dependencies), sometimes under resource_system's own locks: an
	// implementation answers from an immutable snapshot, never waits for long and never calls resource_system (a debug
	// build asserts on that). Its answers change only before set_asset_index or apply_asset_batch.
	class asset_index
	{
	public:
		virtual ~asset_index() = default;
		// The ID of the file `tag` names, for any spelling of that file. None for a file without an ID, and for a copy
		// whose ID another file holds (a duplicate): it is cached under its own tag until it gets an ID of its own.
		virtual std::optional<asset_id> id_of(const tag& tag) const = 0;
		// The file that holds `id` now; of duplicates, the one id_of answers for.
		virtual std::optional<tag> path_of(const asset_id& id) const = 0;
	};
}
