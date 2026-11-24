#pragma once
#include "edt_autosave_settings.h"
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/editor/edt_document.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/platform/plt_file_lock.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace res { class resource_system; }

namespace edt
{
	class document_manager;
	class job_service;
	class problem_list;

	// The project's cache folder autosave writes below (the top root of the cache:// mount); empty without one.
	std::filesystem::path autosave_cache_root(const res::resource_system& resources);

	// Autosave of the dirty documents into cache://autosave/<session>/ (#1186, #1187, EPIC-60): a document's countdown starts at its first
	// unsaved edit; `interval` later its bytes (what Save would write, autosave_source) and a meta file are written beside
	// the project, never over the source, and the document stays as it is (dirty, same history). A write due while a person's
	// gesture is in progress waits for its end. After a write the next one is `interval` after the next edit. The files go
	// when the document is saved or its changes are dropped (dirty -> clean), when it is closed, when its source changes (another
	// level), when the project's cache folder changes and at a clean exit (shutdown). Without a cache:// mount it is off.
	// Before its first record it starts its session: a folder of its own, session.lock held while it runs (another editor
	// tells a crashed run by a lock it can take) and session.json; a clean exit removes the whole folder.
	class autosave_component
	{
	public:
		using clock = std::chrono::steady_clock;

		struct ports
		{
			document_manager& documents;
			editor_events& events;
			// The base resource system: the cache:// folder the files go into and the sources' files on disk.
			res::resource_system& resources;
			// The bytes are printed and written on its io lane. Null: at once, on the calling thread (tests).
			job_service* jobs = nullptr;
			// A person's gesture is in progress (editor_system::person_is_editing). Null: never.
			std::function<bool()> gesture_active;
			// A document that is not autosaved (nothing to capture, a write that failed) is a warning of the source `autosave`
			// until it is written, clean or closed. Null: only logged.
			problem_list* problems = nullptr;
		};

		static constexpr std::string_view k_problem_source = "autosave";

		explicit autosave_component(ports from);
		~autosave_component();
		autosave_component(const autosave_component&) = delete;
		autosave_component& operator=(const autosave_component&) = delete;

		// Once a frame (the `editor_autosave` frame stage).
		void tick();
		// Removes the files it wrote and stops: after a clean exit there is nothing to recover. Waits for a write that is
		// running and drops the ones not started; no job callback reaches the component afterwards.
		void shutdown();

		const autosave_settings& settings() const { return m_settings; }
		void set_settings(const autosave_settings& settings) { m_settings = settings; }
		// cfg `editor/autosave`: false turns autosave off whatever the settings say.
		void set_allowed(bool allowed) { m_allowed = allowed; }
		bool allowed() const { return m_allowed; }
		// The cache folder the files go into; empty without a project (autosave is off).
		const std::filesystem::path& cache_root() const { return m_root; }
		// The id of this run's session folder ("autosave/<id>").
		const std::string& session() const { return m_session; }
		// Starts the session in the current cache folder when it is not started yet; false when there is no cache folder or
		// its folder could not be made.
		bool ensure_session();
		// Whether the session holds its session.lock.
		bool session_locked() const { return m_lock.has_value(); }
		// The autosave of document `id` lies in this session's folder already (recovery wrote it there, of the document at
		// `generation`): it is this component's to remove, as if it had written it.
		void adopt(document_id id, std::uint64_t generation);

		void set_clock_for_tests(std::function<clock::time_point()> clock) { m_clock = std::move(clock); }
		using lock_port = std::function<platform::lock_attempt(const std::filesystem::path& root, const std::filesystem::path& relative,
			platform::lock_open open)>;
		// Takes the session's lock instead of platform::file_lock::acquire.
		void set_lock_for_tests(lock_port lock) { m_lock_port = std::move(lock); }
		// The file the last autosave of document `id` went to, relative to the cache folder; empty while none is there.
		std::string written_file(document_id id) const;
		// How many autosaves were written so far.
		std::size_t writes() const { return m_writes; }

	private:
		// Shared by a write's job and the component: shutdown() takes the lock (waiting out a write that runs) and cancels.
		struct write_gate
		{
			std::mutex mutex;
			bool cancelled = false;
		};

		struct tracked
		{
			// Relative to the cache folder (autosave_file_of); follows the document's kind and source.
			std::string file;
			// Where the last write went (cache folder and file): a project switch or a new source does not move those files.
			std::filesystem::path root;
			std::string disk_file;
			std::shared_ptr<write_gate> gate;
			std::optional<clock::time_point> due;
			// The generation the last write (or the last capture that could not be written) was of.
			std::optional<std::uint64_t> written_generation;
			// A write was started: the files may be there.
			bool on_disk = false;
			bool in_flight = false;
			// Remove the files once the write in flight ended (the document went clean or closed meanwhile).
			bool remove_after = false;
			bool closed = false;
		};

		void refresh_root();
		bool start_session();
		// Removes this run's session folder from `root` and lets go of the lock.
		void end_session() noexcept;
		void follow(document& doc, clock::time_point now, bool can_write, std::optional<bool>& gesture);
		void write(document& doc, tracked& record, document_id id);
		void finish_write(document_id id, const std::string& error);
		void discard(tracked& record);
		void forget(document_id id);
		void remove_files(const std::filesystem::path& root, const std::string& file) noexcept;
		// Empty `message` clears the document's row.
		void report(document_id id, std::string file, std::string message);

		ports m_ports;
		autosave_settings m_settings;
		bool m_allowed = true;
		bool m_shut = false;
		bool m_reported_no_project = false;
		std::filesystem::path m_root;
		std::string m_session;
		std::string m_host;
		// Where the session was started; empty while none is.
		std::filesystem::path m_session_root;
		bool m_session_failed = false;
		std::optional<platform::file_lock> m_lock;
		lock_port m_lock_port;
		std::function<clock::time_point()> m_clock;
		std::unordered_map<document_id, tracked> m_tracked;
		// document -> {source, message} of its Problems row.
		std::unordered_map<document_id, std::pair<std::string, std::string>> m_reported;
		std::size_t m_writes = 0;
		connection m_closed;
		// Declared last: destroyed (and its jobs' results dropped) first.
		ds::lifetime m_lifetime;
	};
}
