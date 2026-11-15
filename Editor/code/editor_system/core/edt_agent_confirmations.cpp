#include "edt_agent_confirmations.h"
#include "edt_forge_types.h"
#include <snk/common/logger/engine_log.h>
#include <boost/json/serialize.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <exception>
#include <cstddef>
#include <cstdint>
#include <format>
#include <utility>

namespace json = boost::json;

namespace
{
	// Holds `ticket` in `slot` while the retry it approved runs; the value before it comes back however the run ends.
	class internal_ticket_scope
	{
	public:
		internal_ticket_scope(std::string& slot, std::string ticket) : m_slot(slot), m_previous(std::exchange(slot, std::move(ticket))) {}
		~internal_ticket_scope() { m_slot = std::move(m_previous); }
		internal_ticket_scope(const internal_ticket_scope&) = delete;
		internal_ticket_scope& operator=(const internal_ticket_scope&) = delete;

	private:
		std::string& m_slot;
		std::string m_previous;
	};
}

namespace edt
{
	bool session_grants::has(std::string_view session, cmd::agent_category category) const
	{
		return m_grants.contains({ std::string{ session }, category });
	}

	void session_grants::add(std::string session, cmd::agent_category category)
	{
		m_grants.emplace(std::move(session), category);
	}

	void session_grants::drop_session(std::string_view session)
	{
		std::erase_if(m_grants, [session](const auto& grant) { return grant.first == session; });
	}

	void confirmation_layout::settle(std::span<const shown_request> frame, time_point now)
	{
		std::vector<slot> next;
		next.reserve(frame.size());
		for (const shown_request& shown : frame) {
			const auto before = std::find_if(m_slots.begin(), m_slots.end(), [&shown](const slot& s) { return s.id == shown.id; });
			if (before != m_slots.end() && before->allow == shown.allow) {
				next.push_back(*before);
				// Only the second frame proves the first was on screen: built is not shown, and a hang between the two
				// must not count as time a person had to read it.
				if (++next.back().frames == min_frames) next.back().since = now;
			} else {
				next.push_back(slot{ shown.id, shown.allow, now, 1 });
			}
		}
		m_slots = std::move(next);
	}

	void confirmation_layout::forget(std::uint64_t id)
	{
		std::erase_if(m_slots, [id](const slot& s) { return s.id == id; });
	}

	std::optional<confirmation_layout::time_point> confirmation_layout::steady_since(std::uint64_t id) const
	{
		const auto found = std::find_if(m_slots.begin(), m_slots.end(), [id](const slot& s) { return s.id == id; });
		if (found == m_slots.end() || found->frames < min_frames) return std::nullopt;
		return found->since;
	}

	std::optional<std::string> confirmation_detail_text(std::string_view described)
	{
		// Refused without being scanned: a bound on the work done for one request, whatever sanitizing would leave of it.
		if (described.size() > agent_confirmations::k_max_detail * 4) return std::nullopt;

		std::string out;
		out.reserve(described.size());
		std::size_t lines = 0;
		// Blank lines before the text and at its end go, a run of them inside is one: they would hide the text behind the scroll.
		bool blank_before = false;
		std::size_t at = 0;
		while (at <= described.size()) {
			const std::size_t end = std::min(described.find_first_of("\r\n", at), described.size());
			std::string line;
			for (const char c : described.substr(at, end - at)) {
				if (c == '\t') line += "    ";
				else line += c;
			}
			// Invalid bytes grow into U+FFFD (3 bytes each): room for all of them, so nothing is cut here.
			const std::string plain = edt::forge::sanitize_text(line, line.size() * 3 + 3);
			if (plain.find_first_not_of(' ') == std::string::npos) {
				blank_before = lines > 0;
			} else {
				if (blank_before) {
					out += '\n';
					++lines;
					blank_before = false;
				}
				if (lines++ > 0) out += '\n';
				out += plain;
			}
			if (end == described.size()) break;
			at = end + ((described[end] == '\r' && end + 1 < described.size() && described[end + 1] == '\n') ? 2 : 1);
		}
		if (out.size() > agent_confirmations::k_max_detail || lines > agent_confirmations::k_max_detail_lines) return std::nullopt;
		return out;
	}

