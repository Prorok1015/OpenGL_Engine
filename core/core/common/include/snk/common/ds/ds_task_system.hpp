#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <concepts>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include <snk/common/engine_assert.h>
#include <snk/common/snk_api.h>

namespace ds {

	enum class lane : std::uint8_t {
		compute, // adapters, import: clamp(hw-1, 2, 8) threads
		io,      // blocking files / processes: 2 threads
	};

	// threaded: asio pools. manual: no threads at all, tests drive run_one() / drain_main().
	enum class policy : std::uint8_t { threaded, manual };

	enum class main_class : std::uint8_t {
		general,    // time budget per frame, at least one item
		gpu_upload, // at most N items per frame
	};

	enum class task_status : std::uint8_t { pending, running, ready, error, cancelled };

	struct task_desc {
		std::string name{};
		std::string category{};
		bool visible = true;
	};

	// Thrown by a task body to finish as `cancelled` (task_context::throw_if_stopped).
	struct task_cancelled {};

	class SNK_API task_system;

	namespace detail {
		struct task_system_impl;
		class SNK_API task_state_base;

		// True on a pool thread (and while task_system::run_one executes a task).
		[[nodiscard]] SNK_API bool on_worker_thread() noexcept;

		struct lifetime_state;
	}

	enum class lifetime_option : std::uint8_t {
		none,
		cancel_on_destroy, // the destructor requests stop on every task still bound to it
	};

	// A member of the object that receives results. Create, bind and destroy it on the main thread.
	// Not copyable or movable: callbacks capture the owner's address, so a token cannot outlive it.
	class SNK_API lifetime
	{
	public:
		explicit lifetime(lifetime_option option = lifetime_option::none);
		~lifetime();
		lifetime(const lifetime&) = delete;
		lifetime& operator=(const lifetime&) = delete;

		// Requests stop on every task bound so far (cancel_on_destroy does this in the destructor).
		void cancel_tasks() const;
		[[nodiscard]] std::weak_ptr<detail::lifetime_state> token() const { return m_state; }

	private:
		friend class task_system;
		void bind(const std::shared_ptr<detail::task_state_base>& state) const;
		[[nodiscard]] std::vector<std::shared_ptr<detail::task_state_base>> bound() const;

		std::shared_ptr<detail::lifetime_state> m_state;
	};

	class task_context
	{
	public:
		std::stop_token stop;

		SNK_API void set_progress(float value, std::string_view text = {});
		void throw_if_stopped() const
		{
			if (stop.stop_requested()) {
				throw task_cancelled{};
			}
		}

	private:
		friend class detail::task_state_base;
		detail::task_state_base* m_state = nullptr;
	};

	template<class T>
	struct task_result {
		task_status status = task_status::pending;
		std::string error;
		T* value = nullptr; // valid only inside the then_main callback, only when ok()

		[[nodiscard]] bool ok() const noexcept { return status == task_status::ready; }
	};

	template<>
	struct task_result<void> {
		task_status status = task_status::pending;
		std::string error;

		[[nodiscard]] bool ok() const noexcept { return status == task_status::ready; }
	};

	namespace detail {

		// One task's shared state: owned by the pool queue and by every handle.
		class SNK_API task_state_base
		{
		public:
			task_state_base(task_desc desc, std::weak_ptr<task_system_impl> sys, std::uint64_t id);
			virtual ~task_state_base();
			task_state_base(const task_state_base&) = delete;
			task_state_base& operator=(const task_state_base&) = delete;

			[[nodiscard]] task_status status() const noexcept { return m_status.load(std::memory_order_acquire); }
			[[nodiscard]] bool done() const noexcept
			{
				const task_status s = status();
				return s == task_status::ready || s == task_status::error || s == task_status::cancelled;
			}
			[[nodiscard]] float progress() const noexcept { return m_progress.load(std::memory_order_relaxed); }
			[[nodiscard]] std::string progress_text() const;
			[[nodiscard]] std::string error() const;
			[[nodiscard]] std::uint64_t id() const noexcept { return m_id; }
			[[nodiscard]] const task_desc& desc() const noexcept { return m_desc; }
			// True on the thread that created the owning task_system (or if that is already gone).
			[[nodiscard]] bool on_main_thread() const;

			// Cooperative; a task that has not started yet finishes as cancelled and never runs.
			void cancel();
			// Runs the body on the calling (lane) thread. No-op unless the task is still pending.
			void execute();
			void wait();

			// A continuation of a finished task. `on_main` ones go through post_main(general),
			// the others run inline on the thread that completes the task (keep them short).
			void add_continuation(bool on_main, std::function<void()> fn);
			// pending -> `outcome`; false if the task already left `pending`.
			bool finish_pending(task_status outcome, std::string message);

			void set_progress(float value, std::string_view text);

		protected:
			virtual void invoke(task_context& ctx) = 0;

		private:
			void finalize();

			const task_desc m_desc;
			const std::uint64_t m_id;
			std::weak_ptr<task_system_impl> m_sys;
			std::stop_source m_stop;
			std::atomic<task_status> m_status{ task_status::pending };
			std::atomic<float> m_progress{ 0.f };

