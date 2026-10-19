#include <snk/resource/res_system.h>
#include <snk/resource/adapters/res_pct_adapter.h>
#include <snk/resource/adapters/res_text_adapter.h>
#include <snk/config/cfg_api.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <utility>
#include <vector>

CFG_VAR_DEF_PATH(cfg_res_path, "resource/path", "./res");
// Optional lower layer (engine base resources). Empty = single-root setup.
CFG_VAR_DEF_PATH(cfg_res_base_path, "resource/base_path", "");

res::resource_system::resource_system(ds::task_system &tasks)
    : m_tasks(tasks) {
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

res::resource_system::~resource_system() {
  {
    std::lock_guard lock(m_stop_mutex);
    m_stopping = true;
  }
  m_lifetime.cancel_tasks();
  m_tasks.wait_idle(m_lifetime);
  // The vfs threads are joined only now: the tasks that were running may still store() through them.
  m_resolvers.clear();
}

namespace {
// Owned by the task body: if the body is destroyed without having run, the block it was to settle gets an error.
struct unrun_guard {
  explicit unrun_guard(std::shared_ptr<res::resource_system::resource_handle> b)
      : block(std::move(b)) {}
  ~unrun_guard() {
    if (!ran && block) {
      block->set_error("Load cancelled");
    }
  }
  std::shared_ptr<res::resource_system::resource_handle> block;
  bool ran = false;
};
} // namespace

void res::resource_system::run_on_compute(std::function<void()> cb,
                                          std::shared_ptr<resource_handle> block) {
  std::lock_guard lock(m_stop_mutex);
  if (m_stopping) {
    if (block) {
      block->set_error("Load cancelled");
    }
    return;
  }
  auto guard = std::make_shared<unrun_guard>(std::move(block));
  m_tasks.run(ds::lane::compute, ds::task_desc{"resource load", "resource", false},
              m_lifetime, [cb = std::move(cb), guard](ds::task_context &) {
                guard->ran = true;
                cb();
              });
}

void res::resource_system::signal_changed(const res::tag &tag) {
  std::uint64_t epoch = 0;
  {
    std::unique_lock lock(m_cache_mutex);
    m_cache.erase_tag(tag);
    epoch = ++m_watch_epoch;
  }

  m_tasks.post_main([this, tag, epoch, alive = m_lifetime.token()]() {
    if (!alive.expired()) {
      deliver_to_watchers(tag, epoch);
    }
  });
}

void res::resource_system::deliver_to_watchers(const res::tag &tag,
                                               std::uint64_t signal_epoch) {
  // Only watchers registered before the signal: a load that follows a signal registers its own watcher.
  std::vector<std::pair<void *, std::uint64_t>> targets;
  {
    std::shared_lock lock(m_cache_mutex);
    if (auto it = m_watchers.find(tag); it != m_watchers.end()) {
      for (const auto &[owner, entry] : it->second) {
        if (entry.epoch < signal_epoch) {
          targets.emplace_back(owner, entry.epoch);
        }
      }
    }
  }

  for (const auto &[owner, epoch] : targets) {
    reload_callback callback;
    {
      // A previous callback of this delivery may have unwatched (and destroyed) this owner.
      std::shared_lock lock(m_cache_mutex);
      auto it = m_watchers.find(tag);
      if (it == m_watchers.end()) {
        continue;
      }
      auto entry = it->second.find(owner);
      if (entry == it->second.end() || entry->second.epoch != epoch) {
        continue;
      }
      callback = entry->second.callback;
    }
    callback(tag);
  }
}

void res::resource_system::register_alias(const res::tag &original,
                                          const res::tag &alias) {
  {
    std::unique_lock lock(m_alias_mutex);
    m_aliases[original] = alias;
  }

  {
    std::unique_lock lock(m_cache_mutex);
    m_cache.copy_tag(original, alias, [](const cache_entry &entry) { return !entry.expired(); });
  }

  {
    std::unique_lock lock(m_pinning_mutex);
    m_pinned_resources.copy_tag(original, alias, [](const auto &entry) { return entry != nullptr; });
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

