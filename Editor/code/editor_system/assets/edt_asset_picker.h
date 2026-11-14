#pragma once
#include "edt_asset_database.h"
#include <snk/desc/desc_schema.h>
#include <snk/resource/res_tag.h>
#include <boost/json.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// What a reference field (res_tag, desc_ref) may point at, and what the picker lists for it (EPIC-56 section 4.5, S56-9).
// No ImGui here: the schema inspector draws the list and the warnings from these answers.
namespace edt::picker
{
	// The files the picker chooses from. The editor gives the asset database; tests give a list of records.
	class catalog
	{
	public:
		virtual ~catalog() = default;
		// The first walk of the resource tree has finished: before it, a missing file proves nothing.
		virtual bool ready() const = 0;
		virtual void for_each(const std::function<void(const asset_record&)>& visit) const = 0;
		virtual const asset_record* find(const res::tag& path) const = 0;
		// How many layers res:// has (the lowest layer is the engine's, the highest the project's).
		virtual std::size_t layer_count() const = 0;
		// Moves whenever the records change: a list made at an older revision is stale (and its record pointers too).
		virtual std::uint64_t revision() const = 0;
		// The records are up to date with the tree: no walk runs. A file that is not a record may still be new.
		virtual bool settled() const = 0;
	};

	class database_catalog final : public catalog
	{
	public:
		explicit database_catalog(const asset_database& database) : m_database(database) {}

		bool ready() const override { return m_database.walked_once(); }
		void for_each(const std::function<void(const asset_record&)>& visit) const override;
		const asset_record* find(const res::tag& path) const override { return m_database.find(path); }
		std::size_t layer_count() const override { return m_database.layers().size(); }
		std::uint64_t revision() const override { return m_database.revision(); }
		bool settled() const override { return m_database.walked_once() && !m_database.scanning(); }

	private:
		const asset_database& m_database;
	};

	// What a field asks of a file.
	struct filter
	{
		desc::field_kind kind = desc::field_kind::res_tag;
		// desc_ref: the `__type` of the desc; empty = any desc.
		std::string ref_type;
		// Extensions ("png,jpg"); empty = any.
		std::string extensions;

		bool operator==(const filter&) const = default;
	};

	filter filter_of(const desc::field_schema& field);

	// `path` ends with one of the extensions of `extensions` (letter case ignored); an empty list takes everything.
	bool extension_matches(std::string_view path, std::string_view extensions);

	// "project" for the top layer, "engine" for the lowest, "package" between.
	std::string layer_label(std::size_t layer, std::size_t layer_count);

	struct verdict
	{
		enum class result
		{
			fits,
			rejected,
			// A desc whose type the background pass has not read yet: it may fit.
			unknown,
		};
		result outcome = result::fits;
		std::string reason;

		bool fits() const { return outcome == result::fits; }
	};

	// Whether the file `record` is something the field accepts, and why not.
	verdict judge(const filter& wanted, const asset_record& record);

	struct candidate
	{
		const asset_record* record = nullptr;
		std::string layer_label;
	};

	struct candidates_page
	{
		// At most `limit` files, the project's first, then packages, then the engine's; by path inside a layer.
		std::vector<candidate> items;
		// Files that fit and match the query (items is cut at the limit).
		std::size_t total = 0;
		// Descs that matched the query and whose type is not read yet: not listed, they may fit.
		std::size_t unread = 0;
	};

	// The files of `source` the field accepts whose path holds `query` (letter case ignored; empty = all). A file a higher
	// layer covers is not a record of its own, so it is not listed.
	candidates_page candidates(const catalog& source, const filter& wanted, std::string_view query, std::size_t limit);

	// The page of the last call, kept by whoever draws the list: while the query, the filter and the revision of the catalog stay
	// what they were, the files are not walked again.
	struct page_cache
	{
		bool valid = false;
		filter wanted;
		std::string query;
		std::size_t limit = 0;
		std::uint64_t revision = 0;
		candidates_page page;
	};

	const candidates_page& cached_candidates(page_cache& cache, const catalog& source, const filter& wanted, std::string_view query, std::size_t limit);

	// What is written into the field for the picked file.
	boost::json::value reference_value(const asset_record& record);

	struct reference_state
	{
		enum class level
		{
			ok,
			// Worth saying, not an error (the type is not read yet).
			note,
			error,
		};
		level severity = level::ok;
		std::string text;
	};

	// What to say under a field that holds `text`: nothing for an empty text, a text with no res:// protocol (the field's own
	// check speaks of that) and while the tree has not been walked.
	reference_state judge_text(const filter& wanted, std::string_view text, const catalog& source);

	// A file dropped on the field: `path` is below res://, as the asset browser sends it. A file the records do not hold yet (just
	// imported) is `unknown`, not refused: it comes from the browser, so it is there.
	verdict judge_drop(const filter& wanted, std::string_view path, const catalog* source);
}