	agent_confirmations::agent_confirmations(cmd::command_registry& commands, session_grants& grants, clock_function now)
		: m_commands(commands), m_grants(grants), m_clock(std::move(now))
	{
	}

	cmd::result agent_confirmations::run_command(std::string_view command_id, const cmd::call& request)
	{
		// A command that throws must not take the frame down, nor leave the agent without an answer.
		try {
			return m_commands.invoke(command_id, request);
		} catch (const std::exception& e) {
			// What the exception says may be a path or a string of the editor's: the agent gets a plain refusal, the log the cause.
			egLOG_ERROR("agent", "command '{}' threw: {}", command_id, e.what());
			return cmd::result::failure(cmd::codes::failed, "the command failed");
		} catch (...) {
			egLOG_ERROR("agent", "command '{}' threw an unknown exception", command_id);
			return cmd::result::failure(cmd::codes::failed, "the command failed");
		}
	}

	agent_confirmations::~agent_confirmations()
	{
		while (!m_waiting.empty()) {
			finish_waiting(m_waiting.front().request.id, std::nullopt, "the editor is closing");
		}
		std::vector<retrying> parked;
		parked.swap(m_retrying);
		for (retrying& call : parked) {
			call.done(cmd::result::failure(cmd::codes::denied, std::format("'{}' was not run: the editor is closing", call.command_id)));
		}
	}

	std::chrono::steady_clock::time_point agent_confirmations::now() const
	{
		return m_clock ? m_clock() : std::chrono::steady_clock::now();
	}

	void agent_confirmations::invoke(std::string_view command_id, cmd::call request, finish done)
	{
		start(std::string(command_id), std::move(request), std::move(done), std::nullopt);
	}

	void agent_confirmations::park_or_finish(std::string command_id, cmd::call request, finish done, bool approved, cmd::result outcome,
		std::optional<time_point> give_up_at)
	{
		if (outcome.code != cmd::codes::busy) {
			done(std::move(outcome));
			return;
		}
		const time_point limit = give_up_at ? *give_up_at : now() + busy_wait;
		if (now() >= limit) {
			done(std::move(outcome));
			return;
		}
		retrying parked;
		parked.command_id = std::move(command_id);
		parked.call = std::move(request);
		parked.call.confirmation.clear();
		parked.done = std::move(done);
		parked.approved = approved;
		parked.give_up_at = limit;
		m_retrying.push_back(std::move(parked));
	}

	void agent_confirmations::run_approved(std::string command_id, cmd::call request, finish done, std::optional<time_point> give_up_at)
	{
		request.confirmation.clear();
		cmd::result outcome = run_command(command_id, request);
		if (outcome.code == cmd::codes::confirmation_required) {
			const json::object* body = outcome.value.if_object();
			const json::value* ticket = body != nullptr ? body->if_contains("ticket") : nullptr;
			if (ticket == nullptr || !ticket->is_string()) {
				done(cmd::result::failure(cmd::codes::denied, std::format("'{}' was refused: the registry gave no confirmation ticket", command_id)));
				return;
			}
			const std::string ticket_id(ticket->as_string());
			// The registry hands out one ticket for one (command, arguments, session): an identical call that a person is
			// being asked about holds it. That ticket is theirs to answer, never spent here; this try waits for the next tick.
			if (std::any_of(m_waiting.begin(), m_waiting.end(), [&ticket_id](const waiting& w) { return w.ticket == ticket_id; })) {
				park_or_finish(std::move(command_id), std::move(request), std::move(done), true,
					cmd::result::failure(cmd::codes::busy, "an identical request is waiting for a person's answer"), give_up_at);
				return;
			}
			if (!m_commands.approve(ticket_id)) {
				done(cmd::result::failure(cmd::codes::denied, std::format("'{}' was refused: the confirmation expired", command_id)));
				return;
			}
			request.confirmation = ticket_id;
			{
				const internal_ticket_scope internal(m_internal_ticket, ticket_id);
				outcome = run_command(command_id, request);
			}
			// A ticket the registry did not consume is not left waiting for its lifetime.
			m_commands.discard(ticket_id);
		}
		park_or_finish(std::move(command_id), std::move(request), std::move(done), true, std::move(outcome), give_up_at);
	}

