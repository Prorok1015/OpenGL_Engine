#include <snk/resource/res_system.h>
#include <snk/resource/adapters/res_pct_adapter.h>
#include <snk/resource/adapters/res_text_adapter.h>
#include <snk/resource/adapters/res_binary_adapter.h>
#include <snk/config/cfg_variables.h>
#include <snk/project/prj_code.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <snk/resource/res_asset_id.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

CFG_VAR_DEF_PATH(cfg_res_path, "resource/path", "./res");
// Optional lower layer (engine base resources). Empty = single-root setup.
CFG_VAR_DEF_PATH(cfg_res_base_path, "resource/base_path", "");
// Off in a shipped game (#591): nothing on disk is expected to change while it runs.
CFG_VAR_DEF_BOOL(cfg_res_watch_files, "res/watch_files", true);
CFG_VAR_DEF_BOOL(cfg_res_read_only_project, "res/read_only_project", false);

void core::res::detail::then_on_main(const std::shared_ptr<res_resource_block>& block, ds::task_system& tasks,
                                     std::shared_ptr<ds::detail::held_slot> slot) {
  // Both lambdas are compiled here, in the engine: the block and the main queue hold nothing of the caller's binary.
  block->on_settled([tasks = &tasks, slot = std::move(slot)](res_resource_block &settled) {
    if (!settled.data) {
      ds::detail::drop_held(slot);
      return;
    }
    tasks->post_main([slot, data = settled.data]() mutable { ds::detail::call_held(slot, &data); });
  });
}

namespace {
constexpr std::size_t k_max_dependency_depth = 32;

// The key of an asset with an ID (resource_system::node_of): a protocol no mount serves, so no load ever reads it.
constexpr std::string_view k_asset_node_protocol = "asset-id";

res::tag asset_node(const res::asset_id &id) { return res::tag{k_asset_node_protocol, res::to_text(id)}; }

std::optional<res::asset_id> asset_of_node(const res::tag &node) {
  if (node.protocol() != k_asset_node_protocol) {
    return std::nullopt;
  }
  return res::asset_id_from_text(node.relative());
}

// Set while the asset index answers on this thread: it must not call back into resource_system (res_asset_index.h).
thread_local bool t_in_asset_index = false;

struct asset_index_call {
  asset_index_call() {
    ASSERT_MSG(!t_in_asset_index, "the asset index called resource_system: it answers from a snapshot of its own");
    t_in_asset_index = true;
  }
  ~asset_index_call() { t_in_asset_index = false; }
  asset_index_call(const asset_index_call &) = delete;
  asset_index_call &operator=(const asset_index_call &) = delete;
};

std::optional<res::tag> path_in(const res::asset_index &index, const res::asset_id &id) {
  const asset_index_call call;
  return index.path_of(id);
}

// A key as people read it: the file that holds the ID now.
std::string display_name(const res::tag &node, const res::asset_index *index) {
  if (const auto id = asset_of_node(node); id && index) {
    if (const auto path = path_in(*index, *id)) {
      return std::string{path->view()};
    }
  }
  return std::string{node.view()};
}

bool is_finished(const res::resource_system::resource_handle &block) {
  const auto status = block.status.load(std::memory_order_acquire);
  return status == core::res::res_status::ready ||
         status == core::res::res_status::error;
}

// Claims the block for the adapter. False when it has finished meanwhile (a cycle failed it, a load was cancelled).
bool begin_processing(res::resource_system::resource_handle &block) {
  auto status = block.status.load(std::memory_order_acquire);
  while (true) {
    if (status == core::res::res_status::ready ||
        status == core::res::res_status::error) {
      return false;
    }
    if (status == core::res::res_status::processing) {
      return true;
    }
    if (block.status.compare_exchange_weak(status, core::res::res_status::processing)) {
      return true;
    }
  }
}
} // namespace

// Who waits for whom, among the blocks that have not finished. An edge X -> Y is added before X starts waiting for Y,
// and only if that closes no loop: a loop here would be a wait that never ends, however many threads and roots it
// spans. Edges go when Y settles.
struct res::resource_system::wait_graph {
  using block = resource_handle;
  enum class verdict { added, settled, cycle, too_deep };
  struct outcome {
    verdict result = verdict::added;
    // For a cycle: its blocks (each once) and the text "a -> b -> a".
    std::vector<std::shared_ptr<block>> members;
    std::string path;
  };

  outcome add_edge(const std::shared_ptr<block> &from, std::string_view from_name,
                   const std::shared_ptr<block> &to, std::string_view to_name) {
    std::lock_guard lock(m_mutex);
    outcome out;
    if (is_finished(*to)) {
      out.result = verdict::settled;
      return out;
    }
    if (from == to) {
      out.result = verdict::cycle;
      out.members.push_back(from);
      out.path = std::string{from_name} + " -> " + std::string{from_name};
      return out;
    }

    std::vector<const block *> loop;
    if (find_path(to.get(), from.get(), loop)) {
      out.result = verdict::cycle;
      out.path = std::string{from_name};
      for (const block *at : loop) {
        const auto &entry = m_nodes.at(at);
        out.members.push_back(entry.self);
        out.path += " -> " + entry.name;
      }
      return out;
    }

    const std::size_t chain = depth(from.get(), false) + 1 + depth(to.get(), true);
    if (chain > k_max_dependency_depth) {
      out.result = verdict::too_deep;
      return out;
    }

    auto &waiter = m_nodes[from.get()];
    waiter.self = from;
    waiter.name = std::string{from_name};
    auto &awaited = m_nodes[to.get()];
    awaited.self = to;
    awaited.name = std::string{to_name};
    if (std::ranges::find(waiter.waits_on, to.get()) == waiter.waits_on.end()) {
      waiter.waits_on.push_back(to.get());
      awaited.waited_by.push_back(from.get());
    }
    return out;
  }

  void remove_edge(const block *from, const block *to) {
    std::lock_guard lock(m_mutex);
    if (auto it = m_nodes.find(from); it != m_nodes.end()) {
      std::erase(it->second.waits_on, to);
      drop_if_idle(it);
    }
    if (auto it = m_nodes.find(to); it != m_nodes.end()) {
      std::erase(it->second.waited_by, from);
      drop_if_idle(it);
    }
  }

  // Every edge goes, with the blocks the nodes hold (released outside the lock): once the loads stopped no wait
  // settles any more, and an edge left behind would keep both its blocks alive (#657).
  void clear() {
    node_map released;
    {
      std::lock_guard lock(m_mutex);
      released.swap(m_nodes);
    }
  }

  bool empty() const {
    std::lock_guard lock(m_mutex);
    return m_nodes.empty();
  }

  std::size_t size() const {
    std::lock_guard lock(m_mutex);
    return m_nodes.size();
  }

private:
  struct node {
    std::shared_ptr<block> self;
    std::string name;
    std::vector<const block *> waits_on;
    std::vector<const block *> waited_by;
  };
  using node_map = std::unordered_map<const block *, node>;

  void drop_if_idle(node_map::iterator it) {
    if (it->second.waits_on.empty() && it->second.waited_by.empty()) {
      m_nodes.erase(it);
    }
  }

  // The unfinished blocks from `start` to `goal`, both included, by the waits_on edges.
  bool find_path(const block *start, const block *goal, std::vector<const block *> &path) const {
    struct frame {
      const block *at;
      std::size_t next;
    };
    std::unordered_set<const block *> seen{start};
    std::vector<frame> stack{{start, 0}};
    while (!stack.empty()) {
      if (stack.back().at == goal) {
        for (const auto &f : stack) {
          path.push_back(f.at);
        }
        return true;
      }
      const auto it = m_nodes.find(stack.back().at);
      if (it == m_nodes.end() || stack.back().next >= it->second.waits_on.size()) {
        stack.pop_back();
        continue;
      }
      const block *child = it->second.waits_on[stack.back().next++];
      if (!is_finished(*child) && seen.insert(child).second) {
        stack.push_back({child, 0});
      }
    }
    return false;
  }

  // Edges on the longest chain that leaves `at` downwards (toward what it waits for) or upwards (toward its waiters).
  std::size_t depth(const block *at, bool downwards) const {
    std::unordered_map<const block *, std::size_t> memo;
    return depth(at, downwards, memo);
  }

  std::size_t depth(const block *at, bool downwards, std::unordered_map<const block *, std::size_t> &memo) const {
    const auto it = m_nodes.find(at);
    if (it == m_nodes.end()) {
      return 0;
    }
    if (const auto known = memo.find(at); known != memo.end()) {
      return known->second;
    }
    std::size_t best = 0;
    for (const block *other : downwards ? it->second.waits_on : it->second.waited_by) {
      if (is_finished(*other)) {
        continue;
      }
      best = std::max(best, 1 + depth(other, downwards, memo));
      if (best > k_max_dependency_depth) {
        break;
      }
    }
    memo.emplace(at, best);
    return best;
  }

  mutable std::mutex m_mutex;
  node_map m_nodes;
};

struct res::resource_system::held_changes {
  struct protocol_batch {
    std::string protocol;
    std::vector<file_change> changes;
  };

  std::mutex mutex;
  std::size_t holds = 0;
  // From the release of the last hold until deliver_held has handed everything on: new reports queue behind.
  bool delivering = false;
  // First-seen order; `signalled` keeps a tag to one entry.
  std::vector<res::tag> signals;
  std::unordered_set<res::tag> signalled;
  std::vector<protocol_batch> batches;
};

namespace {

// A held change that adds nothing to the last one of its path in the merged batch: the same change again, or a
// modification of a file that change left there (created, modified, renamed to it).
bool coalesces(const std::vector<res::file_change> &merged, const res::file_change &change) {
  const auto last = std::ranges::find_if(merged.rbegin(), merged.rend(), [&](const res::file_change &earlier) {
    return earlier.tag == change.tag || earlier.old_tag == change.tag;
  });
  if (last == merged.rend() || last->tag != change.tag || last->is_directory != change.is_directory) {
    return false;
  }
  if (last->kind == change.kind && last->old_tag == change.old_tag) {
    return true;
  }
  return change.kind == res::change_kind::modified && last->kind != res::change_kind::deleted;
}

} // namespace

res::resource_system::resource_system(ds::task_system &tasks)
    : resource_system(tasks, config_mount_table()) {}

res::resource_system::resource_system(ds::task_system &tasks, mount_table mounts)
    : m_graph(std::make_unique<wait_graph>()), m_tasks(tasks),
      m_mounts(std::move(mounts)), m_held(std::make_unique<held_changes>()) {
  m_gate->self = this;
  for (const auto &mount : m_mounts) {
    register_mount(mount);
  }

  registrate_adapter<raw_image_adapter>(res::raw_image_adapter::INFO);
  registrate_adapter<pct_adapter>(res::pct_adapter::INFO);
  registrate_adapter<text_adapter>(res::text_adapter::INFO);
  registrate_adapter<binary_adapter>(res::binary_adapter::INFO);
}

void res::resource_system::register_mount(const mount &mounted) {
  egLOG("resource/system", "Mount {}:// ({}): {}", mounted.protocol,
        mounted.access == mount_access::read_only ? "read-only" : "read-write",
        mounted.roots.empty() ? std::string{} : mounted.roots.back().string());
  const bool watch_files = watches_files();
  const watch_options watch = watch_files ? mounted.watch : watch_options{};
  auto &resolver = registrate_resolver<vfs_resolver>(mounted.protocol, mounted.roots, mounted.access,
                                                      mounted.writable, watch, mounted.file_operations, watch_files);
  // Everything a resolver reports passes hold_changes first, so nothing that consumes changes sees a held one early.
  resolver.set_resource_changed_callback([this](const res::tag &tag) {
    if (!hold_signal(tag)) {
      signal_changed(tag);
    }
  });
  if (watch.report_changes) {
    {
      std::lock_guard lock(m_waiting_mutex);
      m_reporting_protocols.emplace(mounted.protocol);
    }
    resolver.set_changes_callback([this, protocol = std::string{mounted.protocol}](std::vector<res::file_change> batch) {
      if (!hold_batch(protocol, batch)) {
        post_batch(protocol, std::move(batch));
      }
    });
  }
}

void res::resource_system::post_batch(const std::string &protocol, std::vector<file_change> batch) {
  // Numbered here, as it is made: a batch already queued for the main thread does not take what goes after it. The
  // number and the post go together (post_main only queues), so the queue holds batches in number order whichever
  // thread makes them (the watcher's, the one of an operation, or the one that delivers what a hold kept).
  std::lock_guard order(m_batch_order_mutex);
  const std::uint64_t number = number_batch(batch);
  m_tasks.post_main([this, protocol, batch = std::move(batch), number, alive = m_lifetime.token()]() {
    if (alive.expired()) {
      return;
    }
    deliver_changes(protocol, batch, number);
  });
}

res::change_hold::~change_hold() { release(); }

void res::change_hold::release() noexcept {
  if (resource_system *system = std::exchange(m_system, nullptr)) {
    system->release_hold();
  }
}

res::change_hold res::resource_system::hold_changes() {
  auto &base = root();
  std::lock_guard lock(base.m_held->mutex);
  ++base.m_held->holds;
  return change_hold(base);
}

bool res::resource_system::changes_held() const {
  const auto &base = root();
  std::lock_guard lock(base.m_held->mutex);
  return base.m_held->holds > 0;
}

bool res::resource_system::hold_signal(const res::tag &tag) {
  auto &held = *m_held;
  std::lock_guard lock(held.mutex);
  if (held.holds == 0 && !held.delivering) {
    return false;
  }
  if (held.signalled.insert(tag).second) {
    held.signals.push_back(tag);
  }
  return true;
}

