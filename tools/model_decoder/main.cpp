// snk_model_decoder: reads one model file into the engine's descs in a process of its own, so a parser that crashes,
// hangs or eats memory on a hostile file costs the editor nothing but a failed import (#647).
//
//   snk_model_decoder --request <bundle> --out <bundle>
//
// The request bundle holds the JSON of edt::decode_request, the model's bytes and the bytes of the side files the editor
// read for it by its rule (#1043): this process opens no file but its run folder. The out bundle holds a manifest
// ({"kind":"scan","requests"|"error"} or {"kind":"import",...}) and, for an import, the resources it made under
// memory://, held in memory until they are written out; the prefab is not stamped (the editor stamps what it accepts).
// Exit codes: 0 an out bundle was written (a refused model is an "error" inside it), 1 no out bundle could be made, 2 usage,
// 4 it refused to run without its OS sandbox (#1043, `--sandbox off|best_effort|required`, one line on stderr says why).
// Run it with the working directory a scratch folder: the sandbox leaves it nothing else to touch.
// It links no engine (#1077): the import core, snk_model_codec, common, the logger and platform, no window, GL or
// resource system.
// The self-test flags (--self-test-*) exist only in the test build (SNK_MODEL_DECODER_SELF_TEST), never in the shipped tool.
#include "decoder_sandbox.h"
#include "edt_decode_bundle.h"
#include "edt_decode_codec.h"
#include "edt_model_import_core.h"
#include <snk/common/logger/engine_log.h>
#include <snk/model_import/mdl_import.h>
#include <snk/platform/plt_process.h>
#include <snk/model_codec/res_tag.h>
#include <boost/json/object.hpp>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstddef>
#include <new>
#include <optional>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <sys/prctl.h>
#include <sys/resource.h>
#include <unistd.h>
#include <csignal>
#if defined(SNK_MODEL_DECODER_SELF_TEST)
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#endif
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#if defined(SNK_MODEL_DECODER_SELF_TEST)
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include <Windows.h>
#include <shellapi.h>
#include <winioctl.h>
#endif

namespace fs = std::filesystem;