	void agent_confirmations::start(std::string command_id_text, cmd::call request, finish done, std::optional<time_point> give_up_at)
	{
		const std::string_view command_id = command_id_text;
		cmd::result first = run_command(command_id, request);
		if (first.code != cmd::codes::confirmation_required) {
			park_or_finish(command_id_text, std::move(request), std::move(done), false, std::move(first), give_up_at);
			return;
		}
		const json::object* body = first.value.if_object();
		const json::value* ticket = body != nullptr ? body->if_contains("ticket") : nullptr;
		if (ticket == nullptr || !ticket->is_string()) {
			done(cmd::result::failure(cmd::codes::denied, std::format("'{}' was refused: the registry gave no confirmation ticket", command_id)));
			return;
		}

		const std::string ticket_id(ticket->as_string());
		// The same question asked again while it waits is the ticket already in the list: one question to the person.
		if (std::any_of(m_waiting.begin(), m_waiting.end(), [&ticket_id](const waiting& w) { return w.ticket == ticket_id; })) {
			done(cmd::result::failure(cmd::codes::denied, std::format("'{}' was refused: the same request is already waiting for a person's answer", command_id)));
			return;
		}
		// After the duplicate above: its ticket is the waiting question's own, and discarding it here would leave a person
		// approving a ticket that is gone.
		const std::string_view asking_session = request.from.session;
		const auto of_session = std::count_if(m_waiting.begin(), m_waiting.end(),
			[asking_session](const waiting& w) { return w.request.session == asking_session; });
		if (static_cast<std::size_t>(of_session) >= max_waiting_per_session) {
			m_commands.discard(ticket_id);
			done(cmd::result::failure(cmd::codes::denied, std::format(
				"'{}' was refused: {} requests of this agent session already wait for a person's answer", command_id, max_waiting_per_session)));
			return;
		}
		// What the person reads is what the registry bound to the ticket, never text the agent chose.
		const std::optional<cmd::command_registry::pending_confirmation> bound = m_commands.confirmation_of(ticket_id);
		if (!bound) {
			done(cmd::result::failure(cmd::codes::denied, std::format("'{}' was refused: its confirmation is gone", command_id)));
			return;
		}
		// Text a command made up (and, via the repository's files, text a stranger may have influenced): shown as plain text.
		std::optional<std::string> detail = confirmation_detail_text(bound->detail);
		if (!detail) {
			m_commands.discard(ticket_id);
			egLOG_WARN("editor/agent", "'{}' from agent session '{}' was refused: its description is {} bytes, too long to show to a person",
				command_id, bound->session, bound->detail.size());
			done(cmd::result::failure(cmd::codes::denied, std::format(
				"'{}' was refused: what it would do is too long to show to a person (at most {} bytes in {} lines)", command_id,
				k_max_detail, k_max_detail_lines)));
			return;
		}
		const cmd::command_desc* command = m_commands.find(command_id);
		waiting entry;
		entry.ticket = ticket_id;
		entry.command_id = std::string(command_id);
		entry.call = std::move(request);
		entry.done = std::move(done);
		entry.request.id = m_next_id++;
		entry.request.command = bound->command;
		entry.request.summary = command != nullptr ? command->summary : std::string{};
		entry.request.reason = bound->reason;
		// All of it: what the person approves is never cut (the window scrolls).
		entry.request.args = bound->args;
		entry.request.session = bound->session;
		// The category the gate asked for (cmake_presets for a CMake file), so "allow for this session" grants that one.
		entry.request.category = bound->category;
		entry.request.detail = std::move(*detail);
		entry.request.can_grant_session = command != nullptr && command->risk == cmd::danger::none;
		entry.request.asked_at = now();
		entry.request.deadline = entry.request.asked_at + timeout;
		m_waiting.push_back(std::move(entry));
	}

