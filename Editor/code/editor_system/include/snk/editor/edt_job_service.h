#pragma once
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/common/engine_assert.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace edt
{
	// What a running job reports: `fraction` in [0, 1], `note` is the last text given to task_context::set_progress.
	struct job_progress
	{
		std::string label;
		float fraction = 0.f;
		std::string note;
	};

	// Background work of the editor (import, export, ...) on top of ds::task_system. The work runs on a pool
	// lane; its result is delivered by the task system's main drain (the `tasks` frame stage), so there is no
	// polling and no second drain. Main thread only.
	class job_service
	{
	public:
		using job_id = std::uint64_t;

		explicit job_service(ds::task_system& tasks);
		// cancel_all_and_wait(); the task system must still be alive.
		~job_service();
		job_service(const job_service&) = delete;
		job_service& operator=(const job_service&) = delete;

		// Runs `work` on lane `where`; later `on_done` runs on the main thread with its result. `owner` is the
		// object `on_done` touches: after it is destroyed nothing is delivered, and a cancel_on_destroy owner
		// also stops the job. `on_done` is not called for a cancelled job (cancel(), or the stop token seen by
		// `work`), even if `work` had already finished: the result is discarded. A failed job (exception) is
		// delivered with status `error`.
		template<class F>
		job_id start(ds::lane where, ds::task_desc desc, const ds::lifetime& owner, F&& work,
			std::function<void(ds::task_result<std::invoke_result_t<F&, ds::task_context&>>&)> on_done)
		{
			using result_t = std::invoke_result_t<F&, ds::task_context&>;
			ASSERT_MSG(m_tasks.on_main(), "job_service::start: main thread only");
			std::string label = desc.name;
			auto handle = m_tasks.run(where, std::move(desc), owner, std::forward<F>(work));
			const job_id id = handle.id();
			m_jobs.emplace(id, record{ handle, std::move(label) });
			// Bound to m_life, not to `owner`: the bookkeeping must run even if the owner is gone.
			handle.then_main(m_life, [this, id, owner_token = owner.token(), on_done = std::move(on_done)](
				ds::task_result<result_t>& result) {
				const bool known = m_jobs.erase(id) > 0;
				const bool cancelled = m_cancelled.erase(id) > 0 || result.status == ds::task_status::cancelled;
				if (known && !cancelled && !owner_token.expired() && on_done) {
					on_done(result);
				}
			});
			return id;
		}

		// Cooperative: sets the stop token seen by `work`. A job that has not started never runs.
		// Its result is discarded either way. False if the job is unknown or already delivered.
		bool cancel(job_id id);
		// Queued, running, or finished but not delivered yet. False once delivered or discarded.
		[[nodiscard]] bool is_running(job_id id) const;
		[[nodiscard]] std::optional<job_progress> progress(job_id id) const;
		[[nodiscard]] std::size_t running_count() const { return m_jobs.size(); }

		// Editor shutdown: cancels every job, waits until none is executing and drops what was not delivered.
		// Call it before the code the jobs run is destroyed. Nothing is delivered afterwards.
		void cancel_all_and_wait();

	private:
		struct record
		{
			ds::task_handle_base handle;
			std::string label;
		};

		ds::task_system& m_tasks;
		std::unordered_map<job_id, record> m_jobs;
		std::unordered_set<job_id> m_cancelled;
		// Last: destroyed first, so continuations that still hold `this` are dropped before the maps die.
		ds::lifetime m_life;
	};
}
