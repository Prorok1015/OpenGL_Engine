#include "edt_mcp_bridge_host.h"
#include "edt_agent_access.h"

#include <snk/command/cmd_json_schema.h>
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_job_service.h>

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/string.hpp>
#include <boost/json/value.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json = boost::json;

namespace {
	// What an agent may ask of viewport.capture. A request for 8192 x 8192 is a quarter of a gigabyte of readback
	// and copies; the answer is a PNG that has to be base64 encoded and sent.
	constexpr std::int64_t agent_max_capture_side = 2048;
	constexpr std::int64_t agent_max_capture_pixels = agent_max_capture_side * agent_max_capture_side;
	// The view is read back whole before it is scaled down to that, on the main thread: a view of 8 Mpixels (4K UHD
	// fits) is 32 MiB, plus the flipped copy. A bigger view is refused for an agent instead of read.
	constexpr std::int64_t agent_max_source_pixels = 8LL * 1024 * 1024;
	// Width asked when the agent names no size (the design's 1280 pixels on the long side, for a landscape view).
	constexpr std::int64_t agent_default_capture_width = 1280;
	// Largest answer sent to an agent, in bytes of JSON or base64 text.
	constexpr std::size_t agent_max_response_bytes = 16u * 1024u * 1024u;

	bool is_capture(std::string_view command_id)
	{
		return command_id == "viewport.capture";
	}

	// Limits the size an agent asks of viewport.capture. A size that is not an integer is left for the schema check of
	// the registry to refuse. Returns the reason when the request is too big.
	// The side the agent does not name is derived by the command from the aspect of the view (a tall view and
	// width=2048 would be 2048 x 8192), so the limits go into the request too (max_side, max_pixels): the command
	// refuses a derived side or a pixel count over them before it reads the view back.
	std::optional<std::string> limit_capture_args(json::value& args)
	{
		if (!args.is_object()) {
			return std::nullopt;
		}
		json::object& object = args.as_object();
		bool sized = false;
		for (const char* side : { "width", "height" }) {
			const json::value* value = object.if_contains(side);
			if (value == nullptr) {
				continue;
			}
			sized = true;
			const std::optional<std::int64_t> number = cmd::json_schema::integer_of(*value);
			if (number && *number > agent_max_capture_side) {
				return std::string(side) + " is limited to " + std::to_string(agent_max_capture_side) + " pixels for an agent";
			}
		}
		if (!sized) {
			object["width"] = agent_default_capture_width;
		}
		// The agent may ask for less than the ceilings, never for more.
		const auto ceiling = [&object](const char* key, std::int64_t limit) {
			std::int64_t value = limit;
			if (const json::value* given = object.if_contains(key)) {
				if (const std::optional<std::int64_t> number = cmd::json_schema::integer_of(*given)) {
					value = std::min(*number, limit);
				}
			}
			object[key] = value;
		};
		ceiling("max_side", agent_max_capture_side);
		ceiling("max_pixels", agent_max_capture_pixels);
		ceiling("max_source_pixels", agent_max_source_pixels);
		return std::nullopt;
	}

	std::string too_large_text(std::size_t bytes)
	{
		return "the result is too large to send (" + std::to_string(bytes) + " bytes, the limit is "
			+ std::to_string(agent_max_response_bytes) + "); ask for less (a smaller size, a smaller page)";
	}

	std::string text_of(const json::string& text)
	{
		return std::string(text.data(), text.size());
	}

	const cmd::command_desc* find_tool(const cmd::command_registry& registry, edt::agent_policy& policy, std::string_view name)
	{
		for (const cmd::command_desc* command : registry.list()) {
			if (edt::agent_tool_name(*command) == name && edt::agent_may_use(*command, policy)) {
				return command;
			}
		}
		return nullptr;
	}

	// A result that carries a PNG as base64 text (viewport.capture) goes to the client as an image block, not as a
	// megabyte string inside the JSON; its tool declares no outputSchema because the structured part lacks the field.
	bool has_png_field(const json::value& schema)
	{
		if (!schema.is_object()) {
			return false;
		}
		const json::value* properties = schema.as_object().if_contains("properties");
		return properties != nullptr && properties->is_object() && properties->as_object().contains("png_base64");
	}

	// A command that needs a display in a headless editor: every call is refused by the registry with this code.
	bool unavailable_headless(const cmd::command_desc& command, const cmd::command_registry& registry)
	{
		return command.needs_display && registry.headless();
	}

