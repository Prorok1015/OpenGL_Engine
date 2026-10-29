#include <snk/editor/edt_mcp_bridge_host.h>
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>
#include <format>
#include <string_view>
#include <utility>

namespace json = boost::json;

namespace {
	constexpr const char* log_category = "Editor/MCP";
	constexpr const char* log_uri = "snk://log";
	constexpr const char* viewport_uri_prefix = "snk://viewport/";
	constexpr const char* viewport_uri_suffix = ".png";

	bool is_name_char(char c)
	{
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
	}

	// The result schema without the image bytes: they travel as MCP image content, not in structuredContent.
	json::value published_output_schema(const json::value& schema)
	{
		if (!schema.is_object()) return json::value();
		json::value out = schema;
		json::object& root = out.as_object();
		json::value* properties = root.if_contains("properties");
		if (!properties || !properties->is_object() || !properties->as_object().contains("png_base64")) return out;

		properties->as_object().erase("png_base64");
		if (json::value* required = root.if_contains("required"); required && required->is_array()) {
			json::array kept;
			for (const json::value& name : required->as_array()) {
				if (!(name.is_string() && std::string_view(name.as_string()) == "png_base64")) kept.push_back(name);
			}
			*required = std::move(kept);
		}
		return out;
	}

	std::string fingerprint(const cmd::command_desc& command)
	{
		return std::format("{}#{}#{}", command.id, command.version, static_cast<const void*>(&command));
	}

	mcp::call_result failed_call(std::string text)
	{
		mcp::call_result out;
		out.is_error = true;
		out.items.push_back(mcp::content::make_text(std::move(text)));
		return out;
	}

	bool viewport_id_is_uri_safe(const std::string& id)
	{
		if (id.empty()) return false;
		for (const char c : id) {
			if (!is_name_char(c) && c != '.') return false;
		}
		return true;
	}
}

edt::mcp_bridge_host::mcp_bridge_host(cmd::command_registry& registry, cmd::main_thread_queue& queue, options opts)
	: m_registry(registry)
	, m_queue(queue)
	, m_options(std::move(opts))
{
	ASSERT_MSG(m_registry.on_main_thread(), "mcp_bridge_host: create it on the main thread");
	refresh();
	m_waiter = std::thread([this] { run_waiter(); });
}

edt::mcp_bridge_host::~mcp_bridge_host()
{
	{
		std::lock_guard lock(m_wait_mutex);
		m_stop = true;
	}
	m_wait_changed.notify_all();
	if (m_waiter.joinable()) m_waiter.join();
}

bool edt::mcp_bridge_host::is_read_only_tool(const cmd::command_desc& command)
{
	return command.expose_as_tool && !command.mutates && !command.long_running && command.risk == cmd::danger::none;
}

std::string edt::mcp_bridge_host::tool_name(const cmd::command_desc& command)
{
	std::string name = command.id;
	for (char& c : name) {
		if (!is_name_char(c)) c = '_';
	}
	if (command.version > 1) name += std::format("_v{}", command.version);
	return name;
}