bool res::resource_system::hold_batch(const std::string &protocol, const std::vector<file_change> &batch) {
  auto &held = *m_held;
  std::lock_guard lock(held.mutex);
  if (held.holds == 0 && !held.delivering) {
    return false;
  }
  auto merged = std::ranges::find(held.batches, protocol, &held_changes::protocol_batch::protocol);
  if (merged == held.batches.end()) {
    merged = held.batches.insert(held.batches.end(), held_changes::protocol_batch{protocol, {}});
  }
  for (const auto &change : batch) {
    if (!coalesces(merged->changes, change)) {
      merged->changes.push_back(change);
    }
  }
  return true;
}

void res::resource_system::release_hold() noexcept {
  auto &held = *m_held;
  {
    std::lock_guard lock(held.mutex);
    ASSERT_MSG(held.holds > 0, "change_hold released more often than taken");
    if (held.holds == 0 || --held.holds > 0 || held.delivering) {
      return;
    }
    if (held.signals.empty() && held.batches.empty()) {
      return;
    }
    held.delivering = true;
  }
  if (m_tasks.on_main()) {
    deliver_held();
    return;
  }
  try {
    m_tasks.post_main([this, alive = m_lifetime.token()]() {
      if (alive.expired()) {
        return;
      }
      deliver_held();
    });
  } catch (const std::exception &e) {
    egLOG_ERROR("resource/system", "Posting the changes held back to the main thread failed: {}", e.what());
    std::lock_guard lock(held.mutex);
    held.delivering = false;
  }
}

void res::resource_system::deliver_held() {
  auto &held = *m_held;
  try {
    while (true) {
      std::vector<res::tag> signals;
      std::vector<held_changes::protocol_batch> batches;
      {
        std::lock_guard lock(held.mutex);
        // A hold taken meanwhile keeps the rest for its own release.
        if (held.holds > 0 || (held.signals.empty() && held.batches.empty())) {
          held.delivering = false;
          return;
        }
        signals.swap(held.signals);
        batches.swap(held.batches);
        held.signalled.clear();
      }
      // The order a resolver reports in: the files' signals, then the batch that names them (a gone file waits for
      // its batch number, number_batch).
      for (const auto &tag : signals) {
        signal_changed(tag);
      }
      for (auto &batch : batches) {
        if (!batch.changes.empty()) {
          post_batch(batch.protocol, std::move(batch.changes));
        }
      }
    }
  } catch (const std::exception &e) {
    egLOG_ERROR("resource/system", "Delivering the changes held back failed: {}", e.what());
    std::lock_guard lock(held.mutex);
    held.delivering = false;
  }
}

void res::resource_system::add_mount(mount added) {
  ASSERT_MSG(!m_parent, "A resource layer uses the mounts of its root");
  register_mount(added);
  m_mounts.push_back(std::move(added));
}

bool res::resource_system::remount(std::string_view protocol, std::vector<std::filesystem::path> roots) {
  ASSERT_MSG(!m_parent, "A resource layer uses the mounts of its root");
  ASSERT_MSG(m_tasks.on_main(), "remount: main thread only");
  const auto found = std::ranges::find_if(m_mounts, [protocol](const mount &m) { return m.protocol == protocol; });
  if (found == m_mounts.end()) {
    egLOG_WARN("resource/system", "remount: {}:// is not mounted", protocol);
    return false;
  }
  found->roots = std::move(roots);
  unregistrate_resolver(found->protocol);
  register_mount(*found);
  // What was read from the old roots is not served any more: for the cache the files behind the tags changed.
  std::vector<res::tag> cached;
  collect_cached_tags(protocol, cached);
  std::ranges::sort(cached, [](const res::tag &a, const res::tag &b) { return a.view() < b.view(); });
  const auto repeated = std::ranges::unique(cached, [](const res::tag &a, const res::tag &b) { return a.view() == b.view(); });
  cached.erase(repeated.begin(), repeated.end());
  for (const auto &tag : cached) {
    signal_changed(tag);
  }
  return true;
}

void res::resource_system::collect_cached_tags(std::string_view protocol, std::vector<res::tag> &out) {
  const auto index = current_index();
  {
    std::shared_lock lock(m_cache_mutex);
    m_cache.for_each_tag([&](const res::tag &tag) {
      if (tag.protocol() == protocol) {
        out.push_back(tag);
        return;
      }
      // An ID's key: by the protocol of the file that holds it.
      if (const auto id = asset_of_node(tag); id && index) {
        if (const auto path = path_in(*index, *id); path && path->protocol() == protocol) {
          out.push_back(tag);
        }
      }
    });
  }
  std::lock_guard layers_lock(m_layers_mutex);
  for (resource_system *layer : m_layers) {
    layer->collect_cached_tags(protocol, out);
  }
}

res::resource_system::resource_system(resource_system &parent)
    : m_graph(std::make_unique<wait_graph>()), m_tasks(parent.m_tasks), m_parent(&parent),
      m_held(std::make_unique<held_changes>()) {
  m_gate->self = this;
  std::lock_guard lock(parent.m_layers_mutex);
  parent.m_layers.push_back(this);
}

