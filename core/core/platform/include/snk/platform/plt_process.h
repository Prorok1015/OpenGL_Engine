#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>
#include <snk/common/snk_api.h>

namespace platform
{
	// The file of the running program; empty when the system cannot tell.
	SNK_API std::filesystem::path current_executable();

	SNK_API std::int64_t current_process_id();
	// Whether a process with this id runs now: true for one this process may not look at, false for an id <= 0. An id
	// the system reused for another program counts as running.
	SNK_API bool process_is_running(std::int64_t pid);

	struct spawn_result
	{
		// The id the system gave the new process; 0 exactly when `error` is set.
		std::uint64_t process_id = 0;
		std::string error;

		explicit operator bool() const { return error.empty(); }
	};

	// Starts `program` with `args` (the program itself not among them, UTF-8) and does not wait for it: the new process
	// outlives this one.
	SNK_API spawn_result spawn_detached(const std::filesystem::path& program, const std::vector<std::string>& args);

	// One argument of a Windows command line, quoted so CommandLineToArgvW (and the C runtime) give it back unchanged:
	// quotes only around an empty argument or one with a space, tab or '"'; inside them a '"' becomes \" and the
	// backslashes right before a '"' (or the closing one) are doubled. Pure, so it is tested on every platform.
	SNK_API std::wstring quote_windows_arg(std::wstring_view arg);

	// Windows (#1054, #1099): an AppContainer profile and its SID, made (or found) once and held for every confined start.
	// The profile exists before the first start, so a start that fails never has to be read as "no profile". Other
	// systems have no AppContainer: `ok()` is false there, with the reason.
	class SNK_API app_container
	{
	public:
		// CreateAppContainerProfile; one that already exists is taken by its name.
		static std::shared_ptr<const app_container> ensure(std::wstring name);

		~app_container();
		app_container(const app_container&) = delete;
		app_container& operator=(const app_container&) = delete;

		bool ok() const;
		// Why there is no container; empty when ok().
		const std::string& error() const;
		const std::wstring& name() const;
		// The container's PSID (a Windows SID); null unless ok().
		void* sid() const;

	private:
		struct state;
		explicit app_container(std::unique_ptr<state> held);
		std::unique_ptr<state> m_state;
	};

	// Windows (#1054): how a child is confined. An AppContainer with no capabilities (no network, no user profile, no
	// file a container may not open), a child process policy and process mitigations (no child processes, no dynamic
	// code, no extension points, no images from remote paths (the C/C++ runtime beside the program wins over an older one
	// in System32), strict handle checks, no fonts), and a job of one process without UI access. The child gets no console
	// (an AppContainer process that attaches one does not start); its stdio are the pipes.
	struct process_confinement
	{
		// Null or not ok(): nothing starts, and the result says why (confinement_failed).
		std::shared_ptr<const app_container> container;
		// Read and write for the container, inherited by what the folder holds, and the low integrity label (an
		// AppContainer writes nowhere else). Both are set by name (through a reparse point there) and never taken back:
		// only a folder the caller made for this one start and removes after it, never a path a person chose.
		std::vector<std::filesystem::path> read_write;
		// Read and execute for the container, inherited by what the folder holds; added only when the folder's access
		// list does not hold it yet, and never taken back.
		std::vector<std::filesystem::path> read_execute;
	};

	struct process_options
	{
		// Empty: the working directory of this process.
		std::filesystem::path working_dir;
		// The child's whole environment ("NAME=value" each) when set; the parent's environment otherwise.
		std::optional<std::vector<std::string>> environment;
		// `on_line` gets the output bytes in the chunks they were read in, not split into lines: for output that is
		// not 8-bit text (cmd /u writes UTF-16LE, where a 0x0A byte is not always a line break).
		bool raw_output = false;
		// Below normal priority (Windows) / nice 10 for the child's process group (POSIX), so a background build
		// leaves the machine usable. Failing to lower it is not an error.
		bool low_priority = false;
		// Windows: the most memory the child may commit (a job limit; a child that goes over fails its allocation).
		// POSIX: not applied here (posix_spawn has no hook between fork and exec); the child limits itself, as
		// snk_model_decoder does with RLIMIT_AS. Zero: no limit.
		std::uint64_t memory_limit_bytes = 0;
		// Set: the child's stderr gets a pipe of its own, and its lines (split as stdout's are; raw_output does not
		// apply) go here while stdout's still go to `on_line`. The two callbacks never run at the same time; on
		// Windows this one runs on a helper thread. Null: stderr shares stdout's pipe.
		std::function<void(std::string_view line)> on_stderr_line;
		// Windows: start the child confined (#1054). When the confinement cannot be set up, nothing starts and the
		// result says so (confinement_failed). Ignored elsewhere.
		std::optional<process_confinement> confinement;
	};

	struct process_result
	{
		int exit_code = -1;
		// Stopped through the stop_token. Also true when the stop raced a child that had already finished; exit_code
		// is then still its real exit code.
		bool cancelled = false;
		// Could not start (or another system failure); exit_code is then -1.
		std::string error;
		// With `error`: process_options::confinement could not be set up (no container SID, an access list that could
		// not be changed, a start the system refused for the container or its policies). Nothing was started.
		bool confinement_failed = false;

		explicit operator bool() const { return error.empty() && !cancelled && exit_code == 0; }
	};

	// Runs `program` with `args` (UTF-8, the program itself not among them) and waits for it. A program without a
	// directory is looked up only in the PATH of the environment the child gets (on Windows with the PATHEXT
	// extensions; never in the current directory). stdout and stderr go to one pipe (unless
	// process_options::on_stderr_line is set) and arrive line by line in `on_line` (without the line break; "\r\n", "\n" and a lone "\r" end a line; a line is cut at k_max_line_size; a
	// last line without a break is delivered too), on the calling thread. stdin is empty. A stop request ends the
	// process and everything it started, then returns with `cancelled`; what it started and left running is ended as
	// well once it exits. Blocks: call it from a worker thread.
	// Output bytes are passed through as received: a localized MSVC prints in the OEM code page (set VSLANG=1033 or
	// use /utf-8).
	// On POSIX the tree is the child's process group: descendants that start their own group or session escape it.
	SNK_API process_result run_process(const std::filesystem::path& program, const std::vector<std::string>& args,
		const process_options& options, const std::function<void(std::string_view line)>& on_line,
		std::stop_token stop);

	namespace detail
	{
		inline constexpr std::size_t k_max_line_size = 64 * 1024;

		// Appends `chunk` to `pending`, hands every complete line in it to `on_line` and keeps the rest; at the end of
		// the output the rest too, when there is any. Pure, so it is tested on every platform.
		SNK_API void take_lines(std::string& pending, std::string_view chunk, bool end_of_output,
			const std::function<void(std::string_view line)>& on_line);
		// take_lines, or with `raw` every non-empty chunk as it is (process_options::raw_output). Pure.
		SNK_API void take_output(bool raw, std::string& pending, std::string_view chunk, bool end_of_output,
			const std::function<void(std::string_view line)>& on_line);
	}
}