namespace {

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr bool k_address_space_limit_works = false;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
constexpr bool k_address_space_limit_works = false;
#else
constexpr bool k_address_space_limit_works = true;
#endif
#else
constexpr bool k_address_space_limit_works = true;
#endif

// First thing main does, before anything of the request is read: whatever the model makes the parser do, the process
// leaves no core dump (it would hold the model's bytes, outside the run folder), no crash dialog and does not outlive its
// parent. POSIX also takes away privilege gain.
void harden_process(long expected_parent)
{
#if defined(__linux__)
	prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
	prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
	const rlimit no_core{ 0, 0 };
	setrlimit(RLIMIT_CORE, &no_core);
	prctl(PR_SET_PDEATHSIG, SIGKILL);
	// The signal only comes for a death after the prctl: a parent that died before it is seen here, against the pid the
	// parent passed (without one, against nothing: getppid() of an orphan is 1 or a subreaper, not a pid to trust).
	if (expected_parent > 0 && getppid() != static_cast<pid_t>(expected_parent)) {
		std::_Exit(1);
	}
#elif defined(_WIN32)
	(void)expected_parent;
	SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
#else
	(void)expected_parent;
#endif
}

// --parent-pid <n>, read before anything else (the full parse comes after the hardening).
long parent_pid_argument(int argc, char** argv)
{
	for (int i = 1; i + 1 < argc; ++i) {
		if (std::string_view(argv[i]) == "--parent-pid") {
			return std::strtol(argv[i + 1], nullptr, 10);
		}
	}
	return 0;
}

// The sanitizers reserve terabytes of address space for their shadow memory, so RLIMIT_AS cannot be used under them.
// Windows: the editor's Job object limits the memory.
// False when the limit was asked for and could not be set: the caller ends, a decoder without its limit must not run.
bool limit_address_space(std::uint64_t memory_limit_bytes)
{
#if defined(__linux__)
	if (memory_limit_bytes != 0 && k_address_space_limit_works) {
		const rlimit limit{ static_cast<rlim_t>(memory_limit_bytes), static_cast<rlim_t>(memory_limit_bytes) };
		return setrlimit(RLIMIT_AS, &limit) == 0;
	}
#else
	(void)memory_limit_bytes;
#endif
	return true;
}

struct arguments
{
	fs::path request;
	fs::path out;
	edt::sandbox_mode sandbox = edt::sandbox_mode::best_effort;
	std::uint64_t max_output_bytes = 1ull << 30;
#if defined(SNK_MODEL_DECODER_SELF_TEST)
	// For the process tests: sleep this long / allocate and touch this much / start a copy of this program that sleeps
	// and write its process id to a file, instead of decoding.
	std::uint64_t self_test_sleep_ms = 0;
	std::uint64_t self_test_alloc_bytes = 0;
	fs::path self_test_spawn_pid_file;
	// For the sandbox tests (#1043), once the sandbox is applied: open this file / connect to TCP / start a program /
	// print what the sandbox applied, instead of decoding. Exit 0: refused (or printed), 5: it worked.
	fs::path self_test_open;
	bool self_test_connect = false;
	// Connect here instead of port 9: a listener the test holds, so a connection that is not refused is seen.
	unsigned short self_test_connect_port = 0;
	bool self_test_exec = false;
	bool self_test_sandbox_report = false;
	// Signal the parent, or make the kernel signal it for this process's I/O (fcntl F_SETOWN, F_SETSIG, O_ASYNC,
	// F_NOTIFY).
	bool self_test_signal = false;
	bool self_test_pretend_no_landlock = false;
	// Start a second thread before the sandbox is applied: the sandbox must refuse to cover only one of them.
	bool self_test_extra_thread = false;
	// Plant a link in the run folder that points at this folder (a symlink; Windows: a junction): the editor must remove
	// the run folder without going through it (#1054). Exit 5: planted, 0: refused.
	fs::path self_test_link_out;
	// Turn the run folder itself into a link to this folder (Windows: a junction, after emptying it), then exit 0 so
	// the editor goes on to read and remove it: neither may go through the link (#1099).
	fs::path self_test_link_self;
	// Hard-link this file (outside the run folder) into the run folder: a link would give it the file's contents and a
	// way to write them (#1099). Exit 5: linked, 0: refused.
	fs::path self_test_hardlink;
	// One call the sandbox must refuse (#1082): write, truncate, chmod (on self_test_path), io_uring, ptrace, fork.
	std::string self_test_call;
	fs::path self_test_path;
#endif
};

// The command line as paths: on Windows main's char** is the ANSI code page, which loses a path with other letters (a
// Cyrillic profile name), so the wide command line is read instead.
std::vector<fs::path> command_line(int argc, char** argv)
{
	std::vector<fs::path> out;
#if defined(_WIN32)
	(void)argc;
	(void)argv;
	int count = 0;
	if (LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count)) {
		for (int i = 0; i < count; ++i) out.emplace_back(wide[i]);
		LocalFree(wide);
	}
#else
	for (int i = 0; i < argc; ++i) out.emplace_back(argv[i]);
#endif
	return out;
}

