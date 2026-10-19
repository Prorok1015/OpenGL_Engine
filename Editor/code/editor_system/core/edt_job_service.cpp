#include <snk/editor/edt_job_service.h>
#include <vector>

edt::job_service::job_service(ds::task_system& tasks)
	: m_tasks(tasks)
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

void edt::job_service::cancel_all_and_wait()
{
	std::vector<ds::task_handle_base> handles;
	handles.reserve(m_jobs.size());
	for (auto& [id, job] : m_jobs) {
		job.handle.cancel();
		handles.push_back(job.handle);
	}
	for (const auto& handle : handles) {
		handle.wait();
	}
	// A continuation still queued on main finds its id unknown and delivers nothing.
	m_jobs.clear();
	m_cancelled.clear();
}
