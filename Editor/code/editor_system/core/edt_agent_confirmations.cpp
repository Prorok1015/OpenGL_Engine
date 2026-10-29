#include "edt_agent_confirmations.h"
#include <snk/common/logger/engine_log.h>
#include <boost/json/serialize.hpp>
#include <imgui.h>
#include <algorithm>
#include <cstdint>
#include <format>

namespace json = boost::json;

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

	agent_confirmations::agent_confirmations(cmd::command_registry& commands, session_grants& grants, clock_function now)
		: m_commands(commands), m_grants(grants), m_clock(std::move(now))
	{
	}

	agent_confirmations::~agent_confirmations()
	{
		while (!m_waiting.empty()) {
			finish_waiting(m_waiting.front().request.id, std::nullopt, "the editor is closing");
		}
	}

	std::chrono::steady_clock::time_point agent_confirmations::now() const
	{
		return m_clock ? m_clock() : std::chrono::steady_clock::now();
	}

	void agent_confirmations::invoke(std::string_view command_id, cmd::call request, finish done)
	{
		cmd::result first = m_commands.invoke(command_id, request);
		if (first.code != cmd::codes::confirmation_required) {
			done(std::move(first));
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
		// What the person reads is what the registry bound to the ticket, never text the agent chose.
		const std::optional<cmd::command_registry::pending_confirmation> bound = m_commands.confirmation_of(ticket_id);
		if (!bound) {
			done(cmd::result::failure(cmd::codes::denied, std::format("'{}' was refused: its confirmation is gone", command_id)));
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
		entry.request.category = command != nullptr ? command->category : cmd::agent_category::unset;
		entry.request.asked_at = now();
		entry.request.deadline = entry.request.asked_at + timeout;
		m_waiting.push_back(std::move(entry));
	}

	void agent_confirmations::finish_waiting(std::uint64_t id, std::optional<confirmation_answer> what, std::string_view why_not)
	{
		const auto found = std::find_if(m_waiting.begin(), m_waiting.end(), [id](const waiting& w) { return w.request.id == id; });
		if (found == m_waiting.end()) return;
		waiting entry = std::move(*found);
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
		// Only an approved ticket earns the session its grant.
		const bool grant = *what == confirmation_answer::allow_session && !entry.request.session.empty()
			&& entry.request.category != cmd::agent_category::unset;
		if (grant) {
			m_grants.add(entry.request.session, entry.request.category);
		}
		entry.call.confirmation = entry.ticket;
		entry.done(m_commands.invoke(entry.command_id, entry.call));

		if (grant) {
			// What already waits for the same session and category is covered by the answer just given: settle it the way
			// "Allow once" would, so the person is not asked about each of them again.
			std::vector<std::uint64_t> covered;
			for (const waiting& other : m_waiting) {
				if (other.request.session == entry.request.session && other.request.category == entry.request.category) {
					covered.push_back(other.request.id);
				}
			}
			for (const std::uint64_t id : covered) {
				finish_waiting(id, confirmation_answer::allow_once, {});
			}
		}
	}

	bool agent_confirmations::answer(std::uint64_t id, confirmation_answer what)
	{
		const auto found = std::find_if(m_waiting.begin(), m_waiting.end(), [id](const waiting& w) { return w.request.id == id; });
		if (found == m_waiting.end()) return false;
		// Whether or not tick() has run yet: a late yes is no yes.
		if (found->request.deadline <= now()) {
			finish_waiting(id, std::nullopt, std::format("nobody answered within {} seconds", timeout.count()));
			return false;
		}
		finish_waiting(id, what, {});
		return true;
	}

	void agent_confirmations::tick()
	{
		const auto current = now();
		std::vector<std::uint64_t> late;
		for (const waiting& w : m_waiting) {
			if (w.request.deadline <= current) late.push_back(w.request.id);
		}
		for (const std::uint64_t id : late) {
			finish_waiting(id, std::nullopt, std::format("nobody answered within {} seconds", timeout.count()));
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
		m_grants.drop_session(session);
	}

	std::vector<confirmation_request> agent_confirmations::pending() const
	{
		std::vector<confirmation_request> out;
		out.reserve(m_waiting.size());
		const auto current = now();
		for (const waiting& w : m_waiting) {
			if (w.request.deadline > current) out.push_back(w.request);
		}
		return out;
	}

	namespace {

		// Escapes, not Cyrillic in the source: MSVC reads a source file without /utf-8 in the active code page.
		const char* utf8(const char8_t* text)
		{
			return reinterpret_cast<const char*>(text);
		}

	}

	bool draw_agent_confirmations(agent_confirmations& confirmations)
	{
		const std::vector<confirmation_request> requests = confirmations.pending();
		if (requests.empty()) {
			return true;
		}

		// Answers are applied after the window is closed: allowing runs the command, which must not happen inside it.
		std::vector<std::pair<std::uint64_t, confirmation_answer>> chosen;

		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - 12.f, viewport->WorkPos.y + 12.f), ImGuiCond_Always,
			ImVec2(1.f, 0.f));
		ImGui::SetNextWindowSizeConstraints(ImVec2(420.f, 0.f), ImVec2(640.f, 720.f));
		const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
			| ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking;
		if (ImGui::Begin("Agent request##agent_confirmations", nullptr, flags)) {
			for (const confirmation_request& request : requests) {
				ImGui::PushID(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(request.id)));
				ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), "An agent wants to run %s", request.command.c_str());
				if (!request.summary.empty()) ImGui::TextWrapped("%s", request.summary.c_str());
				ImGui::TextDisabled("session %s, %s", request.session.empty() ? "?" : request.session.c_str(), cmd::to_string(request.category));
				// The arguments in full, as the registry bound them: what is approved is what is shown, however long.
				if (ImGui::BeginChild("arguments", ImVec2(0.f, 140.f), true, ImGuiWindowFlags_HorizontalScrollbar)) {
					ImGui::TextUnformatted(request.args.c_str(), request.args.c_str() + request.args.size());
				}
				ImGui::EndChild();
				if (ImGui::Button(utf8(u8"\u0420\u0430\u0437\u0440\u0435\u0448\u0438\u0442\u044c \u043e\u0434\u0438\u043d \u0440\u0430\u0437"))) {
					chosen.emplace_back(request.id, confirmation_answer::allow_once);
				}
				ImGui::SameLine();
				if (ImGui::Button(utf8(u8"\u0420\u0430\u0437\u0440\u0435\u0448\u0438\u0442\u044c \u0434\u043e \u043a\u043e\u043d\u0446\u0430 \u0441\u0435\u0441\u0441\u0438\u0438 \u044d\u0442\u043e\u0433\u043e \u0430\u0433\u0435\u043d\u0442\u0430"))) {
					chosen.emplace_back(request.id, confirmation_answer::allow_session);
				}
				ImGui::SameLine();
				if (ImGui::Button(utf8(u8"\u041e\u0442\u043a\u043b\u043e\u043d\u0438\u0442\u044c"))) {
					chosen.emplace_back(request.id, confirmation_answer::deny);
				}
				ImGui::Separator();
				ImGui::PopID();
			}
		}
		ImGui::End();

		for (const auto& [id, what] : chosen) {
			confirmations.answer(id, what);
		}
		return true;
	}
}
