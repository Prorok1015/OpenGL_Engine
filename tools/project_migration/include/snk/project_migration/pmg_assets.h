#pragma once
#include <snk/resource/res_asset_id.h>
#include <snk/resource/res_tag.h>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace res
{
	class resource_system;
}

// The assets of a project as its .resource files on the disk say, for the project steps of a migration (EPIC-58 3.3,
// S5 #756). It is the walk of the editor's asset database (res_asset_walk.h) over the layers of res:// stacked as res://
// stacks them (the first content root wins a name, the engine's layer is the lowest), without its cache, watcher or
// hashes: a migration runs from the command line too.
namespace prj::migration
{
	// The layers of res:// outside the project folder (the engine's core/res), lowest first: what a migration reads IDs
	// from and never writes (project_assets, scan_options::read_only_layers, #776).
	std::vector<std::filesystem::path> read_only_layers(const res::resource_system& resources, const std::filesystem::path& root);

	// A .resource of a read-only layer: what a plan's fingerprint stats, so an ID a reference would take cannot change
	// unseen between the plan and its run (#759).
	struct read_only_resource
	{
		std::filesystem::path layer;
		// Relative to the layer, with '/'.
		std::string path;
	};

	struct indexed_asset
	{
		// Relative to the project root, with '/' ("res/textures/a.png").
		std::string path;
		// Below res:// ("textures/a.png").
		std::string res_path;
		// None: the file has no .resource, or it could not be read (problems() says why).
		std::optional<res::asset_id> id;
		// A .resource pairs with the file, read or not.
		bool has_meta = false;
	};

	class project_assets
	{
	public:
		// `content_roots` relative to `root`, as project.snkproj lists them; `read_only_layers` (lowest first) below them, as
		// read_only_layers gives them. Nothing is read before rebuild().
		project_assets(std::filesystem::path root, std::vector<std::filesystem::path> content_roots,
			std::vector<std::filesystem::path> read_only_layers = {});

		// Walks the layers again; what was known before is dropped.
		void rebuild();

		// Every file of the content roots the walk lists (no .resource among them), by path. A file a higher content root
		// covers is not listed, as the editor does not list it; a file of a read-only layer is never listed, it only
		// answers id_of(tag) and path_of.
		const std::map<std::string, indexed_asset>& assets() const { return m_assets; }
		std::optional<res::asset_id> id_of(std::string_view path) const;
		// resource_system::asset_id_of over this index, as the editor's asset index answers it: the ID of the file a res://
		// tag names in the highest layer that has it (read-only layers too), the name compared as
		// res::asset_names_case_insensitive says. None for another protocol, a file without an ID (no .resource, or one
		// that does not read), and a copy whose ID another file holds by res::holds_id_before.
		std::optional<res::asset_id> id_of(const res::tag& tag) const;
		// The res:// tag of the file that holds `id` by res::holds_id_before; none for an unknown ID.
		std::optional<res::tag> path_of(const res::asset_id& id) const;
		// The listed files that carry `id`: more than one is a duplicate ID, which the editor reports too.
		std::vector<std::string> paths_of(const res::asset_id& id) const;
		// .resource files no listed file pairs with, by path; `id` when it could be read.
		const std::map<std::string, indexed_asset>& orphans() const { return m_orphans; }
		// A .resource that cannot be read, an orphan, a content root that is not walked: one line each.
		const std::vector<std::string>& problems() const { return m_problems; }
		// Every .resource the walk found in the read-only layers, in the order of the walk.
		const std::vector<read_only_resource>& read_only_resources() const { return m_read_only_resources; }

	private:
		std::filesystem::path m_root;
		std::vector<std::filesystem::path> m_content_roots;
		std::vector<std::filesystem::path> m_read_only_layers;
		std::map<std::string, indexed_asset> m_assets;
		std::map<std::string, indexed_asset> m_orphans;
		// res::asset_name_key of a holder's res_path -> its ID.
		std::map<std::string, res::asset_id> m_held_ids;
		// An ID -> the res_path of its holder.
		std::map<res::asset_id, std::string> m_holders;
		std::vector<std::string> m_problems;
		std::vector<read_only_resource> m_read_only_resources;
	};
}
