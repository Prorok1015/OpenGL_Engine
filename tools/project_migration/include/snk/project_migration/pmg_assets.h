#pragma once
#include <snk/resource/res_asset_id.h>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The assets of a project as its .resource files on the disk say, for the project steps of a migration (EPIC-58 3.3,
// S5 #756). It is the walk of the editor's asset database (res_asset_walk.h) over the project's content roots stacked
// as res:// stacks them (the first root wins a name), without the engine's layer, its cache, watcher or hashes: a
// migration runs from the command line too.
namespace prj::migration
{
	// A name directly in the project folder that is no content even when the folder is a content root ".": the
	// project's code, its build output and project.snkproj. The scan and the index leave them out alike.
	inline bool is_project_own_name(std::string_view name)
	{
		return name == "code" || name == "build" || name == "project.snkproj";
	}

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
		// `content_roots` relative to `root`, as project.snkproj lists them. Nothing is read before rebuild().
		project_assets(std::filesystem::path root, std::vector<std::filesystem::path> content_roots);

		// Walks the content roots again; what was known before is dropped.
		void rebuild();

		// Every file the walk lists (no .resource among them), by path. A file a higher content root covers is not
		// listed, as the editor does not list it.
		const std::map<std::string, indexed_asset>& assets() const { return m_assets; }
		std::optional<res::asset_id> id_of(std::string_view path) const;
		// The listed files that carry `id`: more than one is a duplicate ID, which the editor reports too.
		std::vector<std::string> paths_of(const res::asset_id& id) const;
		// .resource files no listed file pairs with, by path; `id` when it could be read.
		const std::map<std::string, indexed_asset>& orphans() const { return m_orphans; }
		// A .resource that cannot be read, an orphan, a content root that is not walked: one line each.
		const std::vector<std::string>& problems() const { return m_problems; }

	private:
		std::filesystem::path m_root;
		std::vector<std::filesystem::path> m_content_roots;
		std::map<std::string, indexed_asset> m_assets;
		std::map<std::string, indexed_asset> m_orphans;
		std::vector<std::string> m_problems;
	};
}
