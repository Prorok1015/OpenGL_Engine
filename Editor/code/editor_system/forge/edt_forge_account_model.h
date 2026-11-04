#pragma once
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_registration.h>
#include <snk/platform/plt_secret.h>
#include <boost/json/value.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The Git Account window's logic (#458 PR-4), without ImGui: one row per code host the forge commands may talk to, the
// state of the token kept for it and the two things a person can do - set it and remove it. The token never comes out
// of here: there is no getter, a row says only "set (OS keychain)", "set (this session only)" or "not set", every message
// is built from the host and the secret store's own text (scrubbed of the token anyway), and the token enters through a
// secret_text or a buffer that is wiped before any job starts. The secret store's calls can block on an OS prompt, so each
// runs as a job on the io lane; the result comes back through editor_events::job_finished. Main thread only.
namespace edt
{
	enum class forge_token_state : std::uint8_t
	{
		// A job is looking.
		checking,
		not_set,
		set_keychain,
		set_session,
		// The OS store could not be reached (or a removal could not reach it: a saved copy may remain).
		unknown,
	};

	// "set (OS keychain)", "set (this session only)", "not set", ...
	std::string_view forge_token_state_text(forge_token_state state);

	// Overwrites the bytes in a way the compiler may not drop.
	void wipe_buffer(char* data, std::size_t size);

	class forge_account_model
	{
	public:
		// Longest token accepted, in bytes.
		inline static constexpr std::size_t k_max_token = 512;

		struct ports
		{
			// The store the forge service reads from (service forge_service::k_secret_service, account = host).
			std::shared_ptr<platform::secret_store> secrets;
			// Runs `work` on the io lane as a job; its result arrives through job_finished. nullopt: not started.
			std::function<std::optional<std::uint64_t>(std::string label, std::function<boost::json::value()> work)> start_job;
		};

		struct row
		{
			std::string host;
			forge_token_state state = forge_token_state::checking;
			bool busy = false;
		};

		// `hosts`: the web hosts of forge_options::allowed_hosts. Nothing else is accepted.
		forge_account_model(editor_events& events, std::vector<std::string> hosts, ports ports);

		// The panel's frame or its hidden frames; becoming visible reads the states once.
		void set_visible(bool visible);
		// Asks the store about every host (one job each).
		void refresh();

		const std::vector<row>& rows() const { return m_rows; }
		bool allows(std::string_view host) const;
		bool busy() const;
		// What the last save or removal came to; never holds a token. Cleared by clear_message().
		const std::string& message() const { return m_message; }
		bool message_is_error() const { return m_message_error; }
		void clear_message() { m_message.clear(); m_message_error = false; }

		// Stores the token for the host; false (and a message) when the host is not allowed, the token is not a token or
		// the host is busy. The token is dropped (wiped) in every case but the one where the job holds it.
		bool save(std::string_view host, platform::secret_text&& token);
		// The same from a NUL-terminated text buffer of `capacity` bytes (an ImGui field): the buffer is copied, then
		// wiped, before any job starts.
		bool save_from_buffer(std::string_view host, char* buffer, std::size_t capacity);
		// Forgets the token (the session copy and the OS store's); the message says when a saved copy may remain.
		bool remove(std::string_view host);

	private:
		enum class op : std::uint8_t { probe, save, remove };
		struct pending
		{
			op kind = op::probe;
			std::string host;
		};

		const row* find(std::string_view host) const;
		row* find(std::string_view host);
		bool start(op kind, const std::string& host, std::string label, std::function<boost::json::value()> work);
		void finished(std::uint64_t id, job_status status, const std::string& error, const std::optional<boost::json::value>& result);
		void say(std::string text, bool error);

		editor_events& m_events;
		ports m_ports;
		std::vector<row> m_rows;
		std::map<std::uint64_t, pending> m_jobs;
		std::string m_message;
		bool m_message_error = false;
		bool m_visible = false;
		bool m_read = false;
		connection m_finished;
	};

	// Action forge.account.open: opens the Git Account window for a person. Category not_for_agents and not a tool: an agent
	// never reaches the place where the token is set.
	[[nodiscard]] registration register_forge_account_action(action_registry& actions, std::function<void()> open);
}
