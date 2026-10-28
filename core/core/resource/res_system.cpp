#include <snk/resource/res_system.h>
#include <snk/resource/adapters/res_pct_adapter.h>
#include <snk/resource/adapters/res_text_adapter.h>
#include <snk/resource/adapters/res_binary_adapter.h>
#include <snk/config/cfg_variables.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <algorithm>
#include <atomic>
#include <chrono>
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

  std::mutex m_mutex;
  node_map m_nodes;
};

res::resource_system::resource_system(ds::task_system &tasks)
    : resource_system(tasks, config_mount_table()) {}

res::resource_system::resource_system(ds::task_system &tasks, mount_table mounts)
    : m_graph(std::make_unique<wait_graph>()), m_tasks(tasks),
      m_mounts(std::move(mounts)) {
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
  auto &resolver =
      registrate_resolver<vfs_resolver>(mounted.protocol, mounted.roots, mounted.access);
  resolver.set_resource_changed_callback(
      [this](const res::tag &tag) { signal_changed(tag); });
}

void res::resource_system::add_mount(mount added) {
  ASSERT_MSG(!m_parent, "A resource layer uses the mounts of its root");
  register_mount(added);
  m_mounts.push_back(std::move(added));
}

res::resource_system::resource_system(resource_system &parent)
    : m_graph(std::make_unique<wait_graph>()), m_tasks(parent.m_tasks), m_parent(&parent) {
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
  {
    std::lock_guard lock(m_stop_mutex);
    m_stopping = true;
  }
  m_lifetime.cancel_tasks();
  m_tasks.wait_idle(m_lifetime);
  // The gate is closed: nothing parked runs any more, and what it holds (adapters, blocks) goes now, not when the root
  // settles what it waited for.
  cancel_parked();
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
    }
    if (const std::size_t live = live_resources(); live > 0) {
      egLOG_WARN("resource/layer", "{} resources loaded through a destroyed layer are still referenced", live);
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

void res::resource_system::start_loading(const res::tag &tag,
                                         const std::shared_ptr<resource_handle> &block,
                                         adapter ad) {
  // The cache slot already holds `block`: any escape from here would leave waiters blocked forever.
  try {
    const auto &resolvers = root().m_resolvers;
    auto resolver_it = resolvers.find(tag.protocol());
    if (resolver_it == resolvers.end()) {
      egLOG_ERROR("res/resolver", "Protocol '{0}' is not supported, cannot load {1}", tag.protocol(), tag.view());
      block->set_error(std::string{"Unsupported protocol: "} + std::string{tag.protocol()});
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
    blocks.reserve(declared.size());
    for (const auto &dep : declared) {
      if (!dep.request) {
        throw std::runtime_error("dependency '" + std::string{dep.target.view()} + "' was not made with make_dep");
      }
      blocks.emplace_back(dep.request(*this, dep.target), dep.target);
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
                 });
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
    block->set_ready(ad->deserialize(tag, raw->data, deps));
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
};

void res::resource_system::await_blocks(
    const std::shared_ptr<resource_handle> &owner, const res::tag &owner_tag,
    std::vector<std::pair<std::shared_ptr<resource_handle>, res::tag>> blocks,
    std::function<void()> on_done) {
  if (blocks.empty()) {
    on_done();
    return;
  }

  auto state = std::make_shared<await_state>();
  state->owner = owner;
  state->blocks = std::move(blocks);
  state->on_done = std::move(on_done);
  state->pending = state->blocks.size();
  // The dependencies (a parent's blocks too) hold only this id, never the state with its continuation.
  const std::uint64_t id = register_await(state);

  const auto fail_owner = [](const std::shared_ptr<resource_handle> &block, const std::string &message) {
    if (!is_finished(*block)) {
      block->set_error(message);
    }
  };
  // The owner may have failed meanwhile (a cycle), and then nobody is left to continue. The last one drops the wait.
  const auto one_settled = [fail_owner, id](resource_system &self, const std::shared_ptr<await_state> &s) {
    if (--s->pending != 0) {
      return;
    }
    self.drop_await(id);
    if (is_finished(*s->owner)) {
      return;
    }
    std::string failure;
    {
      std::lock_guard lock(s->mutex);
      failure = s->failure;
    }
    if (failure.empty()) {
      s->on_done();
    } else {
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
        }
      }
      one_settled(*this, state);
      continue;
    }

    auto added = m_graph->add_edge(owner, owner_tag.view(), dep, dep_tag.view());
    if (added.result == wait_graph::verdict::cycle) {
      const std::string message = "dependency cycle: " + added.path;
      egLOG_ERROR("res/graph", "{}", message);
      for (const auto &member : added.members) {
        fail_owner(member, message);
      }
      drop_await(id);
      return;
    }
    if (added.result == wait_graph::verdict::too_deep) {
      const std::string message = "dependency chain of '" + std::string{owner_tag.view()} + "' is deeper than " +
                                  std::to_string(k_max_dependency_depth);
      egLOG_ERROR("res/graph", "{}", message);
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
          std::lock_guard lock(s->mutex);
          if (s->failure.empty()) {
            s->failure = "Dependency failed: " + std::string{s->blocks[i].second.view()} + ": " + settled.error_msg;
          }
        }
        one_settled(self, s);
      });
    });
  }
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

