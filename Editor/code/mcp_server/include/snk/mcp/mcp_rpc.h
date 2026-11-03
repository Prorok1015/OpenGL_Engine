#pragma once
#include <snk/mcp/mcp_host.h>

#include <boost/json/object.hpp>
#include <boost/json/value.hpp>

#include <functional>
#include <string>
#include <string_view>

namespace mcp
{
	struct server_info
	{
		std::string name = "snk-editor";
		std::string version = "0";
		// Version of the command set (serverInfo.snk_api); empty: not reported.
		std::string snk_api;
		// Sent to the client as `instructions`; empty: not sent.
		std::string instructions;
	};

	// JSON-RPC and MCP error codes.
	namespace rpc_codes
	{
		inline constexpr int parse_error = -32700;
		inline constexpr int invalid_request = -32600;
		inline constexpr int method_not_found = -32601;
		inline constexpr int invalid_params = -32602;
		inline constexpr int internal_error = -32603;
		inline constexpr int resource_not_found = -32002;
	}

	inline constexpr std::string_view latest_protocol_version = "2025-06-18";
	bool is_supported_protocol(std::string_view version);

	boost::json::value rpc_result(const boost::json::value& id, boost::json::value result);
	boost::json::value rpc_error(const boost::json::value& id, int code, std::string_view text);
	// params null: no "params" member.
	boost::json::value rpc_notification(std::string_view method, boost::json::value params);

	// A new session id: 32 random hex digits.
	std::string new_session_id();

	// One JSON-RPC message sorted out before any method runs, the same way for every transport.
	struct rpc_message
	{
		enum class kind { request, notification, response, invalid };

		kind type = kind::invalid;
		std::string method;
		// A request's id; null otherwise.
		boost::json::value id;
		boost::json::object params;
		// invalid: why.
		std::string error;
		// A request or notification whose params is not an object.
		bool bad_params = false;
	};
	rpc_message classify_rpc(const boost::json::object& message);

	// The session a request runs in, as its transport sees it.
	struct rpc_session
	{
		std::string id;
		// Sends a notification to the client of this session (progress). Null: the client cannot get one now, progress
		// is dropped. Called from any thread, also after the request was answered.
		std::function<void(const boost::json::value&)> notify;
		// Adds (true) or removes (false) a subscription. Returns false when it is refused (too many). Called from any
		// thread, possibly after the transport moved on. Null: nothing is kept.
		std::function<bool(const std::string& uri, bool subscribe)> subscribe;
	};

	// The MCP methods without a transport: the Streamable HTTP endpoint and the stdio transport both hand their
	// parsed messages here. It keeps no session state of its own; the transport owns the sessions.
	class rpc_dispatcher
	{
	public:
		using reply_sink = std::function<void(boost::json::value)>;

		rpc_dispatcher(host& application, server_info info);

		// The answer to `initialize`. `accepted` is true when it succeeded and the transport should open a session.
		boost::json::value initialize(const boost::json::value& id, const boost::json::object& params, bool& accepted) const;

		// A request of an open session, other than initialize: ping, tools/*, resources/*; anything else is
		// method_not_found. `reply` is called exactly once, possibly later and from another thread (when the host answers
		// later). A host that throws inline throws out of here; the transport answers with an internal error.
		void dispatch(const std::string& method, const boost::json::value& id, const boost::json::object& params,
			const rpc_session& session, const reply_sink& reply) const;

		// A notification of an open session (no answer): notifications/cancelled goes to host::cancel_request, the rest
		// is ignored.
		void notification(const std::string& method, const boost::json::object& params, const rpc_session& session) const;

	private:
		host& m_host;
		server_info m_info;
	};
}
