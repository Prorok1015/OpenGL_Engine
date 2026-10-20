// Windows: the system headers are included here only, and must not define min/max.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <snk/mcp/mcp_connection_file.h>

#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/string.hpp>
#include <boost/json/value.hpp>
#include <boost/system/error_code.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {
	constexpr const char* file_name = "mcp.json";
	constexpr std::size_t max_file_bytes = 4096;

	std::string connection_json(const mcp::connection_info& info)
	{
		boost::json::object object;
		object["url"] = boost::json::string(std::string_view(info.url()));
		object["port"] = static_cast<std::int64_t>(info.port);
		object["token"] = boost::json::string(std::string_view(info.token));
		object["pid"] = info.pid;
		return boost::json::serialize(object) + "\n";
	}

	// A name nobody can guess, so the exclusive create below cannot be pre-empted by a planted file.
	std::string temp_name()
	{
		return std::string(file_name) + "." + mcp::generate_token().substr(0, 16) + ".tmp";
	}

#ifdef _WIN32
	std::string last_error_text(const char* what)
	{
		return std::string(what) + " (error " + std::to_string(static_cast<unsigned long>(GetLastError())) + ")";
	}

	bool is_reparse_point(const fs::path& path)
	{
		const DWORD attributes = GetFileAttributesW(path.c_str());
		return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
	}

	// The file as it is, if it is a plain file: opened without following a link, and a pipe, a device or a directory is
	// not read (reading one could block for ever).
	bool read_small_file(const fs::path& directory, std::string& out)
	{
		const fs::path path = directory / file_name;
		HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		if (file == INVALID_HANDLE_VALUE) {
			return false;
		}
		BY_HANDLE_FILE_INFORMATION information;
		bool ok = GetFileType(file) == FILE_TYPE_DISK && GetFileInformationByHandle(file, &information)
			&& (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
		if (ok) {
			out.assign(max_file_bytes, '\0');
			DWORD got = 0;
			ok = ReadFile(file, out.data(), static_cast<DWORD>(max_file_bytes), &got, nullptr) != 0;
			out.resize(ok ? static_cast<std::size_t>(got) : 0);
		}
		CloseHandle(file);
		return ok;
	}

	// Creates a file only the current user can open: the DACL is part of the creation, so the token is never in a
	// file that others could read, and SE_DACL_PROTECTED keeps the folder's inheritable entries out.
	std::string write_atomic(const fs::path& directory, const std::string& text)
	{
		HANDLE process_token = nullptr;
		if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &process_token)) {
			return last_error_text("cannot read the identity of the current user");
		}
		DWORD size = 0;
		GetTokenInformation(process_token, TokenUser, nullptr, 0, &size);
		std::vector<unsigned char> user_buffer(size);
		const BOOL got_user = size > 0 && GetTokenInformation(process_token, TokenUser, user_buffer.data(), size, &size);
		CloseHandle(process_token);
		if (!got_user) {
			return last_error_text("cannot read the identity of the current user");
		}
		PSID user_sid = reinterpret_cast<PTOKEN_USER>(user_buffer.data())->User.Sid;

		EXPLICIT_ACCESS_W access;
		ZeroMemory(&access, sizeof(access));
		access.grfAccessPermissions = FILE_ALL_ACCESS;
		access.grfAccessMode = SET_ACCESS;
		access.grfInheritance = NO_INHERITANCE;
		access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
		access.Trustee.TrusteeType = TRUSTEE_IS_USER;
		access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(user_sid);
		PACL acl = nullptr;
		if (SetEntriesInAclW(1, &access, nullptr, &acl) != ERROR_SUCCESS) {
			return "cannot build an owner-only access list for the token file";
		}

		std::string failure;
		SECURITY_DESCRIPTOR descriptor;
		if (!InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION)
			|| !SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE)
			|| !SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) {
			failure = last_error_text("cannot build an owner-only security descriptor for the token file");
		} else {
			SECURITY_ATTRIBUTES attributes;
			attributes.nLength = sizeof(attributes);
			attributes.lpSecurityDescriptor = &descriptor;
			attributes.bInheritHandle = FALSE;
			const fs::path temp = directory / temp_name();
			const fs::path target = directory / file_name;
			HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, &attributes, CREATE_NEW,
				FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
			if (file == INVALID_HANDLE_VALUE) {
				failure = last_error_text("cannot create the token file");
			} else {
				DWORD written = 0;
				const BOOL wrote = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
				CloseHandle(file);
				if (!wrote || static_cast<std::size_t>(written) != text.size()) {
					failure = last_error_text("cannot write the token file");
				} else if (!MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
					failure = last_error_text("cannot replace the token file");
				}
				if (!failure.empty()) {
					DeleteFileW(temp.c_str());
				}
			}
		}
		LocalFree(acl);
		return failure;
	}