	void agent_confirmations::note_shown(std::span<const shown_request> frame)
	{
		m_layout.settle(frame, now());
	}

	bool agent_confirmations::stood_still(std::uint64_t id) const
	{
		const std::optional<time_point> since = m_layout.steady_since(id);
		return since && now() >= *since + input_guard;
	}

	void agent_confirmations::finish_waiting(std::uint64_t id, std::optional<confirmation_answer> what, std::string_view why_not)
	{
		const auto found = std::find_if(m_waiting.begin(), m_waiting.end(), [id](const waiting& w) { return w.request.id == id; });
		if (found == m_waiting.end()) return;
		waiting entry = std::move(*found);
		// What stands below moves up in the next frame; an answer that comes before that frame must not find it ready.
		for (auto below = found + 1; below != m_waiting.end(); ++below) {
			m_layout.forget(below->request.id);
		}
		m_layout.forget(entry.request.id);
		m_waiting.erase(found);

		// A yes after the deadline is no yes, whoever asks: the answer itself, or a grant that settles the ones still waiting.
		std::string late_text;
		if (what && *what != confirmation_answer::deny && entry.request.deadline <= now()) {
			late_text = std::format("nobody answered within {} seconds", timeout.count());
			what = std::nullopt;
			why_not = late_text;
		}

		if (!what || *what == confirmation_answer::deny) {
			m_commands.discard(entry.ticket);
			const std::string why = what ? "a person declined it" : std::string(why_not);
			egLOG_INFO("editor/agent", "'{}' from agent session '{}' was not allowed: {}", entry.command_id, entry.request.session, why);
			entry.done(cmd::result::failure(cmd::codes::denied, std::format("'{}' was refused: {}", entry.command_id, why)));
			return;
		}

		if (m_commands.find(entry.command_id) == nullptr) {
			m_commands.discard(entry.ticket);
			entry.done(cmd::result::failure(cmd::codes::denied, std::format("'{}' was refused: the command is gone", entry.command_id)));
			return;
		}
		if (!m_commands.approve(entry.ticket)) {
			entry.done(cmd::result::failure(cmd::codes::denied, std::format("'{}' was refused: the confirmation expired", entry.command_id)));
			return;
		}
		// Only an approved ticket earns the session its grant, and never one for a command with a risk.
		const bool grant = *what == confirmation_answer::allow_session && entry.request.can_grant_session
			&& !entry.request.session.empty() && entry.request.category != cmd::agent_category::unset;
		if (grant) {
			m_grants.add(entry.request.session, entry.request.category);
		}
		entry.call.confirmation = entry.ticket;
		cmd::result outcome = run_command(entry.command_id, entry.call);
		// A ticket the registry did not consume (the policy turned to deny meanwhile) is not left waiting for its lifetime.
		m_commands.discard(entry.ticket);
		// A person in the middle of a gesture (often the one who just clicked "Allow") is waited out, not asked again.
		park_or_finish(entry.command_id, entry.call, std::move(entry.done), true, std::move(outcome), std::nullopt);

		if (grant) {
			// What already waits for the same session and category may be covered by the answer just given, so the
			// person is not asked about each of them again. Never by the ticket of its own question: nobody answered
			// that one, and the command would take the call for one a person looked at (#931).
			std::vector<std::uint64_t> covered;
			for (const waiting& other : m_waiting) {
				if (other.request.can_grant_session && other.request.session == entry.request.session
					&& other.request.category == entry.request.category) {
					covered.push_back(other.request.id);
				}
			}
			for (const std::uint64_t id : covered) {
				settle_by_grant(id);
			}
		}
	}

