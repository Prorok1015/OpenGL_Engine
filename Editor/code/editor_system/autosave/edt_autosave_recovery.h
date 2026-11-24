#pragma once
#include "edt_autosave_files.h"
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/editor/edt_document.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/model_codec/res_tag.h>
#include <snk/platform/plt_file_lock.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace edt
{
	class job_service;

	// Limits of the scan of crash recovery (#1187, EPIC-60).
	inline constexpr std::size_t k_recovery_max_sessions = 64;
	inline constexpr std::size_t k_recovery_max_records = 256;
	inline constexpr std::uint64_t k_recovery_max_meta = std::uint64_t{ 64 } << 10;
	inline constexpr std::uint64_t k_recovery_max_payload = std::uint64_t{ 256 } << 20;
	inline constexpr std::uint64_t k_recovery_scan_budget = std::uint64_t{ 512 } << 20;
	// A session folder without a lock file younger than this is an editor still starting.
	inline constexpr std::chrono::minutes k_recovery_starting_age{ 5 };
	inline constexpr std::size_t k_recovery_max_text = 200;
	inline constexpr std::size_t k_recovery_max_lines = 50;

	// What a session folder of another editor run was found to be.
	enum class recovery_state : std::uint8_t {
		// Its lock is held: that editor runs. Nothing of it is touched.
		live,
		// No lock file yet and the folder is young: that editor is starting. Nothing of it is touched.
		starting,
		// Its lock was taken by this scan: that editor is gone. The lock is held until its question is answered.
		dead,
		// Nothing tells whether that editor runs (another machine, a file system without locks): asked with Later first.
		unverifiable
	};

	// One autosave of a session that passed every check: its bytes are the ones a restore uses.
	struct recovery_record
	{
		// The payload, relative to the cache folder ("autosave/<session>/<kind>/<name>").
		std::string file;
		autosave_meta meta;
		std::shared_ptr<const std::vector<std::byte>> bytes;
		bool source_exists = false;
		// The source file's size or SHA-256 now differs from what the meta recorded.
		bool source_changed = false;
		// Whether the source changed was not checked (the scan's byte budget was used up); it can still be restored.
		bool source_not_checked = false;
		// Why it cannot be restored (it can be discarded); empty: it can.
		std::string not_restorable;
		// Set by a restore: its files left this session.
		bool restored = false;
	};

	// An autosave that failed a check: never restored, removed only by Discard all.
	struct recovery_rejected
	{
		// Relative to the cache folder: the meta (or the entry that is no meta).
		std::string file;
		std::string reason;
	};

	struct recovery_session
	{
		std::string id;
		recovery_state state = recovery_state::unverifiable;
		// From its session.json; empty when it has none.
		std::string host;
		bool other_host = false;
		std::optional<platform::file_lock> lock;
		std::vector<recovery_record> records;
		std::vector<recovery_rejected> rejected;
		// Records past k_recovery_max_records, not looked at.
		std::size_t over_limit = 0;
		// Records not read because the scan's byte budget was used up.
		std::size_t not_checked = 0;
		// Files in its kind folders that are no meta nor the payload of one (a payload left alone, a temp file).
		std::size_t unread = 0;
		// Why its folder was not listed whole (an error, more entries than are read); empty when it was. Such a folder is
		// never removed: it may hold what the scan never saw.
		std::string incomplete;

		// Nothing in it to restore or to report.
		bool empty() const { return records.empty() && rejected.empty() && fully_read(); }
		// Everything in its folder was read: only then may the folder itself go.
		bool fully_read() const { return incomplete.empty() && over_limit == 0 && not_checked == 0 && unread == 0; }
	};

	struct recovery_scan_result
	{
		// The dead and the unverifiable sessions; live and starting ones are counted only.
		std::vector<recovery_session> sessions;
		std::size_t live = 0;
		std::size_t starting = 0;
		// Dead or unverifiable sessions past k_recovery_max_sessions, not read; `unchecked_more`: the listing itself was
		// cut, there may be more.
		std::size_t unchecked_sessions = 0;
		bool unchecked_more = false;
		std::string error;
	};

	using recovery_lock_port = std::function<platform::lock_attempt(const std::filesystem::path& root,
		const std::filesystem::path& relative, platform::lock_open open)>;

	struct recovery_scan_options
	{
		std::filesystem::path cache_root;
		std::string own_session;
		std::string host;
		// The res:// layers, the lowest first: a record's source is looked for from the top one down.
		std::vector<std::filesystem::path> res_roots;
		recovery_lock_port lock;
		std::function<std::filesystem::file_time_type()> now;
	};

	// Lists autosave/ below the cache folder, judges every session folder of another run and reads and checks the records
	// of the dead and unverifiable ones (EPIC-60). Writes nothing; takes the locks of the dead sessions. Any thread.
	recovery_scan_result scan_autosave_sessions(const recovery_scan_options& options);

	// A text from a file shown as data: control characters, invalid UTF-8 and other non-printables as \xNN, at most
	// k_recovery_max_text characters, then "...".
	std::string recovery_shown_text(std::string_view text);
	// "Level", "Prefab", "Data", "Mesh".
	std::string_view recovery_kind_label(std::string_view kind);
	// The question about the sessions, one line per record (at most k_recovery_max_lines).
	std::string recovery_question_text(const std::vector<const recovery_session*>& sessions, bool unverifiable,
		std::size_t unchecked_sessions, bool unchecked_more);

	// Crash recovery (#1187, EPIC-60): once the start level settled, a job scans the autosaves other editor runs left
	// behind; the dead sessions are one question (Restore all / Discard all / Later), the ones nothing can judge another
	// (Later first). Restore opens each document through its kind's own open path with the verified bytes, unsaved; the
	// source file is never written. The restored autosave moves into this run's session folder. No ImGui.
	class autosave_recovery
	{
	public:
		struct restore_request
		{
			std::string kind;
			// Invalid for an untitled level.
			res::tag source;
			std::shared_ptr<const std::vector<std::byte>> bytes;
			// The payload's file name (the encoded source): the name of a memory:// working copy.
			std::string leaf;
			std::uint64_t serial = 0;
		};
		struct restore_outcome
		{
			// Empty: the document is open with the autosaved content, unsaved.
			std::string error;
			document_id document{};
			std::uint64_t generation = 0;
		};
		using restore_port = std::function<void(const restore_request&, std::function<void(restore_outcome)> done)>;
		using choose_port = std::function<void(std::string title, std::string text, std::vector<std::string> options,
			std::function<void(std::optional<std::size_t>)> answer)>;

		struct ports
		{
			// The cache folder (autosave_cache_root); empty: no project, nothing is scanned.
			std::function<std::filesystem::path()> cache_root;
			std::function<std::vector<std::filesystem::path>()> res_roots;
			// This run's session (autosave_component::session / ensure_session).
			std::function<std::string()> own_session;
			std::function<bool()> ensure_own_session;
			// The restored document's autosave now lies in this run's session folder (autosave_component::adopt).
			std::function<void(document_id, std::uint64_t generation)> adopt;
			// The start level settled: the scan starts. Null: at once.
			std::function<bool()> settled;
			// Runs `poll` every UI frame under `id` until it returns false. Null: install() scans at once.
			std::function<void(std::string_view id, std::function<bool()> poll)> register_frame;
			// Nobody answers: WARN, ask nothing, delete nothing.
			bool headless = false;
			// The scan and the moves of restored autosaves run on its io lane. Null: on the calling thread.
			job_service* jobs = nullptr;
			// Its job_finished tells a job of this component cancelled (job_service then calls no on_done). Null: none.
			editor_events* events = nullptr;
			choose_port choose;
			std::function<void(std::string text)> message;
			// A desc type marked as a document (desc_system::document_types).
			std::function<bool(std::string_view type)> is_document_type;
			restore_port restore_scene;
			restore_port restore_prefab;
			restore_port restore_mesh;
			restore_port restore_data;
			std::function<std::string()> host_name;
			// platform::file_lock::acquire when null.
			recovery_lock_port lock;
			// std::filesystem::file_time_type::clock::now when null.
			std::function<std::filesystem::file_time_type()> now;
		};

		static constexpr std::string_view k_title = "Recover unsaved changes";
		static constexpr std::string_view k_title_unverifiable = "Recover unsaved changes of another editor";
		static constexpr std::string_view k_restore = "Restore all";
		static constexpr std::string_view k_discard = "Discard all";
		static constexpr std::string_view k_later = "Later";

		explicit autosave_recovery(ports from);
		~autosave_recovery();
		autosave_recovery(const autosave_recovery&) = delete;
		autosave_recovery& operator=(const autosave_recovery&) = delete;

		// Waits for the start level to settle (register_frame), then scans.
		void install();
		// Scans now (once per run; a scan that is running or a question that is open makes it do nothing).
		void start();
		// A scan, a question or a restore is under way.
		bool busy() const { return m_busy; }

	private:
		struct pending_move
		{
			std::size_t session = 0;
			std::size_t record = 0;
			document_id document{};
			std::uint64_t generation = 0;
		};

		struct batch
		{
			std::vector<std::size_t> sessions;
			bool unverifiable = false;
		};

		void take(std::shared_ptr<recovery_scan_result> scan);
		void prepare(recovery_session& session);
		void ask_next();
		void answer(std::optional<std::size_t> picked);
		void discard_batch();
		void restore_next();
		void finish_record(std::size_t session, std::size_t record, const restore_outcome& outcome);
		void moved(const pending_move& move, const std::string& error);
		void job_ended(std::uint64_t id, job_status status);
		// Removes the record files the scan read (verified and rejected), nothing else of the session.
		void remove_listed(recovery_session& session);
		// Removes the folder of a dead session whose lock it holds, when a fresh listing finds nothing in it but
		// session.json, session.lock and empty kind folders; else keeps it.
		void remove_if_still_empty(recovery_session& session);
		void end_batch();
		void finish();
		restore_port port_of(std::string_view kind) const;

		ports m_ports;
		std::filesystem::path m_root;
		std::shared_ptr<recovery_scan_result> m_scan;
		std::vector<batch> m_batches;
		std::optional<batch> m_current;
		std::vector<std::pair<std::size_t, std::size_t>> m_queue;
		std::uint64_t m_serial = 0;
		bool m_started = false;
		bool m_busy = false;
		std::shared_ptr<int> m_alive = std::make_shared<int>(0);
		std::uint64_t m_scan_job = 0;
		std::uint64_t m_move_job = 0;
		std::optional<pending_move> m_move;
		connection m_finished;
		// Declared last: destroyed (and its jobs' results dropped) first.
		ds::lifetime m_lifetime;
	};
}
