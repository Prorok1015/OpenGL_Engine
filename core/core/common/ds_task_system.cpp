#include <snk/common/ds/ds_task_system.hpp>

#include <algorithm>
#include <array>
#include <deque>
#include <exception>
#include <format>
#include <thread>
#include <unordered_map>

#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>

#include <snk/common/core_thread_utils.hpp>
#include <snk/common/logger/engine_log.h>

namespace ds::detail {

	namespace {
		thread_local bool g_on_worker = false;

		struct worker_scope {
			bool previous;
			worker_scope() : previous(g_on_worker) { g_on_worker = true; }
			~worker_scope() { g_on_worker = previous; }
			worker_scope(const worker_scope&) = delete;
			worker_scope& operator=(const worker_scope&) = delete;
		};

		constexpr std::size_t lane_index(lane l) noexcept { return static_cast<std::size_t>(l); }
		constexpr std::size_t class_index(main_class c) noexcept { return static_cast<std::size_t>(c); }

		void run_guarded(const std::function<void()>& fn, const char* what) noexcept
		{
			try {
				fn();
			} catch (const std::exception& e) {
				egLOG_ERROR("task", "{} threw: {}", what, e.what());
			} catch (...) {
				egLOG_ERROR("task", "{} threw an unknown exception", what);
			}
		}
	}

	bool on_worker_thread() noexcept
	{
		return g_on_worker;
	}

	// A main-thread continuation of a task bound to a lifetime: run once, by the queue or by run_pending_main.
	struct main_item {
		std::function<void()> fn;
		std::atomic<bool> taken{ false };

		void run()
		{
			if (!taken.exchange(true)) {
				auto call = std::move(fn); // the captures go with it, right after the call
				run_guarded(call, "main-thread task");
			}
		}
	};

	struct lifetime_state {
		explicit lifetime_state(bool cancel) : cancel_on_destroy(cancel) {}

		const bool cancel_on_destroy;
		const std::thread::id owner_thread = std::this_thread::get_id();
		std::mutex mutex;
		std::vector<std::weak_ptr<task_state_base>> tasks;
		// Callbacks the owner holds (hold_callback); cleared by ~lifetime.
		std::unordered_map<const void*, std::shared_ptr<void>> held;
		// Queued main-thread continuations of the bound tasks (the queue owns them).
		std::vector<std::weak_ptr<main_item>> main_items;
	};

	void hold_callback(const std::weak_ptr<lifetime_state>& owner, std::shared_ptr<void> callback)
	{
		if (const auto state = owner.lock()) {
			std::lock_guard lock(state->mutex);
			const void* key = callback.get();
			state->held.emplace(key, std::move(callback));
		}
	}

	void release_callback(const std::weak_ptr<lifetime_state>& owner, const void* callback)
	{
		std::shared_ptr<void> released; // destructed after the unlock
		if (const auto state = owner.lock()) {
			std::lock_guard lock(state->mutex);
			if (const auto it = state->held.find(callback); it != state->held.end()) {
				released = std::move(it->second);
				state->held.erase(it);
			}
		}
	}

	struct task_system_impl {
		using work_queue = std::deque<std::function<void()>>;

		explicit task_system_impl(task_system_config c)
			: cfg(std::move(c))
			, main_id(std::this_thread::get_id())
			, budget_ns(cfg.main_budget.count())
			, gpu_limit(cfg.gpu_upload_per_frame)
		{
			if (!cfg.clock) {
				cfg.clock = [] { return std::chrono::steady_clock::now(); };
			}
			if (cfg.mode != policy::threaded) {
				return;
			}
			const std::size_t hw = std::max<std::size_t>(1, std::thread::hardware_concurrency());
			threads[lane_index(lane::compute)] = cfg.compute_threads != 0
				? cfg.compute_threads
				: std::clamp<std::size_t>(hw - 1, 2, 8);
			threads[lane_index(lane::io)] = std::max<std::size_t>(1, cfg.io_threads);
			start_pool(lane::compute, "compute");
			start_pool(lane::io, "io");
		}

