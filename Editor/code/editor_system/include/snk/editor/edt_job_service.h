#pragma once
#include "edt_editor_events.h"
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/common/engine_assert.h>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace edt
{
	class dialog_service;

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

		// What the service knows about one job, running or recently finished.
		struct job_info
		{
			job_id id = 0;
			std::string label;
			job_status status = job_status::running;
			float fraction = 0.f;
			std::string note;
			// Failed jobs: the error text.
			std::string error;
			// A cancel was asked for and the job has not been retired yet.
			bool cancel_requested = false;
		};

		// `events` (null: none) gets job_started / job_progress / job_finished; it must outlive the service.
		explicit job_service(ds::task_system& tasks, editor_events* events = nullptr);
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
			m_jobs.emplace(id, record{ handle, label, 0.f, {} });
			if (m_events) m_events->job_started.emit(id, label);
			// Bound to m_life, not to `owner`: the bookkeeping must run even if the owner is gone.
			handle.then_main(m_life, [this, id, owner_token = owner.token(), on_done = std::move(on_done)](
				ds::task_result<result_t>& result) {
				const bool known = m_jobs.contains(id);
				const bool cancelled = m_cancelled.erase(id) > 0 || result.status == ds::task_status::cancelled;
				// Retired before on_done runs, so on_done sees the job as finished.
				if (known) {
					retire(id, cancelled ? job_status::cancelled : (result.ok() ? job_status::done : job_status::failed),
						cancelled || result.ok() ? std::string{} : result.error);
				}
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

		// The running jobs, oldest first.
		[[nodiscard]] std::vector<job_info> list() const;
		// A running job, or one of the last few finished ones (the outcome stays readable after the notification).
		[[nodiscard]] std::optional<job_info> info(job_id id) const;

		// Once per frame: raises job_progress for each job whose fraction or note changed since the last time.
		// Progress comes from worker threads without a callback, so this is where it is polled and throttled.
		void pump();

		// Editor shutdown: cancels every job, waits until none is executing and drops what was not delivered.
		// Call it before the code the jobs run is destroyed. Nothing is delivered afterwards.
		void cancel_all_and_wait();

	private:
		struct record
		{
			ds::task_handle_base handle;
			std::string label;
			// What job_progress last reported.
			float reported_fraction = 0.f;
			std::string reported_note;
		};

		static constexpr std::size_t FINISHED_HISTORY = 64;

		// Drops the job from m_jobs, keeps its outcome in the history and raises job_finished.
		void retire(job_id id, job_status status, std::string error);
		job_info describe(job_id id, const record& job) const;

		ds::task_system& m_tasks;
		editor_events* m_events = nullptr;
		std::unordered_map<job_id, record> m_jobs;
		std::unordered_set<job_id> m_cancelled;
		std::deque<job_info> m_finished;
		// Last: destroyed first, so continuations that still hold `this` are dropped before the maps die.
		ds::lifetime m_life;
	};

	// A job that threw is logged and shown to the person as an error message dialog (the dialog is skipped while the
	// dialog service has no registrar). Cancelled and finished jobs are not reported. `jobs` and `dialogs` must
	// outlive the returned subscription.
	[[nodiscard]] connection report_failed_jobs(editor_events& events, const job_service& jobs, dialog_service& dialogs);
}
