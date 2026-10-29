#pragma once
#include <snk/command/cmd_registry.h>
#include <snk/editor/edt_registration.h>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace edt
{
	class document_manager;

	// agent.session.begin/checkpoint/end/rollback/get (A5, #291; AI_INTEGRATION.md §3.4): the edits an agent makes in a
	// session go into history groups of its own, so the session is one undo record per step and can be rolled back
	// whole, up to the first edit someone else (a person, another session) made to the same node after it. One session
	// per agent origin (an MCP session id; "test" for tests): any other caller is refused. A boundary of the history (a
	// level loaded, a world added) ends them all.
	class agent_sessions
	{
	public:
		explicit agent_sessions(document_manager& documents) : m_documents(documents) {}
		agent_sessions(const agent_sessions&) = delete;
		agent_sessions& operator=(const agent_sessions&) = delete;

		[[nodiscard]] std::vector<registration> register_commands(cmd::command_registry& commands);

		// The MCP session went away: its open session ends as with agent.session.end (nothing is rolled back).
		void session_closed(const std::string& session_id);

	private:
		struct session
		{
			std::string label;
			// One group per step (checkpoint), oldest first; the last one is open.
			std::vector<std::uint64_t> groups;
		};

		cmd::result begin(const cmd::call& call);
		cmd::result checkpoint(const cmd::call& call);
		cmd::result end(const cmd::call& call);
		cmd::result rollback(const cmd::call& call);
		cmd::result get(const cmd::call& call);

		// The open session of the caller, null when there is none (or the history forgot its group).
		session* find(const cmd::origin& from);
		void end_session(const std::string& key, const cmd::origin& from);

		document_manager& m_documents;
		std::map<std::string, session> m_sessions;
	};
}