std::optional<arguments> parse_arguments(const std::vector<fs::path>& args)
{
	arguments parsed;
	for (std::size_t i = 1; i < args.size(); ++i) {
		const std::string arg = args[i].string();
		const bool has_value = i + 1 < args.size();
		if (arg == "--request" && has_value) {
			parsed.request = fs::absolute(args[++i]);
		} else if (arg == "--out" && has_value) {
			parsed.out = fs::absolute(args[++i]);
		} else if (arg == "--parent-pid" && has_value) {
			++i;
		} else if (arg == "--sandbox" && has_value) {
			const std::optional<edt::sandbox_mode> mode = edt::sandbox_mode_from_name(args[++i].string());
			if (!mode) return std::nullopt;
			parsed.sandbox = *mode;
		} else if (arg == "--max-output-bytes" && has_value) {
			parsed.max_output_bytes = std::strtoull(args[++i].string().c_str(), nullptr, 10);
			if (parsed.max_output_bytes == 0) return std::nullopt;
#if defined(SNK_MODEL_DECODER_SELF_TEST)
		} else if (arg == "--self-test-sleep" && has_value) {
			parsed.self_test_sleep_ms = std::strtoull(args[++i].string().c_str(), nullptr, 10);
		} else if (arg == "--self-test-alloc" && has_value) {
			parsed.self_test_alloc_bytes = std::strtoull(args[++i].string().c_str(), nullptr, 10);
		} else if (arg == "--self-test-spawn" && has_value) {
			parsed.self_test_spawn_pid_file = args[++i];
		} else if (arg == "--self-test-open" && has_value) {
			parsed.self_test_open = args[++i];
		} else if (arg == "--self-test-connect") {
			parsed.self_test_connect = true;
		} else if (arg == "--self-test-connect-port" && has_value) {
			parsed.self_test_connect = true;
			parsed.self_test_connect_port = static_cast<unsigned short>(std::strtoul(args[++i].string().c_str(), nullptr, 10));
		} else if (arg == "--self-test-exec") {
			parsed.self_test_exec = true;
		} else if (arg == "--self-test-sandbox-report") {
			parsed.self_test_sandbox_report = true;
		} else if (arg == "--self-test-signal") {
			parsed.self_test_signal = true;
		} else if (arg == "--self-test-pretend-no-landlock") {
			parsed.self_test_pretend_no_landlock = true;
		} else if (arg == "--self-test-extra-thread") {
			parsed.self_test_extra_thread = true;
		} else if (arg == "--self-test-link-out" && has_value) {
			parsed.self_test_link_out = args[++i];
		} else if (arg == "--self-test-link-self" && has_value) {
			parsed.self_test_link_self = args[++i];
		} else if (arg == "--self-test-hardlink" && has_value) {
			parsed.self_test_hardlink = args[++i];
		} else if (arg == "--self-test-call" && has_value) {
			parsed.self_test_call = args[++i].string();
		} else if (arg == "--self-test-path" && has_value) {
			parsed.self_test_path = args[++i];
#endif
		} else {
			return std::nullopt;
		}
	}
#if defined(SNK_MODEL_DECODER_SELF_TEST)
	if (parsed.self_test_sleep_ms != 0 || parsed.self_test_alloc_bytes != 0 || !parsed.self_test_spawn_pid_file.empty()
		|| !parsed.self_test_open.empty() || parsed.self_test_connect || parsed.self_test_exec || parsed.self_test_sandbox_report
		|| parsed.self_test_signal || parsed.self_test_extra_thread || !parsed.self_test_link_out.empty()
		|| !parsed.self_test_call.empty() || !parsed.self_test_link_self.empty() || !parsed.self_test_hardlink.empty()) {
		return parsed;
	}
#endif
	if (parsed.request.empty() || parsed.out.empty()) {
		return std::nullopt;
	}
	return parsed;
}

// Where the import writes: memory:// of this process only. The tags in the order they were first stored, each once.
class memory_sink final : public mdl::import_sink
{
public:
	bool exists(const res::tag& tag) override { return m_index.contains(std::string(tag.view())); }

	void store(const res::tag& tag, const std::vector<std::byte>& bytes) override
	{
		const auto [found, added] = m_index.try_emplace(std::string(tag.view()), m_stored.size());
		if (added) {
			m_stored.push_back({ tag, bytes });
		} else {
			m_stored[found->second].second = bytes;
		}
	}

	const std::vector<std::pair<res::tag, std::vector<std::byte>>>& stored() const { return m_stored; }

private:
	std::unordered_map<std::string, std::size_t> m_index;
	std::vector<std::pair<res::tag, std::vector<std::byte>>> m_stored;
};

