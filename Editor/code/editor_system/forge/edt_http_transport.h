#pragma once
#include "edt_forge_types.h"
#include <snk/platform/plt_secret.h>
#include <functional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace edt::forge
{
	struct http_request
	{
		// "GET", "POST", ...
		std::string method;
		// https only; the transport refuses anything else.
		std::string url;
		// Never the credentials: the token travels in bearer_token only.
		std::vector<std::pair<std::string, std::string>> headers;
		std::string body;
		// Sent as `Authorization: Bearer <token>`; null: none. Owned by the caller for the duration of the call.
		const platform::secret_text* bearer_token = nullptr;
	};

	struct http_response
	{
		// 0 when no answer came (see error_text).
		int status = 0;
		std::string body;
		// Not empty: the request did not complete (status and body are then meaningless); safe to show.
		std::string error_text;
		// What error_text means; only read when it is not empty.
		forge_error_code error_code = forge_error_code::network;
	};

	// Blocks; call it from a worker thread. A stop request ends the request.
	using http_transport = std::function<http_response(const http_request&, std::stop_token)>;
}
