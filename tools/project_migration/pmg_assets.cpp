#include <snk/project_migration/pmg_assets.h>
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>
#include <snk/project/prj_manifest.h>
#include <snk/resource/res_asset_index.h>
#include <snk/resource/res_asset_walk.h>
#include <snk/resource/res_system.h>
#include <cstdint>
#include <format>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace
{
	// The real path of `path`; where the file system cannot tell it, the absolute path lexically normalized, so a layer
	// whose real path is unknown is still compared with the project instead of taken for one outside it.
	fs::path comparable(const fs::path& path)
	{
		std::error_code ec;
		fs::path out = fs::weakly_canonical(path, ec);
		if (ec) {
			std::error_code absolute_error;
			out = fs::absolute(path, absolute_error);
			if (absolute_error) out = path;
			out = out.lexically_normal();
		}
		if (!out.has_filename() && out.has_parent_path()) out = out.parent_path();
		return out;
	}
}

namespace
{
	// An immutable copy: resource_system asks it from any thread.
	class project_asset_index final : public res::asset_index
	{
	public:
		explicit project_asset_index(prj::migration::project_assets assets)
			: m_assets(std::move(assets))
		{
		}

		std::optional<res::asset_id> id_of(const res::tag& tag) const override { return m_assets.id_of(tag); }
		std::optional<res::tag> path_of(const res::asset_id& id) const override { return m_assets.path_of(id); }

	private:
		const prj::migration::project_assets m_assets;
	};
}

std::shared_ptr<const res::asset_index> prj::migration::make_asset_index(project_assets assets)
{
	return std::make_shared<const project_asset_index>(std::move(assets));
}

prj::migration::scoped_project_index::scoped_project_index(res::resource_system& resources, const fs::path& root,
	std::vector<fs::path> read_only_layers)
{
	if (resources.has_asset_index()) return;
	if (resources.parent()) {
		egLOG_WARN("project/migrate", "the resource system is a layer: its root gets no project index, type steps that ask "
			"for an asset ID get none");
		return;
	}
	const prj::manifest_result manifest = prj::load_manifest(root);
	if (!manifest.data) return;
	project_assets assets(root, manifest.data->content_roots, std::move(read_only_layers));
	assets.rebuild();
	resources.set_asset_index(make_asset_index(std::move(assets)));
	m_resources = &resources;
}

prj::migration::scoped_project_index::~scoped_project_index()
{
	if (m_resources) m_resources->set_asset_index(nullptr);
}

std::vector<fs::path> prj::migration::read_only_layers(const res::resource_system& resources, const fs::path& root)
{
	const fs::path project = comparable(root);
	std::vector<fs::path> out;
	for (const fs::path& layer : resources.get_resource_layers()) {
		if (!fs_paths::is_inside(project, comparable(layer))) out.push_back(layer);
	}
	return out;
}

prj::migration::project_assets::project_assets(fs::path root, std::vector<fs::path> content_roots, std::vector<fs::path> read_only_layers)
	: m_root(std::move(root))
	, m_content_roots(std::move(content_roots))
	, m_read_only_layers(std::move(read_only_layers))
{
}

