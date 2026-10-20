#pragma once
// A fake mcp::host and request helpers for the tests of the snk_mcp_server module. The module is tested without the
// engine: nothing here includes a snk/ header other than snk/mcp.
#include <snk/mcp/mcp_endpoint.h>
#include <snk/mcp/mcp_host.h>
#include <snk/mcp/mcp_http.h>

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp_test {

inline const char* const test_token = "0123456789abcdef0123456789abcdef";

inline boost::json::value json_of(std::string_view text)
{
	return boost::json::parse(text);
}

// Answers from lists the test fills in. Thread-safe: the socket tests call it from the server's I/O thread.
class fake_host : public mcp::host
{
public:
	struct call_record
	{
		std::string session_id;
		std::string name;
		boost::json::value args;
		mcp::progress_sink progress;
	};

	void list_tools(std::function<void(std::vector<mcp::tool_info>)> done) override
	{
		std::vector<mcp::tool_info> copy;
		{
			std::lock_guard lock(m_mutex);
			if (m_throw_on_list) {
				throw std::runtime_error("list_tools failed");
			}
			copy = m_tools;
		}
		done(std::move(copy));
	}

	void call_tool(mcp::request_context context, std::string name, boost::json::value args, mcp::progress_sink progress,
		std::function<void(mcp::call_result)> done) override
	{
		mcp::call_result answer;
		{
			std::lock_guard lock(m_mutex);
			m_calls.push_back({ context.session_id, name, args, progress });
			if (m_throw_on_call) {
				throw std::runtime_error("call_tool failed");
			}
			if (m_defer_calls) {
				m_deferred.push_back(std::move(done));
				m_deferred_cv.notify_all();
				return;
			}
			answer = m_next_result;
		}
		done(std::move(answer));
	}

	void list_resources(std::function<void(std::vector<mcp::resource_info>)> done) override
	{
		std::vector<mcp::resource_info> copy;
		{
			std::lock_guard lock(m_mutex);
			copy = m_resources;
		}
		done(std::move(copy));
	}

	void read_resource(mcp::request_context, std::string uri, std::function<void(mcp::resource_result)> done) override
	{
		mcp::resource_result answer;
		{
			std::lock_guard lock(m_mutex);
			const auto found = m_resource_results.find(uri);
			if (found != m_resource_results.end()) {
				answer = found->second;
			} else {
				answer.found = false;
				answer.error = "no such resource: " + uri;
			}
		}
		done(std::move(answer));
	}

	void log(mcp::log_level, std::string_view message) override
	{
		std::lock_guard lock(m_mutex);
		m_logs.emplace_back(message);
	}

	void set_tools(std::vector<mcp::tool_info> tools)
	{
		std::lock_guard lock(m_mutex);
		m_tools = std::move(tools);
	}
	void set_resources(std::vector<mcp::resource_info> resources)
	{
		std::lock_guard lock(m_mutex);
		m_resources = std::move(resources);
	}
	void set_resource_result(const std::string& uri, mcp::resource_result result)
	{
		std::lock_guard lock(m_mutex);
		m_resource_results[uri] = std::move(result);
	}
	void set_next_result(mcp::call_result result)
	{
		std::lock_guard lock(m_mutex);
		m_next_result = std::move(result);
	}
	void set_defer_calls(bool defer)
	{
		std::lock_guard lock(m_mutex);
		m_defer_calls = defer;
	}
	void set_throw_on_list(bool value)
	{
		std::lock_guard lock(m_mutex);
		m_throw_on_list = value;
	}
	void set_throw_on_call(bool value)
	{
		std::lock_guard lock(m_mutex);
		m_throw_on_call = value;
	}

	std::vector<call_record> calls() const
	{
		std::lock_guard lock(m_mutex);
		return m_calls;
	}
	std::vector<std::string> logs() const
	{
		std::lock_guard lock(m_mutex);
		return m_logs;
	}
	// Blocks until `count` calls wait for an answer (no polling: the host wakes it). False after 20 s.
	bool wait_for_deferred(std::size_t count)
	{
		std::unique_lock lock(m_mutex);
		return m_deferred_cv.wait_for(lock, std::chrono::seconds(20), [&] { return m_deferred.size() >= count; });
	}
	// Answers the oldest deferred call with the next result.
	bool answer_deferred(mcp::call_result result)
	{
		std::function<void(mcp::call_result)> done;
		{
			std::lock_guard lock(m_mutex);
			if (m_deferred.empty()) {
				return false;
			}
			done = std::move(m_deferred.front());
			m_deferred.erase(m_deferred.begin());
		}
		done(std::move(result));
		return true;
	}

private:
	mutable std::mutex m_mutex;
	std::condition_variable m_deferred_cv;
	std::vector<mcp::tool_info> m_tools;
	std::vector<mcp::resource_info> m_resources;
	std::map<std::string, mcp::resource_result> m_resource_results;
	std::vector<call_record> m_calls;
	std::vector<std::function<void(mcp::call_result)>> m_deferred;
	std::vector<std::string> m_logs;
	mcp::call_result m_next_result;
	bool m_defer_calls = false;
	bool m_throw_on_list = false;
	bool m_throw_on_call = false;
};

inline mcp::tool_info make_tool(std::string name, bool read_only = true)
{
	mcp::tool_info tool;
	tool.name = std::move(name);
	tool.description = "A test tool";
	tool.input_schema = json_of(R"({"type":"object","properties":{"n":{"type":"integer"}}})");
	tool.read_only = read_only;
	return tool;
}

}
