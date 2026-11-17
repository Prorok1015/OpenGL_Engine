#pragma once
#include "edt_api.h"
#include "edt_callback_gate.h"
#include "edt_editor_events.h"
#include "edt_registration.h"
#include <snk/common/ds/ds_task_system.hpp>
#include <cstddef>
#include <functional>
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
		// Something the module changed that is not a handle (a problems source it set): runs at clear(), after the
		// registrations.
		void keep_cleanup(std::function<void()> cleanup);

		// The owner of the jobs and continuations the module starts (job_service::start*, task::then_main): clear() stops
		// them.
		const ds::lifetime& tasks() const { return m_tasks; }

		// Stops the module's tasks: new ones are refused, running ones are asked to stop and waited for, and the
		// main-thread continuations queued so far run now (a cancelled job delivers nothing), so none of them runs the
		// module's code after this returns. A continuation of a cancelled task still runs here, with a cancelled result
		// (before shutdown_editor, never after). The module's tasks must honour the stop token. Same order as
		// gs::game_module::shutdown_services; the host calls it before the module's own shutdown_editor.
		void stop_tasks();

		// stop_tasks(), then the subscriptions, then the registrations and then the cleanups, each the last made first. Must not be called
		// from inside one of the module's own callbacks.
		void clear();

		std::size_t kept_count() const { return m_registrations.size() + m_connections.size(); }
		// The module's callbacks that run now (continuations of its tasks and the callbacks callbacks() counts): unloading
		// from inside one of them would destroy the code that runs it.
		int running_callbacks() const;
		// Counts the callbacks the module registered (panels, actions, inspectors, asset kinds, event handlers).
		callback_gate& callbacks() { return m_callbacks; }

	private:
		ds::task_system& m_tasks_system;
		ds::lifetime m_tasks{ ds::lifetime_option::cancel_on_destroy };
		callback_gate m_callbacks;
		std::vector<registration> m_registrations;
		std::vector<connection> m_connections;
		std::vector<std::function<void()>> m_cleanups;
	};
}