		// Names every pool thread: each of the n tasks blocks until all n have arrived, so no
		// thread can take two of them.
		void start_pool(lane which, const char* prefix)
		{
			const std::size_t n = threads[lane_index(which)];
			auto& pool = pools[lane_index(which)];
			pool = std::make_unique<boost::asio::thread_pool>(n);
			struct barrier {
				std::mutex mutex;
				std::condition_variable cv;
				std::size_t arrived = 0;
			};
			auto sync = std::make_shared<barrier>();
			for (std::size_t i = 0; i < n; ++i) {
				boost::asio::post(*pool, [sync, n, prefix, i] {
					g_on_worker = true;
					core::thread::set_current_name(std::format("snk-{}-{}", prefix, i));
					std::unique_lock lock(sync->mutex);
					++sync->arrived;
					sync->cv.notify_all();
					sync->cv.wait(lock, [&] { return sync->arrived == n; });
				});
			}
		}

		void enqueue(lane which, std::function<void()> fn)
		{
			if (cfg.mode == policy::manual) {
				std::lock_guard lock(lane_mutex);
				lane_queues[lane_index(which)].push_back(std::move(fn));
				return;
			}
			boost::asio::post(*pools[lane_index(which)], std::move(fn));
		}

		bool run_one(lane which)
		{
			std::function<void()> fn;
			{
				std::lock_guard lock(lane_mutex);
				auto& queue = lane_queues[lane_index(which)];
				if (queue.empty()) {
					return false;
				}
				fn = std::move(queue.front());
				queue.pop_front();
			}
			worker_scope scope;
			fn();
			return true;
		}

		void post_main(std::function<void()> fn, main_class cls)
		{
			std::lock_guard lock(main_mutex);
			if (main_closed) {
				return;
			}
			main_queues[class_index(cls)].push_back(std::move(fn));
		}

		std::function<void()> pop_main(main_class cls)
		{
			std::lock_guard lock(main_mutex);
			auto& queue = main_queues[class_index(cls)];
			if (queue.empty()) {
				return {};
			}
			auto fn = std::move(queue.front());
			queue.pop_front();
			return fn;
		}

		void register_live(const std::shared_ptr<task_state_base>& state)
		{
			std::lock_guard lock(live_mutex);
			live.emplace(state->id(), state);
		}

		void unregister_live(std::uint64_t id)
		{
			std::lock_guard lock(live_mutex);
			live.erase(id);
		}

		void enqueue_task(lane which, const std::shared_ptr<task_state_base>& state)
		{
			if (shutting_down.load()) {
				state->cancel();
				return;
			}
			enqueue(which, [state] { state->execute(); });
		}

		task_system_config cfg;
		const std::thread::id main_id;
		std::atomic<std::int64_t> budget_ns;
		std::atomic<std::size_t> gpu_limit;
		std::array<std::size_t, 2> threads{};
		std::array<std::unique_ptr<boost::asio::thread_pool>, 2> pools;

		std::mutex lane_mutex;
		std::array<work_queue, 2> lane_queues;

		std::mutex main_mutex;
		std::array<work_queue, 2> main_queues;
		bool main_closed = false;

		std::atomic<bool> shutting_down{ false };
		std::atomic<std::uint64_t> next_id{ 1 };
		std::mutex live_mutex;
		std::unordered_map<std::uint64_t, std::weak_ptr<task_state_base>> live;
	};

	task_state_base::task_state_base(task_desc desc, std::weak_ptr<task_system_impl> sys, std::uint64_t id)
		: m_desc(std::move(desc))
		, m_id(id)
		, m_sys(std::move(sys))
	{
	}

	task_state_base::~task_state_base() = default;

	std::string task_state_base::progress_text() const
	{
		std::lock_guard lock(m_mutex);
		return m_progress_text;
	}

	std::string task_state_base::error() const
	{
		if (status() != task_status::error) {
			return {};
		}
		std::lock_guard lock(m_mutex);
		return m_error;
	}

