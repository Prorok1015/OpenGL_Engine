#pragma once
#include "edt_forge_service.h"
#include <snk/command/cmd_types.h>
#include <snk/editor/edt_editor_events.h>
#include <boost/json/value.hpp>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

// The Git panel's "Create PR" dialog (#458 PR-4), without ImGui: the form, its checks (the forge service's own), the
// forge.create_pr call as a person's call (origin ui, the same path the agents take) and the answer of its job
// (editor_events::job_finished). It has no way to open a link: the address of the new pull request is plain text for the
// person to read or copy. Create stays off until the repository the request goes to (host/owner/repo and remote) has been
// read and shown (forge_service::describe_target): the call is held to it by the service, and an address that insteadOf or a
// pushurl rewrites is sent only after the person ticks that they understand it. It knows no token; "no token stored" is an error code it points to the Git Account window for.
// Main thread only.
namespace edt
{
	class forge_pr_dialog_model
	{
	public:
		struct ports
		{
			// A command of the registry, as a person's call (origin ui).
			std::function<cmd::result(std::string_view id, boost::json::value args)> invoke;
			// job_service::cancel.
			std::function<bool(std::uint64_t job)> cancel;
			// forge_service::describe_target for the default remote (an empty name). Without it nothing can be created.
			std::function<forge_service::target_description(const std::string& remote)> describe_target;
		};

		enum class phase : std::uint8_t { editing, running, created };

		inline static constexpr std::string_view k_default_base = "main";

		forge_pr_dialog_model(editor_events& events, ports ports);

		void set_describe_target(std::function<forge_service::target_description(const std::string&)> describe)
		{
			m_ports.describe_target = std::move(describe);
		}

		// A fresh form for a pull request from `head`: base main, empty title and body, draft on, and the repository read
		// again. Ignored while a request runs.
		void reset(std::string_view head);

		std::string& base() { return m_base; }
		std::string& head() { return m_head; }
		std::string& title() { return m_title; }
		std::string& body() { return m_body; }
		bool& draft() { return m_draft; }
		const std::string& base() const { return m_base; }
		const std::string& head() const { return m_head; }
		const std::string& title() const { return m_title; }
		const std::string& body() const { return m_body; }
		bool draft() const { return m_draft; }

		// Why the form cannot be sent now (forge_service::check_create_request), else nullopt.
		std::optional<std::string> input_error() const;
		bool can_create() const { return m_phase == phase::editing && !input_error() && target_accepted(); }
		// The repository this request goes to, as read when the dialog opened (or by refresh_target); nullopt: not read yet,
		// or forgotten after a `denied`.
		const std::optional<forge_service::target_description>& target() const { return m_target; }
		// Reads it again (the person's "check again"); clears the tick for a rewritten address. Ignored while a request runs.
		void refresh_target();
		// The tick "I understand the address is rewritten"; it counts only when the shown target is rewritten.
		bool& rewrite_accepted() { return m_rewrite_accepted; }
		bool rewrite_accepted() const { return m_rewrite_accepted; }
		// The target is read, may be requested and, when rewritten, accepted.
		bool target_accepted() const { return m_target && m_target->ok && (!m_target->rewritten || m_rewrite_accepted); }
		// forge.create_pr {base, head, title, body, draft}; the job's answer comes through job_finished.
		void create();
		// Stops the running request (job.cancel).
		void cancel();

		phase state() const { return m_phase; }
		bool running() const { return m_phase == phase::running; }
		// After a creation: the number and the address of the pull request. The address is shown only when it is an
		// https link, sanitized; empty otherwise.
		std::optional<std::int64_t> number() const { return m_number; }
		const std::string& url() const { return m_url; }
		// The last failure, while editing: a forge_error_code name, no_token, denied, invalid_args, ..., and its plain text.
		const std::string& error_code() const { return m_error_code; }
		const std::string& error_text() const { return m_error_text; }
		// What a person can do about the failure, else empty.
		const std::string& error_hint() const { return m_error_hint; }
		// The token is missing: the dialog offers the Git Account window.
		bool needs_token() const { return m_error_code == "no_token"; }
		void clear_error();

	private:
		void finished(std::uint64_t id, job_status status, const std::string& error, const std::optional<boost::json::value>& result);
		void fail(std::string code, std::string_view text);

		editor_events& m_events;
		ports m_ports;
		std::string m_base{ k_default_base };
		std::string m_head;
		std::string m_title;
		std::string m_body;
		bool m_draft = true;

		phase m_phase = phase::editing;
		std::optional<std::uint64_t> m_job;
		std::optional<forge_service::target_description> m_target;
		bool m_rewrite_accepted = false;
		std::optional<std::int64_t> m_number;
		std::string m_url;
		std::string m_error_code;
		std::string m_error_text;
		std::string m_error_hint;
		// Last: gone before the state its slot touches.
		connection m_finished;
	};
}
