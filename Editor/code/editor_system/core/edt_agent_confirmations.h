#pragma once
#include <snk/command/cmd_registry.h>
#include <boost/json/value.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <span>
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
		// into an allow, never lift a deny. Never for a command whose `risk` is not none: such a request is offered only
		// "once" (confirmation_request::can_grant_session), the answer given anyway is "once", and the grant covers
		// and settles only commands without a risk.
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
		// command_desc::describe of the call (the registry bound it to the ticket) as confirmation_detail_text made it:
		// whole, its line breaks kept; empty: none.
		std::string detail;
		// false: the command has a risk, so the window offers no "until the end of this agent session".
		bool can_grant_session = false;
		std::chrono::steady_clock::time_point asked_at{};
		std::chrono::steady_clock::time_point deadline{};
		// false: the request has just appeared or moved in the window, and an "allow" is not taken yet (input_guard).
		bool can_allow = false;
	};

	// Where the window drew the "allow" of a request, in screen pixels.
	struct shown_rect
	{
		float min_x = 0.f;
		float min_y = 0.f;
		float max_x = 0.f;
		float max_y = 0.f;

		bool operator==(const shown_rect&) const = default;
	};

	struct shown_request
	{
		std::uint64_t id = 0;
		shown_rect allow;
	};

	// Since when the "allow" of each request has been drawn at the same place: a click aimed at what was under the
	// cursor a moment ago must not land on a button that has just moved there (#913). It goes by what was drawn, not
	// by the list: a request leaving, the window changing its width and a scroll all move a button, and a frame that
	// was never shown (the main thread hung) gave nobody the time to read it.
	class confirmation_layout
	{
	public:
		using time_point = std::chrono::steady_clock::time_point;
		// One frame is no proof a person saw the button there: a click queued before it is replayed on that frame.
		static constexpr int min_frames = 2;

		// `frame`: every request the window drew this frame. One that is not in it starts again when it is drawn.
		void settle(std::span<const shown_request> frame, time_point now);
		void forget(std::uint64_t id);
		// When the min_frames-th frame in a row drew the button at one place (it stands there since); else nullopt.
		std::optional<time_point> steady_since(std::uint64_t id) const;

	private:
		struct slot
		{
			std::uint64_t id = 0;
			shown_rect allow;
			time_point since{};
			int frames = 0;
		};

		std::vector<slot> m_slots;
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
		// The longest description of a call a question shows. A longer one refuses the call: a person must never
		// approve a ticket whose description was cut where they could not see it (#932).
		static constexpr std::size_t k_max_detail = 32 * 1024;
		static constexpr std::size_t k_max_detail_lines = 512;
		// How long a call that finds a person in the middle of a gesture (`busy`) is tried again, once per tick(), before
		// the agent is told. Also for a call a person has just allowed: that answer belongs to this very call, so it is
		// not asked again, and the command itself still checks the state it finds (expected_generation, refs).
		static constexpr std::chrono::seconds busy_wait{ 5 };
		// How long the "allow" of a request must have been drawn at one place before it is taken. Each request waits
		// for itself, so a new request at the end holds back none of the others.
		static constexpr std::chrono::milliseconds input_guard{ 750 };
		// Requests of one agent session that wait for a person at a time; one more is refused. A session that keeps
		// asking could otherwise fill the window above another session's request for as long as it likes.
		static constexpr std::size_t max_waiting_per_session = 4;

		agent_confirmations(cmd::command_registry& commands, session_grants& grants, clock_function now = {});
		~agent_confirmations();
		agent_confirmations(const agent_confirmations&) = delete;
		agent_confirmations& operator=(const agent_confirmations&) = delete;

		// `done` is called exactly once, on the main thread: at once for an answer that needs no person, later for a
		// confirmed one or one that waited for a gesture to end.
		void invoke(std::string_view command_id, cmd::call request, finish done);

		// A person's answer to request `id`; false when it is no longer waiting (answered, timed out, session dropped), and
		// for an "allow" while its button has not been drawn at one place for input_guard (note_shown; it keeps
		// waiting). A "deny" is taken at any time: a misplaced one runs nothing. Allowing runs the command, so call it from the frame's update, never
		// from inside an ImGui window.
		bool answer(std::uint64_t id, confirmation_answer what);

		// What the window drew this frame, before the answers of that frame are given: once per frame the window is
		// drawn, with nothing when it drew no request (collapsed, or left out while a person's modal popup is open).
		void note_shown(std::span<const shown_request> frame);

		// Once per frame, from the editor's per-frame update, not from drawing the window: times out what waited too long.
		// An answer after the deadline is refused whether or not tick() has run, and pending() leaves the late ones out.
		void tick();

		// The agent session ended: its waiting requests are declined and its grants forgotten.
		void drop_session(std::string_view session);

		// Nobody can answer any more (the window cannot be shown): everything waiting is declined with `why`.
		void decline_all(std::string_view why);

		// decline_all, and the calls parked for a retry are answered `denied` too: afterwards no call of this object holds a `done`
		// that can still run. For an owner that is about to destroy what those closures write to (the fixtures of the tests).
		void cancel_all(std::string_view why);

		std::vector<confirmation_request> pending() const;

		// `ticket` is the one a retry of a call a person allowed made and approved itself (run_approved), not one a person
		// answered: a command that binds what it described to the ticket judges such a call by what the person approved.
		bool approved_internally(std::string_view ticket) const { return !ticket.empty() && ticket == m_internal_ticket; }

	private:
		// A call that found a person in the middle of a gesture and is tried again by tick().
		struct retrying
		{
			std::string command_id;
			cmd::call call;
			finish done;
			// The person already allowed exactly this call: every try gets a fresh ticket approved here, never a question.
			bool approved = false;
			std::chrono::steady_clock::time_point give_up_at{};
		};

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
		// A waiting question a new grant may cover runs again through the gate without its ticket, as a call answered
		// in advance: one the gate still asks about or refuses stays waiting for its own answer, and so does one whose
		// command binds its run to what the question showed (describe_before_allowed_invoke); otherwise its question
		// goes and the call gets the outcome.
		void settle_by_grant(std::uint64_t id);
		void expire_late();
		bool stood_still(std::uint64_t id) const;
		using time_point = std::chrono::steady_clock::time_point;
		cmd::result run_command(std::string_view command_id, const cmd::call& request);
		// One try of a call nobody has been asked about yet (a person may be asked now).
		void start(std::string command_id, cmd::call request, finish done, std::optional<time_point> give_up_at);
		// One try of a call a person allowed: a ticket is made and approved for exactly this call.
		void run_approved(std::string command_id, cmd::call request, finish done, std::optional<time_point> give_up_at);
		// `busy` parks the call for tick() until `give_up_at` (busy_wait after the first busy), anything else is the answer.
		void park_or_finish(std::string command_id, cmd::call request, finish done, bool approved, cmd::result outcome,
			std::optional<time_point> give_up_at);

		cmd::command_registry& m_commands;
		session_grants& m_grants;
		clock_function m_clock;
		std::uint64_t m_next_id = 1;
		std::vector<waiting> m_waiting;
		confirmation_layout m_layout;
		std::vector<retrying> m_retrying;
		// The ticket run_approved made and approved, while its command runs.
		std::string m_internal_ticket;
	};

	// The text of command_desc::describe as the window shows it: line breaks kept ("\r\n" and "\r" become "\n"), a tab
	// becomes spaces, and within a line what edt::forge::sanitize_text replaces is replaced (control, bidi and invisible
	// characters, invalid UTF-8); lines left empty or with spaces only go at the start and at the end, and a run of them
	// between two lines becomes one empty line. No other text is cut: nullopt when the result is longer than
	// k_max_detail bytes or k_max_detail_lines lines.
	std::optional<std::string> confirmation_detail_text(std::string_view described);

	// Whether the font draws every letter of the window's buttons. The built-in ImGui font has no Cyrillic and shows '?',
	// so a person would answer buttons they cannot read: without the glyphs nobody is asked (ask = deny).
	bool confirmation_glyphs_available(ImFont& font);

	// The window with the waiting requests: nothing is drawn (no ImGui call at all) while none waits. Returns true
	// while it should be called again, so it fits an implicit-layout registrar.
	bool draw_agent_confirmations(agent_confirmations& confirmations);
}