	bool task_state_base::on_main_thread() const
	{
		auto sys = m_sys.lock();
		return !sys || sys->main_id == std::this_thread::get_id();
	}

	void task_state_base::set_progress(float value, std::string_view text)
	{
		m_progress.store(std::clamp(value, 0.f, 1.f), std::memory_order_relaxed);
		if (!text.empty()) {
			std::lock_guard lock(m_mutex);
			m_progress_text.assign(text);
		}
	}

	void task_state_base::cancel()
	{
		m_stop.request_stop();
		finish_unrun(task_status::cancelled);
	}

	bool task_state_base::finish_pending(task_status outcome, std::string message)
	{
		if (!message.empty()) {
			std::lock_guard lock(m_mutex);
			m_error = std::move(message);
		}
		return finish_unrun(outcome);
	}

	bool task_state_base::finish_unrun(task_status outcome)
	{
		// Claimed as running, so execute() cannot take the body while it is dropped. A queue entry outlives a
		// cancelled task until a worker pops it: without the drop, its captures would outlive wait_idle().
		task_status expected = task_status::pending;
		if (!m_status.compare_exchange_strong(expected, task_status::running, std::memory_order_acq_rel)) {
			return false;
		}
		drop_body();
		m_status.store(outcome, std::memory_order_release);
		finalize();
		return true;
	}

	bool task_state_base::execute()
	{
		task_status expected = task_status::pending;
		if (!m_status.compare_exchange_strong(expected, task_status::running, std::memory_order_acq_rel)) {
			return false;
		}

		task_context ctx;
		ctx.stop = m_stop.get_token();
		ctx.m_state = this;

		task_status outcome = task_status::ready;
		try {
			invoke(ctx);
		} catch (const task_cancelled&) {
			outcome = task_status::cancelled;
		} catch (const std::exception& e) {
			std::lock_guard lock(m_mutex);
			m_error = e.what();
			outcome = task_status::error;
		} catch (...) {
			std::lock_guard lock(m_mutex);
			m_error = "unknown exception";
			outcome = task_status::error;
		}
		// A body that returned after cancel() was asked to stop: its result is not trusted.
		if (outcome == task_status::ready) {
			if (m_stop.stop_requested()) {
				outcome = task_status::cancelled;
			} else {
				m_progress.store(1.f, std::memory_order_relaxed);
			}
		}
		m_status.store(outcome, std::memory_order_release);
		finalize();
		return true;
	}

	void task_state_base::wait()
	{
		ASSERT_MSG(!on_worker_thread(), "task wait(): not allowed on a pool thread, it would stall the pool");
		ASSERT_MSG(on_main_thread(), "task wait(): main thread only");
		if (done()) {
			return;
		}
		auto sys = m_sys.lock();
		if (sys && sys->cfg.mode == policy::manual) {
			while (!done()) {
				if (!sys->run_one(lane::compute) && !sys->run_one(lane::io)) {
					egLOG_ERROR("task", "wait('{}'): nothing left to run in manual mode", m_desc.name);
					return;
				}
			}
			return;
		}
		std::unique_lock lock(m_mutex);
		m_cv.wait(lock, [this] { return done(); });
	}

	void task_state_base::set_origin(std::weak_ptr<lifetime_state> origin)
	{
		std::lock_guard lock(m_mutex);
		m_origin = std::move(origin);
	}

	std::function<void()> task_state_base::for_main(std::function<void()> fn)
	{
		std::weak_ptr<lifetime_state> origin;
		{
			std::lock_guard lock(m_mutex);
			origin = m_origin;
		}
		const auto state = origin.lock();
		if (!state) {
			return fn;
		}
		auto item = std::make_shared<main_item>();
		item->fn = std::move(fn);
		{
			std::lock_guard lock(state->mutex);
			std::erase_if(state->main_items, [](const std::weak_ptr<main_item>& weak) {
				const auto queued = weak.lock();
				return !queued || queued->taken.load();
			});
			state->main_items.push_back(item);
		}
		return [item] { item->run(); };
	}

