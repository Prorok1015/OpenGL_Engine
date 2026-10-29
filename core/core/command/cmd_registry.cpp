#include <snk/command/cmd_registry.h>
#include <snk/command/cmd_json_schema.h>
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>
#include <boost/json/serialize.hpp>
#include <algorithm>
#include <format>
#include <random>
#include <utility>

namespace json = boost::json;

boost::json::value cmd::no_args_schema()
{
	return boost::json::value{ { "type", "object" }, { "properties", boost::json::object{} }, { "additionalProperties", false } };
}

struct cmd::command_registry::entry
{
	std::uint64_t key = 0;
	command_desc desc;

	bool is_enabled() const { return !desc.enabled || desc.enabled(); }
};

cmd::command_registry::command_registry() = default;
cmd::command_registry::~command_registry() = default;

cmd::registration cmd::command_registry::add(command_desc desc)
{
	if (desc.id.empty() || !desc.invoke) {
		egLOG_ERROR("command", "command '{}' not registered: it needs an id and invoke", desc.id);
		return {};
	}
	if (contains(desc.id)) {
		egLOG_ERROR("command", "command '{}' not registered: the id is already taken", desc.id);
		return {};
	}

	if (desc.args_schema.is_null()) desc.args_schema = no_args_schema();
	if (desc.result_schema.is_null()) desc.result_schema = no_args_schema();
	for (const auto* schema : { &desc.args_schema, &desc.result_schema }) {
		if (auto problem = json_schema::check_schema(*schema)) {
			egLOG_ERROR("command", "command '{}' not registered: bad schema {}", desc.id, *problem);
			return {};
		}
	}
	const json::value* args_type = desc.args_schema.as_object().if_contains("type");
	if (!args_type || *args_type != json::value("object")) {
		egLOG_ERROR("command", "command '{}' not registered: args_schema must have \"type\":\"object\" at the top level", desc.id);
		return {};
	}
	if (desc.version == 0) {
		egLOG_ERROR("command", "command '{}' not registered: version starts at 1", desc.id);
		return {};
	}

	auto added = std::make_shared<entry>();
	added->key = m_next_key++;
	added->desc = std::move(desc);
	const std::uint64_t key = added->key;
	m_commands.push_back(std::move(added));
	return registration(m_alive, [this, key] {
		std::erase_if(m_commands, [key](const std::shared_ptr<entry>& e) { return e->key == key; });
	});
}

bool cmd::command_registry::contains(std::string_view id) const
{
	return find(id) != nullptr;
}

std::size_t cmd::command_registry::size() const
{
	return m_commands.size();
}

const cmd::command_desc* cmd::command_registry::find(std::string_view id) const
{
	const auto it = std::find_if(m_commands.begin(), m_commands.end(),
		[id](const std::shared_ptr<entry>& e) { return e->desc.id == id; });
	return it != m_commands.end() ? &(*it)->desc : nullptr;
}

bool cmd::command_registry::is_enabled(std::string_view id) const
{
	const auto it = std::find_if(m_commands.begin(), m_commands.end(),
		[id](const std::shared_ptr<entry>& e) { return e->desc.id == id; });
	return it != m_commands.end() && (*it)->is_enabled();
}

std::vector<const cmd::command_desc*> cmd::command_registry::list() const
{
	std::vector<const command_desc*> out;
	out.reserve(m_commands.size());
	for (const auto& c : m_commands) out.push_back(&c->desc);
	return out;
}

cmd::result cmd::command_registry::invoke(std::string_view id, const call& request)
{
	ASSERT_MSG(on_main_thread(), "command_registry::invoke('{}'): main thread only, use main_thread_queue from other threads", id);

	const auto it = std::find_if(m_commands.begin(), m_commands.end(),
		[id](const std::shared_ptr<entry>& e) { return e->desc.id == id; });
	if (it == m_commands.end()) return result::failure(codes::not_found, std::format("no command '{}'", id));
	// Held while it runs: the command may unregister itself.
	const std::shared_ptr<entry> command = *it;
	if (!command->is_enabled()) return result::failure(codes::disabled, std::format("command '{}' is disabled", id));
	if (!request.args.is_object()) return result::failure(codes::invalid_args, std::format("invalid arguments for '{}': expected an object", id));
	// Before the schema: a call that is refused learns nothing about the arguments.
	if (m_gate && request.from.kind == origin_kind::agent) {
		if (std::optional<result> refused = apply_policy(command->desc, request)) return std::move(*refused);
	}
	if (auto problem = json_schema::validate(command->desc.args_schema, request.args))
		return result::failure(codes::invalid_args, std::format("invalid arguments for '{}': {}", id, *problem));

	result outcome = command->desc.invoke(request);
	if (outcome.ok) {
		if (auto problem = json_schema::validate(command->desc.result_schema, outcome.value)) {
			egLOG_ERROR("command", "command '{}' ran but returned a malformed result: {}", id, *problem);
			return result::failure(codes::result_invalid, std::format("command '{}' ran but returned a malformed result: {}", id, *problem));
		}
	}
	return outcome;
}

void cmd::command_registry::set_policy_gate(std::shared_ptr<policy_gate> gate)
{
	ASSERT_MSG(on_main_thread(), "command_registry::set_policy_gate: main thread only");
	m_gate = std::move(gate);
	if (!m_gate) m_tickets.clear();
}

void cmd::command_registry::set_clock(std::function<std::chrono::steady_clock::time_point()> now)
{
	m_clock = std::move(now);
}

