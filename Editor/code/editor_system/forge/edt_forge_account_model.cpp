#include "edt_forge_account_model.h"
#include "edt_forge_service.h"
#include "edt_forge_types.h"
#include <snk/command/cmd_types.h>
#include <algorithm>
#include <cctype>
#include <format>
#include <utility>

namespace {
	namespace json = boost::json;

	constexpr std::size_t k_message_cap = 300;

	std::string lower(std::string_view text)
	{
		std::string out(text);
		for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return out;
	}

	platform::secret_key key_of(const std::string& host)
	{
		return { edt::forge_service::k_secret_service, host };
	}

	// The store's own text, made plain; the token (when the call had one) is cut out of it as well.
	std::string safe_text(std::string_view text, const platform::secret_text* token)
	{
		std::string out = edt::forge::sanitize_text(text, k_message_cap);
		if (token != nullptr) edt::forge::scrub_secret(out, *token);
		return out;
	}

	// Visible ASCII only: a token with a space, a line break or a control character would break the header it goes into.
	bool looks_like_token(const std::string& value)
	{
		if (value.empty() || value.size() > edt::forge_account_model::k_max_token) return false;
		return std::all_of(value.begin(), value.end(), [](char c) {
			const auto byte = static_cast<unsigned char>(c);
			return byte > 0x20 && byte < 0x7f;
		});
	}

	std::string string_of(const json::object& object, std::string_view key)
	{
		const json::value* value = object.if_contains(key);
		return value && value->is_string() ? std::string(value->as_string()) : std::string{};
	}
}

std::string_view edt::forge_token_state_text(forge_token_state state)
{
	switch (state) {
	case forge_token_state::checking: return "checking...";
	case forge_token_state::not_set: return "not set";
	case forge_token_state::set_keychain: return "set (OS keychain)";
	case forge_token_state::set_session: return "set (this session only)";
	case forge_token_state::unknown: break;
	}
	return "unknown (the OS keychain could not be reached)";
}

void edt::wipe_buffer(char* data, std::size_t size)
{
	volatile char* bytes = data;
	for (std::size_t i = 0; i < size; ++i) bytes[i] = 0;
}

edt::forge_account_model::forge_account_model(editor_events& events, std::vector<std::string> hosts, ports ports)
	: m_events(events), m_ports(std::move(ports))
{
	for (std::string& host : hosts) {
		host = lower(host);
		if (host.empty() || find(host) != nullptr) continue;
		m_rows.push_back({ std::move(host), forge_token_state::checking, false });
	}
	m_finished = m_events.job_finished.subscribe(
		[this](std::uint64_t id, job_status status, const std::string& error, const std::optional<json::value>& result) {
			finished(id, status, error, result);
		});
}

const edt::forge_account_model::row* edt::forge_account_model::find(std::string_view host) const
{
	const std::string wanted = lower(host);
	const auto it = std::find_if(m_rows.begin(), m_rows.end(), [&wanted](const row& entry) { return entry.host == wanted; });
	return it == m_rows.end() ? nullptr : &*it;
}

edt::forge_account_model::row* edt::forge_account_model::find(std::string_view host)
{
	return const_cast<row*>(std::as_const(*this).find(host));
}

bool edt::forge_account_model::allows(std::string_view host) const
{
	return find(host) != nullptr;
}

bool edt::forge_account_model::busy() const
{
	return std::any_of(m_rows.begin(), m_rows.end(), [](const row& entry) { return entry.busy; });
}

void edt::forge_account_model::set_visible(bool visible)
{
	if (visible == m_visible) return;
	m_visible = visible;
	if (visible && !m_read) refresh();
}

void edt::forge_account_model::say(std::string text, bool error)
{
	m_message = std::move(text);
	m_message_error = error;
}

bool edt::forge_account_model::start(op kind, const std::string& host, std::string label, std::function<json::value()> work)
{
	row* entry = find(host);
	if (entry == nullptr) return false;
	if (!m_ports.start_job) {
		say("The token store is not available in this editor.", true);
		return false;
	}
	const std::optional<std::uint64_t> job = m_ports.start_job(std::move(label), std::move(work));
	if (!job) {
		say(std::format("Could not start the token job for {}.", host), true);
		return false;
	}
	m_jobs[*job] = pending{ kind, entry->host };
	entry->busy = true;
	return true;
}

void edt::forge_account_model::refresh()
{
	m_read = true;
	for (row& entry : m_rows) {
		if (entry.busy) continue;
		entry.state = forge_token_state::checking;
		const std::string host = entry.host;
		start(op::probe, host, std::format("Token state of {}", host), [secrets = m_ports.secrets, host]() -> json::value {
			if (!secrets) return json::object{ { "presence", "unknown" } };
			switch (secrets->presence(key_of(host))) {
			case platform::secret_presence::absent: return json::object{ { "presence", "absent" } };
			case platform::secret_presence::in_memory: return json::object{ { "presence", "memory" } };
			case platform::secret_presence::in_os_store: return json::object{ { "presence", "os" } };
			case platform::secret_presence::unknown: break;
			}
			return json::object{ { "presence", "unknown" } };
		});
		if (!entry.busy) entry.state = forge_token_state::unknown;
	}
}