	void agent_confirmations::settle_by_grant(std::uint64_t id)
	{
		const auto waiting_of = [this, id] {
			return std::find_if(m_waiting.begin(), m_waiting.end(),
				[id](const waiting& w) { return w.request.id == id; });
		};
		auto found = waiting_of();
		// A question past its deadline is left to expire_late: a grant given later is no answer to it.
		if (found == m_waiting.end() || found->request.deadline <= now()) return;
		// A command that binds its run to what its question showed (a checkout to the commit shown, #931) waits for
		// the answer to that question.
		const cmd::command_desc* command = m_commands.find(found->command_id);
		if (command == nullptr || command->describe_before_allowed_invoke) return;
		cmd::call bare = found->call;
		bare.confirmation.clear();
		cmd::result outcome = run_command(found->command_id, bare);
		// Still asked (another category it needs: the registry answered with the ticket already waiting), or refused
		// without the person's answer: the question stays for that answer.
		if (outcome.code == cmd::codes::confirmation_required || outcome.code == cmd::codes::denied) return;
		found = waiting_of();
		if (found == m_waiting.end()) return;
		waiting entry = std::move(*found);
		for (auto below = found + 1; below != m_waiting.end(); ++below) {
			m_layout.forget(below->request.id);
		}
		m_layout.forget(entry.request.id);
		m_waiting.erase(found);
		m_commands.discard(entry.ticket);
		park_or_finish(entry.command_id, std::move(bare), std::move(entry.done), false, std::move(outcome),
			std::nullopt);
	}

	bool agent_confirmations::answer(std::uint64_t id, confirmation_answer what)
	{
		// Whether or not tick() has run yet: a late yes is no yes, and a request that has timed out above this one has
		// already moved it.
		expire_late();
		const auto found = std::find_if(m_waiting.begin(), m_waiting.end(), [id](const waiting& w) { return w.request.id == id; });
		if (found == m_waiting.end()) return false;
		if (what != confirmation_answer::deny && !stood_still(id)) {
			egLOG_INFO("editor/agent", "an answer to '{}' came before its button had stood still in the window: not taken", found->command_id);
			return false;
		}
		finish_waiting(id, what, {});
		return true;
	}

	void agent_confirmations::expire_late()
	{
		const auto current = now();
		std::vector<std::uint64_t> late;
		for (const waiting& w : m_waiting) {
			if (w.request.deadline <= current) late.push_back(w.request.id);
		}
		for (const std::uint64_t id : late) {
			try {
				finish_waiting(id, std::nullopt, std::format("nobody answered within {} seconds", timeout.count()));
			} catch (const std::exception& e) {
				egLOG_ERROR("agent", "an answer to an agent could not be delivered: {}", e.what());
			} catch (...) {
				egLOG_ERROR("agent", "an answer to an agent could not be delivered");
			}
		}
	}

	void agent_confirmations::tick()
	{
		expire_late();

		std::vector<retrying> due;
		due.swap(m_retrying);
		for (retrying& call : due) {
			// One call whose answer cannot be delivered must not leave the others in the list without theirs.
			try {
				if (call.approved) {
					run_approved(std::move(call.command_id), std::move(call.call), std::move(call.done), call.give_up_at);
				} else {
					start(std::move(call.command_id), std::move(call.call), std::move(call.done), call.give_up_at);
				}
			} catch (const std::exception& e) {
				egLOG_ERROR("agent", "an answer to an agent could not be delivered: {}", e.what());
			} catch (...) {
				egLOG_ERROR("agent", "an answer to an agent could not be delivered");
			}
		}
	}

