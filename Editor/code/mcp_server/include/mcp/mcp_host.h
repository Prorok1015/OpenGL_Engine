#pragma once
#include <boost/json/value.hpp>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The boundary between the MCP server module and the program that embeds it. The module knows nothing about the
// engine: the editor implements `host` (edt::mcp_bridge_host) and hands it to `server`.
namespace mcp
{
	enum class level { debug, info, warn, error };

	struct tool_info
	{
		// What the client calls: ASCII letters, numbers, '_' and '-' only.
		std::string name;
		std::string description;
		// JSON Schema of the arguments (an object schema). Null: any object.
		boost::json::value input_schema;
		// JSON Schema of structured results. Null: not published.
		boost::json::value output_schema;
		// MCP annotations: readOnlyHint / destructiveHint.
		bool read_only = false;
		bool destructive = false;
	};

	// One piece of a tool result.
	struct content
	{
		enum class kind { text, image };

		kind type = kind::text;
		std::string text;       // kind::text
		std::string data;       // kind::image: base64
		std::string mime_type;  // kind::image: "image/png"

		static content make_text(std::string text)
		{
			content out;
			out.text = std::move(text);
			return out;
		}

		static content make_image(std::string base64, std::string mime_type)
		{
			content out;
			out.type = kind::image;
			out.data = std::move(base64);
			out.mime_type = std::move(mime_type);
			return out;
		}
	};

	struct call_result
	{
		bool is_error = false;
		// An object, or null. Published as structuredContent (not on errors).
		boost::json::value structured;
		// Empty and `structured` set: the server adds the serialized `structured` as one text item.
		std::vector<content> items;
	};

	// Progress of a running call: how far it is, out of `total` when known.
	using progress_sink = std::function<void(double progress, std::optional<double> total, std::string message)>;

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
		// One of them: `text`, or `blob` (base64) when `is_blob`.
		std::string text;
		std::string blob;
		bool is_blob = false;
	};

	struct resource_result
	{
		bool found = false;
		std::vector<resource_content> contents;
		// found == false: why, for the client.
		std::string error;
	};

	// Implemented by the embedding program. The server calls it from its own io thread, so every member must be
	// callable from any thread and must not block: `call_tool` and `read_resource` start the work and return, the
	// answer goes to `done`. `done` runs exactly once, on any thread, and may even run before the call returns.
	class host
	{
	public:
		virtual ~host() = default;

		virtual std::vector<tool_info> list_tools() = 0;
		virtual void call_tool(std::string name, boost::json::value args, progress_sink progress, std::function<void(call_result)> done) = 0;
		virtual std::vector<resource_info> list_resources() = 0;
		virtual void read_resource(std::string uri, std::function<void(resource_result)> done) = 0;
		// The module has no logger of its own.
		virtual void log(level severity, std::string_view message) = 0;
	};
}