res::resource_system::~resource_system() {
  ASSERT_MSG(m_layers.empty(), "A resource_system outlives its layers: destroy the layers first");
  if (m_parent) {
    // First: a signal from the root may be walking the layers right now.
    std::lock_guard lock(m_parent->m_layers_mutex);
    std::erase(m_parent->m_layers, this);
  }
  {
    std::lock_guard lock(m_gate->mutex);
    m_gate->self = nullptr;
  }
  // The gate is closed: nothing parked runs any more, and what it holds (adapters, blocks) goes now, not when the root
  // settles what it waited for.
  stop_loading();
  ASSERT_MSG(m_graph->empty(), "a stopped resource_system still has waits in its graph");
  if (m_parent) {
    std::unique_lock lock(m_cache_mutex);
    for (const auto &[dependent, targets] : m_depends_on) {
      for (const auto &target : targets) {
        release_in_parents(target);
      }
    }
  }
  // The vfs threads are joined only now: the tasks that were running may still store() through them.
  m_resolvers.clear();
  if (m_parent) {
    {
      std::unique_lock lock(m_pinning_mutex);
      m_pinned_resources = {};
      m_pin_owners = {};
      m_pin_generations = {};
      m_pins_to_refresh.clear();
    }
    if (const std::size_t live = live_resources(); live > 0) {
      constexpr std::size_t k_named = 8;
      const std::vector<std::string> named = live_resource_tags(k_named);
      std::string list;
      for (const auto &item : named) {
        list += list.empty() ? item : ", " + item;
      }
      const std::string more = live > named.size() ? std::format(" (+{} more)", live - named.size()) : std::string{};
      egLOG_WARN("resource/layer", "{} resources loaded through a destroyed layer are still referenced: {}{}", live, list,
                 more);
    }
  }
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

void res::resource_system::stop_loading() {
  {
    std::lock_guard lock(m_stop_mutex);
    m_stopping = true;
  }
  m_lifetime.refuse_new_tasks();
  m_lifetime.cancel_tasks();
  m_tasks.wait_idle(m_lifetime);
  cancel_parked();
  m_graph->clear();
}

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

std::pair<std::shared_ptr<res::resource_system::resource_handle>, bool>
res::resource_system::find_or_insert_cache(const res::tag &tag, ds::type_id type) {
  const node_candidate candidate = candidate_node(tag);
  std::unique_lock lock(m_cache_mutex);
  auto &slot = m_cache[cache_key{bind_locked(tag, candidate), type}];
  if (auto sp = slot.lock()) {
    return {std::move(sp), false};
  }
  auto created = std::make_shared<resource_handle>();
  slot = created;
  return {std::move(created), true};
}

res::resource_system::adapter res::resource_system::find_adapter(ds::type_id type, std::string_view ext) const {
  auto key = adapter_key{type, std::string(ext)};
  if (m_extensions_with_magic.contains(key)) {
    // Need to check magic number
  }
  auto it = m_adapters.find(key);
  if (it != m_adapters.end()) {
    return it->second;
  }
  return nullptr;
}

res::resource_system::adapter res::resource_system::find_adapter_in_chain(const res::tag &tag,
                                                                          std::span<const ds::type_id> chain,
                                                                          ds::type_id *loaded_as,
                                                                          const resource_system **owner) const {
  for (const ds::type_id type : chain) {
    for (const resource_system *system = this; system; system = system->m_parent) {
      auto found = system->find_adapter(type, tag.extension());
      if (!found) {
        found = system->find_adapter(type, "*");
      }
      if (found) {
        if (loaded_as) {
          *loaded_as = type;
        }
        if (owner) {
          *owner = system;
        }
        return found;
      }
    }
  }
  return nullptr;
}

std::shared_ptr<res::resource_system::resource_handle>
res::resource_system::require_block(const res::tag &tag, std::span<const ds::type_id> chain, bool sync) {
  ds::type_id type = chain.front();
  const resource_system *owner = nullptr;
  auto found = find_adapter_in_chain(tag, chain, &type, &owner);
  if (m_parent && owner != this) {
    // The most specific adapter is a parent's (or there is none): the parent's block, unless the layer
    // pinned one (warmup and pin_resource also put it in the layer's cache, which a change evicts).
    if (auto pinned = find_cached(cache_key{node_of(tag), type})) {
      if (sync) {
        wait_for_block(pinned);
      }
      return pinned;
    }
    return m_parent->require_block(tag, chain, sync);
  }
  if (!found) {
    // Not fatal: a format whose importer is not built in (SNK_WITH_ASSIMP=OFF) ends up here. The block below
    // settles with an error, so whoever waits for it fails instead of the process.
    egLOG_ERROR("res/adapter",
                "No adapter found for '{}' (extension '{}'): the importer for this format is not registered or not built in",
                tag.view(), tag.extension());
  }

  auto [block, is_new] = find_or_insert_cache(tag, type);
  if (is_new) {
    block->diagnostics.set_resource(std::string{tag.view()});
    start_loading(tag, block, std::move(found));
  }
  if (sync) {
    wait_for_block(block);
  }
  return block;
}

std::shared_ptr<res::resource_system::resource_handle>
res::resource_system::refused_block(const res::tag &tag, std::string reason) {
  egLOG_WARN("resource/require", "{}", reason);
  auto block = std::make_shared<resource_handle>();
  block->diagnostics.set_resource(std::string{tag.view()});
  block->set_error(std::move(reason));
  return block;
}

void res::resource_system::start_loading(const res::tag &tag,
                                         const std::shared_ptr<resource_handle> &block,
                                         adapter ad) {
  // The cache slot already holds `block`: any escape from here would leave waiters blocked forever.
  try {
    const auto &resolvers = root().m_resolvers;
    auto resolver_it = resolvers.find(tag.protocol());
    if (resolver_it == resolvers.end()) {
      std::string error = not_mounted_error(tag);
      egLOG_ERROR("res/resolver", "cannot load {}: {}", tag.view(), error);
      block->set_error(std::move(error));
      return;
    }

    auto raw = resolver_it->second->resolve(tag);
    if (!raw) {
      egLOG_ERROR("res/resolver", "Resolver returned no data block for {}", tag.view());
      block->set_error("Resolver returned no data");
      return;
    }

    // A resolver thread of the root settles `raw`, possibly after this layer is gone: it holds only the id.
    const auto id = park(block, [this, tag, block, raw, ad = std::move(ad)] {
      run_on_compute([this, tag, block, raw, ad] { collect_stage(tag, block, raw, ad); }, block);
    });
    raw->on_settled([gate = m_gate, id](auto &) { run_parked(gate, id); });
  } catch (const std::exception &e) {
    egLOG_ERROR("res/resolver", "Failed to start loading {0}: {1}", tag.view(), e.what());
    block->set_error(std::string{"Load error: "} + e.what());
  } catch (...) {
    egLOG_ERROR("res/resolver", "Failed to start loading {}: unknown exception", tag.view());
    block->set_error("Load error: unknown exception");
  }
}

void res::resource_system::collect_stage(const res::tag &tag,
                                         const std::shared_ptr<resource_handle> &block,
                                         const data &raw, const adapter &ad) {
  if (raw->has_error()) {
    block->set_error(std::string{"Resolver error: "} + raw->error_msg);
    egLOG_ERROR("res/resolver", "Failed to resolve resource {0}: {1}", tag.view(), raw->error_msg);
    return;
  }

  try {
    if (!ad) {
      throw std::runtime_error("No adapter found for resource type");
    }
    if (!begin_processing(*block)) {
      return;
    }

    auto declared = ad->collect_dependencies(tag, raw->data);
    // Before the requests: a change of a dependency that lands after them must still find this resource.
    record_dependencies(tag, block, declared);
    if (declared.empty()) {
      deserialize_stage(tag, block, raw, ad, res_deps{});
      return;
    }

    std::vector<std::pair<std::shared_ptr<resource_handle>, res::tag>> blocks;
    std::vector<dependency_role> roles;
    blocks.reserve(declared.size());
    roles.reserve(declared.size());
    for (const auto &dep : declared) {
      if (!dep.request) {
        throw std::runtime_error("dependency '" + std::string{dep.target.view()} + "' was not made with make_dep");
      }
      blocks.emplace_back(dep.request(*this, dep.target), dep.target);
      block->diagnostics.add_child(blocks.back().first);
      roles.push_back(dependency_role{dep.failure_kind, dep.json_path});
    }

    // The waiting runs nothing: the dependencies load on their own tasks, and this block goes on once they are done.
    await_blocks(block, tag, blocks,
                 [this, tag, block, raw, ad, declared = std::move(declared), blocks]() {
                   run_on_compute(
                       [this, tag, block, raw, ad, declared, blocks] {
                         res_deps ready;
                         for (std::size_t i = 0; i < declared.size(); ++i) {
                           ready.add(declared[i], blocks[i].first->data);
                         }
                         deserialize_stage(tag, block, raw, ad, ready);
                       },
                       block);
                 },
                 std::move(roles));
  } catch (const res::load_error &e) {
    res::add_while_unsettled(*block, e.what_diagnostic());
    block->set_error(std::string{"Adapter error: "} + e.what());
    egLOG_ERROR("res/adapter", "Failed to collect dependencies of {0}: {1}", tag.view(), e.what());
  } catch (const std::exception &e) {
    block->set_error(std::string{"Adapter error: "} + e.what());
    egLOG_ERROR("res/adapter", "Failed to collect dependencies of {0}: {1}", tag.view(), e.what());
  } catch (...) {
    block->set_error("Adapter error: unknown exception");
    egLOG_ERROR("res/adapter", "Failed to collect dependencies of {}: unknown exception", tag.view());
  }
}

void res::resource_system::deserialize_stage(const res::tag &tag,
                                             const std::shared_ptr<resource_handle> &block,
                                             const data &raw, const adapter &ad,
                                             const res_deps &deps) {
  if (!begin_processing(*block)) {
    return;
  }
  try {
    const res::load_scope scope(block);
    block->set_ready(ad->deserialize(tag, raw->data, deps));
  } catch (const res::load_error &e) {
    res::add_while_unsettled(*block, e.what_diagnostic());
    block->set_error(std::string{"Adapter error: "} + e.what());
    egLOG_ERROR("res/adapter", "Failed to deserialize resource {0}: {1}", tag.view(), e.what());
  } catch (const std::exception &e) {
    block->set_error(std::string{"Adapter error: "} + e.what());
    egLOG_ERROR("res/adapter", "Failed to deserialize resource {0}: {1}", tag.view(), e.what());
  } catch (...) {
    block->set_error("Adapter error: unknown exception");
    egLOG_ERROR("res/adapter", "Failed to deserialize resource {}: unknown exception", tag.view());
  }
}

struct res::resource_system::await_state {
  std::shared_ptr<resource_handle> owner;
  std::vector<std::pair<std::shared_ptr<resource_handle>, res::tag>> blocks;
  std::function<void()> on_done;
  std::atomic<std::size_t> pending{0};
  std::mutex mutex;
  std::string failure; // the first failed dependency
  std::size_t failed_index = 0;
  // What the failed dependency recorded, with its resource: nobody keeps a failed dependency, so the owner carries it.
  std::vector<res::diagnostic> failed_diagnostics;
  std::vector<dependency_role> roles; // empty, or one per block
};

namespace {
// The diagnostics a settled block recorded, each with its resource; a failure it did not describe as kind `load`.
std::vector<res::diagnostic> own_diagnostics(const core::res::res_control_block_base &block) {
  auto snapshot = block.diagnostics.read();
  bool error_recorded = false;
  for (auto &item : snapshot.items) {
    error_recorded = error_recorded || item.severity == res::diagnostic_severity::error;
    if (item.resource.empty()) {
      item.resource = snapshot.resource;
    }
  }
  if (block.status.load(std::memory_order_acquire) == core::res::res_status::error && !error_recorded) {
    snapshot.items.push_back(res::diagnostic{res::diagnostic_severity::error, std::string{res::diagnostic_kind::load},
                                             snapshot.resource, {}, block.error_msg});
  }
  return std::move(snapshot.items);
}
} // namespace

void res::resource_system::await_blocks(
    const std::shared_ptr<resource_handle> &owner, const res::tag &owner_tag,
    std::vector<std::pair<std::shared_ptr<resource_handle>, res::tag>> blocks,
    std::function<void()> on_done, std::vector<dependency_role> roles) {
  if (blocks.empty()) {
    on_done();
    return;
  }

  auto state = std::make_shared<await_state>();
  state->owner = owner;
  state->blocks = std::move(blocks);
  state->on_done = std::move(on_done);
  state->pending = state->blocks.size();
  state->roles = std::move(roles);
  // Blocks that have all finished settle the wait at once (their edges go as they are added), so it is not refused.
  const bool waits_for_any = std::ranges::any_of(state->blocks, [](const auto &entry) {
    return entry.first && !is_finished(*entry.first);
  });
  bool stopping = false;
  std::uint64_t id = 0;
  {
    // Under the lock stop_loading sets m_stopping with: it either sees this wait in cancel_parked or we see it stopping.
    std::lock_guard lock(m_stop_mutex);
    stopping = m_stopping && waits_for_any;
    if (!stopping) {
      // The dependencies (a parent's blocks too) hold only this id, never the state with its continuation.
      id = register_await(state);
    }
  }
  if (stopping) {
    // Outside the lock: failing the owner runs its callbacks, which may take m_stop_mutex (run_on_compute).
    if (!is_finished(*owner)) {
      owner->set_error("Load cancelled");
    }
    return;
  }

  const auto fail_owner = [](const std::shared_ptr<resource_handle> &block, const std::string &message) {
    if (!is_finished(*block)) {
      block->set_error(message);
    }
  };
  // Recorded before the block settles (add_while_unsettled), so whoever sees the failure sees its diagnostic too.
  const auto describe = [](const std::shared_ptr<resource_handle> &block, std::string_view kind, std::string json_path,
                           const std::string &message) {
    res::add_while_unsettled(*block, res::diagnostic{res::diagnostic_severity::error,
                                                     std::string{kind.empty() ? res::diagnostic_kind::load : kind}, {},
                                                     std::move(json_path), message});
  };
  const auto role_of = [](const await_state &s, std::size_t i) {
    return i < s.roles.size() ? s.roles[i] : dependency_role{};
  };
  // The owner may have failed meanwhile (a cycle), and then nobody is left to continue. The last one drops the wait.
  const auto one_settled = [fail_owner, describe, role_of, id](resource_system &self, const std::shared_ptr<await_state> &s) {
    if (--s->pending != 0) {
      return;
    }
    self.drop_await(id);
    if (is_finished(*s->owner)) {
      return;
    }
    std::string failure;
    std::size_t failed_index = 0;
    std::vector<res::diagnostic> carried;
    {
      std::lock_guard lock(s->mutex);
      failure = s->failure;
      failed_index = s->failed_index;
      carried = s->failed_diagnostics;
    }
    if (failure.empty()) {
      s->on_done();
    } else {
      const dependency_role role = role_of(*s, failed_index);
      describe(s->owner, role.kind, role.json_path, failure);
      for (auto &item : carried) {
        res::add_while_unsettled(*s->owner, std::move(item));
      }
      fail_owner(s->owner, failure);
    }
  };

  for (std::size_t i = 0; i < state->blocks.size(); ++i) {
    const auto &[dep, dep_tag] = state->blocks[i];
    if (!dep) {
      {
        std::lock_guard lock(state->mutex);
        if (state->failure.empty()) {
          state->failure = "Dependency failed: invalid handle";
          state->failed_index = i;
        }
      }
      one_settled(*this, state);
      continue;
    }

    auto added = m_graph->add_edge(owner, owner_tag.view(), dep, dep_tag.view());
    if (added.result == wait_graph::verdict::cycle) {
      const std::string message = "dependency cycle: " + added.path;
      egLOG_ERROR("res/graph", "{}", message);
      // Every member waits on the next one through the same kind of edge (a desc: its __parent). Each member carries
      // the entry of every other one too: nobody keeps a failed member alive for a walk to reach it.
      const dependency_role role = role_of(*state, i);
      std::vector<std::string> resources;
      resources.reserve(added.members.size());
      for (const auto &member : added.members) {
        resources.push_back(member->diagnostics.read().resource);
      }
      for (const auto &member : added.members) {
        for (const auto &resource : resources) {
          res::add_while_unsettled(*member, res::diagnostic{res::diagnostic_severity::error,
                                                            std::string{res::diagnostic_kind::cycle}, resource,
                                                            role.json_path, message});
        }
        fail_owner(member, message);
      }
      drop_await(id);
      return;
    }
    if (added.result == wait_graph::verdict::too_deep) {
      const std::string message = "dependency chain of '" + std::string{owner_tag.view()} + "' is deeper than " +
                                  std::to_string(k_max_dependency_depth);
      egLOG_ERROR("res/graph", "{}", message);
      describe(owner, res::diagnostic_kind::load, role_of(*state, i).json_path, message);
      fail_owner(owner, message);
      drop_await(id);
      return;
    }

    // A dependency may be the parent's block, which settles after this layer is gone.
    dep->on_settled([gate = m_gate, id, i, one_settled](auto &settled) {
      gate->with([&](resource_system &self) {
        const auto s = self.find_await(id);
        if (!s) {
          return;
        }
        self.m_graph->remove_edge(s->owner.get(), &settled);
        if (settled.has_error()) {
          auto carried = own_diagnostics(settled);
          std::lock_guard lock(s->mutex);
          if (s->failure.empty()) {
            s->failure = "Dependency failed: " + std::string{s->blocks[i].second.view()} + ": " + settled.error_msg;
            s->failed_index = i;
            s->failed_diagnostics = std::move(carried);
          }
        }
        one_settled(self, s);
      });
    });
  }
}

std::size_t res::resource_system::wait_graph_nodes() const {
  return m_graph->size();
}

res::res_handle<res::when_all_result> res::resource_system::when_all_blocks(
    std::vector<std::pair<std::shared_ptr<resource_handle>, res::tag>> blocks) {
  auto joined = std::make_shared<resource_handle>();
  await_blocks(joined, res::tag{}, std::move(blocks),
               [joined] { joined->set_ready(std::make_shared<when_all_result>()); });
  return res_handle<when_all_result>(joined, res::tag{}, &m_tasks);
}

void res::resource_system::wait_for_block(const std::shared_ptr<resource_handle> &block) const {
  if (is_finished(*block)) {
    return;
  }
  if (m_tasks.mode() == ds::policy::manual) {
    // Nothing runs in the background, so run the queued loads here. When none is queued the block may still be
    // waiting for a resolver thread, whose completion queues the next one.
    while (!is_finished(*block)) {
      if (!m_tasks.run_one(ds::lane::compute) && !m_tasks.run_one(ds::lane::io)) {
        block->wait_for(std::chrono::milliseconds(1));
      }
    }
    return;
  }
  block->wait();
}

void res::resource_system::record_dependencies(const res::tag &requested,
                                               const std::shared_ptr<resource_handle> &block,
                                               const std::vector<res_dep> &declared) {
  // The keys the dependencies' own requests use (their bindings), read before the lock.
  std::vector<node_candidate> candidates;
  candidates.reserve(declared.size());
  for (const auto &dep : declared) {
    candidates.push_back(candidate_node(dep.target));
  }
  if (declared.empty()) {
    // The common case: nothing to record, and nothing recorded by an earlier load to drop.
    std::shared_lock lock(m_cache_mutex);
    if (!m_depends_on.contains(node_locked(requested))) {
      return;
    }
  }

  std::unique_lock lock(m_cache_mutex);
  // The key the block is cached under: the binding its request made (a rebind since then moved the block with it).
  const res::tag dependent = node_locked(requested);
  const auto is_block = [&block](const cache_entry &entry) { return entry.lock() == block; };
  const auto displaced = m_displaced.find(dependent);
  if (!m_cache.any_of_tag(dependent, is_block) &&
      (displaced == m_displaced.end() || std::ranges::none_of(displaced->second, is_block))) {
    return;
  }
  if (m_depends_on.size() >= m_edge_sweep_at) {
    sweep_edges();
  }
  if (const auto old = m_depends_on.find(dependent); old != m_depends_on.end()) {
    for (const auto &target : old->second) {
      if (const auto it = m_dependents.find(target); it != m_dependents.end()) {
        std::erase(it->second, dependent);
        if (it->second.empty()) {
          m_dependents.erase(it);
        }
      }
      release_in_parents(target);
    }
    m_depends_on.erase(old);
  }

  std::vector<res::tag> targets;
  targets.reserve(declared.size());
  for (std::size_t i = 0; i < declared.size(); ++i) {
    const res::tag target = bind_locked(declared[i].target, candidates[i]);
    // A resource that names itself fails on its own; it needs no edge to be reloaded with itself.
    if (target != dependent && std::ranges::find(targets, target) == targets.end()) {
      targets.push_back(target);
      m_dependents[target].push_back(dependent);
      retain_in_parents(target);
    }
  }
  if (!targets.empty()) {
    m_depends_on[dependent] = std::move(targets);
  }
}

void res::resource_system::sweep_edges() {
  prune_released_edges();
  m_signals_since_sweep = 0;
  ++m_edge_sweeps;
  m_edge_sweep_at = std::max(k_edge_sweep_floor, m_depends_on.size() * 2);
}

void res::resource_system::prune_released_edges() {
  // Released: the tag has no live cache entry. Either every entry expired, or a signal evicted it (#326) and nobody
  // watches it: a watched tag evicted by a signal keeps its edges, because its watcher is told about the next change
  // whether or not the tag has been loaded again. An opt-in watcher also keeps edges after the last handle expires,
  // so a GPU-resident resource can follow dependency changes without keeping decoded data. A load in flight has an
  // entry, and a tag that loads again records its edges anew, so nothing is lost.
  // Edges sit under the key the block is cached by (the alias target); the watcher may have used the original tag.
  // Lock order: m_cache_mutex first, m_alias_mutex nested inside it (as in signal_changed); never the reverse.
  std::unordered_multimap<res::tag, res::tag> originals_by_alias;
  {
    std::shared_lock alias_lock(m_alias_mutex);
    for (const auto &[original, alias] : m_aliases) {
      originals_by_alias.emplace(node_locked(alias), original);
    }
  }
  // A displaced block whose holders let go is gone for good.
  for (auto it = m_displaced.begin(); it != m_displaced.end();) {
    std::erase_if(it->second, [](const cache_entry &entry) { return entry.expired(); });
    it = it->second.empty() ? m_displaced.erase(it) : std::next(it);
  }
  const auto is_released = [this, &originals_by_alias](const res::tag &tag) {
    const bool live = m_cache.any_of_tag(tag, [](const cache_entry &entry) { return !entry.expired(); });
    if (live || m_displaced.contains(tag)) {
      return false;
    }
    if (needed_by_layers(tag)) {
      return false;
    }
    const bool expired_entry = m_cache.any_of_tag(tag, [](const cache_entry &) { return true; });
    const auto has_watchers = [this, expired_entry](const res::tag &key) {
      const auto it = m_watchers.find(key);
      return it != m_watchers.end() && std::ranges::any_of(it->second, [expired_entry](const auto &entry) {
        return !expired_entry || entry.second.keep_dependencies;
      });
    };
    if (has_watchers(tag)) {
      return false;
    }
    const auto [first, last] = originals_by_alias.equal_range(tag);
    return std::none_of(first, last, [&](const auto &pair) { return has_watchers(pair.second); });
  };
  std::unordered_set<res::tag> released;
  for (const auto &[tag, targets] : m_depends_on) {
    if (is_released(tag)) {
      released.insert(tag);
    }
  }
  if (released.empty()) {
    return;
  }

  // A released tag that a tag still in use is built through keeps its edges: a change at the bottom of the chain has
  // to pass through it. Walk down from every tag that is not released.
  std::unordered_set<res::tag> needed;
  std::vector<res::tag> pending;
  for (const auto &[tag, targets] : m_depends_on) {
    if (!released.contains(tag)) {
      pending.insert(pending.end(), targets.begin(), targets.end());
    }
  }
  while (!pending.empty()) {
    const res::tag next = std::move(pending.back());
    pending.pop_back();
    if (!released.contains(next) || !needed.insert(next).second) {
      continue;
    }
    const auto edges = m_depends_on.find(next);
    pending.insert(pending.end(), edges->second.begin(), edges->second.end());
  }

  for (const auto &tag : released) {
    if (needed.contains(tag)) {
      continue;
    }
    const auto it = m_depends_on.find(tag);
    for (const auto &target : it->second) {
      if (const auto dependents = m_dependents.find(target); dependents != m_dependents.end()) {
        std::erase(dependents->second, tag);
        if (dependents->second.empty()) {
          m_dependents.erase(dependents);
        }
      }
      release_in_parents(target);
    }
    m_depends_on.erase(it);
  }
}

std::vector<res::tag> res::resource_system::changed_batch(const res::tag &root) const {
  // Breadth first over the reverse edges; `seen` is what keeps a cycle from looping.
  // An alias edge (original -> alias) is followed like a recorded one but kept apart from them: pruning and the
  // reload of a tag rewrite the recorded ones, and must not drop it.
  // `tag` by value: the callers pass an element of the vector that `visit` appends to, and the alias lookup runs
  // after the visits, so a reference would dangle once that vector grows.
  const auto for_each_dependent = [this](const res::tag tag, const auto &visit) {
    if (const auto it = m_dependents.find(tag); it != m_dependents.end()) {
      for (const auto &dependent : it->second) {
        visit(dependent);
      }
    }
    if (const auto alias = m_alias_edges.find(tag); alias != m_alias_edges.end()) {
      visit(alias->second);
    }
  };

  std::vector<res::tag> reached{root};
  std::unordered_set<res::tag> seen{root};
  for (std::size_t at = 0; at < reached.size(); ++at) {
    for_each_dependent(reached[at], [&](const res::tag &dependent) {
      if (seen.insert(dependent).second) {
        reached.push_back(dependent);
      }
    });
  }
  if (reached.size() == 1) {
    return reached;
  }

  // Parents first (Kahn): a tag goes out once every dependency of it that is in the batch has. The members of a
  // cycle never get there and follow in the order they were reached.
  std::unordered_map<res::tag, std::size_t> waiting;
  waiting.reserve(reached.size());
  for (const auto &member : reached) {
    waiting.emplace(member, 0);
  }
  // Counted over the same edges the loop below releases, so alias edges balance too.
  for (const auto &member : reached) {
    for_each_dependent(member, [&](const res::tag &dependent) { ++waiting.at(dependent); });
  }

  std::vector<res::tag> ordered;
  ordered.reserve(reached.size());
  for (const auto &member : reached) {
    if (waiting.at(member) == 0) {
      ordered.push_back(member);
    }
  }
  for (std::size_t at = 0; at < ordered.size(); ++at) {
    for_each_dependent(ordered[at], [&](const res::tag &dependent) {
      if (--waiting.at(dependent) == 0) {
        ordered.push_back(dependent);
      }
    });
  }

  if (ordered.size() != reached.size()) {
    const std::unordered_set<res::tag> done(ordered.begin(), ordered.end());
    for (const auto &member : reached) {
      if (!done.contains(member)) {
        ordered.push_back(member);
      }
    }
  }
  return ordered;
}

void res::resource_system::signal_changed(const res::tag &tag) {
  if (m_parent) {
    root().signal_changed(tag);
    return;
  }
  drop_held_signal(tag);
  // Under its key first; what sits under the tag itself (a duplicate, an entry made before the index knew the file)
  // goes as before.
  const std::uint64_t generation = m_index_generation.load(std::memory_order_acquire);
  if (const res::tag node = node_of(tag); node != tag && !wait_for_index(tag, node, generation, false)) {
    signal_changed_local(node);
  }
  signal_changed_local(tag);
}

std::shared_ptr<const res::asset_index> res::resource_system::current_index() const {
  if (m_parent) {
    return root().current_index();
  }
  std::shared_lock lock(m_index_mutex);
  return m_index;
}

res::tag res::resource_system::index_node(const res::tag &tag, const asset_index *index) {
  if (!index || asset_of_node(tag)) {
    return tag;
  }
  std::optional<res::asset_id> id;
  {
    const asset_index_call call;
    id = index->id_of(tag);
  }
  return id && !id->is_nil() ? asset_node(*id) : tag;
}

bool res::resource_system::keyed_by_tag_locked(const res::tag &tag) const {
  return m_cache.any_of_tag(tag, [](const cache_entry &) { return true; }) || m_displaced.contains(tag) ||
         m_watchers.contains(tag) || m_depends_on.contains(tag) || m_dependents.contains(tag);
}

res::tag res::resource_system::node_locked(const res::tag &tag) const {
  if (const auto bound = m_bindings.find(tag); bound != m_bindings.end()) {
    return bound->second;
  }
  if (keyed_by_tag_locked(tag)) {
    return tag;
  }
  return index_node(tag, current_index().get());
}

res::tag res::resource_system::node_of(const res::tag &tag) const {
  {
    std::shared_lock lock(m_cache_mutex);
    if (const auto bound = m_bindings.find(tag); bound != m_bindings.end()) {
      return bound->second;
    }
    if (keyed_by_tag_locked(tag)) {
      return tag;
    }
  }
  return index_node(tag, current_index().get());
}

res::resource_system::node_candidate res::resource_system::candidate_node(const res::tag &tag) const {
  const std::uint64_t epoch = root().m_rebind_epoch.load(std::memory_order_acquire);
  return node_candidate{node_of(tag), epoch};
}

res::tag res::resource_system::bind_locked(const res::tag &tag, const node_candidate &candidate) {
  if (const auto bound = m_bindings.find(tag); bound != m_bindings.end()) {
    return bound->second;
  }
  if (keyed_by_tag_locked(tag)) {
    return tag;
  }
  // A rebind between the candidate and the lock may have changed the answer: asked again, under the lock (rare).
  res::tag node = candidate.epoch == root().m_rebind_epoch.load(std::memory_order_acquire)
                      ? candidate.node
                      : index_node(tag, current_index().get());
  if (asset_of_node(node)) {
    m_bindings.emplace(tag, node);
  }
  return node;
}

bool res::resource_system::wait_for_index(const res::tag &tag, const res::tag &node, std::uint64_t generation,
                                          bool forget) {
  // Still on disk: its content changed, its key's resource goes now. Gone: the index owner's batch says whether it moved.
  if (exists(tag)) {
    return false;
  }
  std::lock_guard lock(m_waiting_mutex);
  // The index changed since `node` was read: no batch of the old one will decide it.
  if (generation != m_index_generation.load(std::memory_order_acquire)) {
    return false;
  }
  auto &entry = m_waiting_for_index[tag];
  entry.node = node;
  entry.forget = entry.forget || forget;
  entry.confirmed = true;
  return true;
}

bool res::resource_system::expect_gone(const res::tag &tag, const res::tag &node, std::uint64_t generation) {
  std::lock_guard lock(m_waiting_mutex);
  if (generation != m_index_generation.load(std::memory_order_acquire)) {
    return false;
  }
  // A file already waiting (gone from disk, perhaps numbered) keeps its entry, node and number.
  auto [entry, inserted] = m_waiting_for_index.try_emplace(tag);
  if (inserted) {
    entry->second.node = node;
  }
  ++entry->second.operations;
  return true;
}

void res::resource_system::finish_expected(const res::tag &tag, bool done) {
  std::lock_guard lock(m_waiting_mutex);
  const auto waiting = m_waiting_for_index.find(tag);
  if (waiting == m_waiting_for_index.end()) {
    return;
  }
  auto &entry = waiting->second;
  if (entry.operations > 0) {
    --entry.operations;
  }
  if (done) {
    entry.confirmed = true;
    entry.forget = true;
  } else if (entry.operations == 0 && !entry.confirmed) {
    // Even when a batch has numbered it meanwhile (a mount that reports nothing): nothing went.
    m_waiting_for_index.erase(waiting);
  }
}

std::uint64_t res::resource_system::number_batch(const std::vector<file_change> &batch) {
  std::lock_guard lock(m_waiting_mutex);
  const std::uint64_t number = ++m_created_batches;
  for (const auto &change : batch) {
    if (change.is_directory) {
      continue;
    }
    const res::tag *gone = change.kind == change_kind::deleted   ? &change.tag
                           : change.kind == change_kind::renamed ? &change.old_tag
                                                                 : nullptr;
    if (!gone) {
      continue;
    }
    if (const auto waiting = m_waiting_for_index.find(*gone);
        waiting != m_waiting_for_index.end() && waiting->second.batch == 0) {
      waiting->second.batch = number;
    }
  }
  // A file of a mount that reports nothing is never named: it waits for the first batch made after it went.
  for (auto &[tag, waiting] : m_waiting_for_index) {
    if (waiting.batch == 0 && !m_reporting_protocols.contains(std::string{tag.protocol()})) {
      waiting.batch = number;
    }
  }
  return number;
}

std::unordered_map<res::tag, res::tag> res::resource_system::alias_value_nodes() const {
  std::vector<res::tag> aliases;
  {
    std::shared_lock lock(m_alias_mutex);
    aliases.reserve(m_aliases.size());
    for (const auto &[original, alias] : m_aliases) {
      aliases.push_back(alias);
    }
  }
  std::unordered_map<res::tag, res::tag> nodes;
  for (const auto &alias : aliases) {
    if (!nodes.contains(alias)) {
      nodes.emplace(alias, node_of(alias));
    }
  }
  return nodes;
}

void res::resource_system::evict_waiting(std::vector<std::pair<res::tag, waiting_entry>> entries,
                                         const std::unordered_map<res::tag, res::tag> &remap) {
  for (const auto &[tag, entry] : entries) {
    // Removed through this system and still gone: forgotten like erase(). Back on disk (removed and made again before
    // the batch was decided, or a lower root holds the same path and the tag now serves that file), its pins and
    // aliases stay.
    if (entry.forget && !exists(tag)) {
      forget_erased(tag);
    }
    const auto moved = remap.find(entry.node);
    signal_changed_local(moved == remap.end() ? entry.node : moved->second);
  }
}

void res::resource_system::set_asset_index(std::shared_ptr<const asset_index> index) {
  ASSERT_MSG(!m_parent, "set_asset_index: the root only, its layers use its index");
  ASSERT_MSG(m_tasks.on_main(), "set_asset_index: main thread only");
  {
    std::unique_lock lock(m_index_mutex);
    m_index = std::move(index);
  }
  std::vector<std::pair<res::tag, waiting_entry>> flushed;
  {
    // No batch of the old index decides what waits for one: evicted now. A gone file signalled meanwhile sees the new
    // generation and is evicted at once.
    std::lock_guard lock(m_waiting_mutex);
    for (auto &entry : m_waiting_for_index) {
      flushed.emplace_back(entry.first, std::move(entry.second));
    }
    m_waiting_for_index.clear();
    m_index_generation.fetch_add(1, std::memory_order_acq_rel);
  }
  m_rebind_epoch.fetch_add(1, std::memory_order_acq_rel);
  const auto remap = rebind_all();
  evict_waiting(std::move(flushed), remap);
  refresh_pins();
}

std::uint64_t res::resource_system::delivering_batch() const {
  return root().m_delivering_batch;
}

void res::resource_system::apply_asset_batch(std::uint64_t through,
                                             std::span<const std::pair<res::tag, res::tag>> moved) {
  ASSERT_MSG(!m_parent, "apply_asset_batch: the root only, its layers use its index");
  ASSERT_MSG(m_tasks.on_main(), "apply_asset_batch: main thread only");
  std::vector<std::pair<res::tag, waiting_entry>> decided;
  {
    // Only what came in a batch the owner has taken in: a later one may still pair its file with a new one.
    std::lock_guard lock(m_waiting_mutex);
    for (auto it = m_waiting_for_index.begin(); it != m_waiting_for_index.end();) {
      // An entry only an operation still running expects is decided once that operation is over.
      if (it->second.batch != 0 && it->second.batch <= through && it->second.confirmed) {
        decided.emplace_back(it->first, std::move(it->second));
        it = m_waiting_for_index.erase(it);
      } else {
        ++it;
      }
    }
  }
  const auto index = current_index();
  std::vector<std::pair<res::tag, waiting_entry>> evicted;
  for (auto &[tag, entry] : decided) {
    const auto move = std::ranges::find_if(moved, [&tag](const auto &pair) { return pair.first == tag; });
    if (move != moved.end() && index_node(move->second, index.get()) == entry.node) {
      follow_move(entry.node, tag, move->second);
    } else {
      evicted.emplace_back(tag, std::move(entry));
    }
  }
  m_rebind_epoch.fetch_add(1, std::memory_order_acq_rel);
  const auto remap = rebind_all();
  evict_waiting(std::move(evicted), remap);
  refresh_pins();
}

std::size_t res::resource_system::waiting_for_index_count() const {
  if (m_parent) {
    return root().waiting_for_index_count();
  }
  std::lock_guard lock(m_waiting_mutex);
  return m_waiting_for_index.size();
}

void res::resource_system::follow_move(const res::tag &node, const res::tag &from, const res::tag &to) {
  {
    std::unique_lock lock(m_cache_mutex);
    if (const auto watched = m_watchers.find(node); watched != m_watchers.end()) {
      for (auto &[key, entry] : watched->second) {
        if (entry.current == from) {
          entry.current = to;
        }
      }
    }
    std::unique_lock alias_lock(m_alias_mutex);
    for (auto &[original, alias] : m_aliases) {
      if (alias == from) {
        alias = to;
      }
    }
    // A pin's owner is the moved file by its new name: unpin_resource(to) takes it away (#869).
    std::unique_lock pin_lock(m_pinning_mutex);
    std::vector<cache_key> keys;
    m_pin_owners.for_each_of_tag(node, [&keys](const cache_key &key, const auto &) { keys.push_back(key); });
    for (const cache_key &key : keys) {
      auto &owners = *m_pin_owners.find(key);
      if (owners.erase(from) > 0) {
        owners.insert(to);
      }
    }
  }
  std::lock_guard layers_lock(m_layers_mutex);
  for (resource_system *layer : m_layers) {
    layer->follow_move(node, from, to);
  }
}

void res::resource_system::collect_bindings(std::vector<std::pair<res::tag, res::tag>> &out) {
  const auto add_key = [&out](const res::tag &key) {
    if (!asset_of_node(key)) {
      out.emplace_back(key, key);
    }
  };
  {
    std::shared_lock lock(m_cache_mutex);
    for (const auto &binding : m_bindings) {
      out.push_back(binding);
    }
    m_cache.for_each_tag(add_key);
    for (const auto &[key, blocks] : m_displaced) {
      add_key(key);
    }
    for (const auto &[key, watchers] : m_watchers) {
      add_key(key);
    }
    for (const auto &[dependent, targets] : m_depends_on) {
      add_key(dependent);
      for (const auto &target : targets) {
        add_key(target);
      }
    }
    std::shared_lock alias_lock(m_alias_mutex);
    for (const auto &[original, alias] : m_aliases) {
      add_key(original);
    }
    for (const auto &[original, alias] : m_alias_edges) {
      add_key(original);
      add_key(alias);
    }
  }
  {
    std::shared_lock lock(m_pinning_mutex);
    m_pinned_resources.for_each_tag(add_key);
    // An owner's own node, so a rebind takes each owner where its tag goes (#869).
    m_pin_owners.for_each_entry([&out](const cache_key &key, const std::unordered_set<res::tag> &owners) {
      for (const auto &owner : owners) {
        out.emplace_back(owner, key.tag);
      }
    });
  }
  {
    std::lock_guard lock(m_layer_needs_mutex);
    for (const auto &[target, count] : m_layer_needs) {
      add_key(target);
    }
  }
  std::lock_guard layers_lock(m_layers_mutex);
  for (resource_system *layer : m_layers) {
    layer->collect_bindings(out);
  }
}

std::unordered_map<res::tag, res::tag> res::resource_system::rebind_all() {
  // The index is asked here, before any lock is taken.
  const auto index = current_index();
  std::vector<std::pair<res::tag, res::tag>> bound;
  collect_bindings(bound);
  std::unordered_map<res::tag, res::tag> fresh; // tag -> its key now
  std::unordered_map<res::tag, res::tag> remap; // old key -> new key
  for (const auto &[tag, old] : bound) {
    auto [it, inserted] = fresh.try_emplace(tag);
    if (inserted) {
      it->second = index_node(tag, index.get());
    }
    if (it->second == old) {
      continue;
    }
    // An ID that a file holds now keeps its entries (it moved, or another spelling still names it); a tag key, or an
    // ID no file holds any more, hands its entries to the tag's new key.
    if (const auto id = asset_of_node(old); id && index && path_in(*index, *id)) {
      continue;
    }
    remap.try_emplace(old, it->second);
  }
  {
    std::lock_guard lock(m_waiting_mutex);
    for (auto &[tag, entry] : m_waiting_for_index) {
      if (const auto moved = remap.find(entry.node); moved != remap.end()) {
        entry.node = moved->second;
      }
    }
  }
  apply_rebind(fresh, remap);
  return remap;
}

void res::resource_system::apply_rebind(const std::unordered_map<res::tag, res::tag> &fresh,
                                        const std::unordered_map<res::tag, res::tag> &remap) {
  const auto mapped = [&remap](const res::tag &key) -> const res::tag & {
    const auto it = remap.find(key);
    return it == remap.end() ? key : it->second;
  };
  // Held until the layers are rebound too (parents before layers, as everywhere): no load of this system records an
  // edge meanwhile. Lock order: cache, alias, pinning, as register_alias.
  std::unique_lock lock(m_cache_mutex);
  std::unique_lock alias_lock(m_alias_mutex);
  std::unique_lock pin_lock(m_pinning_mutex);
  {
    // The tags this system binds: its bindings and its keys that are tags.
    std::vector<res::tag> tags;
    for (const auto &[tag, node] : m_bindings) {
      tags.push_back(tag);
    }
    const auto add_key = [&tags](const res::tag &key) {
      if (!asset_of_node(key)) {
        tags.push_back(key);
      }
    };
    m_cache.for_each_tag(add_key);
    for (const auto &[key, blocks] : m_displaced) {
      add_key(key);
    }
    m_pinned_resources.for_each_tag(add_key);
    m_pin_owners.for_each([&tags](const std::unordered_set<res::tag> &owners) {
      tags.insert(tags.end(), owners.begin(), owners.end());
    });
    for (const auto &[key, watchers] : m_watchers) {
      add_key(key);
    }
    for (const auto &[key, alias] : m_aliases) {
      add_key(key);
    }
    rekey_locked(remap);
    rehome_pin_owners_locked(fresh, remap);
    std::unordered_set<res::tag> alias_nodes;
    for (const auto &[original, alias] : m_alias_edges) {
      alias_nodes.insert(original);
      alias_nodes.insert(alias);
    }
    const auto has_entries = [&](const res::tag &node) {
      return m_cache.any_of_tag(node, [](const cache_entry &) { return true; }) || m_displaced.contains(node) ||
             m_pinned_resources.any_of_tag(node, [](const auto &) { return true; }) || m_watchers.contains(node) ||
             m_depends_on.contains(node) || m_dependents.contains(node) || m_aliases.contains(node) ||
             alias_nodes.contains(node) || needed_by_layers(node);
    };
    // Bindings to an ID only, and only while that ID has an entry here; a tag the collection missed (bound meanwhile)
    // keeps its binding.
    std::unordered_map<res::tag, res::tag> bindings;
    for (const auto &tag : tags) {
      const auto now = fresh.find(tag);
      const auto old = m_bindings.find(tag);
      const res::tag &node = now != fresh.end() ? now->second : old != m_bindings.end() ? old->second : tag;
      if (asset_of_node(node) && has_entries(node)) {
        bindings.insert_or_assign(tag, node);
      }
    }
    m_bindings = std::move(bindings);
  }
  {
    std::lock_guard layers_lock(m_layers_mutex);
    for (resource_system *layer : m_layers) {
      layer->apply_rebind(fresh, remap);
    }
  }
  // After the layers: a load of a layer records its edges (and counts them here) under the layer's lock, so once each
  // layer is rebound every count it made under an old key is in the map, and none comes after.
  std::lock_guard needs_lock(m_layer_needs_mutex);
  std::unordered_map<res::tag, std::size_t> needs;
  for (const auto &[target, count] : m_layer_needs) {
    needs[mapped(target)] += count;
  }
  m_layer_needs = std::move(needs);
}

void res::resource_system::rekey_locked(const std::unordered_map<res::tag, res::tag> &remap) {
  if (remap.empty()) {
    return;
  }
  const auto mapped = [&remap](const res::tag &key) -> const res::tag & {
    const auto it = remap.find(key);
    return it == remap.end() ? key : it->second;
  };
  for (const auto &[old_key, new_key] : remap) {
    // A live entry already under the new key wins (the other spelling's block): requests get it. The one that loses
    // stays under the key, displaced (#858). One pin per node and type (#869): a pin the new key has stays, else the
    // pin holds the new key's live block; a displaced block holds none. Its owners go home afterwards.
    m_cache.for_each_of_tag(old_key, [&](const cache_key &key, const cache_entry &entry) {
      const auto block = entry.lock();
      if (!block) {
        return;
      }
      const cache_entry *existing = m_cache.find(cache_key{new_key, key.type});
      if (const auto winner = existing ? existing->lock() : nullptr; winner && winner != block) {
        m_displaced[new_key].push_back(entry);
      }
    });
    if (auto moved = m_displaced.extract(old_key)) {
      auto &into = m_displaced[new_key];
      into.insert(into.end(), moved.mapped().begin(), moved.mapped().end());
    }
    m_cache.copy_tag(old_key, new_key, [](const cache_entry &entry) { return !entry.expired(); });
    m_cache.erase_tag(old_key);
    std::vector<std::pair<cache_key, std::shared_ptr<resource_handle>>> pins;
    m_pinned_resources.for_each_of_tag(old_key, [&](const cache_key &key, const auto &pinned) {
      const cache_key moved{new_key, key.type};
      const auto *kept = m_pinned_resources.find(moved);
      if (!pinned || (kept && *kept)) {
        return;
      }
      const cache_entry *live = m_cache.find(moved);
      auto block = live ? live->lock() : nullptr;
      pins.emplace_back(moved, block ? std::move(block) : pinned);
    });
    m_pinned_resources.erase_tag(old_key);
    for (auto &[key, block] : pins) {
      m_pinned_resources[key] = std::move(block);
    }
    if (const auto alias = m_aliases.find(old_key); alias != m_aliases.end()) {
      res::tag target = alias->second;
      m_aliases.erase(alias);
      m_aliases.try_emplace(new_key, std::move(target));
    }
    if (const auto watched = m_watchers.find(old_key); watched != m_watchers.end()) {
      auto moving = std::move(watched->second);
      m_watchers.erase(watched);
      auto &into = m_watchers[new_key];
      for (auto &[key, entry] : moving) {
        // Watching the new key from now on: a signal of it queued before the move is not theirs.
        entry.epoch = ++m_watch_epoch;
        into.try_emplace(key, std::move(entry));
      }
    }
  }
  std::unordered_map<res::tag, res::tag> alias_edges;
  for (const auto &[original, alias] : m_alias_edges) {
    if (mapped(original) != mapped(alias)) {
      alias_edges.insert_or_assign(mapped(original), mapped(alias));
    }
  }
  m_alias_edges = std::move(alias_edges);
  // m_dependents is the reverse of m_depends_on: rebuilt from it.
  std::unordered_map<res::tag, std::vector<res::tag>> depends_on;
  for (const auto &[dependent, targets] : m_depends_on) {
    const res::tag &from = mapped(dependent);
    auto &into = depends_on[from];
    for (const auto &target : targets) {
      const res::tag &to = mapped(target);
      if (to != from && std::ranges::find(into, to) == into.end()) {
        into.push_back(to);
      }
    }
  }
  std::erase_if(depends_on, [](const auto &entry) { return entry.second.empty(); });
  m_dependents.clear();
  for (const auto &[dependent, targets] : depends_on) {
    for (const auto &target : targets) {
      m_dependents[target].push_back(dependent);
    }
  }
  m_depends_on = std::move(depends_on);
}

void res::resource_system::pin_bound(const cache_key &key, std::shared_ptr<resource_handle> block, bool always_cache) {
  const node_candidate candidate = candidate_node(key.tag);
  std::unique_lock lock(m_cache_mutex);
  const cache_key bound{bind_locked(key.tag, candidate), key.type};
  {
    std::unique_lock pin_lock(m_pinning_mutex);
    m_pinned_resources[bound] = block;
    m_pin_owners[bound].insert(key.tag);
    bump_pin_generation_locked(bound);
  }
  if (always_cache) {
    m_cache[bound] = block;
  } else if (m_parent) {
    // In a layer the block may be the parent's: the layer's cache entry is what its requests find, and what a change
    // evicts (the pin alone would keep serving the old block).
    const cache_entry *slot = m_cache.find(bound);
    if (!slot || slot->expired()) {
      m_cache[bound] = block;
    }
  }
}

void res::resource_system::evict_batch_locked(const std::vector<res::tag> &batch,
                                              const std::unordered_map<res::tag, res::tag> &alias_nodes) {
  const auto alias_node = [&](const res::tag &alias) {
    const auto known = alias_nodes.find(alias);
    return known != alias_nodes.end() ? known->second : node_locked(alias);
  };
  for (const auto &changed : batch) {
    m_cache.erase_tag(changed);
    m_displaced.erase(changed);
  }
  // A changed original has to run its adapter again (it stores fresh data and registers the alias anew), so its
  // alias goes with its edge; the alias entry itself was evicted above as a dependent. The alias lock is taken inside
  // the cache lock, the order prune_released_edges(), register_alias and rebind_all use too. A mapping that a
  // concurrent register_alias has replaced meanwhile is a different one and stays.
  std::unique_lock alias_lock(m_alias_mutex);
  for (const auto &changed : batch) {
    const auto edge = m_alias_edges.find(changed);
    if (edge == m_alias_edges.end()) {
      continue;
    }
    if (const auto mapped = m_aliases.find(changed); mapped != m_aliases.end() && alias_node(mapped->second) == edge->second) {
      m_aliases.erase(mapped);
    }
    m_alias_edges.erase(edge);
  }
}

void res::resource_system::signal_changed_local(const res::tag &tag) {
  std::vector<res::tag> batch;
  std::uint64_t epoch = 0;
  const auto alias_nodes = alias_value_nodes();
  {
    std::unique_lock lock(m_cache_mutex);
    // A small graph is swept every time (cheap, and a released tag is then never evicted or delivered). A large one only
    // every k_signal_sweep_every signals, so a burst of file events does not hold the exclusive lock for a sweep each.
    if (m_depends_on.size() < k_edge_sweep_floor || ++m_signals_since_sweep >= k_signal_sweep_every) {
      sweep_edges();
    }
    batch = changed_batch(tag);
    evict_batch_locked(batch, alias_nodes);
    epoch = ++m_watch_epoch;
  }

  // Every tag of the batch reaches the layers: a layer's resource may be built from any of them (a layer prefab on a
  // base model on the changed texture), and the layer's own graph walks on from there.
  std::vector<res::tag> for_layers = batch;
  // One task per signal, the batch in one piece: its order is the delivery order.
  m_tasks.post_main([this, batch = std::move(batch), epoch, alive = m_lifetime.token()]() {
    if (alive.expired()) {
      return;
    }
    for (const auto &changed : batch) {
      deliver_to_watchers(changed, epoch);
    }
  });

  // Held while the layers handle the signal: a layer removes itself under this lock before it goes away.
  std::lock_guard layers_lock(m_layers_mutex);
  for (resource_system *layer : m_layers) {
    for (const auto &changed : for_layers) {
      layer->signal_changed_local(changed);
    }
  }
}

void res::resource_system::evict_cached(const res::tag &tag) {
  if (m_parent) {
    root().evict_cached(tag);
    return;
  }
  // Under its key and under the tag, as signal_changed; a gone file is not waited for: nothing is told here.
  if (const res::tag node = node_of(tag); node != tag) {
    evict_cached_local(node);
  }
  evict_cached_local(tag);
}

void res::resource_system::evict_cached_local(const res::tag &tag) {
  std::vector<res::tag> batch;
  const auto alias_nodes = alias_value_nodes();
  {
    std::unique_lock lock(m_cache_mutex);
    batch = changed_batch(tag);
    evict_batch_locked(batch, alias_nodes);
  }
  std::lock_guard layers_lock(m_layers_mutex);
  for (resource_system *layer : m_layers) {
    for (const auto &changed : batch) {
      layer->evict_cached_local(changed);
    }
  }
}

void res::resource_system::drop_held_signal(const res::tag &tag) {
  auto &held = *m_held;
  std::lock_guard lock(held.mutex);
  if (held.signalled.erase(tag) > 0) {
    std::erase(held.signals, tag);
  }
}

void res::resource_system::limit_writes_to(std::string prefix) {
  ASSERT_MSG(m_parent, "limit_writes_to: a layer only, the root writes everywhere");
  m_write_prefix = std::move(prefix);
}

void res::resource_system::check_writable(const res::tag &tag) const {
  if (m_write_prefix.empty()) {
    return;
  }
  const std::string_view full = tag.view();
  bool inside = full.size() > m_write_prefix.size() && full.starts_with(m_write_prefix);
  if (inside) {
    // "memory://modules/a/../b/x" names another module's data.
    const std::string rest = "/" + std::string{full.substr(m_write_prefix.size())} + "/";
    inside = rest.find("/../") == std::string::npos && rest.find("/./") == std::string::npos;
  }
  if (!inside) {
    throw std::logic_error("res::resource_system: '" + std::string{full} + "' is outside what this layer may write (" +
                           m_write_prefix + ")");
  }
}

bool res::resource_system::erase(const res::tag &tag) {
  check_writable(tag);
  auto &resolvers = root().m_resolvers;
  const auto found = resolvers.find(tag.protocol());
  if (found == resolvers.end()) {
    egLOG_WARN("resource/erase", "{}", not_mounted_error(tag));
    return false;
  }
  if (!found->second->erase(tag)) {
    return false;
  }

  forget_erased(tag);
  signal_changed(tag);
  return true;
}

res::file_operation res::resource_system::remove(const res::tag &tag) {
  check_writable(tag);
  auto &resolvers = root().m_resolvers;
  const auto found = resolvers.find(tag.protocol());
  if (found == resolvers.end()) {
    std::string error = not_mounted_error(tag);
    egLOG_WARN("resource/remove", "{}", error);
    return file_operation::failure(std::move(error));
  }
  // A file with an ID waits for the index owner's batch like a deletion found on disk; that batch forgets it.
  resource_system &base = root();
  const std::uint64_t generation = base.m_index_generation.load(std::memory_order_acquire);
  const res::tag node = base.node_of(tag);
  const bool waits = node != tag && base.expect_gone(tag, node, generation);
  auto result = found->second->remove(tag);
  if (waits) {
    base.finish_expected(tag, static_cast<bool>(result));
  }
  if (!result) {
    return result;
  }
  if (waits) {
    base.signal_changed_local(tag);
    return result;
  }
  forget_erased(tag);
  signal_changed(tag);
  return result;
}

res::file_operation res::resource_system::rename(const res::tag &from, const res::tag &to) {
  check_writable(from);
  check_writable(to);
  if (from.protocol() != to.protocol()) {
    return file_operation::failure("a file cannot move between protocols");
  }
  auto &resolvers = root().m_resolvers;
  const auto found = resolvers.find(from.protocol());
  if (found == resolvers.end()) {
    std::string error = not_mounted_error(from);
    egLOG_WARN("resource/rename", "{}", error);
    return file_operation::failure(std::move(error));
  }
  // A file with an ID keeps its pins, aliases and resource if the index owner's batch pairs it with `to`.
  resource_system &base = root();
  const std::uint64_t generation = base.m_index_generation.load(std::memory_order_acquire);
  const res::tag node = base.node_of(from);
  const bool waits = node != from && base.expect_gone(from, node, generation);
  auto result = found->second->rename(from, to);
  if (waits) {
    base.finish_expected(from, static_cast<bool>(result));
  }
  if (!result) {
    return result;
  }
  if (waits) {
    base.signal_changed_local(from);
    signal_changed(to);
    return result;
  }
  forget_erased(from);
  signal_changed(from);
  signal_changed(to);
  return result;
}

void res::resource_system::subscribe_changes(std::string_view protocol, void *owner, changes_listener listener) {
  auto &base = root();
  std::lock_guard lock(base.m_changes_mutex);
  base.m_change_subscriptions.push_back(
      change_subscription{++base.m_next_subscription, std::string{protocol}, owner, std::move(listener)});
}

void res::resource_system::unsubscribe_changes(void *owner) {
  auto &base = root();
  std::lock_guard lock(base.m_changes_mutex);
  std::erase_if(base.m_change_subscriptions, [owner](const change_subscription &entry) { return entry.owner == owner; });
}

void res::resource_system::deliver_changes(const std::string &protocol, const std::vector<file_change> &batch,
                                           std::uint64_t number) {
  m_delivering_batch = number;
  std::vector<std::uint64_t> targets;
  {
    std::lock_guard lock(m_changes_mutex);
    for (const auto &entry : m_change_subscriptions) {
      if (entry.protocol == protocol) {
        targets.push_back(entry.id);
      }
    }
  }
  for (const std::uint64_t id : targets) {
    changes_listener listener;
    {
      // A previous listener of this delivery may have unsubscribed this one.
      std::lock_guard lock(m_changes_mutex);
      const auto entry = std::ranges::find(m_change_subscriptions, id, &change_subscription::id);
      if (entry == m_change_subscriptions.end()) {
        continue;
      }
      listener = entry->listener;
    }
    listener(batch);
  }
}

void res::resource_system::forget_erased(const res::tag &tag) {
  unpin_node(node_of(tag));
  {
    const res::tag node = node_of(tag);
    const auto alias_nodes = alias_value_nodes();
    // Lock order as in signal_changed: cache first, alias nested inside.
    std::unique_lock lock(m_cache_mutex);
    std::unique_lock alias_lock(m_alias_mutex);
    std::erase_if(m_alias_edges, [&](const auto &edge) { return edge.second == node; });
    std::erase_if(m_aliases, [&](const auto &mapped) {
      const auto known = alias_nodes.find(mapped.second);
      return (known != alias_nodes.end() ? known->second : node_locked(mapped.second)) == node;
    });
  }
  std::lock_guard layers_lock(m_layers_mutex);
  for (resource_system *layer : m_layers) {
    layer->forget_erased(tag);
  }
}

void res::resource_system::deliver_to_watchers(const res::tag &node,
                                               std::uint64_t signal_epoch) {
  // Only watchers registered before the signal: a load that follows a signal registers its own watcher.
  std::vector<std::pair<watch_key, std::uint64_t>> targets;
  {
    std::shared_lock lock(m_cache_mutex);
    if (auto it = m_watchers.find(node); it != m_watchers.end()) {
      for (const auto &[key, entry] : it->second) {
        if (entry.epoch < signal_epoch) {
          targets.emplace_back(key, entry.epoch);
        }
      }
    }
  }

  for (const auto &[key, epoch] : targets) {
    reload_callback callback;
    res::tag current;
    {
      // A previous callback of this delivery may have unwatched (and destroyed) this owner.
      std::shared_lock lock(m_cache_mutex);
      auto it = m_watchers.find(node);
      if (it == m_watchers.end()) {
        continue;
      }
      auto entry = it->second.find(key);
      if (entry == it->second.end() || entry->second.epoch != epoch) {
        continue;
      }
      callback = entry->second.callback;
      current = entry->second.current;
    }
    callback(current);
  }
}

void res::resource_system::register_alias(const res::tag &original,
                                          const res::tag &alias) {
  if (original == alias) {
    return;
  }
  const node_candidate original_candidate = candidate_node(original);
  const node_candidate alias_candidate = candidate_node(alias);
  // Lock order: cache, then alias, then pinning (rebind_all takes all three the same way).
  std::unique_lock lock(m_cache_mutex);
  const res::tag original_node = bind_locked(original, original_candidate);
  const res::tag alias_node = bind_locked(alias, alias_candidate);
  if (original_node == alias_node) {
    return;
  }
  {
    std::unique_lock alias_lock(m_alias_mutex);
    m_aliases[original_node] = alias;
  }
  m_cache.copy_tag(original_node, alias_node, [](const cache_entry &entry) { return !entry.expired(); });
  // One alias per original, as in m_aliases: registering the same pair again changes nothing, another alias replaces it.
  m_alias_edges.insert_or_assign(original_node, alias_node);
  std::unique_lock pin_lock(m_pinning_mutex);
  m_pinned_resources.copy_tag(original_node, alias_node, [](const auto &entry) { return entry != nullptr; });
  // The copy is a newer pin of the alias' owners: a reload of theirs still loading must not put its file over it.
  std::vector<cache_key> copied;
  m_pinned_resources.for_each_of_tag(alias_node, [&](const cache_key &key, const auto &pinned) {
    const auto *original_pin = m_pinned_resources.find(cache_key{original_node, key.type});
    if (pinned && original_pin && *original_pin == pinned && m_pin_generations.find(key)) {
      copied.push_back(key);
    }
  });
  for (const cache_key &key : copied) {
    bump_pin_generation_locked(key);
  }
}

std::filesystem::path res::resource_system::get_resources_path() const {
  const auto layers = get_resource_layers();
  return layers.empty() ? std::filesystem::path{} : layers.back();
}

std::vector<std::filesystem::path> res::resource_system::get_resource_layers() const {
  return mount_roots(tag::default_protocol());
}

std::vector<std::filesystem::path> res::resource_system::mount_roots(std::string_view protocol) const {
  if (m_parent) {
    return m_parent->mount_roots(protocol);
  }
  for (const auto &mount : m_mounts) {
    if (mount.protocol == protocol) {
      return mount.roots;
    }
  }
  return {};
}

namespace {
// Empty when resource/base_path is not set; asserts it exists otherwise.
std::filesystem::path engine_base_path() {
  if (cfg_res_base_path->empty()) {
    return {};
  }
  ASSERT_MSG(std::filesystem::exists(cfg_res_base_path),
             "Base resources path does not exist!");
  return cfg_res_base_path->string();
}

void add_engine_mount(res::mount_table &table, const std::filesystem::path &engine_base) {
  if (engine_base.empty()) {
    return;
  }
  table.push_back(res::mount{res::tag::engine, {engine_base}, res::mount_access::read_only});
}
} // namespace

std::filesystem::path res::resource_system::configured_engine_base() {
  return cfg_res_base_path->empty() ? std::filesystem::path{} : std::filesystem::path{cfg_res_base_path->string()};
}

bool res::resource_system::watches_files() {
  return *cfg_res_watch_files;
}

std::vector<std::string> res::resource_system::live_resource_tags(std::size_t limit) const {
  std::vector<std::pair<std::string, long>> live;
  const auto index = current_index();
  {
    std::shared_lock lock(m_cache_mutex);
    m_cache.for_each_entry([&live, &index](const cache_key &key, const cache_entry &entry) {
      if (const auto block = entry.lock()) {
        live.emplace_back(display_name(key.tag, index.get()), block.use_count() - 1);
      }
    });
  }
  std::ranges::sort(live);
  std::vector<std::string> out;
  out.reserve(std::min(limit, live.size()));
  for (const auto &[tag, refs] : live) {
    if (out.size() == limit) {
      break;
    }
    out.push_back(std::format("{} (refs {})", tag, refs));
  }
  return out;
}

bool res::resource_system::read_only_project() {
  return *cfg_res_read_only_project;
}

std::string res::resource_system::not_mounted_error(std::string_view protocol) const {
  std::vector<std::string_view> mounted;
  mounted.reserve(root().m_resolvers.size());
  for (const auto &entry : root().m_resolvers) {
    mounted.push_back(entry.first);
  }
  std::ranges::sort(mounted);
  std::string list;
  for (const auto name : mounted) {
    list += list.empty() ? std::string{name} : std::format(", {}", name);
  }
  return std::format("'{}://' is not mounted in this run (mounted: {})", protocol, list.empty() ? "none" : list);
}

std::string res::resource_system::not_mounted_error(const tag &tag) const {
  if (tag.protocol().empty()) {
    return no_protocol_reason(tag);
  }
  return not_mounted_error(tag.protocol());
}

res::mount_table res::resource_system::config_mount_table() {
  egLOG("resource/system", "Resources path swd '{0}', cfg '{1}'",
        fs::absolute("./").string(), cfg_res_path->string());
  ASSERT_MSG(std::filesystem::exists(cfg_res_path),
             "Resources path does not exist!");

  const auto engine_base = engine_base_path();
  // Mount order: base first, top last (the last mount wins and receives writes).
  std::vector<fs::path> layers;
  if (!engine_base.empty())
    layers.push_back(engine_base);
  layers.push_back(cfg_res_path->string());

  mount_table table;
  table.push_back(mount{tag::default_protocol(), std::move(layers), mount_access::read_write});
  table.push_back(mount{tag::memory, {fs::path{cfg_res_path->string()} / "../sandbox"}, mount_access::read_write}); // TODO: make configurable
  add_engine_mount(table, engine_base);
  return table;
}

res::mount_table res::resource_system::project_mount_table(
    const std::filesystem::path &engine_base,
    const std::vector<std::filesystem::path> &content_roots,
    const std::filesystem::path &cache_dir,
    const std::filesystem::path &user_dir, const std::filesystem::path &code_dir) {
  ASSERT_MSG(!content_roots.empty(), "A project has at least one content root");
  if (engine_base.empty()) {
    egLOG_WARN("resource/system", "resource/base_path is not set: res:// has no engine layer and engine:// is not mounted");
  }

  std::vector<fs::path> layers;
  if (!engine_base.empty())
    layers.push_back(engine_base);
  layers.insert(layers.end(), content_roots.rbegin(), content_roots.rend());

  mount_table table;
  // Every file is reported, for the Git panel (#515), and hot reload keeps every readable folder; only a repository's
  // .git is skipped. The private folders (.cache, .snk, build) never lie in a content root: prj::verify_project_mounts.
  table.push_back(mount{tag::default_protocol(), std::move(layers), mount_access::read_write, write_policy::allow_all(),
                        watch_options{true, [](std::string_view name) { return name == ".git"; }, {}},
                        false});
  table.push_back(mount{tag::memory, {cache_dir / "memory"}, mount_access::read_write});
  table.push_back(mount{tag::cache, {cache_dir}, mount_access::read_write});
  table.push_back(mount{tag::user, {user_dir}, mount_access::read_write});
  if (!code_dir.empty()) {
    // Case folded: on a case-insensitive file system "Build/a.cpp" is the folder the deny patterns name.
    // The watcher skips the folders the write policy refuses and reports code files only: one list for both.
    table.push_back(mount{tag::code, {code_dir}, mount_access::read_write,
                          write_policy{tag::code, prj::code_write_patterns(), false},
                          watch_options{true, [](std::string_view name) { return prj::is_code_ignored_dir(name); },
                                        [](std::string_view name) { return prj::is_code_file(name); }},
                          true});
  }
  add_engine_mount(table, engine_base);
  return table;
}

res::mount_table res::resource_system::shipping_mount_table(
    const std::filesystem::path &engine_base,
    const std::vector<std::filesystem::path> &content_roots,
    const std::filesystem::path &user_dir, const std::filesystem::path &scratch_dir) {
  ASSERT_MSG(!content_roots.empty(), "A project has at least one content root");
  if (engine_base.empty()) {
    egLOG_WARN("resource/system", "resource/base_path is not set: res:// has no engine layer and engine:// is not mounted");
  }

  std::vector<fs::path> layers;
  if (!engine_base.empty())
    layers.push_back(engine_base);
  layers.insert(layers.end(), content_roots.rbegin(), content_roots.rend());

  mount_table table;
  table.push_back(mount{tag::default_protocol(), std::move(layers), mount_access::read_only});
  table.push_back(mount{tag::memory, {scratch_dir}, mount_access::read_write});
  table.push_back(mount{tag::user, {user_dir}, mount_access::read_write});
  add_engine_mount(table, engine_base);
  return table;
}

std::uint64_t res::resource_system::park(std::shared_ptr<resource_handle> block, std::function<void()> run) {
  std::lock_guard lock(m_parked_mutex);
  const std::uint64_t id = m_next_parked++;
  m_parked.emplace(id, parked{ std::move(block), std::move(run) });
  return id;
}

std::optional<res::resource_system::parked> res::resource_system::unpark(std::uint64_t id) {
  std::lock_guard lock(m_parked_mutex);
  const auto found = m_parked.find(id);
  if (found == m_parked.end()) {
    return std::nullopt;
  }
  parked taken = std::move(found->second);
  m_parked.erase(found);
  return taken;
}

void res::resource_system::run_parked(const std::shared_ptr<gate> &gate, std::uint64_t id) {
  gate->with([id](resource_system &self) {
    if (auto step = self.unpark(id)) {
      step->run();
    }
  });
}

std::uint64_t res::resource_system::register_await(std::shared_ptr<await_state> state) {
  std::lock_guard lock(m_parked_mutex);
  const std::uint64_t id = m_next_parked++;
  m_awaits.emplace(id, std::move(state));
  return id;
}

std::shared_ptr<res::resource_system::await_state> res::resource_system::find_await(std::uint64_t id) {
  std::lock_guard lock(m_parked_mutex);
  const auto found = m_awaits.find(id);
  return found == m_awaits.end() ? nullptr : found->second;
}

void res::resource_system::drop_await(std::uint64_t id) {
  std::lock_guard lock(m_parked_mutex);
  m_awaits.erase(id);
}

void res::resource_system::cancel_parked() {
  std::unordered_map<std::uint64_t, parked> steps;
  std::unordered_map<std::uint64_t, std::shared_ptr<await_state>> waits;
  {
    std::lock_guard lock(m_parked_mutex);
    steps.swap(m_parked);
    waits.swap(m_awaits);
  }
  // Outside the lock: failing a block runs its callbacks.
  for (auto &[id, step] : steps) {
    if (step.block && !is_finished(*step.block)) {
      step.block->set_error("Load cancelled");
    }
  }
  for (auto &[id, wait] : waits) {
    if (!is_finished(*wait->owner)) {
      wait->owner->set_error("Load cancelled");
    }
  }
}

void res::resource_system::keep_warm(const std::shared_ptr<resource_handle> &block, cache_key key) {
  // In a layer the block may be the parent's and settle after the layer is gone.
  block->on_settled([gate = m_gate, weak = std::weak_ptr<resource_handle>(block), key = std::move(key)](auto &) {
    const auto held = weak.lock();
    if (!held || held->has_error()) {
      return;
    }
    gate->with([&](resource_system &self) { self.pin_bound(key, held, false); });
  });
}

void res::resource_system::serialize_pinned(const res::tag &tag, std::shared_ptr<resource_handle> block, adapter ad) {
  // Main thread: serializing may wait for field handles, which a pool thread must not do. Run outside the gate (the
  // wait may need a callback that goes through it); the system cannot go away meanwhile, both are on the main thread.
  const auto id = park(block, [this, tag, block, ad = std::move(ad)] {
    try {
      if (!ad) {
        throw std::runtime_error("No adapter found for resource type");
      }
      store(tag, ad->serialize(tag, block->data));
    } catch (const std::exception &e) {
      egLOG_ERROR("res/error", "Failed to auto-serialize {0}: {1}", tag.string(), e.what());
    }
  });
  m_tasks.post_main([gate = m_gate, id] {
    std::optional<parked> step;
    gate->with([&](resource_system &self) { step = self.unpark(id); });
    if (step) {
      step->run();
    }
  });
}

void res::resource_system::retain_in_parents(const res::tag &target) {
  for (resource_system *parent = m_parent; parent; parent = parent->m_parent) {
    std::lock_guard lock(parent->m_layer_needs_mutex);
    ++parent->m_layer_needs[target];
  }
}

void res::resource_system::release_in_parents(const res::tag &target) {
  for (resource_system *parent = m_parent; parent; parent = parent->m_parent) {
    std::lock_guard lock(parent->m_layer_needs_mutex);
    if (const auto it = parent->m_layer_needs.find(target); it != parent->m_layer_needs.end() && --it->second == 0) {
      parent->m_layer_needs.erase(it);
    }
  }
}

bool res::resource_system::needed_by_layers(const res::tag &tag) const {
  std::lock_guard lock(m_layer_needs_mutex);
  return m_layer_needs.contains(tag);
}

bool res::resource_system::exists(const tag& tag) const
{
	const auto& resolvers = root().m_resolvers;
	if (resolvers.find(tag.protocol()) == resolvers.end()) {
		egLOG_WARN("resource/exists", "{}", not_mounted_error(tag));
		return false;
	}
	return resolvers.at(tag.protocol())->exists(tag);
}

bool res::resource_system::store(const tag& tag, const std::vector<std::byte>& data)
{
	check_writable(tag);
	const auto& resolvers = root().m_resolvers;
	if (resolvers.find(tag.protocol()) == resolvers.end()) {
		egLOG_WARN("resource/store", "{}", not_mounted_error(tag));
		return false;
	}

	return resolvers.at(tag.protocol())->store(tag, data);
}

bool res::resource_system::store_sync(const tag& tag, const std::vector<std::byte>& data)
{
	check_writable(tag);
	const auto& resolvers = root().m_resolvers;
	if (resolvers.find(tag.protocol()) == resolvers.end()) {
		egLOG_WARN("resource/store", "{}", not_mounted_error(tag));
		return false;
	}
	return resolvers.at(tag.protocol())->store_sync(tag, data);
}

res::file_operation res::resource_system::create_sync(const tag& tag, const std::vector<std::byte>& data)
{
	check_writable(tag);
	const auto& resolvers = root().m_resolvers;
	if (resolvers.find(tag.protocol()) == resolvers.end()) {
		std::string error = not_mounted_error(tag);
		egLOG_WARN("resource/store", "{}", error);
		return file_operation::failure(std::move(error));
	}
	auto result = resolvers.at(tag.protocol())->create_sync(tag, data);
	// Whatever was loaded under the tag meanwhile is dropped, created or refused.
	signal_changed(tag);
	return result;
}

void res::resource_system::watch(const res::tag& tag, void* owner, reload_callback cb, bool keep_dependencies)
{
	const node_candidate candidate = candidate_node(tag);
	std::lock_guard lock(m_cache_mutex);
	const res::tag node = bind_locked(tag, candidate);
	m_watchers[node][watch_key{ owner, std::string{ tag.view() } }] = watcher{ std::move(cb), ++m_watch_epoch, keep_dependencies, tag };
}

void res::resource_system::unwatch(const res::tag& tag, void* owner)
{
	const watch_key key{ owner, std::string{ tag.view() } };
	std::lock_guard lock(m_cache_mutex);
	const auto bound = m_bindings.find(tag);
	auto it = m_watchers.find(bound == m_bindings.end() ? tag : bound->second);
	if (it == m_watchers.end() || !it->second.contains(key)) {
		// Not under the tag's key now (its file moved, or got another ID): found by the tag it was registered with.
		it = std::ranges::find_if(m_watchers, [&key](const auto& entry) { return entry.second.contains(key); });
		if (it == m_watchers.end()) return;
	}
	it->second.erase(key);
	if (it->second.empty()) {
		m_watchers.erase(it);
	}
}

std::size_t res::resource_system::watched_tag_count() const
{
	std::shared_lock lock(m_cache_mutex);
	return m_watchers.size();
}

std::size_t res::resource_system::pending_loads() const
{
	std::shared_lock lock(m_cache_mutex);
	std::size_t count = 0;
	const auto count_loading = [&count](const cache_entry& entry) {
		if (const auto block = entry.lock()) {
			const auto status = block->status.load();
			if (status != core::res::res_status::ready && status != core::res::res_status::error) {
				++count;
			}
		}
	};
	m_cache.for_each(count_loading);
	for (const auto& [node, blocks] : m_displaced) {
		std::ranges::for_each(blocks, count_loading);
	}
	return count;
}

std::size_t res::resource_system::displaced_count() const
{
	std::shared_lock lock(m_cache_mutex);
	std::size_t count = 0;
	for (const auto& [node, blocks] : m_displaced) {
		const auto alive = std::ranges::count_if(blocks, [](const cache_entry& entry) { return !entry.expired(); });
		count += static_cast<std::size_t>(alive);
	}
	return count;
}

std::size_t res::resource_system::live_resources() const
{
	std::shared_lock lock(m_cache_mutex);
	std::size_t count = 0;
	m_cache.for_each([&count](const cache_entry& entry) {
		if (!entry.expired()) {
			++count;
		}
	});
	return count;
}

std::size_t res::resource_system::layer_needs_count() const
{
	std::lock_guard lock(m_layer_needs_mutex);
	return m_layer_needs.size();
}

std::size_t res::resource_system::pinned_count() const
{
	std::shared_lock lock(m_pinning_mutex);
	std::size_t count = 0;
	m_pinned_resources.for_each([&count](const std::shared_ptr<resource_handle>& entry) {
		if (entry) {
			++count;
		}
	});
	return count;
}

const res::resource_system* res::resource_system::parent() const
{
	return m_parent;
}

const std::string& res::resource_system::write_prefix() const
{
	return m_write_prefix;
}

std::size_t res::resource_system::dependency_edge_count() const
{
	std::shared_lock lock(m_cache_mutex);
	std::size_t count = 0;
	for (const auto& [tag, targets] : m_depends_on) {
		count += targets.size();
	}
	return count;
}

std::size_t res::resource_system::alias_edge_count() const
{
	std::shared_lock lock(m_cache_mutex);
	return m_alias_edges.size();
}

std::size_t res::resource_system::edge_sweep_count() const
{
	std::shared_lock lock(m_cache_mutex);
	return m_edge_sweeps;
}

void res::resource_system::unregistrate_resolver(protocol prt)
{
	m_resolvers.erase(prt);
}

std::vector<std::string> res::resource_system::registered_extensions(const extension& extension) const
{
	std::vector<std::string> out;
	for (const auto& ext : extension.extensions) {
		for (const resource_system* system = this; system; system = system->m_parent) {
			const auto it = system->m_adapters.find(std::pair{ extension.resource_type, ext });
			const auto info = it == system->m_adapters.end() ? system->m_adapter_infos.end() : system->m_adapter_infos.find(it->second);
			if (info != system->m_adapter_infos.end() && info->second == extension) {
				out.push_back(ext);
				break;
			}
		}
	}
	return out;
}

void res::resource_system::unregistrate_adapter(extension extension)
{
	for(auto& ext : extension.extensions) {
		auto it = m_adapters.find(std::pair{ extension.resource_type, ext });
		if (it != m_adapters.end())
		{
			m_adapter_infos.erase(it->second);
			m_adapters.erase(it);
		}
		if (extension.magic_number != 0)
			m_extensions_with_magic.erase(std::pair{ extension.resource_type, ext });
	}

	if (extension.extensions.empty()) {
		auto it = m_adapters.find(std::pair{ extension.resource_type, std::string{ "*" } });
		if (it != m_adapters.end()) {
			m_adapter_infos.erase(it->second);
			m_adapters.erase(it);
		}

		if (extension.magic_number != 0)
			m_extensions_with_magic.erase(std::pair{ extension.resource_type, std::string{ "*" } });
	}
}

res::resource_system::data res::resource_system::fetch_data(const tag& tag) const
{
	const auto& resolvers = root().m_resolvers;
	if (resolvers.find(tag.protocol()) == resolvers.end()) {
		egLOG_WARN("resource/require", "{}", not_mounted_error(tag));
		return data{};
	}

	return resolvers.at(tag.protocol())->resolve(tag);
}

void res::resource_system::unpin_resource(const res::tag& tag)
{
	const res::tag node = node_of(tag);
	std::unique_lock lock(m_pinning_mutex);
	// Another spelling of the node that pinned the same type keeps that pin: one node is one file (#869).
	std::vector<cache_key> keys;
	const auto add = [&keys](const cache_key& key, const auto&) {
		if (std::ranges::find(keys, key) == keys.end()) {
			keys.push_back(key);
		}
	};
	m_pinned_resources.for_each_of_tag(node, add);
	m_pin_owners.for_each_of_tag(node, add);
	for (const cache_key& key : keys) {
		if (auto* owners = m_pin_owners.find(key)) {
			owners->erase(tag);
			if (!owners->empty()) {
				continue;
			}
		}
		m_pinned_resources.erase(key);
		m_pin_owners.erase(key);
		m_pin_generations.erase(key);
	}
}

void res::resource_system::unpin_node(const res::tag& node)
{
	std::unique_lock lock(m_pinning_mutex);
	m_pin_owners.erase_tag(node);
	m_pin_generations.erase_tag(node);
	m_pinned_resources.erase_tag(node);
}

std::uint64_t res::resource_system::bump_pin_generation_locked(const cache_key& key)
{
	return m_pin_generations[key] = ++m_pin_generation;
}

void res::resource_system::reload_pin(const pin_reload& reload)
{
	if (!exists(reload.file)) {
		egLOG_WARN("res/pin", "{}: no file to pin again after its spellings split over two IDs, the pin goes",
			reload.file.string());
		apply_pin_reload(reload, nullptr);
		return;
	}
	const ds::type_id chain[] = { reload.pinned.type };
	const auto block = require_block(reload.file, chain, false);
	block->on_settled([gate = m_gate, weak = std::weak_ptr<resource_handle>(block), reload](auto&) {
		gate->with([&](resource_system& self) { self.apply_pin_reload(reload, weak.lock()); });
	});
}

void res::resource_system::apply_pin_reload(const pin_reload& reload, const std::shared_ptr<resource_handle>& block)
{
	std::unique_lock lock(m_pinning_mutex);
	// Pinned again, evicted again or let go meanwhile: not this reload's any more.
	const auto* generation = m_pin_generations.find(reload.pinned);
	auto* owners = m_pin_owners.find(reload.pinned);
	if (!generation || *generation != reload.generation || !owners || owners->empty()) {
		return;
	}
	if (!block || block->has_error()) {
		if (block) {
			egLOG_WARN("res/pin", "{}: could not be loaded to pin again, the pin goes", reload.file.string());
		}
		m_pin_owners.erase(reload.pinned);
		m_pin_generations.erase(reload.pinned);
		m_pinned_resources.erase(reload.pinned);
		return;
	}
	m_pinned_resources[reload.pinned] = block;
	bump_pin_generation_locked(reload.pinned);
}

void res::resource_system::refresh_pins()
{
	ASSERT_MSG(!m_parent, "refresh_pins: the root only, it takes the layers' reloads too");
	ASSERT_MSG(m_tasks.on_main(), "refresh_pins: main thread only");
	std::vector<std::pair<resource_system*, pin_reload>> pending;
	take_pin_reloads(pending);
	// The spellings now name another file: what was loaded or pinned under the old one is a change for them. Once per
	// node, before any reload: a layer's signal reaches the root, and a later one would evict what the root reloaded.
	std::unordered_set<res::tag> signalled;
	for (const auto& [system, reload] : pending) {
		if (signalled.insert(node_of(reload.file)).second) {
			signal_changed(reload.file);
		}
	}
	for (const auto& [system, reload] : pending) {
		system->reload_pin(reload);
	}
}

void res::resource_system::take_pin_reloads(std::vector<std::pair<resource_system*, pin_reload>>& out)
{
	{
		std::unique_lock lock(m_pinning_mutex);
		for (pin_reload& reload : m_pins_to_refresh) {
			out.emplace_back(this, std::move(reload));
		}
		m_pins_to_refresh.clear();
	}
	std::vector<resource_system*> layers;
	{
		std::lock_guard layers_lock(m_layers_mutex);
		layers = m_layers;
	}
	// Called on a copy, outside m_layers_mutex. Safe: refresh_pins runs on the main thread, where layers are made and
	// destroyed (a module's load and unload), so none goes before its reloads are started.
	for (resource_system* layer : layers) {
		layer->take_pin_reloads(out);
	}
}

void res::resource_system::rehome_pin_owners_locked(const std::unordered_map<res::tag, res::tag>& fresh,
	const std::unordered_map<res::tag, res::tag>& remap)
{
	const auto mapped = [&remap](const res::tag& key) -> const res::tag& {
		const auto it = remap.find(key);
		return it == remap.end() ? key : it->second;
	};
	// rekey_locked moved the pins, not the owners: an owner record's pin now sits under its old key mapped. Owners that
	// all stay together keep it. Owners a rebind split over nodes (another ID for some spellings, the index gone) name
	// other files now: each node they go to loses the pin and gets its owners' file loaded and pinned again (#869).
	// Generations go with the owners, so a reload started before still meets its own.
	tag_keyed_map<std::unordered_set<res::tag>> owners;
	tag_keyed_map<std::uint64_t> generations;
	std::unordered_set<cache_key> reload;
	std::unordered_set<cache_key> owned;
	m_pin_owners.for_each_entry([&](const cache_key& key, const std::unordered_set<res::tag>& tags) {
		const cache_key pinned{ mapped(key.tag), key.type };
		owned.insert(pinned);
		std::vector<cache_key> homes;
		bool split = false;
		// An ID a file still holds keeps its entries (rebind_all): a tag that left it for a tag key is that file moved
		// away (a walk ahead of the move's batch), so its owner stays with the pin and follow_move renames it.
		const bool held_id = asset_of_node(pinned.tag) && !remap.contains(key.tag);
		for (const res::tag& tag : tags) {
			const auto node = fresh.find(tag);
			const res::tag& now = node != fresh.end() ? node->second : pinned.tag;
			const cache_key home{ held_id && !asset_of_node(now) ? pinned.tag : now, key.type };
			owners[home].insert(tag);
			homes.push_back(home);
			split = split || home != pinned;
		}
		// No pin under the key: an earlier rebind's reload is still loading it. Owners that stayed under the same key
		// keep it running; owners that moved have it started again where they are now.
		const auto* pin = m_pinned_resources.find(pinned);
		const bool loading = !pin || !*pin;
		if (split || (loading && pinned != key)) {
			reload.insert(homes.begin(), homes.end());
			reload.insert(pinned);
		} else if (const auto* generation = m_pin_generations.find(key)) {
			generations[pinned] = *generation;
		}
	});
	for (const cache_key& key : reload) {
		m_pinned_resources.erase(key);
		const auto* left = owners.find(key);
		if (!left || left->empty()) {
			continue;
		}
		const std::uint64_t generation = ++m_pin_generation;
		generations[key] = generation;
		// The same spelling in the root and its layers, whatever order the sets keep.
		const res::tag& file = *std::ranges::min_element(*left, {}, &res::tag::string);
		m_pins_to_refresh.push_back(pin_reload{ file, key, generation });
	}
	// A pin whose owners all went to other nodes goes.
	for (const cache_key& key : owned) {
		if (!owners.find(key)) {
			m_pinned_resources.erase(key);
		}
	}
	m_pin_generations = std::move(generations);
	m_pin_owners = std::move(owners);
}

res::tag res::resource_system::resolve_alias(const res::tag& tag) const
{
	{
		const res::tag node = node_of(tag);
		std::shared_lock lock(m_alias_mutex);
		auto it = m_aliases.find(node);
		if (it != m_aliases.end()) {
			return it->second;
		}
	}
	return m_parent ? m_parent->resolve_alias(tag) : tag;
}

const res::resource_system& res::resource_system::root() const
{
	return m_parent ? m_parent->root() : *this;
}

res::resource_system& res::resource_system::root()
{
	return m_parent ? m_parent->root() : *this;
}

std::shared_ptr<res::resource_system::resource_handle> res::resource_system::find_cached(const cache_key& key)
{
	std::shared_lock lock(m_cache_mutex);
	const cache_entry* slot = m_cache.find(key);
	return slot ? slot->lock() : nullptr;
}

void res::resource_system::add_resolver(protocol prt, resolver added)
{
	ASSERT_MSG(!m_parent, "A resource layer uses the resolvers of its root");
	ASSERT_MSG(m_resolvers.find(prt) == m_resolvers.end(), "That protocol already has resolver!");
	m_resolvers[prt] = std::move(added);
}

void res::resource_system::add_adapter(extension extension, adapter added)
{
	m_adapter_infos[added] = extension;

	if (extension.extensions.empty()) {
		m_adapters[std::pair{ extension.resource_type, std::string{ "*" }}] = added;

		if (extension.magic_number != 0) {
			m_extensions_with_magic.insert(std::pair{ extension.resource_type, std::string{ "*" }});
		}
	}

	for (auto& ext : extension.extensions) {
		m_adapters[std::pair{ extension.resource_type, ext }] = added;

		if (extension.magic_number != 0) {
			m_extensions_with_magic.insert(std::pair{ extension.resource_type, ext });
		}
	}
}

res::cache_key res::resource_system::key_for(const res::tag& tag, std::span<const ds::type_id> chain) const
{
	ds::type_id type = chain.front();
	find_adapter_in_chain(tag, chain, &type);
	return cache_key{ tag, type };
}

void res::resource_system::pin_block(const res::tag& tag, std::shared_ptr<resource_entry> resource, std::span<const ds::type_id> chain)
{
	const bool stored = exists(tag);
	if (!stored) {
		check_writable(tag); // a pin that is not stored yet is written out: refused before anything is pinned
	}
	auto block = std::make_shared<resource_handle>();
	block->set_ready(std::move(resource));
	pin_bound(key_for(tag, chain), block, true);

	if (!stored) {
		serialize_pinned(tag, block, find_adapter_in_chain(tag, chain));
	}
}

ds::task_system* res::resource_system::handle_tasks()
{
	return &m_tasks;
}

void res::resource_system::check_sync_require([[maybe_unused]] const res::tag& tag) const
{
	ASSERT_MSG(!(m_tasks.mode() == ds::policy::threaded && ds::task_system::on_worker()),
		"require_sync('{}') on a pool thread: declare it as a dependency of the adapter instead", tag.view());
}

res::tag res::resource_system::resolve_reference(const reference& ref) const
{
	if (!ref.guid.is_nil()) {
		if (const auto index = current_index()) {
			if (auto path = path_in(*index, ref.guid)) {
				return *std::move(path);
			}
		}
		if (ref.path.view().empty()) {
			egLOG_WARN("resource/reference", "{}: no file holds this guid and the reference has no path, it loads nothing",
				res::to_text(ref.guid));
		}
	}
	return ref.path;
}

std::optional<res::tag> res::resource_system::asset_path_of(const asset_id& guid) const
{
	if (guid.is_nil()) {
		return std::nullopt;
	}
	const auto index = current_index();
	return index ? path_in(*index, guid) : std::nullopt;
}

bool res::resource_system::has_asset_index() const
{
	return current_index() != nullptr;
}