	void run_pending_main(const lifetime& owner)
	{
		const auto state = owner.token().lock();
		if (!state) {
			return;
		}
		ASSERT_MSG(std::this_thread::get_id() == state->owner_thread, "run_pending_main: the owner's thread only");
		std::vector<std::shared_ptr<main_item>> items;
		{
			std::lock_guard lock(state->mutex);
			for (const auto& weak : state->main_items) {
				if (auto queued = weak.lock()) {
					items.push_back(std::move(queued));
				}
			}
			state->main_items.clear();
		}
		for (const auto& item : items) {
			item->run();
		}
	}

	void task_state_base::add_continuation(bool on_main, std::function<void()> fn)
	{
		{
			std::lock_guard lock(m_mutex);
			if (!done()) {
				m_continuations.push_back({ on_main, std::move(fn) });
				return;
			}
		}
		if (!on_main) {
			run_guarded(fn, "task continuation");
		} else if (auto sys = m_sys.lock()) {
			sys->post_main(for_main(std::move(fn)), main_class::general);
		}
	}

	void task_state_base::finalize()
	{
		std::vector<continuation> pending;
		{
			std::lock_guard lock(m_mutex);
			pending.swap(m_continuations);
		}
		m_cv.notify_all();
		auto sys = m_sys.lock();
		if (sys) {
			sys->unregister_live(m_id);
		}
		for (auto& c : pending) {
			if (!c.on_main) {
				run_guarded(c.fn, "task continuation");
			} else if (sys) {
				sys->post_main(for_main(std::move(c.fn)), main_class::general);
			}
		}
	}

} // namespace ds::detail

namespace ds {

	lifetime::lifetime(lifetime_option option)
		: m_state(std::make_shared<detail::lifetime_state>(option == lifetime_option::cancel_on_destroy))
	{
	}

	lifetime::~lifetime()
	{
		ASSERT_MSG(std::this_thread::get_id() == m_state->owner_thread, "lifetime: destroy on the thread that created it");
		if (m_state->cancel_on_destroy) {
			cancel_tasks();
		}
		decltype(m_state->held) released; // destructed after the unlock: the captures go now, on this thread
		{
			std::lock_guard lock(m_state->mutex);
			released.swap(m_state->held);
		}
	}

	void lifetime::cancel_tasks() const
	{
		ASSERT_MSG(std::this_thread::get_id() == m_state->owner_thread, "lifetime::cancel_tasks: owner thread only");
		for (const auto& state : bound()) {
			state->cancel();
		}
	}

	void lifetime::bind(const std::shared_ptr<detail::task_state_base>& state) const
	{
		state->set_origin(m_state);
		std::vector<std::shared_ptr<detail::task_state_base>> released; // destructed after the unlock
		std::lock_guard lock(m_state->mutex);
		std::erase_if(m_state->tasks, [&released](const std::weak_ptr<detail::task_state_base>& weak) {
			auto alive = weak.lock();
			const bool drop = !alive || alive->done();
			if (alive) {
				released.push_back(std::move(alive));
			}
			return drop;
		});
		m_state->tasks.push_back(state);
	}

	std::vector<std::shared_ptr<detail::task_state_base>> lifetime::bound() const
	{
		std::vector<std::shared_ptr<detail::task_state_base>> out;
		std::lock_guard lock(m_state->mutex);
		out.reserve(m_state->tasks.size());
		for (const auto& weak : m_state->tasks) {
			if (auto state = weak.lock()) {
				out.push_back(std::move(state));
			}
		}
		return out;
	}

	void task_context::set_progress(float value, std::string_view text)
	{
		if (m_state) {
			m_state->set_progress(value, text);
		}
	}

	task_system::task_system(task_system_config config)
		: m_impl(std::make_shared<detail::task_system_impl>(std::move(config)))
	{
	}