	void agent_confirmations::drop_session(std::string_view session)
	{
		std::vector<std::uint64_t> mine;
		for (const waiting& w : m_waiting) {
			if (w.request.session == session) mine.push_back(w.request.id);
		}
		for (const std::uint64_t id : mine) {
			finish_waiting(id, std::nullopt, "the agent disconnected");
		}
		std::vector<retrying> gone;
		for (auto it = m_retrying.begin(); it != m_retrying.end();) {
			if (it->call.from.session == session) {
				gone.push_back(std::move(*it));
				it = m_retrying.erase(it);
			} else {
				++it;
			}
		}
		for (retrying& call : gone) {
			call.done(cmd::result::failure(cmd::codes::denied, std::format("'{}' was not run: the agent disconnected", call.command_id)));
		}
		m_grants.drop_session(session);
	}

	void agent_confirmations::decline_all(std::string_view why)
	{
		while (!m_waiting.empty()) {
			finish_waiting(m_waiting.front().request.id, std::nullopt, why);
		}
	}

	void agent_confirmations::cancel_all(std::string_view why)
	{
		decline_all(why);
		std::vector<retrying> parked;
		parked.swap(m_retrying);
		for (retrying& call : parked) {
			call.done(cmd::result::failure(cmd::codes::denied, std::format("'{}' was not run: {}", call.command_id, why)));
		}
	}

	std::vector<confirmation_request> agent_confirmations::pending() const
	{
		std::vector<confirmation_request> out;
		out.reserve(m_waiting.size());
		const auto current = now();
		for (const waiting& w : m_waiting) {
			if (w.request.deadline <= current) continue;
			out.push_back(w.request);
			out.back().can_allow = stood_still(w.request.id);
		}
		return out;
	}

	namespace {

		// Escapes, not Cyrillic in the source: MSVC reads a source file without /utf-8 in the active code page.
		const char* utf8(const char8_t* text)
		{
			return reinterpret_cast<const char*>(text);
		}

		constexpr const char8_t* label_allow_once = u8"\u0420\u0430\u0437\u0440\u0435\u0448\u0438\u0442\u044c \u043e\u0434\u0438\u043d \u0440\u0430\u0437";
		constexpr const char8_t* label_allow_session =
			u8"\u0420\u0430\u0437\u0440\u0435\u0448\u0438\u0442\u044c \u0434\u043e \u043a\u043e\u043d\u0446\u0430 \u0441\u0435\u0441\u0441\u0438\u0438 \u044d\u0442\u043e\u0433\u043e \u0430\u0433\u0435\u043d\u0442\u0430";
		constexpr const char8_t* label_deny = u8"\u041e\u0442\u043a\u043b\u043e\u043d\u0438\u0442\u044c";

	}

	bool confirmation_glyphs_available(ImFont& font)
	{
		for (const char8_t* label : { label_allow_once, label_allow_session, label_deny }) {
			for (const char8_t* at = label; *at != u8'\0';) {
				const unsigned int lead = *at;
				const int length = lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 : 0;
				if (length == 0) {
					return false;
				}
				unsigned int codepoint = length == 1 ? lead : lead & (0xFFu >> (length + 1));
				for (int i = 1; i < length; ++i) {
					if ((at[i] & 0xC0) != 0x80) {
						return false;
					}
					codepoint = (codepoint << 6) | (at[i] & 0x3Fu);
				}
				at += length;
				if (font.FindGlyphNoFallback(static_cast<ImWchar>(codepoint)) == nullptr) {
					return false;
				}
			}
		}
		return true;
	}

