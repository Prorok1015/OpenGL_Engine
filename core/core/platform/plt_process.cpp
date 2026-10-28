#include <snk/platform/plt_process.h>
#include <chrono>
#include <cstddef>
#include <format>
#include <string_view>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;

#if defined(__GLIBC__)
#if __GLIBC_PREREQ(2, 29)
#define SNK_PLT_SPAWN_CHDIR 1
#endif
#endif
#endif

namespace fs = std::filesystem;

std::wstring platform::quote_windows_arg(std::wstring_view arg)
{
	if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
		return std::wstring{ arg };
	}
	std::wstring out = L"\"";
	for (auto it = arg.begin();; ++it) {
		std::size_t backslashes = 0;
		while (it != arg.end() && *it == L'\\') {
			++it;
			++backslashes;
		}
		if (it == arg.end()) {
			out.append(backslashes * 2, L'\\');
			break;
		}
		if (*it == L'"') {
			out.append(backslashes * 2 + 1, L'\\');
		} else {
			out.append(backslashes, L'\\');
		}
		out.push_back(*it);
	}
	out.push_back(L'"');
	return out;
}

void platform::detail::take_lines(std::string& pending, std::string_view chunk, bool end_of_output,
	const std::function<void(std::string_view line)>& on_line)
{
	// What was pending holds no line break, so the search starts at the new text.
	const std::size_t search_from = pending.size();
	pending.append(chunk);
	std::size_t start = 0;
	const auto deliver = [&](std::size_t end) {
		std::string_view line{ pending.data() + start, end - start };
		if (!line.empty() && line.back() == '\r') {
			line.remove_suffix(1);
		}
		on_line(line);
	};
	for (std::size_t end = pending.find('\n', search_from); end != std::string::npos; end = pending.find('\n', start)) {
		deliver(end);
		start = end + 1;
	}
	if (end_of_output && start < pending.size()) {
		deliver(pending.size());
		start = pending.size();
	}
	pending.erase(0, start);
}

namespace
{
	template <class F>
	class scope_exit
	{
	public:
		explicit scope_exit(F action) : m_action(std::move(action)) {}
		~scope_exit() { m_action(); }
		scope_exit(const scope_exit&) = delete;
		scope_exit& operator=(const scope_exit&) = delete;

	private:
		F m_action;
	};

	constexpr std::size_t k_read_size = 4096;
}

#ifdef _WIN32

namespace
{
	std::string utf8_of(const fs::path& path)
	{
		const std::u8string text = path.u8string();
		return std::string(text.begin(), text.end());
	}

	std::wstring widen(const std::string& utf8)
	{
		if (utf8.empty()) {
			return {};
		}
		const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
		std::wstring out(static_cast<std::size_t>(size), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), size);
		return out;
	}

	class handle_owner
	{
	public:
		explicit handle_owner(HANDLE handle) : m_handle(handle == INVALID_HANDLE_VALUE ? nullptr : handle) {}
		~handle_owner() { reset(); }
		handle_owner(const handle_owner&) = delete;
		handle_owner& operator=(const handle_owner&) = delete;

		HANDLE get() const { return m_handle; }
		void reset()
		{
			if (m_handle != nullptr) {
				CloseHandle(m_handle);
				m_handle = nullptr;
			}
		}

	private:
		HANDLE m_handle = nullptr;
	};

	platform::process_result system_failure(std::string_view what, const fs::path& program)
	{
		const auto error = std::error_code{ static_cast<int>(GetLastError()), std::system_category() };
		platform::process_result result;
		result.error = std::format("{} '{}': {}", what, utf8_of(program), error.message());
		return result;
	}

	// The process ended: whatever it left running in the job would hold the pipe open, so it ends too.
	void CALLBACK end_leftovers(PVOID job, BOOLEAN)
	{
		TerminateJobObject(static_cast<HANDLE>(job), 1);
	}
}

fs::path platform::current_executable()
{
	std::wstring buffer(MAX_PATH, L'\0');
	// Long paths can exceed MAX_PATH; the documented limit of a Windows path is 32767 characters.
	while (buffer.size() <= 32768) {
		const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
		if (length == 0) {
			return {};
		}
		if (length < buffer.size()) {
			buffer.resize(length);
			return fs::path{ buffer };
		}
		buffer.resize(buffer.size() * 2);
	}
	return {};
}

platform::spawn_result platform::spawn_detached(const fs::path& program, const std::vector<std::string>& args)
{
	std::wstring command_line = quote_windows_arg(program.native());
	for (const auto& arg : args) {
		command_line += L' ';
		command_line += quote_windows_arg(widen(arg));
	}

	STARTUPINFOW startup{};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process{};
	// CreateProcessW may write into the command line, so it gets its own buffer.
	if (!CreateProcessW(program.c_str(), command_line.data(), nullptr, nullptr, FALSE,
			DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &startup, &process)) {
		const auto error = std::error_code{ static_cast<int>(GetLastError()), std::system_category() };
		return { 0, std::format("cannot start '{}': {}", utf8_of(program), error.message()) };
	}
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	return { process.dwProcessId, {} };
}