			mutable std::mutex m_mutex;
			std::condition_variable m_cv;
			std::string m_progress_text;
			std::string m_error;
			struct continuation { bool on_main; std::function<void()> fn; };
			std::vector<continuation> m_continuations;
		};

		struct void_value {};

		template<class T>
		class task_state final : public task_state_base
		{
		public:
			using body_t = std::function<T(task_context&)>;
			using stored_t = std::conditional_t<std::is_void_v<T>, void_value, T>;

			task_state(task_desc desc, std::weak_ptr<task_system_impl> sys, std::uint64_t id, body_t body)
				: task_state_base(std::move(desc), std::move(sys), id)
				, m_body(std::move(body))
			{
			}

			std::optional<stored_t> value;

		protected:
			void invoke(task_context& ctx) override
			{
				body_t body = std::move(m_body);
				m_body = nullptr; // drop the captures as soon as the body is done
				if constexpr (std::is_void_v<T>) {
					body(ctx);
				} else {
					value.emplace(body(ctx));
				}
			}

		private:
			body_t m_body;
		};

	} // namespace detail

	// Type-erased view of a task, what when_all() takes.
	class task_handle_base
	{
	public:
		task_handle_base() = default;

		[[nodiscard]] bool valid() const noexcept { return m_state != nullptr; }
		[[nodiscard]] task_status status() const noexcept { return m_state ? m_state->status() : task_status::cancelled; }
		[[nodiscard]] bool done() const noexcept { return !m_state || m_state->done(); }
		[[nodiscard]] float progress() const noexcept { return m_state ? m_state->progress() : 0.f; }
		[[nodiscard]] std::string progress_text() const { return m_state ? m_state->progress_text() : std::string{}; }
		// Failure message as a value; empty unless status() == error.
		[[nodiscard]] std::string error() const { return m_state ? m_state->error() : std::string{}; }
		[[nodiscard]] std::uint64_t id() const noexcept { return m_state ? m_state->id() : 0; }

		void cancel() const
		{
			if (m_state) {
				m_state->cancel();
			}
		}
		// Main thread only. In `manual` mode it runs queued tasks itself instead of blocking.
		void wait() const
		{
			if (m_state) {
				m_state->wait();
			}
		}

	protected:
		explicit task_handle_base(std::shared_ptr<detail::task_state_base> state) : m_state(std::move(state)) {}

		friend class task_system;
		std::shared_ptr<detail::task_state_base> m_state;
	};

	template<class T>
	class task_handle : public task_handle_base
	{
	public:
		using state_t = detail::task_state<T>;
		using callback_t = std::function<void(task_result<T>&)>;

		task_handle() = default;

		// Main thread only, only after the task is ready.
		template<class U = T> requires(!std::is_void_v<U>)
		[[nodiscard]] const U& get() const
		{
			ASSERT_MSG(m_state && status() == task_status::ready, "task_handle::get: task is not ready");
			ASSERT_MSG(m_state->on_main_thread(), "task_handle::get: main thread only");
			return *state().value;
		}

		// Runs `fn` on the main thread (post_main, general class) once the task has finished,
		// whatever the outcome. If it has finished already, `fn` is queued right away.
		// `fn` must be copyable (std::function): move-only captures do not compile.
		void then_main(callback_t fn) const
		{
			ASSERT_MSG(m_state, "task_handle::then_main: empty handle");
			if (!m_state) {
				return;
			}
			m_state->add_continuation(true, [st = std::static_pointer_cast<state_t>(m_state), fn = std::move(fn)]() {
				deliver(*st, fn);
			});
		}

		// Same, but dropped without a call if `owner` is already destroyed; checked on the main
		// thread right before the call, where the owner dies too, so it cannot die in between.
		void then_main(const lifetime& owner, callback_t fn) const
		{
			ASSERT_MSG(m_state, "task_handle::then_main: empty handle");
			if (!m_state) {
				return;
			}
			m_state->add_continuation(true, [st = std::static_pointer_cast<state_t>(m_state), weak = owner.token(),
				fn = std::move(fn)]() {
				if (!weak.expired()) {
					deliver(*st, fn);
				}
			});
		}

	private:
		static void deliver(state_t& st, const callback_t& fn)
		{
			task_result<T> result;
			result.status = st.status();
			result.error = st.error();
			if constexpr (!std::is_void_v<T>) {
				if (st.value) {
					result.value = &*st.value;
				}
			}
			fn(result);
		}

		friend class task_system;
		explicit task_handle(std::shared_ptr<state_t> state) : task_handle_base(std::move(state)) {}

		state_t& state() const { return static_cast<state_t&>(*m_state); }
	};

	struct task_info {
		std::uint64_t id = 0;
		std::string name;
		std::string category;
		task_status status = task_status::pending;
		float progress = 0.f;
		std::string progress_text;
	};