	// The answer of a `starts_job` command may come back with the job's outcome (a client that asked for progress waits
	// for it): the declared schema lets those fields through, also where it closes the object.
	json::value with_job_outcome(json::value schema)
	{
		if (!schema.is_object()) {
			return schema;
		}
		json::value* properties = schema.as_object().if_contains("properties");
		if (properties == nullptr || !properties->is_object() || !properties->as_object().contains("job")
			|| properties->as_object().contains("status")) {
			return schema;
		}
		json::object& fields = properties->as_object();
		fields["status"] = json::parse(R"json({"type":"string","enum":["running","done","failed","cancelled"]})json");
		fields["result"] = json::object{};
		fields["error"] = json::parse(R"json({"type":"string"})json");
		fields["result_dropped"] = json::parse(R"json({"type":"boolean"})json");
		return schema;
	}

	mcp::tool_info describe(const cmd::command_desc& command, const cmd::command_registry& registry)
	{
		mcp::tool_info tool;
		tool.name = edt::agent_tool_name(command);
		tool.description = command.summary;
		tool.input_schema = command.args_schema;
		if (!has_png_field(command.result_schema)) {
			tool.output_schema = command.starts_job ? with_job_outcome(command.result_schema) : command.result_schema;
		}
		tool.read_only = !command.mutates;
		// MCP reads a missing destructiveHint as true for a tool that writes: say it where the command is not risky.
		tool.destructive = command.mutates && (command.destructive || command.risk != cmd::danger::none);
		tool.idempotent = false;
		tool.open_world = false;
		tool.unavailable_headless = unavailable_headless(command, registry);
		return tool;
	}

	mcp::call_result error_result(std::string text)
	{
		mcp::call_result result;
		result.is_error = true;
		result.content.push_back(mcp::content_block::make_text(std::move(text)));
		return result;
	}

	mcp::call_result to_call_result(cmd::result outcome)
	{
		if (!outcome.ok) {
			return error_result(outcome.code + ": " + outcome.error);
		}
		mcp::call_result result;
		json::value value = std::move(outcome.value);
		if (value.is_object()) {
			json::object& object = value.as_object();
			const json::value* png = object.if_contains("png_base64");
			if (png != nullptr && png->is_string()) {
				if (png->as_string().size() > agent_max_response_bytes) {
					return error_result(too_large_text(png->as_string().size()));
				}
				result.content.push_back(mcp::content_block::make_image(text_of(png->as_string()), "image/png"));
				object.erase("png_base64");
				result.content.push_back(mcp::content_block::make_text(json::serialize(value)));
				result.structured = std::move(value);
				return result;
			}
		}
		const std::size_t size = json::serialize(value).size();
		if (size > agent_max_response_bytes) {
			return error_result(too_large_text(size));
		}
		result.structured = std::move(value);
		return result;
	}

	// A resource is a read the client cannot be asked about: only a command that reads and that the policy simply allows.
	bool resource_readable(const cmd::command_desc& command, edt::agent_policy& policy)
	{
		return command.category == cmd::agent_category::read && !command.mutates && edt::agent_may_use(command, policy)
			&& policy.effective(command).mode == cmd::policy_mode::allow;
	}

	// The resources the editor offers, each read by one command.
	struct resource_def
	{
		std::string_view uri;
		std::string_view name;
		std::string_view description;
		std::string_view mime_type;
		std::string_view command;
		std::string_view args;
	};

	constexpr std::array<resource_def, 5> resource_defs = { {
		{ "snk://log", "Engine log", "The latest entries of the engine log (log.read)", "application/json", "log.read", "{}" },
		{ "snk://jobs", "Background jobs", "The running background jobs (job.list)", "application/json", "job.list", "{}" },
		{ "snk://dialogs", "Open dialogs", "The dialogs waiting for an answer (dialog.list)", "application/json", "dialog.list", "{}" },
		{ "snk://viewport/scene.png", "Scene view", "Screenshot of the scene view", "image/png", "viewport.capture", R"({"viewport":"scene"})" },
		{ "snk://viewport/game.png", "Game view", "Screenshot of the game view", "image/png", "viewport.capture", R"({"viewport":"game"})" },
	} };

	// The job a successful result started: `{job: N}` with N > 0 (a build that did not start says job 0); 0 when none.
	// Not an optional: GCC 13 -O2 reports the optional's value as maybe uninitialized once inlined (-Werror).
	std::uint64_t job_of(const json::value& value)
	{
		if (!value.is_object()) {
			return 0;
		}
		const json::value* job = value.as_object().if_contains("job");
		if (job == nullptr) {
			return 0;
		}
		if (job->is_int64() && job->as_int64() > 0) {
			return static_cast<std::uint64_t>(job->as_int64());
		}
		if (job->is_uint64()) {
			return job->as_uint64();
		}
		return 0;
	}