platform::process_result platform::run_process(const fs::path& program, const std::vector<std::string>& args,
	const process_options& options, const std::function<void(std::string_view line)>& on_line, std::stop_token stop)
{
	SECURITY_ATTRIBUTES inheritable{};
	inheritable.nLength = sizeof(inheritable);
	inheritable.bInheritHandle = TRUE;
	HANDLE read_raw = nullptr;
	HANDLE write_raw = nullptr;
	if (!CreatePipe(&read_raw, &write_raw, &inheritable, 0)) {
		return system_failure("cannot create the output pipe of", program);
	}
	handle_owner read_end{ read_raw };
	handle_owner write_end{ write_raw };
	if (!SetHandleInformation(read_end.get(), HANDLE_FLAG_INHERIT, 0)) {
		return system_failure("cannot create the output pipe of", program);
	}
	handle_owner null_input{ CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
	if (null_input.get() == nullptr) {
		return system_failure("cannot open NUL as the input of", program);
	}

	SIZE_T list_size = 0;
	InitializeProcThreadAttributeList(nullptr, 1, 0, &list_size);
	std::vector<std::byte> list_storage(list_size);
	auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(list_storage.data());
	if (!InitializeProcThreadAttributeList(attributes, 1, 0, &list_size)) {
		return system_failure("cannot prepare the start of", program);
	}
	const scope_exit delete_attributes{ [attributes] { DeleteProcThreadAttributeList(attributes); } };
	// Only these two are inherited: any other inheritable handle of this process would keep the pipe (or a file of
	// the editor) open for as long as the build runs.
	HANDLE inherited[] = { write_end.get(), null_input.get() };
	if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited),
			nullptr, nullptr)) {
		return system_failure("cannot prepare the start of", program);
	}

	STARTUPINFOEXW startup{};
	startup.StartupInfo.cb = sizeof(startup);
	startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	startup.StartupInfo.hStdInput = null_input.get();
	startup.StartupInfo.hStdOutput = write_end.get();
	startup.StartupInfo.hStdError = write_end.get();
	startup.lpAttributeList = attributes;

	std::wstring command_line = quote_windows_arg(program.native());
	for (const auto& arg : args) {
		command_line += L' ';
		command_line += quote_windows_arg(widen(arg));
	}
	std::wstring environment;
	if (options.environment) {
		for (const auto& entry : *options.environment) {
			environment += widen(entry);
			environment.push_back(L'\0');
		}
		if (environment.empty()) {
			environment.push_back(L'\0');
		}
		environment.push_back(L'\0');
	}

	PROCESS_INFORMATION process{};
	// Without a directory the system searches for the program (PATH among other places) and appends ".exe".
	if (!CreateProcessW(program.has_parent_path() ? program.c_str() : nullptr, command_line.data(), nullptr, nullptr,
			TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
			options.environment ? environment.data() : nullptr,
			options.working_dir.empty() ? nullptr : options.working_dir.c_str(), &startup.StartupInfo, &process)) {
		return system_failure("cannot start", program);
	}
	handle_owner process_handle{ process.hProcess };
	handle_owner thread_handle{ process.hThread };
	// ReadFile sees the end of the output only once no process holds a write end, this one included.
	write_end.reset();
	null_input.reset();

	handle_owner job{ CreateJobObjectW(nullptr, nullptr) };
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
	limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	if (job.get() == nullptr
		|| !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))
		|| !AssignProcessToJobObject(job.get(), process_handle.get())) {
		auto result = system_failure("cannot put into a job object", program);
		TerminateProcess(process_handle.get(), 1);
		WaitForSingleObject(process_handle.get(), INFINITE);
		return result;
	}
	if (ResumeThread(thread_handle.get()) == static_cast<DWORD>(-1)) {
		auto result = system_failure("cannot resume", program);
		TerminateJobObject(job.get(), 1);
		WaitForSingleObject(process_handle.get(), INFINITE);
		return result;
	}
	thread_handle.reset();

	HANDLE exit_wait = nullptr;
	if (!RegisterWaitForSingleObject(&exit_wait, process_handle.get(), end_leftovers, job.get(), INFINITE,
			WT_EXECUTEONLYONCE)) {
		exit_wait = nullptr;
	}
	// Declared after the job, so it is undone first: the callback must not run on a closed job handle.
	const scope_exit unregister_wait{ [exit_wait] {
		if (exit_wait != nullptr) {
			UnregisterWaitEx(exit_wait, INVALID_HANDLE_VALUE);
		}
	} };
	{
		// Ending the job closes the pipe along with the processes, so the blocking ReadFile returns.
		const std::stop_callback cancel{ stop, [job = job.get()] { TerminateJobObject(job, 1); } };
		std::string pending;
		char buffer[k_read_size];
		DWORD got = 0;
		while (ReadFile(read_end.get(), buffer, static_cast<DWORD>(sizeof(buffer)), &got, nullptr)) {
			detail::take_lines(pending, std::string_view{ buffer, got }, false, on_line);
		}
		detail::take_lines(pending, {}, true, on_line);
	}

	process_result result;
	result.cancelled = stop.stop_requested();
	WaitForSingleObject(process_handle.get(), INFINITE);
	DWORD exit_code = 0;
	if (!GetExitCodeProcess(process_handle.get(), &exit_code)) {
		return system_failure("cannot read the exit code of", program);
	}
	result.exit_code = static_cast<int>(exit_code);
	return result;
}