void res::resource_system::record_dependencies(const res::tag &dependent,
                                               const std::shared_ptr<resource_handle> &block,
                                               const std::vector<res_dep> &declared) {
  if (declared.empty()) {
    // The common case: nothing to record, and nothing recorded by an earlier load to drop.
    std::shared_lock lock(m_cache_mutex);
    if (!m_depends_on.contains(dependent)) {
      return;
    }
  }

  std::unique_lock lock(m_cache_mutex);
  if (!m_cache.any_of_tag(dependent, [&block](const cache_entry &entry) { return entry.lock() == block; })) {
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
  for (const auto &dep : declared) {
    // A resource that names itself fails on its own; it needs no edge to be reloaded with itself.
    if (dep.target != dependent && std::ranges::find(targets, dep.target) == targets.end()) {
      targets.push_back(dep.target);
      m_dependents[dep.target].push_back(dependent);
      retain_in_parents(dep.target);
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
  const auto is_released = [this](const res::tag &tag) {
    if (m_cache.any_of_tag(tag, [](const cache_entry &entry) { return !entry.expired(); })) {
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
    // Edges sit under the tag the block is cached by (the alias target); the watcher may have used the original tag.
    // Lock order: m_cache_mutex first, m_alias_mutex nested inside it (as in signal_changed); never the reverse.
    std::shared_lock alias_lock(m_alias_mutex);
    return std::ranges::none_of(m_aliases, [&](const auto &pair) { return pair.second == tag && has_watchers(pair.first); });
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
  signal_changed_local(tag);
}

void res::resource_system::signal_changed_local(const res::tag &tag) {
  std::vector<res::tag> batch;
  std::uint64_t epoch = 0;
  {
    std::unique_lock lock(m_cache_mutex);
    // A small graph is swept every time (cheap, and a released tag is then never evicted or delivered). A large one only
    // every k_signal_sweep_every signals, so a burst of file events does not hold the exclusive lock for a sweep each.
    if (m_depends_on.size() < k_edge_sweep_floor || ++m_signals_since_sweep >= k_signal_sweep_every) {
      sweep_edges();
    }
    batch = changed_batch(tag);
    for (const auto &changed : batch) {
      m_cache.erase_tag(changed);
    }
    // A changed original has to run its adapter again (it stores fresh data and registers the alias anew), so its
    // alias goes with its edge; the alias entry itself was evicted above as a dependent. The alias lock is taken inside
    // the cache lock, the order prune_released_edges() uses too (register_alias holds one at a time). A mapping that a
    // concurrent register_alias has replaced meanwhile is a different one and stays.
    {
      std::unique_lock alias_lock(m_alias_mutex);
      for (const auto &changed : batch) {
        const auto edge = m_alias_edges.find(changed);
        if (edge == m_alias_edges.end()) {
          continue;
        }
        if (const auto mapped = m_aliases.find(changed); mapped != m_aliases.end() && mapped->second == edge->second) {
          m_aliases.erase(mapped);
        }
        m_alias_edges.erase(edge);
      }
    }
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
    egLOG_WARN("resource/erase", "Protocol '{}' is not supported!", tag.protocol());
    return false;
  }
  if (!found->second->erase(tag)) {
    return false;
  }

  forget_erased(tag);
  signal_changed(tag);
  return true;
}

void res::resource_system::forget_erased(const res::tag &tag) {
  unpin_resource(tag);
  {
    // Lock order as in signal_changed: cache first, alias nested inside.
    std::unique_lock lock(m_cache_mutex);
    std::unique_lock alias_lock(m_alias_mutex);
    std::erase_if(m_alias_edges, [&](const auto &edge) { return edge.second == tag; });
    std::erase_if(m_aliases, [&](const auto &mapped) { return mapped.second == tag; });
  }
  std::lock_guard layers_lock(m_layers_mutex);
  for (resource_system *layer : m_layers) {
    layer->forget_erased(tag);
  }
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
  if (original == alias) {
    return;
  }
  {
    std::unique_lock lock(m_alias_mutex);
    m_aliases[original] = alias;
  }

  {
    std::unique_lock lock(m_cache_mutex);
    m_cache.copy_tag(original, alias, [](const cache_entry &entry) { return !entry.expired(); });
    // One alias per original, as in m_aliases: registering the same pair again changes nothing, another alias replaces it.
    m_alias_edges.insert_or_assign(original, alias);
  }

  {
    std::unique_lock lock(m_pinning_mutex);
    m_pinned_resources.copy_tag(original, alias, [](const auto &entry) { return entry != nullptr; });
  }
}

std::filesystem::path res::resource_system::get_resources_path() const {
  const auto layers = get_resource_layers();
  return layers.empty() ? std::filesystem::path{} : layers.back();
}

std::vector<std::filesystem::path> res::resource_system::get_resource_layers() const {
  if (m_parent) {
    return m_parent->get_resource_layers();
  }
  for (const auto &mount : m_mounts) {
    if (mount.protocol == tag::default_protocol()) {
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
    const std::filesystem::path &user_dir) {
  ASSERT_MSG(!content_roots.empty(), "A project has at least one content root");
  if (engine_base.empty()) {
    egLOG_WARN("resource/system", "resource/base_path is not set: res:// has no engine layer and engine:// is not mounted");
  }

  std::vector<fs::path> layers;
  if (!engine_base.empty())
    layers.push_back(engine_base);
  layers.insert(layers.end(), content_roots.rbegin(), content_roots.rend());

  mount_table table;
  table.push_back(mount{tag::default_protocol(), std::move(layers), mount_access::read_write});
  table.push_back(mount{tag::memory, {cache_dir / "memory"}, mount_access::read_write});
  table.push_back(mount{tag::cache, {cache_dir}, mount_access::read_write});
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
    gate->with([&](resource_system &self) {
      self.push_resource_to_strong_cache(key, held);
      // In a layer the block may be the parent's: the layer's cache entry is what its requests find, and what a
      // change evicts (the pin alone would keep serving the old block).
      if (self.m_parent && !self.find_cached(key)) {
        self.push_resource_to_cache(key, held);
      }
    });
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
