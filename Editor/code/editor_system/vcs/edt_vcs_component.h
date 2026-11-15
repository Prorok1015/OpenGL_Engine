#pragma once
#include "edt_forge_service.h"
#include "edt_repository_watcher.h"
#include "edt_vcs_policy.h"
#include "edt_vcs_service.h"
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/editor/edt_registration.h>
#include <snk/platform/plt_secret.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace edt
{
	class editor_context;
	class forge_account_panel;
	class git_panel;

	// Version control and the code host, whole: git as jobs (vcs.* commands), the repository watcher behind
	// editor_events::repository_changed, pull requests (forge.* commands), the token store and the Git Account window.
	// editor_system creates it, calls install() where the Git panel's neighbours are registered and asks it the few things other
	// areas need: whether a git write runs, where the git folder is, and to follow the agent policy once it exists.
	class vcs_component
	{
	public:
		struct ports
		{
			editor_context& context;
			git_panel& git;
			// The open project's root; nullopt: no project.
			std::function<std::optional<std::filesystem::path>()> project_root;
			// Live mode is on: an agent's checkout or pull must not bring code in meanwhile (#909).
			std::function<bool()> live_mode_on;
		};

		explicit vcs_component(ports from);
		~vcs_component();
		vcs_component(const vcs_component&) = delete;
		vcs_component& operator=(const vcs_component&) = delete;

		// The vcs.* and forge.* commands, the Git Account window (floating, closed by default) and its action, and the Git panel's
		// links to it. Registration order is the View menu order: call it where the window goes in.
		void install(std::vector<registration>& out);
		// The `editor_vcs` frame stage: re-arms the watcher when a probe changed the cached state (never probes itself) and raises
		// editor_events::repository_changed.
		void tick();
		// Asked at the start of every agent write, so an edit of either policy file applies at once. Without it the defaults apply.
		void follow_agent_policy(std::filesystem::path project_policy_file, std::function<vcs_policy()> source);

		bool writes_running() const { return m_vcs.running_writes() > 0; }
		std::optional<std::filesystem::path> git_dir() const { return m_vcs.cached_state().git_dir; }

	private:
		ports m_ports;
		// The account window: the registry holds it too, and editor_system removes that registration before this component goes.
		std::shared_ptr<forge_account_panel> m_account_panel;
		// Forge tokens (#458): the OS store, shared with the forge service and the account window. Its calls block: only jobs
		// make them.
		std::shared_ptr<platform::secret_store> m_secrets;
		// The commands of the service are in editor_system's registrations, removed before it.
		vcs_service m_vcs;
		// After the service whose write observer reaches it, so it is destroyed first.
		std::unique_ptr<vcs::repository_watcher> m_watcher;
		// After the service it asks.
		std::unique_ptr<forge_service> m_forge;
		// Owner of the Git Account jobs (token save, remove, probe); last, so it is the first to go and stops them before the
		// store and the service they use.
		ds::lifetime m_account_jobs{ ds::lifetime_option::cancel_on_destroy };
	};
}
