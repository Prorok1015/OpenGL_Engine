#include <snk/editor/edt_registration_scope.h>
#include <utility>

edt::registration_scope::registration_scope(ds::task_system& tasks)
	: m_tasks_system(tasks)
{
}

edt::registration_scope::~registration_scope()
{
	clear();
}

void edt::registration_scope::keep(registration handle)
{
	m_registrations.push_back(std::move(handle));
}

void edt::registration_scope::keep(connection handle)
{
	m_connections.push_back(std::move(handle));
}

void edt::registration_scope::clear()
{
	m_tasks.refuse_new_tasks();
	m_tasks.cancel_tasks();
	m_tasks_system.wait_idle(m_tasks);
	ds::detail::run_pending_main(m_tasks);

	while (!m_connections.empty()) {
		m_connections.pop_back();
	}
	while (!m_registrations.empty()) {
		m_registrations.pop_back();
	}
}
