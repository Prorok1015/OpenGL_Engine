#pragma once
#include <snk/resource/res_asset_id.h>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The assets of a project as its .resource files on the disk say, for the project steps of a migration (EPIC-58 3.3,
// S5 #756). It walks the content roots by the rules of the editor's asset database (res_asset_walk.h), one root as one
// layer, but synchronously and without its cache, watcher or hashes: a migration runs from the command line too.
namespace prj::migration
{
	struct indexed_asset
	{
		// Relative to the project root, with '/' ("res/textures/a.png").
		std::string path;
		// None: the file has no .resource, or it could not be read (problems() says why).
		std::optional<res::asset_id> id;
	};

	class project_assets
	{
	public:
		// `content_roots` relative to `root`, as project.snkproj lists them. Nothing is read before rebuild().
		project_assets(std::filesystem::path root, std::vector<std::filesystem::path> content_roots);

		// Walks the content roots again; what was known before is dropped.
		void rebuild();

		// Every file the walk lists (no .resource among them), by path.
		const std::map<std::string, indexed_asset>& assets() const { return m_assets; }
		std::optional<res::asset_id> id_of(std::string_view path) const;
		// The files that carry `id`: more than one is a duplicate the editor reports too.
		std::vector<std::string> paths_of(const res::asset_id& id) const;
		// .resource files without their file, by path, with the ID each holds when it could be read.
		const std::map<std::string, std::optional<res::asset_id>>& orphans() const { return m_orphans; }
		// A .resource that cannot be read, an orphan, a content root that is not walked: one line each.
		const std::vector<std::string>& problems() const { return m_problems; }

	private:
		std::filesystem::path m_root;
		std::vector<std::filesystem::path> m_content_roots;
		std::map<std::string, indexed_asset> m_assets;
		std::map<std::string, std::optional<res::asset_id>> m_orphans;
		std::vector<std::string> m_problems;
	};
}
