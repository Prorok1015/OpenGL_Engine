#pragma once
#include "edt_asset_references.h"
#include <snk/editor/edt_registration.h>
#include <snk/model_codec/res_asset_id.h>
#include <snk/model_codec/res_tag.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cmd { class command_registry; }
namespace desc { class desc_system; }

// Who refers to an asset (EPIC-57 section 6.3, #774): the reverse of the direct references the asset database reads from
// the desc files on the disk, and the project manifest's own.
namespace edt
{
	class asset_database;
	struct asset_record;

	struct asset_usage
	{
		// The res:// tag of the file that refers to the asset, or the manifest's file name.
		std::string file;
		std::string json_pointer;
		usage_kind kind = usage_kind::loose;
		// Found by the guid of a {guid, path}; otherwise by its path.
		bool by_guid = false;
	};

	struct usage_target
	{
		std::optional<res::tag> path;
		std::optional<res::asset_id> id;
	};

	// A reference of the project manifest (start_scene, input_actions): a file outside res://.
	struct manifest_reference
	{
		std::string json_pointer;
		std::string path;

		bool operator==(const manifest_reference&) const = default;
	};

	struct usage_report
	{
		// The asset as the database knows it: the path of the file that holds `id`, or the path asked for.
		std::optional<res::tag> path;
		std::optional<res::asset_id> id;
		// Sorted by file, then pointer.
		std::vector<asset_usage> items;
		// Desc files whose references were not read (bigger than asset_database::max_desc_bytes).
		std::vector<std::string> skipped;
		// The database has walked the tree and hashed every file: no desc file is still to be read.
		bool complete = false;
		// A file of the database is the asset (false: asked by a path or an ID no file has).
		bool found = false;
	};

	// A reference matches when its guid is the asset's ID, or, with no guid or a guid no file holds, when its path names the
	// asset's file. `descs` says what each reference is (classify_reference); null: by its place only.
	usage_report find_usages(const asset_database& assets, const desc::desc_system* descs, const usage_target& target,
		std::string_view manifest_file = {}, std::span<const manifest_reference> manifest = {});

	// The references of every file by the guid and by the path they name (#774, PR 2): built once per revision of the
	// database, so picking asset after asset in the browser does not walk every reference each time.
	class usage_index
	{
	public:
		// As find_usages; the index is built again first when `assets` is another database or its revision moved.
		usage_report find(const asset_database& assets, const desc::desc_system* descs, const usage_target& target,
			std::string_view manifest_file = {}, std::span<const manifest_reference> manifest = {});
		// How many times the index was built. For tests.
		std::size_t builds() const { return m_builds; }

	private:
		// Into the records and their reference lists: valid while the revision it was built at stands.
		struct entry
		{
			const asset_record* record = nullptr;
			const raw_reference* reference = nullptr;
		};

		void build(const asset_database& assets);

		const asset_database* m_assets = nullptr;
		std::uint64_t m_revision = 0;
		bool m_built = false;
		std::unordered_multimap<res::asset_id, entry> m_by_guid;
		// By res::asset_name_key of the path, for the references without a guid or with one no file holds.
		std::unordered_multimap<std::string, entry> m_by_path;
		std::vector<std::string> m_skipped;
		std::size_t m_builds = 0;
	};

	// One row of "Used by" as the asset browser shows it: "<file>  <pointer>  (<kind>)".
	std::string usage_row_text(const asset_usage& usage);

	// The usages of the asset picked in the browser ("Used by"), asked again only when another asset is picked or the
	// database moved (its revision), not at every frame.
	// Everything a report depends on besides the asset: a report with another stamp is asked again.
	struct usage_stamp
	{
		// asset_database::revision(): the files and the references read from them.
		std::uint64_t database = 0;
		// The generation of the game layer: its schemas say what each reference is (field or loose).
		std::uint64_t schemas = 0;
		// The manifest is read from memory: an edit of it moves no revision.
		std::vector<manifest_reference> manifest;

		bool operator==(const usage_stamp&) const = default;
	};

	class usage_list
	{
	public:
		// `report` gives the usages of an asset now (nullopt: no database, no project); `stamp` says what it depends on.
		using report_source = std::function<std::optional<usage_report>(const res::tag& asset)>;
		void set_source(report_source report, std::function<usage_stamp()> stamp);
		bool has_source() const { return static_cast<bool>(m_report); }
		// Null: no source, or the source has no database.
		const usage_report* report_for(const res::tag& asset);
		// usage_row_text of each item of the last report, made once per report.
		const std::vector<std::string>& rows() const { return m_rows; }
		// How many times the source was asked. For tests.
		std::size_t queries() const { return m_queries; }

	private:
		report_source m_report;
		std::function<usage_stamp()> m_stamp;
		std::optional<res::tag> m_asset;
		usage_stamp m_seen;
		std::optional<usage_report> m_cached;
		std::vector<std::string> m_rows;
		std::size_t m_queries = 0;
	};

	struct asset_usage_sources
	{
		// Null: no project, no database.
		std::function<const asset_database*()> assets;
		std::function<const desc::desc_system*()> descs;
		// The manifest's file name and its references (empty: no project).
		std::function<std::string()> manifest_file;
		std::function<std::vector<manifest_reference>()> manifest;
	};

	// asset.usages {path | guid, page_size?, cursor?}
	//   -> {target: {path?, guid?}, source: "disk", complete, items: [{file, json_pointer, kind, via}], skipped: [{file, reason}],
	//       next_cursor?}; skipped on the first page only.
	//   kind: field, parent, component, loose (not a field of a schema: a custom field, the record of a prefab instance) or
	//   manifest; via: guid or path. What is saved on the disk only: an unsaved edit of an open document is not seen.
	std::vector<registration> register_asset_usage_commands(cmd::command_registry& commands, asset_usage_sources sources);
}