#else

fs::path platform::current_executable()
{
	std::error_code ec;
	const fs::path path = fs::read_symlink("/proc/self/exe", ec);
	return ec ? fs::path{} : path;
}

platform::spawn_result platform::spawn_detached(const fs::path& program, const std::vector<std::string>& args)
{
	std::string program_name = program.string();
	std::vector<std::string> owned = args;
	std::vector<char*> argv;
	argv.reserve(owned.size() + 2);
	argv.push_back(program_name.data());
	for (auto& arg : owned) {
		argv.push_back(arg.data());
	}
	argv.push_back(nullptr);

	posix_spawnattr_t attributes;
	posix_spawnattr_init(&attributes);
#ifdef POSIX_SPAWN_SETSID
	// Its own session: closing the terminal (or the process group) of this editor does not take the new one along.
	posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
#endif
	posix_spawn_file_actions_t file_actions;
	posix_spawn_file_actions_init(&file_actions);
#if defined(__GLIBC__)
#if __GLIBC_PREREQ(2, 34)
	// Only stdin/out/err go along: Asio does not open its sockets close-on-exec, so the new editor would otherwise
	// hold the listening socket of this one (and its log files) for as long as it runs.
	posix_spawn_file_actions_addclosefrom_np(&file_actions, 3);
#endif
#endif
	pid_t pid = 0;
	const int result = posix_spawn(&pid, program_name.c_str(), &file_actions, &attributes, argv.data(), environ);
	posix_spawn_file_actions_destroy(&file_actions);
	posix_spawnattr_destroy(&attributes);
	if (result != 0) {
		return { 0, std::format("cannot start '{}': {}", program_name, std::generic_category().message(result)) };
	}
	return { static_cast<std::uint64_t>(pid), {} };
}

namespace
{
	class fd_owner
	{
	public:
		explicit fd_owner(int fd) : m_fd(fd) {}
		~fd_owner() { reset(); }
		fd_owner(const fd_owner&) = delete;
		fd_owner& operator=(const fd_owner&) = delete;

		int get() const { return m_fd; }
		void reset()
		{
			if (m_fd >= 0) {
				::close(m_fd);
				m_fd = -1;
			}
		}

	private:
		int m_fd = -1;
	};

	platform::process_result failure(std::string_view what, const fs::path& program, int error)
	{
		platform::process_result result;
		result.error = std::format("{} '{}': {}", what, program.string(), std::generic_category().message(error));
		return result;
	}

	// Without reaping it, so its process group id stays reserved and can still be signalled.
	bool has_exited(pid_t pid)
	{
		siginfo_t info{};
		return ::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT) == 0 && info.si_pid == pid;
	}

	constexpr std::chrono::seconds k_kill_after{ 2 };
	constexpr std::chrono::seconds k_drain_after{ 2 };
	constexpr int k_poll_ms = 100;
}

