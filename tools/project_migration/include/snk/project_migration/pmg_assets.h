#pragma once
#include <snk/model_codec/res_asset_id.h>
#include <snk/model_codec/res_tag.h>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace res
{
	class asset_index;
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
		// The .resource the walk paired with the file, relative to the project root; empty when it is in a read-only
		// layer or there is none. It may be in another content root than the file.
		std::string meta_path;
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
		// The file that holds `id` is in a read-only layer (the engine's): a project file with that ID is a copy
		// without one (#902).
		bool held_below(const res::asset_id& id) const { return m_held_below.contains(id); }
		// The listed files that carry `id`: more than one is a duplicate ID, which the editor reports too.
		std::vector<std::string> paths_of(const res::asset_id& id) const;
		// .resource files no listed file pairs with, by path; `id` when it could be read.
		const std::map<std::string, indexed_asset>& orphans() const { return m_orphans; }
		// A .resource that cannot be read, an orphan, a content root that is not walked: one line each.
		const std::vector<std::string>& problems() const { return m_problems; }
		// Every .resource the walk found in the read-only layers, in the order of the walk.
		const std::vector<read_only_resource>& read_only_resources() const { return m_read_only_resources; }
		// The read-only layers it was made with, lowest first.
		const std::vector<std::filesystem::path>& read_only_layers() const { return m_read_only_layers; }
		// The content roots it was made with, relative to the project root, as project.snkproj lists them.
		const std::vector<std::filesystem::path>& content_roots() const { return m_content_roots; }

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
		std::set<res::asset_id> m_held_below;
		std::vector<std::string> m_problems;
		std::vector<read_only_resource> m_read_only_resources;
	};

	// `assets` as a res::asset_index (id_of(tag), path_of): what resource_system::asset_id_of answers with it, and so what
	// a type step's migration_context::id_of asks (desc_system::migration_ids, #1200).
	std::shared_ptr<const res::asset_index> make_asset_index(project_assets assets);

	// For its lifetime, `resources` (a root, on the main thread) answers asset_id_of from the project's own index, as the
	// editor's asset database makes it answer: the scan, the project steps and the type steps then decide by one index.
	// Nothing is set when it already has an index (the editor's) or the project.snkproj under `root` does not read; a
	// layer (a resource_system with a parent) is logged and left as it is.
	class scoped_project_index
	{
	public:
		scoped_project_index(res::resource_system& resources, const std::filesystem::path& root, std::vector<std::filesystem::path> read_only_layers);
		~scoped_project_index();
		scoped_project_index(const scoped_project_index&) = delete;
		scoped_project_index& operator=(const scoped_project_index&) = delete;

	private:
		res::resource_system* m_resources = nullptr;
	};
}