	// The job's outcome added to the command's answer. The answer goes out twice (structuredContent and its text copy),
	// so a job result over half the response limit is left out (`result_dropped`; job.get still has it) rather than
	// turning the whole answer into a "too large" error that would lose the job and its status.
	mcp::call_result job_outcome(json::object answer, edt::job_status status, const std::string& error,
		const std::optional<json::value>& result, bool result_dropped)
	{
		answer["status"] = edt::job_status_name(status);
		if (result) {
			if (json::serialize(*result).size() > agent_max_response_bytes / 2) {
				result_dropped = true;
			} else {
				answer["result"] = *result;
			}
		}
		if (status == edt::job_status::failed || !error.empty()) {
			answer["error"] = json::string(std::string_view(error));
		}
		if (result_dropped) {
			answer["result_dropped"] = true;
		}
		return to_call_result(cmd::result::success(std::move(answer)));
	}

	mcp::resource_result unavailable(std::string why)
	{
		mcp::resource_result result;
		result.found = false;
		result.error = std::move(why);
		return result;
	}
}

bool edt::agent_may_use(const cmd::command_desc& command, agent_policy& policy)
{
	if (!command.expose_as_tool) {
		return false;
	}
	// Whatever the policy files say, a command nobody classified, or one that is nothing for an agent, does not exist for it.
	if (command.category == cmd::agent_category::unset || command.category == cmd::agent_category::not_for_agents) {
		return false;
	}
	return policy.effective(command).mode != cmd::policy_mode::deny;
}

std::string edt::agent_tool_name(const cmd::command_desc& command)
{
	std::string name = command.id;
	for (char& c : name) {
		if (c == '.') {
			c = '_';
		}
	}
	if (command.version > 1) {
		name += "_v" + std::to_string(command.version);
	}
	return name;
}

// Read and written on the main thread only: the destructor and the posted calls both run there, so a call that is
// still queued when the host goes sees null pointers and answers with an error.
struct edt::mcp_bridge_host::state
{
	using time_point = std::chrono::steady_clock::time_point;

	// A tools/call whose answer waits for its job.
	struct held_call
	{
		std::string session;
		json::value request_id;
		std::uint64_t job = 0;
		json::object answer;
		mcp::progress_sink progress;
		std::function<void(mcp::call_result)> done;
		time_point deadline{};
		time_point last_sent{};
		bool sent_any = false;
		// MCP wants the progress of one token to increase: a value not above it is not sent.
		float last_fraction = 0.f;
		// The highest progress that came within the interval after the last one sent, with the latest note.
		std::optional<std::pair<float, std::string>> waiting;
	};

	// A tools/call between its hop to the main thread and its answer (or hold). notifications/cancelled or the end of
	// the session may come while it waits for a person or a busy retry: it is marked, and its answer is dropped then.
	struct pending_call
	{
		std::string session;
		json::value request_id;
		bool cancelled = false;
	};

	// At most this many calls of one session wait for their jobs; more are answered at once with `{job}`.
	static constexpr std::size_t max_held_per_session = 16;

	cmd::command_registry* commands = nullptr;
	agent_access* access = nullptr;
	session_closed_hook on_session_closed;
	// Null: calls are not held.
	job_service* jobs = nullptr;
	mcp_bridge_options options;
	std::vector<held_call> held;
	std::map<std::uint64_t, pending_call> pending;
	std::uint64_t next_pending = 1;
	connection progress_listener;
	connection finished_listener;

	time_point now() const
	{
		return options.clock ? options.clock() : std::chrono::steady_clock::now();
	}

	static void send(held_call& call, float fraction, const std::string& note, time_point at)
	{
		if (call.progress) {
			call.progress(static_cast<double>(fraction), 1.0, note);
		}
		call.last_sent = at;
		call.sent_any = true;
		call.last_fraction = fraction;
		call.waiting.reset();
	}

	static void send_waiting(held_call& call, time_point at)
	{
		if (call.waiting) {
			const std::pair<float, std::string> latest = std::move(*call.waiting);
			send(call, latest.first, latest.second, at);
		}
	}

	std::uint64_t begin_call(std::string session, json::value request_id)
	{
		const std::uint64_t key = next_pending++;
		pending.emplace(key, pending_call{ std::move(session), std::move(request_id), false });
		return key;
	}

