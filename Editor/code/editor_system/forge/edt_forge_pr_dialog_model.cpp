#include "edt_forge_pr_dialog_model.h"
#include "edt_forge_service.h"
#include "edt_forge_types.h"
#include <snk/common/logger/engine_log.h>
#include <boost/json/object.hpp>
#include <utility>

namespace {
	namespace json = boost::json;

	constexpr std::size_t k_text_cap = 300;
	constexpr std::size_t k_url_cap = 400;

	std::string string_of(const json::object& object, std::string_view key)
	{
		const json::value* value = object.if_contains(key);
		return value && value->is_string() ? std::string(value->as_string()) : std::string{};
	}

	std::string hint_of(std::string_view code)
	{
		if (code == "no_token") return "No token is stored for this code host. Set one in Git Account.";
		if (code == "unauthorized") return "The code host did not accept the token. Set a new one in Git Account.";
		if (code == "forbidden") return "The token is not allowed to create pull requests in this repository.";
		if (code == "validation") return "The code host refused the request: is the head branch pushed, and is there no pull request for it yet?";
		if (code == "rate_limited") return "The code host is limiting requests. Try again later.";
		if (code == "network" || code == "unavailable") return "Check the network connection and that curl is installed.";
		if (code == "no_local_branch") return "The head branch does not exist in this repository.";
		if (code == "not_a_repository" || code == "vcs_unavailable") return "Open a project that is a git repository with a remote on a supported code host.";
		return {};
	}
}

edt::forge_pr_dialog_model::forge_pr_dialog_model(editor_events& events, ports ports)
	: m_events(events), m_ports(std::move(ports))
{
	m_finished = m_events.job_finished.subscribe(
		[this](std::uint64_t id, job_status status, const std::string& error, const std::optional<json::value>& result) {
			finished(id, status, error, result);
		});
}

void edt::forge_pr_dialog_model::reset(std::string_view head)
{
	if (m_phase == phase::running) return;
	m_phase = phase::editing;
	m_base = std::string(k_default_base);
	m_head = std::string(head);
	m_title.clear();
	m_body.clear();
	m_draft = true;
	m_number.reset();
	m_url.clear();
	clear_error();
}

void edt::forge_pr_dialog_model::clear_error()
{
	m_error_code.clear();
	m_error_text.clear();
	m_error_hint.clear();
}

std::optional<std::string> edt::forge_pr_dialog_model::input_error() const
{
	forge_service::create_request request;
	request.base = m_base;
	request.head = m_head;
	request.title = m_title;
	request.body = m_body;
	return forge_service::check_create_request(request);
}

void edt::forge_pr_dialog_model::fail(std::string code, std::string_view text)
{
	m_error_hint = hint_of(code);
	m_error_code = std::move(code);
	m_error_text = forge::sanitize_text(text, k_text_cap);
	egLOG_WARN("forge", "pull request: {}: {}", m_error_code, m_error_text);
}

void edt::forge_pr_dialog_model::create()
{
	if (m_phase != phase::editing) return;
	clear_error();
	if (const auto error = input_error()) {
		fail("invalid_args", *error);
		return;
	}
	if (!m_ports.invoke) {
		fail("unavailable", "pull requests are not available in this editor");
		return;
	}
	const cmd::result started = m_ports.invoke("forge.create_pr", json::object{
		{ "base", json::string(m_base) }, { "head", json::string(m_head) }, { "title", json::string(m_title) },
		{ "body", json::string(m_body) }, { "draft", m_draft } });
	if (!started.ok) {
		fail(started.code.empty() ? "failed" : started.code, started.error);
		return;
	}
	const json::value* job = started.value.is_object() ? started.value.as_object().if_contains("job") : nullptr;
	if (!job || !job->is_number()) {
		fail("failed", "no job was started");
		return;
	}
	m_job = job->to_number<std::uint64_t>();
	m_phase = phase::running;
}

void edt::forge_pr_dialog_model::cancel()
{
	if (m_job && m_ports.cancel) m_ports.cancel(*m_job);
}

void edt::forge_pr_dialog_model::finished(std::uint64_t id, job_status status, const std::string& error,
	const std::optional<json::value>& result)
{
	if (!m_job || *m_job != id) return;
	m_job.reset();
	m_phase = phase::editing;
	if (status == job_status::cancelled) return;
	if (status == job_status::failed) {
		fail("failed", error);
		return;
	}
	if (!result || !result->is_object()) {
		fail("invalid_response", "the job gave no answer");
		return;
	}
	const json::object& value = result->as_object();
	if (const json::value* failure = value.if_contains("error"); failure && failure->is_object()) {
		const std::string code = string_of(failure->as_object(), "code");
		const std::string message = string_of(failure->as_object(), "message");
		fail(code.empty() ? "failed" : forge::sanitize_text(code, 40), message);
		return;
	}
	const json::value* number = value.if_contains("number");
	if (!number || !number->is_number()) {
		fail("invalid_response", "the answer has no pull request number");
		return;
	}
	m_number = number->to_number<std::int64_t>();
	std::string url = forge::sanitize_text(string_of(value, "url"), k_url_cap);
	m_url = url.starts_with("https://") ? std::move(url) : std::string{};
	m_phase = phase::created;
}
