#pragma once
#include <snk/desc/desc_pretty_json.h>
#include <boost/json/value.hpp>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace edt
{
	// What an autosave of a document writes (#1186): taken on the main thread, from the document as it is, without
	// writing anything and without changing the document. `bytes` or `tree`; neither when `error` says why not.
	struct autosave_snapshot
	{
		std::vector<std::byte> bytes;
		// A document whose text is printed off the main thread (the level): the file form, printed by autosave_bytes.
		std::optional<boost::json::value> tree;
		std::string error;
	};

	// A document the editor autosaves. Editor-internal, not part of document (whose vtable is pinned): the autosave
	// component finds it with a dynamic_cast.
	class autosave_source
	{
	public:
		virtual ~autosave_source() = default;
		// The bytes Save would write for the document as it is now.
		virtual autosave_snapshot autosave_capture() const = 0;
	};

	// The bytes of a snapshot; any thread (printing a tree reads no schema).
	inline std::vector<std::byte> autosave_bytes(const autosave_snapshot& snapshot)
	{
		return snapshot.tree ? desc::pretty_json_bytes(*snapshot.tree) : snapshot.bytes;
	}
}