	// Forgets the call; true when it was cancelled meanwhile.
	bool end_call(std::uint64_t key)
	{
		const auto found = pending.find(key);
		if (found == pending.end()) {
			return false;
		}
		const bool cancelled = found->second.cancelled;
		pending.erase(found);
		return cancelled;
	}

	// Holds the answer while the job runs, answers at once with its outcome when it already ended. False: the service
	// does not know the job, or the session already has max_held_per_session calls held, and `done` was not used.
	bool follow(std::uint64_t job, json::object answer, std::string session, json::value request_id, mcp::progress_sink& progress,
		std::function<void(mcp::call_result)>& done)
	{
		const std::optional<job_service::job_info> info = jobs->info(job);
		if (!info) {
			return false;
		}
		const std::size_t session_held = static_cast<std::size_t>(
			std::count_if(held.begin(), held.end(), [&session](const held_call& call) { return call.session == session; }));
		if (info->status == job_status::running && session_held >= max_held_per_session) {
			return false;
		}
		if (info->status != job_status::running) {
			done(job_outcome(std::move(answer), info->status, info->error, info->result, info->result_dropped));
			return true;
		}
		held_call call;
		call.session = std::move(session);
		call.request_id = std::move(request_id);
		call.job = job;
		call.answer = std::move(answer);
		call.progress = std::move(progress);
		call.done = std::move(done);
		call.deadline = now() + options.hold_limit;
		held.push_back(std::move(call));
		return true;
	}

	void on_progress(std::uint64_t job, float fraction, const std::string& note)
	{
		const time_point at = now();
		for (held_call& call : held) {
			if (call.job != job) {
				continue;
			}
			if (call.sent_any && fraction <= call.last_fraction) {
				if (call.waiting) {
					call.waiting->second = note;
				}
				continue;
			}
			if (!call.sent_any || at - call.last_sent >= options.progress_interval) {
				send(call, fraction, note, at);
			} else {
				const float highest = call.waiting ? std::max(call.waiting->first, fraction) : fraction;
				// A finished task reports 1.0 without a note (job_service::retire): keep the last thing said.
				std::string kept = note.empty() && call.waiting ? std::move(call.waiting->second) : note;
				call.waiting.emplace(highest, std::move(kept));
			}
		}
	}

	void on_finished(std::uint64_t job, job_status status, const std::string& error, const std::optional<json::value>& result)
	{
		std::vector<held_call> ended = take([job](const held_call& call) { return call.job == job; });
		const time_point at = now();
		for (held_call& call : ended) {
			send_waiting(call, at);
			call.done(job_outcome(std::move(call.answer), status, error, result, false));
		}
	}

	// Cancels the jobs of the calls `match` picks and drops their answers (the transport completes a request whose
	// answer is dropped); calls not held yet are marked, and their answers dropped when they come. Only a `starts_job`
	// command's call is held, so the job is the one this call started, never one it merely reported.
	template<class Match>
	void cancel(Match match)
	{
		for (auto& entry : pending) {
			if (match(entry.second)) {
				entry.second.cancelled = true;
			}
		}
		cancel_held(match);
	}

	// Only the held calls: a call still waiting for a person is declined by agent_confirmations::drop_session instead.
	template<class Match>
	void cancel_held(Match match)
	{
		const std::vector<held_call> dropped = take(match);
		for (const held_call& call : dropped) {
			if (jobs != nullptr) {
				jobs->cancel(call.job);
			}
		}
	}

	// Removed from `held` before any answer goes out, so an answer that comes back here finds a consistent list.
	template<class Match>
	std::vector<held_call> take(Match match)
	{
		std::vector<held_call> taken;
		for (auto it = held.begin(); it != held.end();) {
			if (match(*it)) {
				taken.push_back(std::move(*it));
				it = held.erase(it);
			} else {
				++it;
			}
		}
		return taken;
	}

	void tick()
	{
		const time_point at = now();
		for (held_call& call : held) {
			if (call.waiting && at - call.last_sent >= options.progress_interval) {
				send_waiting(call, at);
			}
		}
		std::vector<held_call> expired = take([at](const held_call& call) { return at >= call.deadline; });
		for (held_call& call : expired) {
			json::object answer = std::move(call.answer);
			answer["status"] = job_status_name(job_status::running);
			call.done(to_call_result(cmd::result::success(std::move(answer))));
		}
	}
};