void prj::migration::project_assets::rebuild()
{
	m_assets.clear();
	m_orphans.clear();
	m_problems.clear();
	m_held_ids.clear();
	m_holders.clear();
	m_held_below.clear();
	m_read_only_resources.clear();

	// Lowest first, as res:// stacks them: the read-only layers, then the content roots, the first of project.snkproj
	// the highest layer.
	std::vector<fs::path> layers;
	std::vector<std::string> prefixes;
	std::vector<bool> read_only;
	for (const fs::path& layer : m_read_only_layers) {
		std::error_code ec;
		if (!fs::is_directory(fs::symlink_status(layer, ec))) continue;
		layers.push_back(layer);
		prefixes.emplace_back();
		read_only.push_back(true);
	}
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
		read_only.push_back(false);
	}

	const std::vector<res::asset_walk_file> files = res::walk_asset_layers(layers, {}, res::asset_names_case_insensitive);
	const res::asset_walk_pairing pairing = res::pair_asset_metas(files, res::asset_names_case_insensitive);
	// Below a content root "." (no prefix): what the scan leaves out of the project folder, also before project.snkproj
	// exists there and the walk itself would leave it out.
	const auto project_own = [&](const res::asset_walk_file& file) {
		return !read_only[file.layer] && prefixes[file.layer].empty() && res::is_in_project_own_folder(file.relative);
	};
	const auto entry_of = [&](const res::asset_walk_file& file) {
		indexed_asset asset;
		asset.res_path = fs_paths::generic_utf8_of(file.relative);
		asset.path = prefixes[file.layer] + asset.res_path;
		return asset;
	};

	struct claim
	{
		std::string tag;
		std::string res_path;
		std::size_t layer = 0;
		std::int64_t mtime = 0;
	};
	std::map<res::asset_id, claim> holders;
	const auto claim_id = [&holders](const res::asset_id& id, const res::asset_walk_file& file, const std::string& res_path) {
		std::error_code ec;
		const auto time = fs::last_write_time(file.physical, ec);
		claim next{ "res://" + res_path, res_path, file.layer, ec ? 0 : static_cast<std::int64_t>(time.time_since_epoch().count()) };
		const auto [held, added] = holders.try_emplace(id, next);
		if (!added && res::holds_id_before(res::asset_id_claim{ next.layer, next.mtime, next.tag },
				res::asset_id_claim{ held->second.layer, held->second.mtime, held->second.tag })) {
			held->second = std::move(next);
		}
	};

	for (const res::asset_walk_file& file : files) {
		const std::string relative = fs_paths::generic_utf8_of(file.relative);
		if (read_only[file.layer] && res::is_asset_meta_sidecar(relative)) m_read_only_resources.push_back(read_only_resource{ layers[file.layer], relative });
	}
	for (std::size_t i = 0; i < files.size(); ++i) {
		if (project_own(files[i]) || res::is_asset_meta_sidecar(fs_paths::generic_utf8_of(files[i].relative))) continue;
		indexed_asset asset = entry_of(files[i]);
		if (const std::optional<std::size_t> meta = pairing.meta_of[i]) {
			asset.has_meta = true;
			if (!read_only[files[*meta].layer]) asset.meta_path = entry_of(files[*meta]).path;
			const res::asset_meta_file read = res::read_asset_meta_file(layers[files[*meta].layer], files[*meta].relative);
			if (read.meta) {
				asset.id = read.meta->guid;
			} else if (read.present && !read_only[files[*meta].layer]) {
				m_problems.push_back(std::format("{}: {}", entry_of(files[*meta]).path, read.error));
			}
		}
		if (asset.id && !asset.id->is_nil()) claim_id(*asset.id, files[i], asset.res_path);
		if (read_only[files[i].layer]) continue;
		m_assets.insert_or_assign(asset.path, std::move(asset));
	}
	for (const std::size_t orphan : pairing.orphans) {
		if (project_own(files[orphan]) || read_only[files[orphan].layer]) continue;
		indexed_asset meta = entry_of(files[orphan]);
		const res::asset_meta_file read = res::read_asset_meta_file(layers[files[orphan].layer], files[orphan].relative);
		if (read.meta) meta.id = read.meta->guid;
		m_problems.push_back(std::format("{}: a .resource without its file", meta.path));
		m_orphans.insert_or_assign(meta.path, std::move(meta));
	}

	for (auto& [id, held] : holders) {
		m_held_ids.emplace(res::asset_name_key(held.res_path, res::asset_names_case_insensitive), id);
		if (read_only[held.layer]) m_held_below.insert(id);
		m_holders.emplace(id, std::move(held.res_path));
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

std::optional<res::asset_id> prj::migration::project_assets::id_of(const res::tag& tag) const
{
	if (tag.protocol() != res::tag::default_protocol()) return std::nullopt;
	const std::string_view res_path = tag.view().substr(tag.protocol().size() + 3);
	const auto held = m_held_ids.find(res::asset_name_key(res_path, res::asset_names_case_insensitive));
	return held == m_held_ids.end() ? std::nullopt : std::optional{ held->second };
}

std::optional<res::tag> prj::migration::project_assets::path_of(const res::asset_id& id) const
{
	const auto found = m_holders.find(id);
	if (found == m_holders.end()) return std::nullopt;
	return res::tag::make(found->second);
}
