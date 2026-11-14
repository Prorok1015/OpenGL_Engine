#pragma once
#include "edt_asset_references.h"
#include <snk/editor/edt_registration.h>
#include <snk/resource/res_asset_id.h>
#include <snk/resource/res_tag.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cmd { class command_registry; }
namespace desc { class desc_system; }

// Who refers to an asset (EPIC-57 section 6.3, #774): the reverse of the direct references the asset database reads from
// the desc files on the disk, and the project manifest's own.
namespace edt
{
	class asset_database;

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
	};

	// A reference matches when its guid is the asset's ID, or, with no guid or a guid no file holds, when its path names the
	// asset's file. `descs` says what each reference is (classify_reference); null: by its place only.
	usage_report find_usages(const asset_database& assets, const desc::desc_system* descs, const usage_target& target,
		std::string_view manifest_file = {}, std::span<const manifest_reference> manifest = {});

	// One row of "Used by" as the asset browser shows it: "<file>  <pointer>  (<kind>)".
	std::string usage_row_text(const asset_usage& usage);

	// The usages of the asset picked in the browser ("Used by"), asked again only when another asset is picked or the
	// database moved (its revision), not at every frame.
	class usage_list
	{
	public:
		// `report` gives the usages of an asset now (nullopt: no database, no project); `revision` grows whenever what
		// it would say may have changed.
		using report_source = std::function<std::optional<usage_report>(const res::tag& asset)>;
		void set_source(report_source report, std::function<std::uint64_t()> revision);
		bool has_source() const { return static_cast<bool>(m_report); }
		// Null: no source, or the source has no database.
		const usage_report* report_for(const res::tag& asset);
		// How many times the source was asked. For tests.
		std::size_t queries() const { return m_queries; }

	private:
		report_source m_report;
		std::function<std::uint64_t()> m_revision;
		std::optional<res::tag> m_asset;
		std::uint64_t m_seen = 0;
		std::optional<usage_report> m_cached;
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