edt::mcp_bridge_host::mcp_bridge_host(cmd::command_registry& commands, ds::task_system& tasks, agent_access& access,
	session_closed_hook on_session_closed, mcp_bridge_options options)
	: m_tasks(tasks)
	, m_state(std::make_shared<state>())
{
	ASSERT_MSG(tasks.on_main(), "mcp_bridge_host: create it on the main thread");
	m_state->commands = &commands;
	m_state->access = &access;
	m_state->on_session_closed = std::move(on_session_closed);
	if (options.jobs != nullptr && options.events != nullptr) {
		m_state->jobs = options.jobs;
		// The listeners live in the state and are disconnected by the destructor, so the raw pointer never dangles.
		state* self = m_state.get();
		m_state->progress_listener = options.events->job_progress.subscribe([self](std::uint64_t job, float fraction, const std::string& note) {
			self->on_progress(job, fraction, note);
		});
		m_state->finished_listener = options.events->job_finished.subscribe(
			[self](std::uint64_t job, job_status status, const std::string& error, const std::optional<json::value>& result) {
				self->on_finished(job, status, error, result);
			});
	}
	m_state->options = std::move(options);
}

edt::mcp_bridge_host::~mcp_bridge_host()
{
	m_state->commands = nullptr;
	m_state->access = nullptr;
	m_state->on_session_closed = nullptr;
	m_state->jobs = nullptr;
	m_state->progress_listener.disconnect();
	m_state->finished_listener.disconnect();
	// The answers are dropped unsent: the server that would carry them has stopped.
	m_state->held.clear();
	m_state->pending.clear();
}

void edt::mcp_bridge_host::tick()
{
	m_state->tick();
}

void edt::mcp_bridge_host::cancel_request(std::string session_id, json::value request_id)
{
	m_tasks.post_main([state = m_state, session = std::move(session_id), request_id = std::move(request_id)] {
		// The call hopped to the main thread before this did (one queue, in order), so it is pending or held by now,
		// or already answered (nothing to do).
		state->cancel([&](const auto& call) { return call.session == session && call.request_id == request_id; });
	});
}

void edt::mcp_bridge_host::stop_reporting()
{
	m_state->on_session_closed = nullptr;
}

void edt::mcp_bridge_host::list_tools(std::function<void(std::vector<mcp::tool_info>)> done)
{
	m_tasks.post_main([state = m_state, done = std::move(done)] {
		std::vector<mcp::tool_info> tools;
		if (state->commands != nullptr && state->access != nullptr) {
			for (const cmd::command_desc* command : state->commands->list()) {
				if (agent_may_use(*command, state->access->policy())) {
					tools.push_back(describe(*command, *state->commands));
				}
			}
		}
		done(std::move(tools));
	});
}

void edt::mcp_bridge_host::call_tool(mcp::request_context context, std::string name, json::value args, mcp::progress_sink progress,
	std::function<void(mcp::call_result)> done)
{
	m_tasks.post_main([state = m_state, session = std::move(context.session_id), request_id = std::move(context.request_id),
		wants_progress = context.wants_progress, name = std::move(name), args = std::move(args), progress = std::move(progress),
		done = std::move(done)]() mutable {
		if (state->commands == nullptr || state->access == nullptr) {
			done(error_result("the editor is shutting down"));
			return;
		}
		// The same check as the listing: a tool that is not listed does not exist for the agent.
		const cmd::command_desc* command = find_tool(*state->commands, state->access->policy(), name);
		if (command == nullptr) {
			done(error_result("unknown tool '" + name + "'"));
			return;
		}
		// Copied first: the command may unregister itself while it runs.
		const std::string id = command->id;
		json::value limited = std::move(args);
		if (is_capture(id)) {
			if (const std::optional<std::string> refusal = limit_capture_args(limited)) {
				done(error_result(*refusal));
				return;
			}
		}
		const bool starts_job = command->starts_job;
		cmd::call request;
		request.args = std::move(limited);
		request.from = cmd::origin::agent(session);
		const std::uint64_t key = state->begin_call(session, request_id);
		// `done` may be called later (a person is asked, a gesture is waited out). The closure holds the answer to the
		// client and a weak reference to the state, never the bridge.
		state->access->confirmations().invoke(id, std::move(request),
			[weak = std::weak_ptr<mcp_bridge_host::state>(state), key, session = std::move(session), request_id = std::move(request_id),
				wants_progress, starts_job, progress = std::move(progress), done = std::move(done)](cmd::result outcome) mutable {
				const std::shared_ptr<mcp_bridge_host::state> self = weak.lock();
				const std::uint64_t job = (outcome.ok && starts_job) ? job_of(outcome.value) : 0;
				if (self != nullptr && self->end_call(key)) {
					// Cancelled while it waited: no answer (`done` is dropped), and the job it started stops.
					if (job != 0 && self->jobs != nullptr) {
						self->jobs->cancel(job);
					}
					return;
				}
				if (wants_progress && job != 0 && self != nullptr && self->jobs != nullptr
					&& self->follow(job, outcome.value.as_object(), std::move(session), std::move(request_id), progress, done)) {
					return;
				}
				done(to_call_result(std::move(outcome)));
			});
	});
}

