#include <snk/command/cmd_registry.h>
#include <snk/command/cmd_json_schema.h>
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>
#include <algorithm>
#include <format>
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

cmd::result cmd::command_registry::invoke(std::string_view id, boost::json::value args, origin from)
{
	return invoke(id, call{ std::move(args), std::move(from), 0 });
}
