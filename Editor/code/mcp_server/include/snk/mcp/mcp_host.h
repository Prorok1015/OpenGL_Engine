#pragma once
#include <boost/json/value.hpp>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp
{
	enum class log_level { debug, info, warn, error };

	struct tool_info
	{
		std::string name;
		std::string description;
		// JSON Schema of the arguments. Null: {"type":"object"}.
		boost::json::value input_schema;
		// JSON Schema of the structured result. Null: not declared.
		boost::json::value output_schema;
		// MCP tool annotations (hints for the client, not guarantees).
		bool read_only = false;
		bool destructive = false;
		bool idempotent = false;
		bool open_world = false;
		// Listed, but every call answers `unavailable_headless` (it needs a display the editor does not have). Not a standard
		// annotation: tools/list says it in the description and in `_meta` ("snk/unavailableHeadless").
		bool unavailable_headless = false;
	};

	// One block of a tool result.
	struct content_block
	{
		enum class kind { text, image };

		kind type = kind::text;
		std::string text;      // text
		std::string data;      // image: base64
		std::string mime_type; // image: "image/png"

		static content_block make_text(std::string text)
		{
			content_block block;
			block.text = std::move(text);
			return block;
		}
		static content_block make_image(std::string base64, std::string mime_type)
		{
			content_block block;
			block.type = kind::image;
			block.data = std::move(base64);
			block.mime_type = std::move(mime_type);
			return block;
		}
	};

	struct call_result
	{
		bool is_error = false;
		// An object, or null. Sent as structuredContent; when `content` is empty it is also sent as text.
		boost::json::value structured;
		std::vector<content_block> content;
	};

	struct resource_info
	{
		std::string uri;
		std::string name;
		std::string description;
		std::string mime_type;
	};

	struct resource_content
	{
		std::string uri;
		std::string mime_type;
		std::string text; // text resource
		std::string blob; // binary resource, base64; used when is_blob
		bool is_blob = false;
	};

	struct resource_result
	{
		bool found = false;
		std::string error; // why it was not found or could not be read
		std::vector<resource_content> contents;
	};

	// Who is asking: the MCP session (Mcp-Session-Id), so the host can attribute the call to an agent session.
	struct request_context
	{
		std::string session_id;
		// The JSON-RPC id of the request; notifications/cancelled names it (host::cancel_request).
		boost::json::value request_id;
		// The client sent a progressToken and the transport can deliver notifications: `progress` reaches it.
		bool wants_progress = false;
	};

	// Reports the progress of a running call. Callable from any thread; ignored when the client sent no progressToken
	// (request_context::wants_progress tells). `total` is 0 when unknown.
	using progress_sink = std::function<void(double progress, double total, std::string_view message)>;

	// What the server asks of the application. The module calls it from its own I/O thread; it never touches the
	// application's state itself. Every method answers through `done`, exactly once, from any thread (also inline),
	// so a host that must hop to another thread does not block the I/O thread. A host that throws is answered
	// with an internal error.
	class host
	{
	public:
		virtual ~host() = default;

		virtual void list_tools(std::function<void(std::vector<tool_info>)> done) = 0;
		// An unknown tool is answered with is_error = true.
		virtual void call_tool(request_context context, std::string name, boost::json::value args, progress_sink progress,
			std::function<void(call_result)> done) = 0;
		virtual void list_resources(std::function<void(std::vector<resource_info>)> done) = 0;
		virtual void read_resource(request_context context, std::string uri, std::function<void(resource_result)> done) = 0;
		// The module does not know the application's logger. Callable from any thread.
		virtual void log(log_level level, std::string_view message) = 0;
		// The session is gone (DELETE, pushed out by a newer one, or the server stopped). Called from the I/O thread,
		// outside the endpoint's lock; state kept per session can be dropped.
		virtual void session_closed(std::string session_id) { (void)session_id; }
		// notifications/cancelled: the client no longer wants the answer to request `request_id` of the session. A host
		// that still works on it may stop and must not answer it; one that already answered ignores this (the spec
		// allows both orders). Called from the I/O thread.
		virtual void cancel_request(std::string session_id, boost::json::value request_id)
		{
			(void)session_id;
			(void)request_id;
		}
	};
}