	bool draw_agent_confirmations(agent_confirmations& confirmations)
	{
		const std::vector<confirmation_request> requests = confirmations.pending();
		if (requests.empty()) {
			return true;
		}
		// A person's own modal goes first: this window over it would take the clicks meant for its buttons, so it waits unseen.
		if (ImGui::GetTopMostPopupModal() != nullptr) {
			confirmations.note_shown({});
			return true;
		}

		// Answers are applied after the window is closed: allowing runs the command, which must not happen inside it.
		std::vector<std::pair<std::uint64_t, confirmation_answer>> chosen;
		std::vector<shown_request> shown;
		shown.reserve(requests.size());

		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - 12.f, viewport->WorkPos.y + 12.f), ImGuiCond_Always,
			ImVec2(1.f, 0.f));
		// One width whatever the requests say: a window that grows and shrinks with its text moves the buttons sideways.
		ImGui::SetNextWindowSizeConstraints(ImVec2(640.f, 0.f), ImVec2(640.f, 720.f));
		const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
			| ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking;
		if (ImGui::Begin("Agent request##agent_confirmations", nullptr, flags)) {
			// Above every other window, every frame: a panel opened over this corner and closed again (an agent's command
			// can do both) would otherwise uncover a button that counted as shown all the while. The focus stays where it is.
			ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
			for (const confirmation_request& request : requests) {
				ImGui::PushID(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(request.id)));
				ImGui::PushTextWrapPos(0.f);
				ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), "An agent wants to run %s", request.command.c_str());
				if (!request.summary.empty()) ImGui::TextUnformatted(request.summary.c_str(), request.summary.c_str() + request.summary.size());
				ImGui::TextDisabled("session %s, %s", request.session.empty() ? "?" : request.session.c_str(), cmd::to_string(request.category));
				ImGui::PopTextWrapPos();
				if (!request.detail.empty()) {
					// As high as the text is once wrapped at the width it is drawn with, within 2 to 12 lines: the rest scrolls.
					const ImGuiStyle& style = ImGui::GetStyle();
					const char* const detail_begin = request.detail.c_str();
					const char* const detail_end = detail_begin + request.detail.size();
					const float line_height = ImGui::GetTextLineHeightWithSpacing();
					float wrap_width = std::max(ImGui::GetContentRegionAvail().x - 2.f * style.WindowPadding.x, 1.f);
					const float text_height = ImGui::CalcTextSize(detail_begin, detail_end, false, wrap_width).y;
					// A text that scrolls gives the width of the scrollbar away.
					if (text_height > 12.f * line_height) wrap_width = std::max(wrap_width - style.ScrollbarSize, 1.f);
					const float child_height = std::ceil(std::clamp(text_height, 2.f * line_height, 12.f * line_height)) + 2.f * style.WindowPadding.y;
					if (ImGui::BeginChild("detail", ImVec2(0.f, child_height), true)) {
						ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_width);
						ImGui::TextUnformatted(detail_begin, detail_end);
						ImGui::PopTextWrapPos();
					}
					ImGui::EndChild();
				}
				// The arguments in full, as the registry bound them: what is approved is what is shown, however long.
				if (ImGui::BeginChild("arguments", ImVec2(0.f, 140.f), true, ImGuiWindowFlags_HorizontalScrollbar)) {
					ImGui::TextUnformatted(request.args.c_str(), request.args.c_str() + request.args.size());
				}
				ImGui::EndChild();
				ImGui::BeginDisabled(!request.can_allow);
				if (ImGui::Button(utf8(label_allow_once))) {
					chosen.emplace_back(request.id, confirmation_answer::allow_once);
				}
				const ImVec2 allow_min = ImGui::GetItemRectMin();
				const ImVec2 allow_max = ImGui::GetItemRectMax();
				shown.push_back(shown_request{ request.id, shown_rect{ allow_min.x, allow_min.y, allow_max.x, allow_max.y } });
				if (request.can_grant_session) {
					ImGui::SameLine();
					if (ImGui::Button(utf8(label_allow_session))) {
						chosen.emplace_back(request.id, confirmation_answer::allow_session);
					}
				}
				ImGui::EndDisabled();
				ImGui::SameLine();
				if (ImGui::Button(utf8(label_deny))) {
					chosen.emplace_back(request.id, confirmation_answer::deny);
				}
				ImGui::Separator();
				ImGui::PopID();
			}
		}
		ImGui::End();

		confirmations.note_shown(shown);
		for (const auto& [id, what] : chosen) {
			confirmations.answer(id, what);
		}
		return true;
	}
}