#else
	// The directory, opened without following a link, and only if it belongs to the current user: a directory of
	// someone else is not a place for the token, and its files are not to be trusted. -1 otherwise.
	int open_owned_directory(const fs::path& directory)
	{
		const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (fd < 0) {
			return -1;
		}
		struct stat information;
		if (::fstat(fd, &information) != 0 || information.st_uid != ::geteuid()) {
			::close(fd);
			return -1;
		}
		return fd;
	}

	// The file as it is, if it is a plain file. O_NOFOLLOW refuses a link and O_NONBLOCK keeps the open of a FIFO from
	// waiting for a writer; the type is checked on the descriptor, and a FIFO, a device or a directory is never read.
	bool read_small_file(const fs::path& directory, std::string& out)
	{
		const int dir_fd = open_owned_directory(directory);
		if (dir_fd < 0) {
			return false;
		}
		const int fd = ::openat(dir_fd, file_name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
		::close(dir_fd);
		if (fd < 0) {
			return false;
		}
		struct stat information;
		const bool regular = ::fstat(fd, &information) == 0 && S_ISREG(information.st_mode);
		if (regular) {
			out.assign(max_file_bytes, '\0');
			std::size_t total = 0;
			while (total < max_file_bytes) {
				const ssize_t n = ::read(fd, out.data() + total, max_file_bytes - total);
				if (n <= 0) {
					break;
				}
				total += static_cast<std::size_t>(n);
			}
			out.resize(total);
		}
		::close(fd);
		return regular;
	}

	// Opened relative to the directory's descriptor: the directory is the one that was checked (O_NOFOLLOW refuses a
	// link), the file is created exclusively and without following links, and rename() puts it in place atomically.
	std::string write_atomic(const fs::path& directory, const std::string& text)
	{
		const int dir_fd = open_owned_directory(directory);
		if (dir_fd < 0) {
			return "cannot use " + directory.string() + " (it is a link, not a directory, or not yours)";
		}
		const std::string temp = temp_name();
		const int fd = ::openat(dir_fd, temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR);
		std::string failure;
		if (fd < 0) {
			failure = "cannot create a file in " + directory.string();
		} else {
			if (::fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
				failure = "cannot restrict the permissions of the token file";
			}
			std::size_t written = 0;
			while (failure.empty() && written < text.size()) {
				const ssize_t n = ::write(fd, text.data() + written, text.size() - written);
				if (n <= 0) {
					failure = "cannot write the token file";
				} else {
					written += static_cast<std::size_t>(n);
				}
			}
			::close(fd);
			if (failure.empty() && ::renameat(dir_fd, temp.c_str(), dir_fd, file_name) != 0) {
				failure = "cannot replace " + (directory / file_name).string();
			}
			if (!failure.empty()) {
				::unlinkat(dir_fd, temp.c_str(), 0);
			}
		}
		::close(dir_fd);
		return failure;
	}
#endif
}