bool edt::forge_account_model::save(std::string_view host, platform::secret_text&& token)
{
	// Whatever happens below, the token does not outlive this call except inside the job.
	platform::secret_text mine = std::move(token);
	row* entry = find(host);
	if (entry == nullptr) {
		say("Tokens can only be set for the hosts the editor allows.", true);
		return false;
	}
	if (entry->busy) {
		say(std::format("Wait until the current job for {} has finished.", entry->host), true);
		return false;
	}
	if (!looks_like_token(mine.reveal())) {
		say(std::format("That is not a token: it must be 1 to {} visible characters without spaces.", k_max_token), true);
		return false;
	}
	const std::string name = entry->host;
	// std::function needs a copyable work: the token is shared by the copies and wiped when the last one goes.
	auto held = std::make_shared<platform::secret_text>(std::move(mine));
	return start(op::save, name, std::format("Save the token of {}", name), [secrets = m_ports.secrets, name, held]() -> json::value {
		if (!secrets) return json::object{ { "ok", false }, { "persisted", false }, { "message", "no token store" } };
		const platform::secret_key key = key_of(name);
		const platform::secret_result stored = secrets->store(key, *held);
		const bool ok = stored.status == platform::secret_status::ok;
		const bool persisted = ok && secrets->persisted(key);
		json::string message(safe_text(stored.message, held.get()));
		held->wipe();
		return json::object{ { "ok", ok }, { "persisted", persisted }, { "message", std::move(message) } };
	});
}

bool edt::forge_account_model::save_from_buffer(std::string_view host, char* buffer, std::size_t capacity)
{
	std::size_t length = 0;
	while (length < capacity && buffer[length] != '\0') ++length;
	std::string value(buffer, length);
	wipe_buffer(buffer, capacity);
	// Pasted tokens often carry a line break at the end.
	while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
	std::size_t lead = 0;
	while (lead < value.size() && std::isspace(static_cast<unsigned char>(value[lead]))) ++lead;
	if (lead > 0) value.erase(0, lead);
	return save(host, platform::secret_text{ std::move(value) });
}

bool edt::forge_account_model::remove(std::string_view host)
{
	row* entry = find(host);
	if (entry == nullptr) {
		say("Tokens can only be removed for the hosts the editor allows.", true);
		return false;
	}
	if (entry->busy) {
		say(std::format("Wait until the current job for {} has finished.", entry->host), true);
		return false;
	}
	const std::string name = entry->host;
	return start(op::remove, name, std::format("Remove the token of {}", name), [secrets = m_ports.secrets, name]() -> json::value {
		if (!secrets) return json::object{ { "status", "failed" }, { "message", "no token store" } };
		const platform::secret_result erased = secrets->erase(key_of(name));
		const char* status = "failed";
		switch (erased.status) {
		case platform::secret_status::ok: status = "ok"; break;
		case platform::secret_status::not_found: status = "not_found"; break;
		case platform::secret_status::unavailable: status = "unavailable"; break;
		case platform::secret_status::failed: break;
		}
		return json::object{ { "status", status }, { "message", json::string(safe_text(erased.message, nullptr)) } };
	});
}

void edt::forge_account_model::finished(std::uint64_t id, job_status status, const std::string& error,
	const std::optional<json::value>& result)
{
	const auto it = m_jobs.find(id);
	if (it == m_jobs.end()) return;
	const pending job = std::move(it->second);
	m_jobs.erase(it);
	row* entry = find(job.host);
	if (entry == nullptr) return;
	entry->busy = false;

	if (status != job_status::done || !result || !result->is_object()) {
		// A cancel is the editor going down: nothing is said. The state is read again when it is asked for.
		if (status == job_status::cancelled) {
			entry->state = forge_token_state::unknown;
			return;
		}
		if (entry->state == forge_token_state::checking) entry->state = forge_token_state::unknown;
		say(std::format("The token job for {} failed: {}", job.host, safe_text(error, nullptr)), true);
		return;
	}

	const json::object& value = result->as_object();
	switch (job.kind) {
	case op::probe: {
		const std::string presence = string_of(value, "presence");
		entry->state = presence == "absent" ? forge_token_state::not_set
			: presence == "memory" ? forge_token_state::set_session
			: presence == "os" ? forge_token_state::set_keychain : forge_token_state::unknown;
		break;
	}
	case op::save: {
		const json::value* ok = value.if_contains("ok");
		const json::value* persisted = value.if_contains("persisted");
		const std::string text = string_of(value, "message");
		if (!ok || !ok->is_bool() || !ok->as_bool()) {
			if (entry->state == forge_token_state::checking) entry->state = forge_token_state::unknown;
			say(std::format("The token for {} was not saved{}{}", job.host, text.empty() ? "" : ": ", text), true);
			break;
		}
		if (persisted && persisted->is_bool() && persisted->as_bool()) {
			entry->state = forge_token_state::set_keychain;
			say(std::format("The token for {} was saved to the OS keychain.", job.host), false);
		} else {
			entry->state = forge_token_state::set_session;
			say(std::format("The token for {} is kept for this session only; it is not saved{}{}", job.host,
				text.empty() ? "" : ": ", text), false);
		}
		break;
	}
	case op::remove: {
		const std::string outcome = string_of(value, "status");
		const std::string text = string_of(value, "message");
		if (outcome == "ok") {
			entry->state = forge_token_state::not_set;
			say(std::format("The token for {} was removed.", job.host), false);
		} else if (outcome == "not_found") {
			entry->state = forge_token_state::not_set;
			say(std::format("No token was stored for {}.", job.host), false);
		} else {
			// The session copy is gone; a saved one may remain and come back after a restart.
			entry->state = forge_token_state::unknown;
			say(std::format("The token for {} was dropped from this session, but a saved copy may remain{}{}", job.host,
				text.empty() ? "" : ": ", text), true);
		}
		break;
	}
	}
}

edt::registration edt::register_forge_account_action(action_registry& actions, std::function<void()> open)
{
	return actions.add({ .id = "forge.account.open", .label = "Git Account...",
		.execute = no_args(std::move(open)),
		.expose_as_tool = false, .category = cmd::agent_category::not_for_agents });
}