	task_system::~task_system()
	{
		auto& impl = *m_impl;
		impl.shutting_down.store(true);

		std::vector<std::shared_ptr<detail::task_state_base>> alive;
		{
			std::lock_guard lock(impl.live_mutex);
			alive.reserve(impl.live.size());
			for (auto& [id, weak] : impl.live) {
				if (auto state = weak.lock()) {
					alive.push_back(std::move(state));
				}
			}
		}
		for (auto& state : alive) {
			state->cancel();
		}

		// A body that ignores stop would hang join() with no clue why; give cooperative ones a moment.
		const auto grace_end = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		const auto still_running = [&alive] {
			return std::ranges::any_of(alive, [](const auto& state) { return !state->done(); });
		};
		while (still_running() && std::chrono::steady_clock::now() < grace_end) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		for (const auto& state : alive) {
			if (!state->done()) {
				egLOG_WARN("task", "shutdown: task '{}' (#{}) is still running after stop request, join() waits for it",
					state->desc().name, state->id());
			}
		}
		alive.clear();

		for (auto& pool : impl.pools) {
			if (pool) {
				pool->join();
				pool.reset();
			}
		}

		// Callbacks are destroyed, never invoked: their owners may already be gone.
		std::array<detail::task_system_impl::work_queue, 2> dropped_lane;
		std::array<detail::task_system_impl::work_queue, 2> dropped_main;
		{
			std::lock_guard lock(impl.lane_mutex);
			dropped_lane.swap(impl.lane_queues);
		}
		{
			std::lock_guard lock(impl.main_mutex);
			impl.main_closed = true;
			dropped_main.swap(impl.main_queues);
		}
	}

	std::weak_ptr<detail::task_system_impl> task_system::weak_impl() const
	{
		return m_impl;
	}

	std::uint64_t task_system::next_id()
	{
		return m_impl->next_id.fetch_add(1);
	}

	void task_system::submit(lane where, const std::shared_ptr<detail::task_state_base>& state)
	{
		m_impl->register_live(state);
		// Re-checked after registering: a shutdown that started in between has not seen this task.
		m_impl->enqueue_task(where, state);
	}

	task_handle<void> task_system::when_all(std::span<const task_handle_base> inputs, lane where,
		std::function<void(task_context&)> fn)
	{
		using state_t = detail::task_state<void>;
		auto state = std::make_shared<state_t>(task_desc{ "when_all", {}, false }, weak_impl(), next_id(),
			state_t::body_t(std::move(fn)));
		m_impl->register_live(state);

		struct gate {
			std::atomic<std::size_t> remaining{ 0 };
			std::vector<std::shared_ptr<detail::task_state_base>> inputs;
		};
		auto shared_gate = std::make_shared<gate>();
		for (const task_handle_base& input : inputs) {
			ASSERT_MSG(input.valid(), "task_system::when_all: empty input handle");
			if (input.valid()) {
				shared_gate->inputs.push_back(input.m_state);
			}
		}
		shared_gate->remaining.store(shared_gate->inputs.size() + 1);

		auto arrive = [weak = weak_impl(), state, shared_gate, where] {
			if (shared_gate->remaining.fetch_sub(1, std::memory_order_acq_rel) != 1) {
				return;
			}
			std::string failure;
			bool any_cancelled = false;
			for (const auto& input : shared_gate->inputs) {
				const task_status s = input->status();
				if (s == task_status::error && failure.empty()) {
					failure = std::format("input '{}' failed: {}", input->desc().name, input->error());
				} else if (s == task_status::cancelled) {
					any_cancelled = true;
				}
			}
			if (!failure.empty()) {
				state->finish_pending(task_status::error, std::move(failure));
			} else if (any_cancelled) {
				state->finish_pending(task_status::cancelled, {});
			} else if (auto impl = weak.lock()) {
				impl->enqueue_task(where, state);
			} else {
				state->cancel();
			}
		};
		for (const auto& input : shared_gate->inputs) {
			input->add_continuation(false, arrive);
		}
		arrive(); // releases the guard count held while the continuations were being attached
		return task_handle<void>(std::move(state));
	}