platform::process_result platform::run_process(const fs::path& program, const std::vector<std::string>& args,
	const process_options& options, const std::function<void(std::string_view line)>& on_line, std::stop_token stop)
{
	int pipe_ends[2] = { -1, -1 };
	if (::pipe2(pipe_ends, O_CLOEXEC) != 0) {
		return failure("cannot create the output pipe of", program, errno);
	}
	fd_owner read_end{ pipe_ends[0] };
	fd_owner write_end{ pipe_ends[1] };

	posix_spawn_file_actions_t file_actions;
	posix_spawn_file_actions_init(&file_actions);
	const scope_exit destroy_file_actions{ [&file_actions] { posix_spawn_file_actions_destroy(&file_actions); } };
	posix_spawn_file_actions_adddup2(&file_actions, write_end.get(), STDOUT_FILENO);
	posix_spawn_file_actions_adddup2(&file_actions, write_end.get(), STDERR_FILENO);
	posix_spawn_file_actions_addopen(&file_actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
#if defined(__GLIBC__)
#if __GLIBC_PREREQ(2, 34)
	// Only stdin/out/err go along: Asio does not open its sockets close-on-exec, so the build would otherwise hold
	// the listening socket of the editor (and its log files) for as long as it runs.
	posix_spawn_file_actions_addclosefrom_np(&file_actions, 3);
#endif
#endif
	if (!options.working_dir.empty()) {
#if SNK_PLT_SPAWN_CHDIR
		posix_spawn_file_actions_addchdir_np(&file_actions, options.working_dir.c_str());
#else
		return failure("cannot set the working directory of", program, ENOSYS);
#endif
	}

	posix_spawnattr_t attributes;
	posix_spawnattr_init(&attributes);
	const scope_exit destroy_attributes{ [&attributes] { posix_spawnattr_destroy(&attributes); } };
	// The child leads a new process group, so a stop reaches it and everything it starts.
	posix_spawnattr_setpgroup(&attributes, 0);
	// A SIGTERM blocked or ignored in this process must not make the child deaf to a stop.
	sigset_t no_signals;
	sigemptyset(&no_signals);
	posix_spawnattr_setsigmask(&attributes, &no_signals);
	sigset_t default_signals;
	sigemptyset(&default_signals);
	sigaddset(&default_signals, SIGTERM);
	sigaddset(&default_signals, SIGPIPE);
	posix_spawnattr_setsigdefault(&attributes, &default_signals);
	posix_spawnattr_setflags(&attributes,
		static_cast<short>(POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF));

	std::string program_name = program.string();
	std::vector<std::string> owned_args = args;
	std::vector<char*> argv;
	argv.reserve(owned_args.size() + 2);
	argv.push_back(program_name.data());
	for (auto& arg : owned_args) {
		argv.push_back(arg.data());
	}
	argv.push_back(nullptr);
	std::vector<std::string> owned_environment;
	std::vector<char*> envp;
	if (options.environment) {
		owned_environment = *options.environment;
		envp.reserve(owned_environment.size() + 1);
		for (auto& entry : owned_environment) {
			envp.push_back(entry.data());
		}
		envp.push_back(nullptr);
	}

	pid_t pid = 0;
	// posix_spawnp: a program without a '/' is looked up in PATH ("cmake").
	const int spawned = ::posix_spawnp(&pid, program_name.c_str(), &file_actions, &attributes, argv.data(),
		options.environment ? envp.data() : environ);
	write_end.reset();
	if (spawned != 0) {
		return failure("cannot start", program, spawned);
	}

	bool reaped = false;
	// on_line may throw: no process is left behind then either.
	const scope_exit reap_on_unwind{ [&reaped, pid] {
		if (!reaped) {
			::kill(-pid, SIGKILL);
			while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
			}
		}
	} };

	using steady = std::chrono::steady_clock;
	std::optional<steady::time_point> terminated_at;
	bool killed = false;
	std::optional<steady::time_point> exited_at;
	std::string pending;
	char buffer[k_read_size];
	for (;;) {
		const auto now = steady::now();
		if (!terminated_at && stop.stop_requested()) {
			::kill(-pid, SIGTERM);
			terminated_at = now;
		}
		if (terminated_at && !killed && now - *terminated_at >= k_kill_after) {
			::kill(-pid, SIGKILL);
			killed = true;
		}
		// A descendant that left the group may hold the pipe for good.
		if (exited_at && now - *exited_at >= k_drain_after) {
			break;
		}
		pollfd watched{ read_end.get(), POLLIN, 0 };
		const int ready = ::poll(&watched, 1, k_poll_ms);
		if (ready < 0 && errno != EINTR) {
			break;
		}
		if (ready <= 0) {
			if (!exited_at && has_exited(pid)) {
				exited_at = steady::now();
				// Whatever it left running in its group would hold the pipe open.
				::kill(-pid, SIGKILL);
			}
			continue;
		}
		const ssize_t got = ::read(read_end.get(), buffer, sizeof(buffer));
		if (got > 0) {
			detail::take_lines(pending, std::string_view{ buffer, static_cast<std::size_t>(got) }, false, on_line);
		} else if (got == 0 || (errno != EINTR && errno != EAGAIN)) {
			break;
		}
	}
	detail::take_lines(pending, {}, true, on_line);

	process_result result;
	result.cancelled = stop.stop_requested();
	int status = 0;
	pid_t waited = -1;
	do {
		waited = ::waitpid(pid, &status, 0);
	} while (waited < 0 && errno == EINTR);
	reaped = true;
	if (waited < 0) {
		return failure("cannot wait for", program, errno);
	}
	if (WIFEXITED(status)) {
		result.exit_code = WEXITSTATUS(status);
	} else if (WIFSIGNALED(status)) {
		result.exit_code = 128 + WTERMSIG(status);
	}
	return result;
}

#endif