std::string mcp::generate_token()
{
	static constexpr char digits[] = "0123456789abcdef";
	std::random_device source;
	std::string token;
	token.reserve(64);
	for (int word = 0; word < 8; ++word) {
		std::uint32_t value = static_cast<std::uint32_t>(source());
		for (int nibble = 0; nibble < 8; ++nibble) {
			token.push_back(digits[value & 0xFu]);
			value >>= 4;
		}
	}
	return token;
}

std::int64_t mcp::current_process_id()
{
#ifdef _WIN32
	return static_cast<std::int64_t>(GetCurrentProcessId());
#else
	return static_cast<std::int64_t>(::getpid());
#endif
}

bool mcp::process_is_running(std::int64_t pid)
{
	if (pid <= 0) {
		return false;
	}
#ifdef _WIN32
	HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
	if (process == nullptr) {
		// Running, but not ours to look at.
		return GetLastError() == ERROR_ACCESS_DENIED;
	}
	DWORD code = 0;
	const bool running = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
	CloseHandle(process);
	return running;
#else
	return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

std::string mcp::connection_info::url() const
{
	return "http://127.0.0.1:" + std::to_string(port) + "/mcp";
}

fs::path mcp::connection_file_path(const fs::path& directory)
{
	return directory / file_name;
}

std::optional<mcp::connection_info> mcp::read_connection_file(const fs::path& directory)
{
	std::string text;
	if (!read_small_file(directory, text)) {
		return std::nullopt;
	}

	boost::system::error_code ec;
	const boost::json::value parsed = boost::json::parse(text, ec);
	if (ec || !parsed.is_object()) {
		return std::nullopt;
	}
	const boost::json::object& object = parsed.get_object();
	const boost::json::value* port = object.if_contains("port");
	const boost::json::value* token = object.if_contains("token");
	const boost::json::value* pid = object.if_contains("pid");
	if (port == nullptr || !port->is_int64() || token == nullptr || !token->is_string()) {
		return std::nullopt;
	}
	connection_info info;
	info.port = static_cast<std::uint16_t>(port->get_int64());
	info.token = std::string(token->get_string().data(), token->get_string().size());
	info.pid = (pid != nullptr && pid->is_int64()) ? pid->get_int64() : 0;
	return info;
}

std::string mcp::write_connection_file(const fs::path& directory, const connection_info& info)
{
	std::error_code ec;
	const fs::file_status status = fs::symlink_status(directory, ec);
	if (!ec && fs::is_symlink(status)) {
		return "refusing to write the token into " + directory.string() + ": it is a symbolic link";
	}
#ifdef _WIN32
	if (is_reparse_point(directory)) {
		return "refusing to write the token into " + directory.string() + ": it is a link";
	}
#endif
	ec.clear();
	const bool created = fs::create_directories(directory, ec);
	if (ec) {
		return "cannot create " + directory.string() + ": " + ec.message();
	}
#ifndef _WIN32
	if (created) {
		fs::permissions(directory, fs::perms::owner_all, fs::perm_options::replace, ec);
	}
#else
	(void)created;
#endif

	// Another editor that is still running owns the file; a dead one's, or our own, is replaced.
	if (const std::optional<connection_info> existing = read_connection_file(directory)) {
		if (existing->pid > 0 && existing->pid != info.pid && process_is_running(existing->pid)) {
			return "another editor (process " + std::to_string(existing->pid) + ") already serves this project: "
				+ connection_file_path(directory).string();
		}
	}
	return write_atomic(directory, connection_json(info));
}

void mcp::remove_connection_file(const fs::path& directory, const connection_info& mine)
{
	const std::optional<connection_info> existing = read_connection_file(directory);
	if (!existing || existing->token != mine.token || existing->pid != mine.pid) {
		return;
	}
#ifdef _WIN32
	std::error_code ec;
	fs::remove(connection_file_path(directory), ec);
#else
	const int dir_fd = open_owned_directory(directory);
	if (dir_fd >= 0) {
		::unlinkat(dir_fd, file_name, 0);
		::close(dir_fd);
	}
#endif
}