std::chrono::steady_clock::time_point cmd::command_registry::now() const
{
	return m_clock ? m_clock() : std::chrono::steady_clock::now();
}

void cmd::command_registry::drop_expired_tickets()
{
	const auto current = now();
	std::erase_if(m_tickets, [current](const confirmation_ticket& ticket) { return ticket.expires <= current; });
}

std::size_t cmd::command_registry::pending_confirmations() const
{
	return m_tickets.size();
}

std::optional<cmd::command_registry::pending_confirmation> cmd::command_registry::confirmation_of(std::string_view ticket)
{
	ASSERT_MSG(on_main_thread(), "command_registry::confirmation_of: main thread only");
	drop_expired_tickets();
	const auto found = std::find_if(m_tickets.begin(), m_tickets.end(), [ticket](const confirmation_ticket& t) { return t.id == ticket; });
	if (found == m_tickets.end()) return std::nullopt;
	return pending_confirmation{ found->command, found->args, found->reason, found->session, found->approved };
}

bool cmd::command_registry::approve(std::string_view ticket)
{
	ASSERT_MSG(on_main_thread(), "command_registry::approve: main thread only");
	drop_expired_tickets();
	const auto found = std::find_if(m_tickets.begin(), m_tickets.end(), [ticket](const confirmation_ticket& t) { return t.id == ticket; });
	if (found == m_tickets.end()) return false;
	found->approved = true;
	return true;
}

void cmd::command_registry::discard(std::string_view ticket)
{
	ASSERT_MSG(on_main_thread(), "command_registry::discard: main thread only");
	std::erase_if(m_tickets, [ticket](const confirmation_ticket& t) { return t.id == ticket; });
}

std::optional<cmd::result> cmd::command_registry::apply_policy(const command_desc& command, const call& request)
{
	const auto denied = [&command](std::string why) {
		return result::failure(codes::denied, std::format("'{}' was refused: {}", command.id, why));
	};
	drop_expired_tickets();

	const policy_decision decision = m_gate->decide(command, request);
	if (decision.mode == policy_mode::deny) {
		return denied(decision.reason.empty() ? "the policy denies it" : decision.reason);
	}
	if (decision.mode == policy_mode::allow) {
		// A ticket is dropped only by the call it was issued for: a guessed id must not wipe another session's question.
		if (!request.confirmation.empty()) {
			const std::string args = boost::json::serialize(request.args);
			std::erase_if(m_tickets, [&](const confirmation_ticket& t) {
				return t.id == request.confirmation && t.command == command.id && t.args == args && t.session == request.from.session;
			});
		}
		return std::nullopt;
	}

	if (!m_gate->can_ask()) {
		return denied("it needs a person's confirmation and nobody can be asked here");
	}
	const std::string args = boost::json::serialize(request.args);
	if (!request.confirmation.empty()) {
		const auto found = std::find_if(m_tickets.begin(), m_tickets.end(),
			[&request](const confirmation_ticket& t) { return t.id == request.confirmation; });
		if (found == m_tickets.end()) return denied("the confirmation is unknown, expired or already used");
		if (found->command != command.id || found->args != args || found->session != request.from.session) {
			return denied("the confirmation was given for another call");
		}
		if (!found->approved) return denied("the confirmation was not given");
		m_tickets.erase(found);
		return std::nullopt;
	}

	const auto describe = [&command](const confirmation_ticket& ticket) {
		result answer = result::failure(codes::confirmation_required, std::format("'{}' needs a person's confirmation", command.id));
		answer.value = boost::json::object{
			{ "ticket", ticket.id },
			{ "command", command.id },
			{ "reason", ticket.reason },
			{ "expires_in_s", static_cast<std::int64_t>(confirmation_lifetime.count()) },
		};
		return answer;
	};
	// A call that cannot run is not worth a person's attention: it fails here, with nobody asked.
	if (auto problem = json_schema::validate(command.args_schema, request.args)) {
		return result::failure(codes::invalid_args, std::format("invalid arguments for '{}': {}", command.id, *problem));
	}
	// The same question asked again is the question already waiting, not a second one.
	const auto same = std::find_if(m_tickets.begin(), m_tickets.end(), [&](const confirmation_ticket& t) {
		return t.command == command.id && t.args == args && t.session == request.from.session;
	});
	if (same != m_tickets.end()) return describe(*same);

	const auto of_session = std::count_if(m_tickets.begin(), m_tickets.end(),
		[&request](const confirmation_ticket& t) { return t.session == request.from.session; });
	if (static_cast<std::size_t>(of_session) >= max_pending_confirmations_per_session) {
		return denied("too many of its confirmations are waiting for a person");
	}
	if (m_tickets.size() >= max_pending_confirmations) {
		return denied("too many confirmations are waiting for a person");
	}
	// The ticket is a secret between the registry and whoever asks: 128 bits straight from the system's source.
	std::random_device entropy;
	const auto word = [&entropy] {
		const std::uint64_t high = entropy();
		return (high << 32) | entropy();
	};
	confirmation_ticket ticket;
	const std::uint64_t first = word();
	const std::uint64_t second = word();
	ticket.id = std::format("{:016x}{:016x}", first, second);
	ticket.command = command.id;
	ticket.args = args;
	ticket.session = request.from.session;
	ticket.reason = decision.reason;
	ticket.expires = now() + confirmation_lifetime;
	result answer = describe(ticket);
	m_tickets.push_back(std::move(ticket));
	return answer;
}

cmd::result cmd::command_registry::invoke(std::string_view id, boost::json::value args, origin from)
{
	return invoke(id, call{ std::move(args), std::move(from), 0 });
}