	void task_system::wait_idle(const lifetime& owner)
	{
		ASSERT_MSG(on_main(), "task_system::wait_idle: main thread only");
		for (bool waited = true; waited;) {
			waited = false;
			for (const auto& state : owner.bound()) {
				if (!state->done()) {
					state->wait();
					waited = true;
				}
			}
		}
	}

	void task_system::post_main(std::function<void()> fn, main_class cls)
	{
		m_impl->post_main(std::move(fn), cls);
	}

	std::size_t task_system::drain_main()
	{
		return drain_main(std::chrono::nanoseconds(m_impl->budget_ns.load()));
	}

	std::size_t task_system::drain_main(std::chrono::nanoseconds budget)
	{
		ASSERT_MSG(on_main(), "task_system::drain_main: main thread only");
		auto& impl = *m_impl;
		std::size_t ran = 0;

		const auto start = impl.cfg.clock();
		while (auto fn = impl.pop_main(main_class::general)) {
			detail::run_guarded(fn, "main-thread task");
			++ran;
			if (impl.cfg.clock() - start >= budget) {
				break;
			}
		}

		const std::size_t limit = impl.gpu_limit.load();
		for (std::size_t i = 0; i < limit; ++i) {
			auto fn = impl.pop_main(main_class::gpu_upload);
			if (!fn) {
				break;
			}
			detail::run_guarded(fn, "gpu upload");
			++ran;
		}
		return ran;
	}

	bool task_system::run_one(lane which)
	{
		ASSERT_MSG(m_impl->cfg.mode == policy::manual, "task_system::run_one: manual policy only");
		return m_impl->cfg.mode == policy::manual && m_impl->run_one(which);
	}

	bool task_system::try_run_inline(std::uint64_t task_id)
	{
		std::shared_ptr<detail::task_state_base> state;
		{
			std::lock_guard lock(m_impl->live_mutex);
			if (auto it = m_impl->live.find(task_id); it != m_impl->live.end()) {
				state = it->second.lock();
			}
		}
		if (!state) {
			return false;
		}
		detail::worker_scope scope;
		return state->execute(); // the pool job of this task finds it claimed and does nothing
	}

	std::vector<task_info> task_system::snapshot() const
	{
		std::vector<std::shared_ptr<detail::task_state_base>> alive;
		{
			std::lock_guard lock(m_impl->live_mutex);
			alive.reserve(m_impl->live.size());
			for (const auto& [id, weak] : m_impl->live) {
				if (auto state = weak.lock()) {
					alive.push_back(std::move(state));
				}
			}
		}
		std::sort(alive.begin(), alive.end(), [](const auto& a, const auto& b) { return a->id() < b->id(); });

		std::vector<task_info> infos;
		infos.reserve(alive.size());
		for (const auto& state : alive) {
			if (!state->desc().visible || state->done()) {
				continue;
			}
			infos.push_back(task_info{ state->id(), state->desc().name, state->desc().category,
				state->status(), state->progress(), state->progress_text() });
		}
		return infos;
	}

	policy task_system::mode() const noexcept
	{
		return m_impl->cfg.mode;
	}

	std::size_t task_system::thread_count(lane which) const noexcept
	{
		return m_impl->threads[static_cast<std::size_t>(which)];
	}

	std::size_t task_system::pending_main(main_class cls) const
	{
		std::lock_guard lock(m_impl->main_mutex);
		return m_impl->main_queues[static_cast<std::size_t>(cls)].size();
	}

	void task_system::set_main_budget(std::chrono::nanoseconds budget) noexcept
	{
		m_impl->budget_ns.store(budget.count());
	}

	void task_system::set_gpu_upload_limit(std::size_t per_frame) noexcept
	{
		m_impl->gpu_limit.store(per_frame);
	}

	bool task_system::on_main() const noexcept
	{
		return m_impl->main_id == std::this_thread::get_id();
	}

} // namespace ds
