#pragma once
#include "edt_api.h"
#include "edt_editor_events.h"
#include "edt_registration.h"
#include <snk/common/ds/ds_task_system.hpp>
#include <cstddef>
#include <vector>

namespace edt
{
	// Everything one game_editor module (#588) put into the editor, held in one place so unloading it leaves the editor
	// as it was: its registrations, its subscriptions and the tasks it started. Main thread only.
	// A module never makes one itself: the host gives it an editor_module_context that keeps into its scope.
	class SNK_EDITOR_API registration_scope
	{
	public:
		explicit registration_scope(ds::task_system& tasks);
		~registration_scope();
		registration_scope(const registration_scope&) = delete;
		registration_scope& operator=(const registration_scope&) = delete;

		void keep(registration handle);
		void keep(connection handle);

		// The owner of the jobs and continuations the module starts (job_service::start*, task::then_main): clear() stops
		// them.
		const ds::lifetime& tasks() const { return m_tasks; }

		// Tasks first: new ones are refused, running ones are asked to stop and waited for, and the main-thread
		// continuations queued meanwhile run now (with a cancelled result), so none of them runs the module's code
		// after it is gone. Then the subscriptions and the registrations, the last made first.
		void clear();

		std::size_t kept_count() const { return m_registrations.size() + m_connections.size(); }

	private:
		ds::task_system& m_tasks_system;
		ds::lifetime m_tasks{ ds::lifetime_option::cancel_on_destroy };
		std::vector<registration> m_registrations;
		std::vector<connection> m_connections;
	};
}
