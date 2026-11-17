#include <snk/editor/edt_registration_scope.h>
#include <snk/common/engine_assert.h>
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

void edt::registration_scope::keep_cleanup(std::function<void()> cleanup)
{
	m_cleanups.push_back(std::move(cleanup));
}

void edt::registration_scope::keep(connection handle)
{
	m_connections.push_back(std::move(handle));
}

int edt::registration_scope::running_callbacks() const
{
	return ds::detail::running_callbacks(m_tasks) + m_callbacks.running();
}

void edt::registration_scope::stop_tasks()
{
	ASSERT_MSG(running_callbacks() == 0,
		"registration_scope: unloaded from inside one of the module's own callbacks");
	m_tasks.refuse_new_tasks();
	m_tasks.cancel_tasks();
	m_tasks_system.wait_idle(m_tasks);
	ds::detail::run_pending_main(m_tasks);
}

void edt::registration_scope::clear()
{
	stop_tasks();
	while (!m_connections.empty()) {
		m_connections.pop_back();
	}
	while (!m_registrations.empty()) {
		m_registrations.pop_back();
	}
	while (!m_cleanups.empty()) {
		const auto cleanup = std::move(m_cleanups.back());
		m_cleanups.pop_back();
		cleanup();
	}
}