int fail(const std::string& message)
{
	egLOG_ERROR("model_decoder", "{}", message);
	return 1;
}

// The blob index (provided_entry::blob) that a blob's tag names; SIZE_MAX when it names none.
std::size_t provided_index(const std::string& tag)
{
	const std::string_view prefix = edt::k_decode_file_tag_prefix;
	if (!tag.starts_with(prefix)) return SIZE_MAX;
	const char* begin = tag.data() + prefix.size();
	const char* end = tag.data() + tag.size();
	std::size_t index = 0;
	const auto [last, error] = std::from_chars(begin, end, index);
	return error == std::errc{} && last == end ? index : SIZE_MAX;
}

boost::json::object failure_manifest(const char* kind, const std::string& error)
{
	boost::json::object manifest;
	manifest["kind"] = kind;
	manifest["error"] = error;
	return manifest;
}

#if defined(SNK_MODEL_DECODER_SELF_TEST) && defined(_WIN32)
// A junction ./escape -> `target` (a mount point reparse point, which any user may set on an empty folder of theirs).
bool make_junction(const wchar_t* folder_name, const fs::path& target)
{
	const HANDLE folder = CreateFileW(folder_name, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (folder == INVALID_HANDLE_VALUE) return false;
	const std::wstring substitute = L"\\??\\" + fs::absolute(target).wstring();
	const auto name_bytes = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
	// REPARSE_DATA_BUFFER (MountPointReparseBuffer), spelled out: the type is in the driver kit's headers only.
	std::vector<std::byte> buffer(8 + 8 + name_bytes + 2 * sizeof(wchar_t));
	const auto put16 = [&buffer](std::size_t at, WORD value) { std::memcpy(buffer.data() + at, &value, sizeof(value)); };
	const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
	std::memcpy(buffer.data(), &tag, sizeof(tag));
	put16(4, static_cast<WORD>(buffer.size() - 8));	// ReparseDataLength
	put16(8, 0);									// SubstituteNameOffset
	put16(10, name_bytes);							// SubstituteNameLength
	put16(12, static_cast<WORD>(name_bytes + sizeof(wchar_t)));	// PrintNameOffset
	put16(14, 0);									// PrintNameLength
	std::memcpy(buffer.data() + 16, substitute.data(), name_bytes);
	DWORD returned = 0;
	const BOOL set = DeviceIoControl(folder, FSCTL_SET_REPARSE_POINT, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr, 0,
		&returned, nullptr);
	const DWORD error = GetLastError();
	CloseHandle(folder);
	SetLastError(error);
	return set != FALSE;
}

bool plant_junction(const fs::path& target)
{
	return CreateDirectoryW(L"escape", nullptr) && make_junction(L"escape", target);
}

// The run folder (this process's working directory) emptied and made a junction to `target`.
bool turn_run_folder_into_junction(const fs::path& target)
{
	std::error_code ec;
	for (fs::directory_iterator it{ fs::current_path(), ec }, end; !ec && it != end; it.increment(ec)) {
		std::error_code ignored;
		fs::remove_all(it->path(), ignored);
	}
	const fs::path folder = fs::current_path(ec);
	if (ec || !make_junction(L".", target)) return false;
	// Written by the folder's full path, which now leads through the junction into `target` (a name relative to the
	// working directory would be opened from its handle, not through the junction): the test sees the folder became one.
	const HANDLE marker = CreateFileW((folder / "link_made").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
		nullptr);
	if (marker != INVALID_HANDLE_VALUE) CloseHandle(marker);
	return true;
}
#endif

#if defined(SNK_MODEL_DECODER_SELF_TEST)
#if defined(__linux__)
// One call of --self-test-call, with the codes of self_test_probe.
int self_test_call(std::string_view name, const fs::path& path)
{
	const auto refused = [](int error) { return error == EACCES || error == EPERM ? 0 : 6; };
	if (name == "write") {
		const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
		if (fd < 0) return refused(errno);
		const bool written = ::write(fd, "x", 1) == 1;
		::close(fd);
		return written ? 5 : 6;
	}
	if (name == "truncate") return ::truncate(path.c_str(), 0) == 0 ? 5 : refused(errno);
	if (name == "chmod") return ::chmod(path.c_str(), S_IRUSR | S_IWUSR) == 0 ? 5 : refused(errno);
	if (name == "io_uring") {
#if defined(__NR_io_uring_setup)
		// struct io_uring_params, zeroed: the kernel fills in what it set up.
		alignas(8) unsigned char params[120] = {};
		const long ring = ::syscall(__NR_io_uring_setup, 1u, params);
		if (ring >= 0) {
			::close(static_cast<int>(ring));
			return 5;
		}
		return refused(errno);
#else
		return 6;
#endif
	}
	if (name == "ptrace") {
		// Asks about the parent without attaching: a run that lets the call through gets ESRCH and attaches to nothing.
		errno = 0;
		::ptrace(PTRACE_PEEKUSER, ::getppid(), nullptr, nullptr);
		return errno == ESRCH ? 5 : refused(errno);
	}
	if (name == "fork") {
		const pid_t child = ::fork();
		if (child == 0) ::_exit(0);
		if (child < 0) return refused(errno);
		int status = 0;
		::waitpid(child, &status, 0);
		return 5;
	}
	return 6;
}
#endif

// The self-test probes of the sandbox: 0 when the system refused what the probe tried, 5 when it worked, 6 when it failed
// in a way that says nothing about the sandbox.
int self_test_probe(const arguments& args, const decoder_sandbox::status& applied)
{
	if (args.self_test_sandbox_report) {
		std::fputs((decoder_sandbox::report_line(applied) + "\n").c_str(), stdout);
		std::fflush(stdout);
		return 0;
	}
#if defined(__linux__)
	if (!args.self_test_call.empty()) return self_test_call(args.self_test_call, args.self_test_path);
	const auto refused = [](int error) { return error == EACCES || error == EPERM ? 0 : 6; };
	if (!args.self_test_open.empty()) {
		const int fd = ::open(args.self_test_open.c_str(), O_RDONLY | O_CLOEXEC);
		if (fd < 0) return refused(errno);
		::close(fd);
		return 5;
	}
	if (args.self_test_connect) {
		const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (fd < 0) return refused(errno);
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_port = htons(9);
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		const int connected = ::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
		const int error = errno;
		::close(fd);
		return connected != 0 && refused(error) == 0 ? 0 : 5;
	}
	if (!args.self_test_link_out.empty()) {
		return ::symlink(args.self_test_link_out.c_str(), "escape") == 0 ? 5 : refused(errno);
	}
	if (!args.self_test_hardlink.empty()) {
		if (::link(args.self_test_hardlink.c_str(), "escape_link") == 0) return 5;
		// Landlock refuses a link between hierarchies it does not both allow with EXDEV.
		return errno == EXDEV ? 0 : refused(errno);
	}
	if (args.self_test_signal) {
		const pid_t parent = ::getppid();
		const int directory = ::open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		const int flags = ::fcntl(STDOUT_FILENO, F_GETFL);
		if (directory < 0 || flags < 0) return 6;	// the probe needs these allowed to test anything
		bool any_worked = false;
		const auto tried = [&](int result) {
			if (result == 0) any_worked = true;
			else if (refused(errno) != 0) any_worked = true;	// an error other than a refusal: not shown refused
		};
		// The kernel's signals are aimed at this process and ignored: a run without the sandbox must not signal the test.
		std::signal(SIGIO, SIG_IGN);
		std::signal(SIGUSR1, SIG_IGN);
		tried(::kill(parent, 0));
		tried(::fcntl(STDOUT_FILENO, F_SETOWN, ::getpid()));
		tried(::fcntl(STDOUT_FILENO, F_SETSIG, SIGUSR1));
		tried(::fcntl(STDOUT_FILENO, F_SETFL, flags | O_ASYNC));
		tried(::fcntl(directory, F_NOTIFY, DN_MODIFY));
		::close(directory);
		return any_worked ? 5 : 0;
	}
	if (args.self_test_exec) {
		// A program that exits 5: an exec that worked is seen as such, not as a refusal.
		std::string shell = "/bin/sh";
		std::string flag = "-c";
		std::string command = "exit 5";
		char* const program_args[] = { shell.data(), flag.data(), command.data(), nullptr };
		char* const no_environment[] = { nullptr };
		::execve(shell.c_str(), program_args, no_environment);
		return refused(errno);
	}
#elif defined(_WIN32)
	// What the AppContainer, its policies and its job refuse (#1054) come back as access denied.
	const auto refused = [](DWORD error) { return error == ERROR_ACCESS_DENIED ? 0 : 6; };
	if (!args.self_test_open.empty()) {
		const HANDLE file = CreateFileW(args.self_test_open.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
		if (file == INVALID_HANDLE_VALUE) return refused(GetLastError());
		CloseHandle(file);
		return 5;
	}
	if (!args.self_test_link_out.empty()) {
		return plant_junction(args.self_test_link_out) ? 5 : refused(GetLastError());
	}
	if (!args.self_test_link_self.empty()) {
		if (turn_run_folder_into_junction(args.self_test_link_self)) {
			std::fputs("link-self: made\n", stderr);
		} else {
			std::fprintf(stderr, "link-self: refused: %lu\n", GetLastError());
		}
		return 0;
	}
	if (!args.self_test_hardlink.empty()) {
		return CreateHardLinkW(L"escape_link", args.self_test_hardlink.c_str(), nullptr) ? 5 : refused(GetLastError());
	}
	if (args.self_test_connect) {
		WSADATA wsa{};
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 6;
		const SOCKET socket_handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (socket_handle == INVALID_SOCKET) {
			const int error = WSAGetLastError();
			WSACleanup();
			return error == WSAEACCES ? 0 : 6;
		}
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_port = htons(args.self_test_connect_port != 0 ? args.self_test_connect_port : 9);
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		// Not blocking: an AppContainer's loopback connect may be dropped rather than refused, and a blocking connect then
		// waits out the system's 21 s timeout (#1144). A listener on the loopback answers at once, so 3 s is plenty.
		u_long non_blocking = 1;
		ioctlsocket(socket_handle, FIONBIO, &non_blocking);
		int connected = ::connect(socket_handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
		int error = connected == 0 ? 0 : WSAGetLastError();
		if (connected != 0 && error == WSAEWOULDBLOCK) {
			fd_set writable;
			fd_set failed;
			FD_ZERO(&writable);
			FD_ZERO(&failed);
			FD_SET(socket_handle, &writable);
			FD_SET(socket_handle, &failed);
			timeval wait{ 3, 0 };
			const int ready = ::select(0, nullptr, &writable, &failed, &wait);
			if (ready > 0 && FD_ISSET(socket_handle, &writable)) {
				connected = 0;
			} else if (ready > 0) {
				int socket_error = 0;
				int length = sizeof(socket_error);
				getsockopt(socket_handle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socket_error), &length);
				error = socket_error;
			} else {
				error = WSAETIMEDOUT;
			}
		}
		closesocket(socket_handle);
		WSACleanup();
		if (connected == 0) return 5;
		std::fprintf(stderr, "connect refused: %d\n", error);
		// With a listener there, any failure is the container's refusal; without one, only an access error is.
		return args.self_test_connect_port != 0 || error == WSAEACCES ? 0 : 5;
	}
	if (args.self_test_exec) {
		// cmd.exe that exits 5: a start that worked is seen as such, not as a refusal.
		wchar_t system_dir[MAX_PATH] = {};
		const UINT length = GetSystemDirectoryW(system_dir, MAX_PATH);
		if (length == 0 || length >= MAX_PATH) return 6;
		const std::wstring shell = std::wstring(system_dir) + L"\\cmd.exe";
		std::wstring command = L"cmd.exe /c exit 5";
		STARTUPINFOW startup{};
		startup.cb = sizeof(startup);
		PROCESS_INFORMATION started{};
		if (!CreateProcessW(shell.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &started)) {
			const DWORD error = GetLastError();
			std::fprintf(stderr, "start refused: %lu\n", error);
			// The child process policy refuses with ERROR_CHILD_PROCESS_BLOCKED (367, missing from older SDKs) or access
			// denied; the job's limit of one process with a quota error.
			constexpr DWORD k_child_process_blocked = 367;
			return error == k_child_process_blocked || error == ERROR_ACCESS_DENIED || error == ERROR_NOT_ENOUGH_QUOTA ? 0 : 6;
		}
		WaitForSingleObject(started.hProcess, INFINITE);
		CloseHandle(started.hThread);
		CloseHandle(started.hProcess);
		return 5;
	}
#endif
	return 5;
}
#endif

} // namespace

int main(int argc, char** argv)
{
#if defined(_WIN32)
	// DLLs only from System32 and this program's folder, never from the run folder the child writes (#1054).
	SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
#endif
	harden_process(parent_pid_argument(argc, argv));
	const auto args = parse_arguments(command_line(argc, argv));
	if (!args) {
		egLOG_ERROR("model_decoder", "usage: snk_model_decoder --request <bundle> --out <bundle>");
		return 2;
	}

#if defined(SNK_MODEL_DECODER_SELF_TEST)
	if (!args->self_test_spawn_pid_file.empty()) {
		// A helper for the test that kills its parent: starts a sleeping copy of this program, writes its process id, then
		// sleeps. The copy must not outlive this process. It is told its parent like the runner tells the decoder: a sanitizer
		// build starts slowly enough for the parent to die before the copy sets its death signal, and only the pid check
		// in harden_process sees that.
		const auto child = platform::spawn_detached(platform::current_executable(),
			{ "--self-test-sleep", "120000", "--parent-pid", std::to_string(platform::current_process_id()) });
		std::ofstream(args->self_test_spawn_pid_file) << child.process_id;
		std::this_thread::sleep_for(std::chrono::minutes(2));
		return 0;
	}
	if (args->self_test_sleep_ms != 0) {
		std::this_thread::sleep_for(std::chrono::milliseconds(args->self_test_sleep_ms));
		return 0;
	}
	if (args->self_test_extra_thread) {
		std::thread([] { std::this_thread::sleep_for(std::chrono::minutes(2)); }).detach();
	}
#endif

	// The sandbox before anything of the request is read (#1043): whatever the model makes the parser do, it does inside it.
	decoder_sandbox::settings sandbox_settings;
	sandbox_settings.mode = args->sandbox;
	sandbox_settings.max_output_bytes = args->max_output_bytes;
#if defined(SNK_MODEL_DECODER_SELF_TEST)
	sandbox_settings.pretend_no_landlock = args->self_test_pretend_no_landlock;
#endif
	const decoder_sandbox::status sandbox = decoder_sandbox::apply(sandbox_settings);
	if (!sandbox.refusal.empty()) {
		const std::string line = std::string(edt::k_decoder_sandbox_refusal_prefix) + sandbox.refusal + "\n";
		std::fputs(line.c_str(), stderr);
		std::fflush(stderr);
		return edt::k_decoder_sandbox_exit_code;
	}
	std::fputs((decoder_sandbox::status_line(sandbox) + "\n").c_str(), stderr);
	std::fflush(stderr);
#if defined(SNK_MODEL_DECODER_SELF_TEST)
	if (!args->self_test_open.empty() || args->self_test_connect || args->self_test_exec || args->self_test_sandbox_report
		|| args->self_test_signal || args->self_test_extra_thread || !args->self_test_link_out.empty()
		|| !args->self_test_call.empty() || !args->self_test_link_self.empty() || !args->self_test_hardlink.empty()) {
		return self_test_probe(*args, sandbox);
	}
#endif

	auto request_bundle = edt::read_bundle(args->request, {}, edt::bundle_folder::own);
	if (!request_bundle.ok()) {
		return fail("request: " + request_bundle.error);
	}
	const auto request = edt::decode_request_from_json(request_bundle.bundle->manifest);
	if (!request) {
		return fail("request: not a valid decode request");
	}
	if (!limit_address_space(request->memory_limit_bytes)) {
		return fail("the memory limit could not be set");
	}

#if defined(SNK_MODEL_DECODER_SELF_TEST)
	if (args->self_test_alloc_bytes != 0) {
		std::vector<char> hog;
		try {
			hog.assign(static_cast<std::size_t>(args->self_test_alloc_bytes), 1);
		} catch (const std::bad_alloc&) {
			return 3;
		}
		return hog.empty() ? 1 : 0;
	}
#endif

	const std::vector<std::byte>* source = nullptr;
	std::map<std::size_t, std::vector<std::byte>*> file_bytes;
	for (edt::bundle_blob& blob : request_bundle.bundle->blobs) {
		if (blob.tag == edt::k_decode_source_tag) {
			source = &blob.bytes;
			continue;
		}
		const std::size_t index = provided_index(blob.tag);
		if (index > UINT32_MAX || blob.tag != edt::provided_blob_tag(index)) {
			return fail("request: it carries bytes that belong to no file of the request");
		}
		file_bytes[index] = &blob.bytes;
	}
	if (!source) {
		return fail("request: the model bytes are missing");
	}
	// Every name of one file shares its one blob: the bytes are held once, however many names the model used.
	std::map<std::size_t, std::shared_ptr<const std::vector<std::byte>>> shared_bytes;
	std::vector<edt::provided_file> files;
	files.reserve(request->provided.size());
	for (const edt::provided_entry& entry : request->provided) {
		std::shared_ptr<const std::vector<std::byte>> bytes;
		if (entry.present) {
			const auto found = file_bytes.find(entry.blob);
			if (found == file_bytes.end()) {
				return fail("request: a file of the request names bytes it does not carry");
			}
			std::shared_ptr<const std::vector<std::byte>>& shared = shared_bytes[entry.blob];
			if (!shared) shared = std::make_shared<const std::vector<std::byte>>(std::move(*found->second));
			bytes = shared;
		}
		files.push_back({ entry.name, entry.relative, entry.kind, std::move(bytes) });
	}
	if (shared_bytes.size() != file_bytes.size()) {
		return fail("request: it carries bytes that belong to no file of the request");
	}

	edt::import_source import;
	import.path = request->source_path;
	import.preloaded = source;
	import.limits = request->limits;
	import.boundary = request->boundary;
	import.memory_folder = request->memory_folder;
	import.on_disk = request->on_disk;
	import.settings = request->settings;
	import.provided = &files;
	if (request->deadline_ms != 0) {
		import.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(request->deadline_ms);
		import.time_limit = std::chrono::milliseconds(request->time_limit_ms);
	}

	edt::decode_bundle out;
	try {
		if (request->mode == edt::decode_request::mode_kind::scan) {
			out.manifest = edt::to_json(edt::list_model_requests(import));
			out.manifest["kind"] = "scan";
		} else {
			// An agent's import: nothing was there before it, so everything it stored is its own (created_tags).
			memory_sink sink;
			edt::import_intermediate imported = edt::import_model_into(import, sink);
			if (imported.error.empty()) {
				for (const auto& [tag, bytes] : sink.stored()) {
					imported.created_tags.push_back(tag);
					out.blobs.push_back({ tag.string(), bytes });
				}
			}
			out.manifest["kind"] = "import";
			out.manifest["import"] = edt::to_json(imported);
		}
	} catch (const std::exception& e) {
		out = {};
		out.manifest = failure_manifest(request->mode == edt::decode_request::mode_kind::scan ? "scan" : "import", e.what());
	}

	std::string error;
	if (!edt::write_bundle(args->out, out, error)) {
		return fail("out: " + error);
	}
	return 0;
}
