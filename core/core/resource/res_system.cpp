#include <snk/resource/res_system.h>
#include <snk/resource/adapters/res_pct_adapter.h>
#include <snk/resource/adapters/res_text_adapter.h>
#include <snk/config/cfg_api.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <utility>
#include <vector>

namespace {
template <class MAP>
void copy_tag_entries(MAP &map, const res::tag &from, const res::tag &to) {
  std::vector<std::pair<res::cache_key, typename MAP::mapped_type>> found;
  for (const auto &[key, value] : map) {
    if (key.tag == from) {
      found.emplace_back(key, value);
    }
  }
  for (auto &[key, value] : found) {
    map[res::cache_key{to, key.type}] = std::move(value);
  }
}
} // namespace

CFG_VAR_DEF_PATH(cfg_res_path, "resource/path", "./res");
// Optional lower layer (engine base resources). Empty = single-root setup.
CFG_VAR_DEF_PATH(cfg_res_base_path, "resource/base_path", "");

res::resource_system::resource_system() {
  egLOG("resource/system", "Resources path swd '{0}', cfg '{1}'",
        fs::absolute("./").string(), cfg_res_path->string());
  ASSERT_MSG(std::filesystem::exists(cfg_res_path),
             "Resources path does not exist!");

  // Mount order: base first, top last (the last mount wins and receives writes).
  std::vector<fs::path> layers = get_resource_layers();
  if (!cfg_res_base_path->empty()) {
    ASSERT_MSG(std::filesystem::exists(cfg_res_base_path),
               "Base resources path does not exist!");
  }

  auto &res_resolver =
      registrate_resolver<vfs_resolver>(tag::default_protocol(), layers);
  res_resolver.set_resource_changed_callback(
      [this](const res::tag &tag) { signal_changed(tag); });
  fs::path memory_path = fs::path{cfg_res_path->string()} /
                         "../sandbox"; // TODO: make configurable
  auto &memory_resolver = registrate_resolver<vfs_resolver>(
      res::tag::memory, std::vector<fs::path>{memory_path});
  memory_resolver.set_resource_changed_callback(
      [this](const res::tag &tag) { signal_changed(tag); });

  registrate_adapter<raw_image_adapter>(res::raw_image_adapter::INFO);
  registrate_adapter<pct_adapter>(res::pct_adapter::INFO);
  registrate_adapter<text_adapter>(res::text_adapter::INFO);
}

void res::resource_system::post_adapter_work(std::function<void()> cb) {
  if (m_adapter_worker)
    m_adapter_worker->post(cb);
}

void res::resource_system::register_alias(const res::tag &original,
                                          const res::tag &alias) {
  {
    std::unique_lock lock(m_alias_mutex);
    m_aliases[original] = alias;
  }

  {
    std::unique_lock lock(m_cache_mutex);
    copy_tag_entries(m_cache, original, alias);
  }

  {
    std::unique_lock lock(m_pinning_mutex);
    copy_tag_entries(m_pinned_resources, original, alias);
  }
}

std::filesystem::path res::resource_system::get_resources_path() {
  return cfg_res_path;
}

std::vector<std::filesystem::path> res::resource_system::get_resource_layers() {
  std::vector<std::filesystem::path> layers;
  if (!cfg_res_base_path->empty())
    layers.push_back(cfg_res_base_path->string());
  layers.push_back(cfg_res_path->string());
  return layers;
}