edt::mcp_bridge_host::refreshed edt::mcp_bridge_host::refresh()
{
	ASSERT_MSG(m_registry.on_main_thread(), "mcp_bridge_host::refresh: main thread only");

	std::vector<tool_entry> tools;
	std::vector<std::string> tool_keys;
	for (const cmd::command_desc* command : m_registry.list()) {
		if (!is_read_only_tool(*command)) continue;
		tool_entry entry;
		entry.command_id = command->id;
		entry.info.name = tool_name(*command);
		bool taken = false;
		for (const tool_entry& other : tools) {
			if (other.info.name == entry.info.name) taken = true;
		}
		if (taken) {
			// "a.b" and "a_b" would both be "a_b": the first keeps the name.
			egLOG_WARN(log_category, "command '{}' is not offered as a tool: the name '{}' is taken", command->id, entry.info.name);
			continue;
		}
		entry.info.description = command->summary;
		entry.info.input_schema = command->args_schema.is_object() ? command->args_schema : cmd::no_args_schema();
		entry.info.output_schema = published_output_schema(command->result_schema);
		entry.info.read_only = true;
		entry.info.destructive = false;
		tool_keys.push_back(fingerprint(*command));
		tools.push_back(std::move(entry));
	}

	std::vector<resource_entry> resources;
	std::vector<std::string> resource_keys;
	if (const cmd::command_desc* log_command = m_registry.find("log.read"); log_command && is_read_only_tool(*log_command)) {
		resource_entry entry;
		entry.command_id = log_command->id;
		entry.info = { log_uri, "Engine log", "The newest entries of the engine log journal (log.read)", "application/json" };
		resources.push_back(std::move(entry));
		resource_keys.push_back(fingerprint(*log_command));
	}
	if (const cmd::command_desc* capture = m_registry.find("viewport.capture"); capture && is_read_only_tool(*capture) && m_options.viewport_ids) {
		for (const std::string& id : m_options.viewport_ids()) {
			if (!viewport_id_is_uri_safe(id)) continue;
			resource_entry entry;
			entry.command_id = capture->id;
			entry.viewport = id;
			entry.info = { std::string(viewport_uri_prefix) + id + viewport_uri_suffix, "Viewport " + id, "Screenshot of the " + id + " viewport (viewport.capture)", "image/png" };
			resources.push_back(std::move(entry));
			resource_keys.push_back(fingerprint(*capture) + "/" + id);
		}
	}

	refreshed changed;
	changed.tools_changed = tool_keys != m_tool_keys;
	changed.resources_changed = resource_keys != m_resource_keys;
	m_tool_keys = std::move(tool_keys);
	m_resource_keys = std::move(resource_keys);
	if (changed.tools_changed || changed.resources_changed) {
		std::lock_guard lock(m_snapshot_mutex);
		m_tools = std::move(tools);
		m_resources = std::move(resources);
	}
	return changed;
}

std::vector<mcp::tool_info> edt::mcp_bridge_host::list_tools()
{
	std::lock_guard lock(m_snapshot_mutex);
	std::vector<mcp::tool_info> out;
	out.reserve(m_tools.size());
	for (const tool_entry& entry : m_tools) out.push_back(entry.info);
	return out;
}

std::vector<mcp::resource_info> edt::mcp_bridge_host::list_resources()
{
	std::lock_guard lock(m_snapshot_mutex);
	std::vector<mcp::resource_info> out;
	out.reserve(m_resources.size());
	for (const resource_entry& entry : m_resources) out.push_back(entry.info);
	return out;
}

mcp::call_result edt::mcp_bridge_host::to_call_result(const cmd::result& result) const
{
	if (!result.ok) return failed_call(result.code + ": " + result.error);

	json::value structured = result.value;
	mcp::call_result out;
	if (structured.is_object()) {
		json::object& object = structured.as_object();
		const json::value* png = object.if_contains("png_base64");
		if (png && png->is_string()) {
			std::string data(png->as_string());
			if (data.size() > m_options.max_image_base64_bytes) {
				return failed_call(std::format("the image is {} bytes as base64, above the limit of {}: ask for a smaller width and height", data.size(), m_options.max_image_base64_bytes));
			}
			object.erase("png_base64");
			out.items.push_back(mcp::content::make_image(std::move(data), "image/png"));
			// The numbers next to the picture, for clients that show text only.
			out.items.push_back(mcp::content::make_text(json::serialize(structured)));
		}
	}
	out.structured = std::move(structured);
	return out;
}

void edt::mcp_bridge_host::call_tool(std::string name, json::value args, mcp::progress_sink progress, std::function<void(mcp::call_result)> done)
{
	std::string command_id;
	{
		std::lock_guard lock(m_snapshot_mutex);
		for (const tool_entry& entry : m_tools) {
			if (entry.info.name == name) command_id = entry.command_id;
		}
	}
	if (command_id.empty()) {
		done(failed_call("unknown tool: " + name));
		return;
	}

	// Which command runs is decided here, from the snapshot of read-only commands; the main thread only runs it.
	std::future<cmd::result> future = m_queue.post(command_id, std::move(args), cmd::origin::agent("mcp"));
	wait_for_main(std::move(future),
		[this, done](cmd::result result) { done(to_call_result(result)); },
		[done](std::string why) { done(failed_call(std::move(why))); });
}

