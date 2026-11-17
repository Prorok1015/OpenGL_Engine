#pragma once
#include "edt_asset_database.h"
#include <snk/editor/edt_problem_list.h>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace edt
{
	// A fix a row of Problems offers, for the person only (never an agent command).
	struct problem_action
	{
		std::string label;
		std::function<asset_fix_result()> run;
	};

	// The source "assets" of Problems (S57-4): what the asset database finds wrong with the files of res://. Rows:
	// asset.no_id (a file of the project's own layer without a .resource), asset.bad_meta (a .resource that cannot be
	// read), asset.orphan (a .resource without its file), asset.duplicate_id (on each copy: of the files with one ID
	// the one of the lowest layer, then the oldest by mtime, then the smaller path, is the original; an error, or a
	// warning without a fix when no holder is in the project's own layer). Main thread only.
	class asset_problem_publisher
	{
	public:
		static constexpr const char* source = "assets";

		// Rebuilds the rows when the database's revision moved; none clears the source.
		void publish(problem_list& problems, const asset_database& assets);
		// The res:// path a row leads to (for an orphan, its .resource).
		std::optional<std::string> file_of(const problem& row) const;
		// The fixes a row offers; none for a row this publisher did not make.
		std::vector<problem_action> actions_of(const problem& row, asset_database& assets) const;

	private:
		enum class kind { no_id, bad_meta, orphan, duplicate_id };

		struct entry
		{
			problem item;
			kind what = kind::no_id;
			// The file of a top-layer record, or the .resource of an orphan.
			std::string path;
			// A duplicate whose file and .resource are both in the project's own layer (its ID may be replaced).
			bool in_top_layer = false;
			// The file an orphan may be linked to; empty: none.
			std::string link_to;
		};

		const entry* find(const problem& row) const;

		std::vector<entry> m_last;
		std::optional<std::uint64_t> m_revision;
	};
}
