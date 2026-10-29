#include "edt_agent_confirmations.h"
#include <snk/common/logger/engine_log.h>
#include <boost/json/serialize.hpp>
#include <imgui.h>
#include <algorithm>
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

	std::string agent_confirmations::describe_arguments(const json::value& args, std::size_t limit)
	{
		std::string text = json::serialize(args);
		if (text.size() > limit) {
			text.resize(limit);
			text += "...";
		}
		return text;
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
		entry.request.args = bound->args.size() > 600 ? bound->args.substr(0, 600) + "..." : bound->args;
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

		if (!what || *what == confirmation_answer::deny) {
			m_commands.discard(entry.ticket);
			const std::string why = what ? "a person declined it" : std::string(why_not);
			egLOG_INFO("editor/agent", "'{}' from agent session '{}' was not allowed: {}", entry.command_id, entry.request.session, why);
			entry.done(cmd::result::failure(cmd::codes::denied, std::format("'{}' was refused: {}", entry.command_id, why)));
			return;
		}

		if (*what == confirmation_answer::allow_session && !entry.request.session.empty()
			&& entry.request.category != cmd::agent_category::unset) {
			m_grants.add(entry.request.session, entry.request.category);
		}
		if (!m_commands.approve(entry.ticket)) {
			entry.done(cmd::result::failure(cmd::codes::denied, std::format("'{}' was refused: the confirmation expired", entry.command_id)));
			return;
		}
		entry.call.confirmation = entry.ticket;
		entry.done(m_commands.invoke(entry.command_id, entry.call));
	}

	bool agent_confirmations::answer(std::uint64_t id, confirmation_answer what)
	{
		const bool waiting_now = std::any_of(m_waiting.begin(), m_waiting.end(), [id](const waiting& w) { return w.request.id == id; });
		if (waiting_now) finish_waiting(id, what, {});
		return waiting_now;
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
		for (const waiting& w : m_waiting) out.push_back(w.request);
		return out;
	}

	bool draw_agent_confirmations(agent_confirmations& confirmations)
	{
		confirmations.tick();
		const std::vector<confirmation_request> requests = confirmations.pending();
		if (requests.empty()) {
			return true;
		}

		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - 12.f, viewport->WorkPos.y + 12.f), ImGuiCond_Always,
			ImVec2(1.f, 0.f));
		ImGui::SetNextWindowSizeConstraints(ImVec2(360.f, 0.f), ImVec2(520.f, 600.f));
		const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
			| ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking;
		if (ImGui::Begin("Agent request##agent_confirmations", nullptr, flags)) {
			for (const confirmation_request& request : requests) {
				ImGui::PushID(static_cast<int>(request.id));
				ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), "An agent wants to run %s", request.command.c_str());
				if (!request.summary.empty()) ImGui::TextWrapped("%s", request.summary.c_str());
				ImGui::TextDisabled("session %s, %s", request.session.empty() ? "?" : request.session.c_str(), cmd::to_string(request.category));
				if (request.args != "{}") ImGui::TextWrapped("%s", request.args.c_str());
				if (ImGui::Button("Allow once")) confirmations.answer(request.id, confirmation_answer::allow_once);
				ImGui::SameLine();
				if (ImGui::Button("Allow until this agent session ends")) confirmations.answer(request.id, confirmation_answer::allow_session);
				ImGui::SameLine();
				if (ImGui::Button("Deny")) confirmations.answer(request.id, confirmation_answer::deny);
				ImGui::Separator();
				ImGui::PopID();
			}
		}
		ImGui::End();
		return true;
	}
}