void edt::mcp_bridge_host::read_resource(std::string uri, std::function<void(mcp::resource_result)> done)
{
	std::string command_id;
	std::string viewport;
	{
		std::lock_guard lock(m_snapshot_mutex);
		for (const resource_entry& entry : m_resources) {
			if (entry.info.uri == uri) {
				command_id = entry.command_id;
				viewport = entry.viewport;
			}
		}
	}
	if (command_id.empty()) {
		mcp::resource_result missing;
		missing.error = "resource not found: " + uri;
		done(std::move(missing));
		return;
	}

	json::object args;
	if (!viewport.empty()) args["viewport"] = json::string(viewport.data(), viewport.size());
	const bool is_image = !viewport.empty();
	std::future<cmd::result> future = m_queue.post(command_id, std::move(args), cmd::origin::agent("mcp"));
	wait_for_main(std::move(future),
		[this, done, uri, is_image](cmd::result result) {
			mcp::resource_result out;
			if (!result.ok) {
				out.error = result.code + ": " + result.error;
				done(std::move(out));
				return;
			}
			mcp::resource_content content;
			content.uri = uri;
			if (is_image) {
				const json::value* png = result.value.is_object() ? result.value.as_object().if_contains("png_base64") : nullptr;
				if (!png || !png->is_string() || png->as_string().size() > m_options.max_image_base64_bytes) {
					out.error = "the screenshot is missing or larger than the limit: use the viewport_capture tool with a smaller size";
					done(std::move(out));
					return;
				}
				content.mime_type = "image/png";
				content.is_blob = true;
				content.blob = std::string(png->as_string());
			} else {
				content.mime_type = "application/json";
				content.text = json::serialize(result.value);
			}
			out.found = true;
			out.contents.push_back(std::move(content));
			done(std::move(out));
		},
		[done](std::string why) {
			mcp::resource_result out;
			out.error = std::move(why);
			done(std::move(out));
		});
}

void edt::mcp_bridge_host::log(mcp::level severity, std::string_view message)
{
	switch (severity) {
	case mcp::level::debug: egLOG_DEBUG(log_category, "{}", message); break;
	case mcp::level::info: egLOG_INFO(log_category, "{}", message); break;
	case mcp::level::warn: egLOG_WARN(log_category, "{}", message); break;
	case mcp::level::error: egLOG_ERROR(log_category, "{}", message); break;
	}
}

void edt::mcp_bridge_host::wait_for_main(std::future<cmd::result> future, std::function<void(cmd::result)> on_result, std::function<void(std::string)> on_failure)
{
	pending entry;
	entry.future = std::move(future);
	entry.deadline = std::chrono::steady_clock::now() + m_options.call_timeout;
	entry.on_result = std::move(on_result);
	entry.on_failure = std::move(on_failure);
	{
		std::lock_guard lock(m_wait_mutex);
		m_waiting.push_back(std::move(entry));
	}
	m_wait_changed.notify_one();
}

void edt::mcp_bridge_host::run_waiter()
{
	using namespace std::chrono_literals;
	std::unique_lock lock(m_wait_mutex);
	while (true) {
		m_wait_changed.wait(lock, [this] { return m_stop.load() || !m_waiting.empty(); });
		if (m_stop.load()) break;
		pending entry = std::move(m_waiting.front());
		m_waiting.pop_front();
		lock.unlock();

		// The calls are drained in order, so waiting for the oldest first loses nothing. The short wait only lets this
		// thread notice a shutdown or a deadline; a result that is ready ends it at once.
		while (true) {
			if (entry.future.wait_for(50ms) == std::future_status::ready) {
				entry.on_result(entry.future.get());
				break;
			}
			if (m_stop.load()) {
				entry.on_failure("the editor is shutting down");
				break;
			}
			if (std::chrono::steady_clock::now() >= entry.deadline) {
				entry.on_failure("the editor did not run the command in time");
				break;
			}
		}
		lock.lock();
	}

	// Shutting down: nobody will run what is still queued.
	while (!m_waiting.empty()) {
		pending entry = std::move(m_waiting.front());
		m_waiting.pop_front();
		lock.unlock();
		entry.on_failure("the editor is shutting down");
		lock.lock();
	}
}
