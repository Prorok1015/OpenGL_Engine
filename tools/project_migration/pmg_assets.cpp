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

	// Lowest first, as res:// stacks them: the first content root of project.snkproj is the highest layer.
	std::vector<fs::path> layers;
	std::vector<std::string> prefixes;
	for (auto it = m_content_roots.rbegin(); it != m_content_roots.rend(); ++it) {
		const fs::path layer = (m_root / *it).lexically_normal();
		std::string prefix = fs_paths::generic_utf8_of(it->lexically_normal());
		while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
		prefix = prefix == "." || prefix.empty() ? std::string{} : prefix + "/";
		std::error_code ec;
		const fs::file_status status = fs::symlink_status(layer, ec);
		if (!fs::is_directory(status)) {
			if (fs::exists(status)) m_problems.push_back(std::format("{}: the content root is not a folder, it is not walked", prefix));
			continue;
		}
		layers.push_back(layer);
		prefixes.push_back(std::move(prefix));
	}

	const std::vector<res::asset_walk_file> files = res::walk_asset_layers(layers, {}, res::asset_names_case_insensitive);
	const res::asset_walk_pairing pairing = res::pair_asset_metas(files, res::asset_names_case_insensitive);
	const auto entry_of = [&](const res::asset_walk_file& file) {
		indexed_asset asset;
		asset.res_path = fs_paths::generic_utf8_of(file.relative);
		asset.path = prefixes[file.layer] + asset.res_path;
		return asset;
	};
	for (std::size_t i = 0; i < files.size(); ++i) {
		if (res::is_asset_meta_sidecar(fs_paths::generic_utf8_of(files[i].relative))) continue;
		indexed_asset asset = entry_of(files[i]);
		if (const std::optional<std::size_t> meta = pairing.meta_of[i]) {
			asset.has_meta = true;
			const res::asset_meta_file read = res::read_asset_meta_file(layers[files[*meta].layer], files[*meta].relative);
			if (read.meta) {
				asset.id = read.meta->guid;
			} else if (read.present) {
				m_problems.push_back(std::format("{}: {}", entry_of(files[*meta]).path, read.error));
			}
		}
		m_assets.insert_or_assign(asset.path, std::move(asset));
	}
	for (const std::size_t orphan : pairing.orphans) {
		indexed_asset meta = entry_of(files[orphan]);
		const res::asset_meta_file read = res::read_asset_meta_file(layers[files[orphan].layer], files[orphan].relative);
		if (read.meta) meta.id = read.meta->guid;
		m_problems.push_back(std::format("{}: a .resource without its file", meta.path));
		m_orphans.insert_or_assign(meta.path, std::move(meta));
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