void edt::mcp_bridge_host::list_resources(std::function<void(std::vector<mcp::resource_info>)> done)
{
	m_tasks.post_main([state = m_state, done = std::move(done)] {
		std::vector<mcp::resource_info> resources;
		if (state->commands != nullptr && state->access != nullptr) {
			for (const resource_def& def : resource_defs) {
				const cmd::command_desc* command = state->commands->find(def.command);
				if (command == nullptr || !resource_readable(*command, state->access->policy())) {
					continue;
				}
				mcp::resource_info info;
				info.uri = std::string(def.uri);
				info.name = std::string(def.name);
				info.description = std::string(def.description);
				info.mime_type = std::string(def.mime_type);
				resources.push_back(std::move(info));
			}
		}
		done(std::move(resources));
	});
}

void edt::mcp_bridge_host::read_resource(mcp::request_context context, std::string uri, std::function<void(mcp::resource_result)> done)
{
	m_tasks.post_main([state = m_state, session = std::move(context.session_id), uri = std::move(uri), done = std::move(done)]() mutable {
		if (state->commands == nullptr || state->access == nullptr) {
			done(unavailable("the editor is shutting down"));
			return;
		}
		const resource_def* def = nullptr;
		for (const resource_def& candidate : resource_defs) {
			if (candidate.uri == uri) {
				def = &candidate;
			}
		}
		const cmd::command_desc* command = def != nullptr ? state->commands->find(def->command) : nullptr;
		if (command == nullptr || !resource_readable(*command, state->access->policy())) {
			done(unavailable("no such resource: " + uri));
			return;
		}
		if (unavailable_headless(*command, *state->commands)) {
			done(unavailable(std::string(cmd::codes::unavailable_headless) + ": " + uri + " needs a display; this editor runs headless"));
			return;
		}
		const std::string id = command->id;
		json::value resource_args = json::parse(def->args);
		if (is_capture(id)) {
			limit_capture_args(resource_args);
		}
		cmd::call request;
		request.args = std::move(resource_args);
		request.from = cmd::origin::agent(session);
		const std::string_view mime_type = def->mime_type;
		state->access->confirmations().invoke(id, std::move(request),
			[done = std::move(done), uri = std::move(uri), mime_type](cmd::result result) {
				if (!result.ok) {
					done(unavailable(result.code + ": " + result.error));
					return;
				}
				mcp::resource_content content;
				content.uri = uri;
				content.mime_type = std::string(mime_type);
				const json::value* png = result.value.is_object() ? result.value.as_object().if_contains("png_base64") : nullptr;
				if (png != nullptr && png->is_string()) {
					content.is_blob = true;
					content.blob = text_of(png->as_string());
				} else {
					content.text = json::serialize(result.value);
				}
				if (content.blob.size() + content.text.size() > agent_max_response_bytes) {
					done(unavailable(too_large_text(content.blob.size() + content.text.size())));
					return;
				}
				mcp::resource_result found;
				found.found = true;
				found.contents.push_back(std::move(content));
				done(std::move(found));
			});
	});
}

void edt::mcp_bridge_host::session_closed(std::string session_id)
{
	m_tasks.post_main([state = m_state, session = std::move(session_id)] {
		state->cancel_held([&session](const auto& call) { return call.session == session; });
		if (state->on_session_closed) {
			state->on_session_closed(session);
		}
	});
}

void edt::mcp_bridge_host::log(mcp::log_level level, std::string_view message)
{
	const std::string text(message);
	switch (level) {
	case mcp::log_level::debug: egLOG_DEBUG("mcp", "{}", text); break;
	case mcp::log_level::info: egLOG_INFO("mcp", "{}", text); break;
	case mcp::log_level::warn: egLOG_WARN("mcp", "{}", text); break;
	case mcp::log_level::error: egLOG_ERROR("mcp", "{}", text); break;
	}
}
