#include <snk/project_migration/pmg_assets.h>
#include <snk/common/fs_paths.h>
#include <snk/resource/res_asset_walk.h>
#include <format>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

prj::migration::project_assets::project_assets(fs::path root, std::vector<fs::path> content_roots)
	: m_root(std::move(root))
	, m_content_roots(std::move(content_roots))
{
}

void prj::migration::project_assets::rebuild()
{
	m_assets.clear();
	m_orphans.clear();
	m_problems.clear();
	for (const fs::path& content_root : m_content_roots) {
		const fs::path layer = (m_root / content_root).lexically_normal();
		std::string prefix = fs_paths::generic_utf8_of(content_root.lexically_normal());
		while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
		prefix = prefix == "." || prefix.empty() ? std::string{} : prefix + "/";

		std::error_code ec;
		if (!fs::is_directory(fs::symlink_status(layer, ec))) {
			if (fs::exists(fs::symlink_status(layer, ec))) m_problems.push_back(std::format("{}: the content root is not a folder, it is not walked", prefix));
			continue;
		}

		// One content root is one layer of the editor's asset database: the same walk, the same pairing.
		const std::vector<fs::path> layers{ layer };
		const std::vector<res::asset_walk_file> files = res::walk_asset_layers(layers, {}, res::asset_names_case_insensitive);
		const res::asset_walk_pairing pairing = res::pair_asset_metas(files, res::asset_names_case_insensitive);
		for (std::size_t i = 0; i < files.size(); ++i) {
			const std::string relative = fs_paths::generic_utf8_of(files[i].relative);
			if (res::is_asset_meta_sidecar(relative)) continue;
			indexed_asset asset{ prefix + relative, std::nullopt };
			if (const std::optional<std::size_t> meta = pairing.meta_of[i]) {
				const res::asset_meta_file read = res::read_asset_meta_file(layer, files[*meta].relative);
				if (read.meta) {
					asset.id = read.meta->guid;
				} else if (read.present) {
					m_problems.push_back(std::format("{}{}: {}", prefix, fs_paths::generic_utf8_of(files[*meta].relative), read.error));
				}
			}
			m_assets.insert_or_assign(asset.path, std::move(asset));
		}
		for (const std::size_t orphan : pairing.orphans) {
			const std::string path = prefix + fs_paths::generic_utf8_of(files[orphan].relative);
			const res::asset_meta_file read = res::read_asset_meta_file(layer, files[orphan].relative);
			m_orphans.insert_or_assign(path, read.meta ? std::optional<res::asset_id>(read.meta->guid) : std::nullopt);
			m_problems.push_back(std::format("{}: a .resource without its file", path));
		}
	}
}

std::optional<res::asset_id> prj::migration::project_assets::id_of(std::string_view path) const
{
	const auto found = m_assets.find(std::string(path));
	return found == m_assets.end() ? std::nullopt : found->second.id;
}

std::vector<std::string> prj::migration::project_assets::paths_of(const res::asset_id& id) const
{
	std::vector<std::string> paths;
	for (const auto& [path, asset] : m_assets) {
		if (asset.id == id) paths.push_back(path);
	}
	return paths;
}
