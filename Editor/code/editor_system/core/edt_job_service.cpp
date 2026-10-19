#include <snk/editor/edt_job_service.h>
#include <algorithm>
#include <vector>

edt::job_service::job_service(ds::task_system& tasks, editor_events* events)
	: m_tasks(tasks)
	, m_events(events)
{
}

edt::job_service::~job_service()
{
	cancel_all_and_wait();
}

bool edt::job_service::cancel(job_id id)
{
	const auto found = m_jobs.find(id);
	if (found == m_jobs.end()) return false;
	m_cancelled.insert(id);
	found->second.handle.cancel();
	return true;
}

bool edt::job_service::is_running(job_id id) const
{
	return m_jobs.contains(id);
}

std::optional<edt::job_progress> edt::job_service::progress(job_id id) const
{
	const auto found = m_jobs.find(id);
	if (found == m_jobs.end()) return std::nullopt;
	const auto& handle = found->second.handle;
	return job_progress{ found->second.label, handle.progress(), handle.progress_text() };
}

edt::job_service::job_info edt::job_service::describe(job_id id, const record& job) const
{
	job_info out;
	out.id = id;
	out.label = job.label;
	out.status = job_status::running;
	out.fraction = job.handle.progress();
	out.note = job.handle.progress_text();
	out.cancel_requested = m_cancelled.contains(id);
	return out;
}

std::vector<edt::job_service::job_info> edt::job_service::list() const
{
	std::vector<job_info> out;
	out.reserve(m_jobs.size());
	for (const auto& [id, job] : m_jobs) {
		out.push_back(describe(id, job));
	}
	std::sort(out.begin(), out.end(), [](const job_info& a, const job_info& b) { return a.id < b.id; });
	return out;
}

std::optional<edt::job_service::job_info> edt::job_service::info(job_id id) const
{
	if (const auto running = m_jobs.find(id); running != m_jobs.end()) {
		return describe(id, running->second);
	}
	const auto done = std::find_if(m_finished.begin(), m_finished.end(), [id](const job_info& j) { return j.id == id; });
	if (done != m_finished.end()) return *done;
	return std::nullopt;
}

void edt::job_service::pump()
{
	if (!m_events) return;
	struct change
	{
		job_id id;
		float fraction;
		std::string note;
	};
	// Collected first: a slot may start or cancel jobs, which would change the map under the loop.
	std::vector<change> changes;
	for (auto& [id, job] : m_jobs) {
		const float fraction = job.handle.progress();
		std::string note = job.handle.progress_text();
		if (fraction == job.reported_fraction && note == job.reported_note) continue;
		job.reported_fraction = fraction;
		job.reported_note = note;
		changes.push_back({ id, fraction, std::move(note) });
	}
	std::sort(changes.begin(), changes.end(), [](const change& a, const change& b) { return a.id < b.id; });
	for (const auto& c : changes) {
		m_events->job_progress.emit(c.id, c.fraction, c.note);
	}
}

void edt::job_service::retire(job_id id, job_status status, std::string error)
{
	const auto found = m_jobs.find(id);
	if (found == m_jobs.end()) return;

	// Progress made since the last pump() is reported before the job ends, so a listener sees its final value.
	const float fraction = found->second.handle.progress();
	std::string note = found->second.handle.progress_text();
	const bool progress_changed = fraction != found->second.reported_fraction || note != found->second.reported_note;

	job_info outcome = describe(id, found->second);
	outcome.status = status;
	outcome.fraction = fraction;
	outcome.note = note;
	outcome.error = error;
	outcome.cancel_requested = status == job_status::cancelled;
	m_jobs.erase(found);

	m_finished.push_back(std::move(outcome));
	while (m_finished.size() > FINISHED_HISTORY) {
		m_finished.pop_front();
	}

	if (!m_events) return;
	if (progress_changed && status != job_status::cancelled) m_events->job_progress.emit(id, fraction, note);
	m_events->job_finished.emit(id, status, error);
}

void edt::job_service::cancel_all_and_wait()
{
	std::vector<ds::task_handle_base> handles;
	std::vector<job_id> ids;
	handles.reserve(m_jobs.size());
	ids.reserve(m_jobs.size());
	for (auto& [id, job] : m_jobs) {
		job.handle.cancel();
		handles.push_back(job.handle);
		ids.push_back(id);
	}
	for (const auto& handle : handles) {
		handle.wait();
	}
	// A continuation still queued on main finds its id unknown and delivers nothing.
	std::sort(ids.begin(), ids.end());
	for (const job_id id : ids) {
		retire(id, job_status::cancelled, {});
	}
	m_jobs.clear();
	m_cancelled.clear();
}
