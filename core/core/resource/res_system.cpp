#include <snk/resource/res_system.h>
#include <snk/resource/adapters/res_pct_adapter.h>
#include <snk/resource/adapters/res_text_adapter.h>
#include <snk/config/cfg_api.h>
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
    : m_graph(std::make_unique<wait_graph>()), m_tasks(tasks) {
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

void res::resource_system::start_loading(const res::tag &tag,
                                         const std::shared_ptr<resource_handle> &block,
                                         adapter ad) {
  // The cache slot already holds `block`: any escape from here would leave waiters blocked forever.
  try {
    auto resolver_it = m_resolvers.find(tag.protocol());
    if (resolver_it == m_resolvers.end()) {
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

    raw->on_settled([this, tag, block, raw, ad = std::move(ad)](auto &) {
      run_on_compute([this, tag, block, raw, ad] { collect_stage(tag, block, raw, ad); }, block);
    });
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

  const auto fail_owner = [](const std::shared_ptr<resource_handle> &block, const std::string &message) {
    if (!is_finished(*block)) {
      block->set_error(message);
    }
  };
  // The owner may have failed meanwhile (a cycle), and then nobody is left to continue.
  const auto one_settled = [fail_owner](const std::shared_ptr<await_state> &s) {
    if (--s->pending != 0 || is_finished(*s->owner)) {
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
      one_settled(state);
      continue;
    }

    auto added = m_graph->add_edge(owner, owner_tag.view(), dep, dep_tag.view());
    if (added.result == wait_graph::verdict::cycle) {
      const std::string message = "dependency cycle: " + added.path;
      egLOG_ERROR("res/graph", "{}", message);
      for (const auto &member : added.members) {
        fail_owner(member, message);
      }
      return;
    }
    if (added.result == wait_graph::verdict::too_deep) {
      const std::string message = "dependency chain of '" + std::string{owner_tag.view()} + "' is deeper than " +
                                  std::to_string(k_max_dependency_depth);
      egLOG_ERROR("res/graph", "{}", message);
      fail_owner(owner, message);
      return;
    }

    dep->on_settled([this, state, i, one_settled](auto &settled) {
      m_graph->remove_edge(state->owner.get(), &settled);
      if (settled.has_error()) {
        std::lock_guard lock(state->mutex);
        if (state->failure.empty()) {
          state->failure = "Dependency failed: " + std::string{state->blocks[i].second.view()} + ": " + settled.error_msg;
        }
      }
      one_settled(state);
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
  // whether or not the tag has been loaded again. A load in flight has an entry, and a tag that loads again records
  // its edges anew, so nothing is lost.
  const auto is_released = [this](const res::tag &tag) {
    if (m_cache.any_of_tag(tag, [](const cache_entry &entry) { return !entry.expired(); })) {
      return false;
    }
    if (m_cache.any_of_tag(tag, [](const cache_entry &) { return true; })) {
      return true;
    }
    const auto has_watchers = [this](const res::tag &key) {
      const auto it = m_watchers.find(key);
      return it != m_watchers.end() && !it->second.empty();
    };
    if (has_watchers(tag)) {
      return false;
    }
    // Edges sit under the tag the block is cached by (the alias target); the watcher may have used the original tag.
    // m_alias_mutex is never held while m_cache_mutex is taken, so this order cannot deadlock.
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
    }
    m_depends_on.erase(it);
  }
}

std::vector<res::tag> res::resource_system::changed_batch(const res::tag &root) const {
  // Breadth first over the reverse edges; `seen` is what keeps a cycle from looping.
  // An alias edge (original -> alias) is followed like a recorded one but kept apart from them: pruning and the
  // reload of a tag rewrite the recorded ones, and must not drop it.
  const auto for_each_dependent = [this](const res::tag &tag, const auto &visit) {
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
    epoch = ++m_watch_epoch;
  }

  // One task per signal, the batch in one piece: its order is the delivery order.
  m_tasks.post_main([this, batch = std::move(batch), epoch, alive = m_lifetime.token()]() {
    if (alive.expired()) {
      return;
    }
    for (const auto &changed : batch) {
      deliver_to_watchers(changed, epoch);
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
    // One alias per original, as in m_aliases: registering the same pair again changes nothing, another alias replaces it.
    if (original == alias) {
      m_alias_edges.erase(original);
    } else {
      m_alias_edges.insert_or_assign(original, alias);
    }
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