	struct task_system_config {
		policy mode = policy::threaded;
		std::size_t compute_threads = 0; // 0: clamp(hardware_concurrency - 1, 2, 8)
		std::size_t io_threads = 2;
		std::chrono::nanoseconds main_budget = std::chrono::milliseconds(2);
		std::size_t gpu_upload_per_frame = 4;
		// Time source of the main-queue budget; tests inject a fake one. Default: steady_clock.
		std::function<std::chrono::steady_clock::time_point()> clock;
	};

	// Service (ds::app_data_storage). Created before resource_system, destroyed after it.
	// Its "main thread" is the creating one: core_module::register_services runs on the thread of
	// main(), which is also the frame-loop thread (application::run is called from the same main()).
	class SNK_API task_system
	{
	public:
		explicit task_system(task_system_config config = {});
		// request_stop on every task -> join the pools -> drop the main queue without calling it.
		~task_system();
		task_system(const task_system&) = delete;
		task_system& operator=(const task_system&) = delete;

		// `fn` must be copyable (std::function). The body must not touch the owner of a lifetime.
		template<class F>
		auto run(lane where, task_desc desc, F&& fn)
		{
			using result_t = std::invoke_result_t<F&, task_context&>;
			using state_t = detail::task_state<result_t>;
			auto state = std::make_shared<state_t>(std::move(desc), weak_impl(), next_id(),
				typename state_t::body_t(std::forward<F>(fn)));
			submit(where, state);
			return task_handle<result_t>(std::move(state));
		}

		// Same, bound to `owner` (before it is queued): cancel_on_destroy / cancel_tasks / wait_idle see it.
		template<class F>
		auto run(lane where, task_desc desc, const lifetime& owner, F&& fn)
		{
			using result_t = std::invoke_result_t<F&, task_context&>;
			using state_t = detail::task_state<result_t>;
			auto state = std::make_shared<state_t>(std::move(desc), weak_impl(), next_id(),
				typename state_t::body_t(std::forward<F>(fn)));
			owner.bind(state);
			submit(where, state);
			return task_handle<result_t>(std::move(state));
		}

		// Continuation on the pool, never blocks: `fn` is queued on `where` once every input has
		// finished. If any input failed, `fn` does not run and the result carries that failure:
		// error if an input errored (message names it), else cancelled.
		task_handle<void> when_all(std::span<const task_handle_base> inputs, lane where,
			std::function<void(task_context&)> fn);

		// Typed handles (vector<task_handle<T>>, ...) without slicing by hand.
		template<std::ranges::input_range R> requires std::derived_from<std::ranges::range_value_t<R>, task_handle_base>
		task_handle<void> when_all(const R& inputs, lane where, std::function<void(task_context&)> fn)
		{
			std::vector<task_handle_base> erased;
			if constexpr (std::ranges::sized_range<R>) {
				erased.reserve(std::ranges::size(inputs));
			}
			for (const auto& input : inputs) {
				erased.push_back(input);
			}
			return when_all(std::span<const task_handle_base>(erased), where, std::move(fn));
		}
		task_handle<void> when_all(std::initializer_list<task_handle_base> inputs, lane where,
			std::function<void(task_context&)> fn)
		{
			return when_all(std::span<const task_handle_base>(inputs.begin(), inputs.size()), where, std::move(fn));
		}

		// Main thread only. Blocks until every task bound to `owner` before the call has finished
		// (manual mode runs the queue instead). Cancel first (owner.cancel_tasks()) to make it short,
		// then unload the code those tasks run. Does not wait for then_main callbacks.
		void wait_idle(const lifetime& owner);

		// Safe from any thread; dropped silently once the system is shutting down.
		void post_main(std::function<void()> fn, main_class cls = main_class::general);

		// Main thread only; the `tasks` frame stage calls it. Runs queued main-thread work:
		//  - general: until `budget` is spent, but at least one item whenever the queue is not empty;
		//  - gpu_upload: at most gpu_upload_per_frame items.
		// Returns the number of items run.
		std::size_t drain_main(std::chrono::nanoseconds budget);
		std::size_t drain_main();

		// `manual` policy only: runs the oldest queued task of the lane on the calling thread.
		bool run_one(lane which);

		// Not yet finished tasks with task_desc::visible, for the task panel / game UI.
		[[nodiscard]] std::vector<task_info> snapshot() const;

		[[nodiscard]] policy mode() const noexcept;
		[[nodiscard]] std::size_t thread_count(lane which) const noexcept;
		[[nodiscard]] std::size_t pending_main(main_class cls) const;

		void set_main_budget(std::chrono::nanoseconds budget) noexcept;
		void set_gpu_upload_limit(std::size_t per_frame) noexcept;

		// True on a pool thread (and while run_one executes a task).
		[[nodiscard]] static bool on_worker() noexcept { return detail::on_worker_thread(); }
		// True on the thread that created the system.
		[[nodiscard]] bool on_main() const noexcept;

	private:
		std::weak_ptr<detail::task_system_impl> weak_impl() const;
		std::uint64_t next_id();
		void submit(lane where, const std::shared_ptr<detail::task_state_base>& state);

		std::shared_ptr<detail::task_system_impl> m_impl;
	};

} // namespace ds
