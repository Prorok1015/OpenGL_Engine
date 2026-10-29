#pragma once
#include <snk/command/cmd_registry.h>
#include <boost/json/value.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct ImFont;

namespace edt
{
	// What a person answers to "the agent wants to ...".
	enum class confirmation_answer : std::uint8_t
	{
		allow_once,
		// Until this agent session ends: the same category is not asked about again for this session id, and what already waits
		// for that session and category is allowed with it (also a request whose command had its own `ask` entry). Kept in
		// memory only (never written to a file), dropped with the session (drop_session), and it can only turn an ask
		// into an allow, never lift a deny.
		allow_session,
		deny,
	};

	// The "Allow until this agent session ends" answers: (MCP session id, category) pairs. Main thread only.
	class session_grants
	{
	public:
		bool has(std::string_view session, cmd::agent_category category) const;
		void add(std::string session, cmd::agent_category category);
		void drop_session(std::string_view session);
		bool empty() const { return m_grants.empty(); }

	private:
		std::set<std::pair<std::string, cmd::agent_category>> m_grants;
	};

	struct confirmation_request
	{
		std::uint64_t id = 0;
		std::string command;
		std::string summary;
		// Why the policy asks (the command's own summary).
		std::string reason;
		// The arguments as the agent sent them, JSON text.
		std::string args;
		std::string session;
		cmd::agent_category category = cmd::agent_category::unset;
		std::chrono::steady_clock::time_point asked_at{};
		std::chrono::steady_clock::time_point deadline{};
	};

	// The non-modal question to a person behind every `ask` of the policy (A6b #293): runs a call from an agent through
	// the registry, and when the registry answers `confirmation_required` holds the call until a person answers, 60 s
	// pass, or the agent's session ends. Allowed: the registry approves the ticket and the same call runs once. Declined,
	// no answer or disconnected: `denied`. Main thread only (the registry's rule); create it before the bridge that uses it
	// and destroy it after: the destructor answers everything still waiting with `denied`.
	class agent_confirmations
	{
	public:
		using clock_function = std::function<std::chrono::steady_clock::time_point()>;
		using finish = std::function<void(cmd::result)>;

		static constexpr std::chrono::seconds timeout{ 60 };

		agent_confirmations(cmd::command_registry& commands, session_grants& grants, clock_function now = {});
		~agent_confirmations();
		agent_confirmations(const agent_confirmations&) = delete;
		agent_confirmations& operator=(const agent_confirmations&) = delete;

		// `done` is called exactly once, on the main thread: at once for an answer that needs no person, later for a
		// confirmed one.
		void invoke(std::string_view command_id, cmd::call request, finish done);

		// A person's answer to request `id`; false when it is no longer waiting (answered, timed out, session dropped). Allowing
		// runs the command, so call it from the frame's update, never from inside an ImGui window.
		bool answer(std::uint64_t id, confirmation_answer what);

		// Once per frame, from the editor's per-frame update, not from drawing the window: times out what waited too long.
		// An answer after the deadline is refused whether or not tick() has run, and pending() leaves the late ones out.
		void tick();

		// The agent session ended: its waiting requests are declined and its grants forgotten.
		void drop_session(std::string_view session);

		// Nobody can answer any more (the window cannot be shown): everything waiting is declined with `why`.
		void decline_all(std::string_view why);

		std::vector<confirmation_request> pending() const;

	private:
		struct waiting
		{
			confirmation_request request;
			std::string ticket;
			std::string command_id;
			cmd::call call;
			finish done;
		};

		std::chrono::steady_clock::time_point now() const;
		void finish_waiting(std::uint64_t id, std::optional<confirmation_answer> what, std::string_view why_not);

		cmd::command_registry& m_commands;
		session_grants& m_grants;
		clock_function m_clock;
		std::uint64_t m_next_id = 1;
		std::vector<waiting> m_waiting;
	};

	// Whether the font draws every letter of the window's buttons. The built-in ImGui font has no Cyrillic and shows '?',
	// so a person would answer buttons they cannot read: without the glyphs nobody is asked (ask = deny).
	bool confirmation_glyphs_available(ImFont& font);

	// The window with the waiting requests: nothing is drawn (no ImGui call at all) while none waits. Returns true
	// while it should be called again, so it fits an implicit-layout registrar.
	bool draw_agent_confirmations(agent_confirmations& confirmations);
}
