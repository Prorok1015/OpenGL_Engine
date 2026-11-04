#include "edt_curl_transport.h"
#include "edt_path_text.h"
#include <snk/common/logger/engine_log.h>
#include <snk/config/cfg_variables.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <exception>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sddl.h>
#else
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace {

	CFG_VAR_DEF_PATH(cfg_forge_curl, "forge/curl", "");
	CFG_VAR_DEF_STRING_LIST(cfg_forge_hosts, "editor/forge_hosts", "api.github.com");

	// curl writes this on a line of its own after the body.
	constexpr std::string_view k_status_marker = "\n@@snk-http-status@@";
	constexpr std::size_t k_stderr_keep = 4096;
	constexpr std::size_t k_stderr_shown = 240;

	std::string lower(std::string_view text)
	{
		std::string out(text);
		std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return out;
	}

	bool has_control(std::string_view text)
	{
		return std::any_of(text.begin(), text.end(), [](char c) {
			const auto byte = static_cast<unsigned char>(c);
			return byte < 0x20 || byte == 0x7F;
		});
	}

	// volatile, so the compiler cannot drop the stores of a buffer that is about to be freed.
	void wipe_string(std::string& value) noexcept
	{
		volatile char* p = value.data();
		for (std::size_t i = 0; i < value.capacity(); ++i) {
			p[i] = 0;
		}
		value.clear();
	}

	struct wipe_on_exit
	{
		std::string& text;
		explicit wipe_on_exit(std::string& value) : text(value) {}
		wipe_on_exit(const wipe_on_exit&) = delete;
		wipe_on_exit& operator=(const wipe_on_exit&) = delete;
		~wipe_on_exit() { wipe_string(text); }
	};

	// Appends the value inside the double quotes of a curl config option; no temporary copy of the value is made, so a
	// token never lives in a buffer that is not wiped.
	void add_option(std::string& config, std::string_view name, std::string_view value)
	{
		config += name;
		config += " = \"";
		for (const char c : value) {
			if (c == '\\' || c == '"') config += '\\';
			config += c;
		}
		config += "\"\n";
	}

	edt::forge::http_response failure(edt::forge::forge_error_code code, std::string_view text)
	{
		edt::forge::http_response response;
		response.error_code = code;
		response.error_text = edt::forge::make_forge_error(code, text).message;
		return response;
	}

	bool is_header_name(std::string_view name)
	{
		return !name.empty() && std::all_of(name.begin(), name.end(), [](char c) {
			return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_';
		});
	}

	// Everything that must hold before a file is written or a program started. Empty: fine.
	std::optional<edt::forge::http_response> refuse(const edt::forge::http_request& request,
		const std::vector<std::string>& allowed_hosts)
	{
		using edt::forge::forge_error_code;
		static constexpr std::string_view methods[] = { "GET", "POST" };
		if (std::find(std::begin(methods), std::end(methods), request.method) == std::end(methods)) {
			return failure(forge_error_code::validation, "unsupported HTTP method");
		}
		if (has_control(request.url)) return failure(forge_error_code::validation, "the URL has a control character");
		const auto host = edt::forge::https_url_host(request.url);
		if (!host) return failure(forge_error_code::host_not_allowed, "only https URLs without a user name or a port are allowed");
		const bool allowed = std::any_of(allowed_hosts.begin(), allowed_hosts.end(), [&](const std::string& entry) {
			return lower(entry) == *host;
		});
		if (!allowed) return failure(forge_error_code::host_not_allowed, std::format("host '{}' is not allowed (editor/forge_hosts)", *host));
		for (const auto& [name, value] : request.headers) {
			if (!is_header_name(name) || has_control(value)) return failure(forge_error_code::validation, "a header is not valid");
			if (lower(name) == "authorization") return failure(forge_error_code::validation, "the token travels as bearer_token only");
		}
		if (request.bearer_token != nullptr) {
			const std::string& token = request.bearer_token->reveal();
			if (token.empty()) return failure(forge_error_code::unauthorized, "no token");
			const bool printable = std::all_of(token.begin(), token.end(), [](char c) {
				const auto byte = static_cast<unsigned char>(c);
				return byte > 0x20 && byte < 0x7F;
			});
			if (!printable) return failure(forge_error_code::unauthorized, "the token has a space or a control character");
		}
		return std::nullopt;
	}

#ifdef _WIN32
	std::wstring current_user_sid()
	{
		HANDLE token = nullptr;
		if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
		DWORD size = 0;
		GetTokenInformation(token, TokenUser, nullptr, 0, &size);
		std::wstring out;
		if (size > 0) {
			std::vector<unsigned char> buffer(size);
			if (GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
				wchar_t* text = nullptr;
				if (ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER*>(buffer.data())->User.Sid, &text)) {
					out = text;
					LocalFree(text);
				}
			}
		}
		CloseHandle(token);
		return out;
	}

	// A security descriptor with one ACE (full control for the current user) and a protected DACL: nothing is inherited
	// from a shared temp folder, nobody else (not even other members of the user's groups) gets in.
	class owner_only_access
	{
	public:
		explicit owner_only_access(bool inherit_to_children)
		{
			const std::wstring sid = current_user_sid();
			if (sid.empty()) return;
			const std::wstring sddl = std::format(L"D:P(A;{};FA;;;{})", inherit_to_children ? L"OICI" : L"", sid);
			if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &m_descriptor, nullptr)) {
				m_descriptor = nullptr;
				return;
			}
			m_attributes.nLength = sizeof(m_attributes);
			m_attributes.lpSecurityDescriptor = m_descriptor;
			m_attributes.bInheritHandle = FALSE;
		}
		owner_only_access(const owner_only_access&) = delete;
		owner_only_access& operator=(const owner_only_access&) = delete;
		~owner_only_access()
		{
			if (m_descriptor != nullptr) LocalFree(m_descriptor);
		}

		bool ok() const { return m_descriptor != nullptr; }
		SECURITY_ATTRIBUTES* attributes() { return &m_attributes; }

	private:
		PSECURITY_DESCRIPTOR m_descriptor = nullptr;
		SECURITY_ATTRIBUTES m_attributes{};
	};
#endif

	std::string write_private_file(const fs::path& path, std::string_view content)
	{
#ifdef _WIN32
		owner_only_access access{ false };
		if (!access.ok()) return "cannot make an owner-only access list for a private file for curl";
		// Not shared and never opened over an existing file; written through the handle, so no stream buffer keeps a copy.
		const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, access.attributes(), CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) return "cannot create a private file for curl";
		bool good = true;
		std::size_t done = 0;
		while (good && done < content.size()) {
			DWORD written = 0;
			const auto chunk = static_cast<DWORD>(std::min<std::size_t>(content.size() - done, 1u << 20));
			good = WriteFile(file, content.data() + done, chunk, &written, nullptr) != FALSE && written > 0;
			done += written;
		}
		good = FlushFileBuffers(file) != FALSE && good;
		CloseHandle(file);
		if (!good) return "cannot write a private file for curl";
		return {};
#else
		const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
		if (fd < 0) return std::format("cannot create a private file for curl: {}", std::strerror(errno));
		bool good = ::fchmod(fd, 0600) == 0;
		std::size_t done = 0;
		while (good && done < content.size()) {
			const ssize_t written = ::write(fd, content.data() + done, content.size() - done);
			if (written < 0) {
				if (errno == EINTR) continue;
				good = false;
				break;
			}
			done += static_cast<std::size_t>(written);
		}
		::close(fd);
		if (!good) return "cannot write a private file for curl";
		return {};
#endif
	}

	// Zeros over the old content, then gone. Never throws.
	void wipe_file(const fs::path& path) noexcept
	{
		try {
			std::error_code ec;
			const auto size = fs::file_size(path, ec);
			if (ec || size == 0) return;
			std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
			if (!file) return;
			const std::string zeros(static_cast<std::size_t>(std::min<std::uintmax_t>(size, 1u << 20)), '\0');
			for (std::uintmax_t left = size; left > 0 && file;) {
				const auto chunk = static_cast<std::size_t>(std::min<std::uintmax_t>(left, zeros.size()));
				file.write(zeros.data(), static_cast<std::streamsize>(chunk));
				left -= chunk;
			}
			file.flush();
		} catch (...) {
		}
	}

	// Removes the file when it goes out of scope: normal end, exception or cancel.
	struct file_guard
	{
		fs::path path;
		bool wipe = false;

		file_guard() = default;
		file_guard(const file_guard&) = delete;
		file_guard& operator=(const file_guard&) = delete;
		~file_guard()
		{
			if (path.empty()) return;
			if (wipe) wipe_file(path);
			std::error_code ec;
			fs::remove(path, ec);
		}
	};

	// A folder only the current user can enter, made for one request and removed with it (when owned). A folder from
	// curl_options::scratch_dir is the caller's: never removed here.
	struct request_folder
	{
		fs::path path;
		bool owned = false;

		request_folder() = default;
		request_folder(const request_folder&) = delete;
		request_folder& operator=(const request_folder&) = delete;
		~request_folder()
		{
			if (!owned || path.empty()) return;
			std::error_code ec;
			fs::remove_all(path, ec);
		}
	};

#ifndef _WIN32
	// $XDG_RUNTIME_DIR when it is a directory of ours that nobody else can write into; else the OS temp folder.
	fs::path posix_scratch_base()
	{
		const char* runtime = std::getenv("XDG_RUNTIME_DIR");
		if (runtime != nullptr && runtime[0] == '/') {
			struct stat info{};
			if (::stat(runtime, &info) == 0 && S_ISDIR(info.st_mode) && info.st_uid == ::geteuid()
				&& (info.st_mode & 0022) == 0 && ::access(runtime, W_OK | X_OK) == 0) {
				return runtime;
			}
		}
		std::error_code ec;
		const fs::path temp = fs::temp_directory_path(ec);
		return ec ? fs::path{} : temp;
	}
#endif

	// Empty: it cannot be made. mkdtemp always gives 0700 and never reuses a name, so nothing planted in a shared
	// /tmp can be picked up.
	fs::path make_request_folder()
	{
#ifdef _WIN32
		std::error_code ec;
		const fs::path temp = fs::temp_directory_path(ec);
		if (ec) return {};
		owner_only_access access{ true };
		if (!access.ok()) return {};
		std::random_device entropy;
		for (int attempt = 0; attempt < 16; ++attempt) {
			const std::uint64_t tag = (std::uint64_t{ entropy() } << 32) ^ std::uint64_t{ entropy() }
				^ static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
			const fs::path root = temp / std::format("snk_editor_forge_{:016x}", tag);
			if (CreateDirectoryW(root.c_str(), access.attributes())) return root;
			if (GetLastError() != ERROR_ALREADY_EXISTS) return {};
		}
		return {};
#else
		const fs::path base = posix_scratch_base();
		if (base.empty()) return {};
		std::string pattern = (base / "snk_forge_XXXXXX").string();
		if (::mkdtemp(pattern.data()) == nullptr) return {};
		return fs::path{ pattern };
#endif
	}

	std::vector<std::string> own_environment()
	{
		std::vector<std::string> out;
#ifdef _WIN32
		wchar_t* block = GetEnvironmentStringsW();
		if (block == nullptr) return out;
		for (const wchar_t* entry = block; *entry != L'\0'; entry += std::wstring_view{ entry }.size() + 1) {
			out.push_back(edt::utf8_of(fs::path{ std::wstring_view{ entry } }));
		}
		FreeEnvironmentStringsW(block);
#else
		for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) out.emplace_back(*entry);
#endif
		return out;
	}

	// Compared without case on every OS, like the environment of Windows.
	bool is_dropped_variable(const std::string& entry)
	{
		constexpr std::string_view names[] = { "curl_home=", "sslkeylogfile=" };
		return std::any_of(std::begin(names), std::end(names), [&](std::string_view name) {
			return entry.size() >= name.size() && lower(std::string_view(entry).substr(0, name.size())) == name;
		});
	}

	// plt_process words a spawn failure as "cannot find '<program>' in PATH" (the search found nothing) or as
	// "cannot start '<program>': <system message>"; only "no such file" messages of the system count as not found.
	bool is_not_found_error(std::string_view error)
	{
		if (error.starts_with("cannot find '")) return true;
		return error.find("No such file or directory") != std::string_view::npos
			|| error.find("cannot find the file specified") != std::string_view::npos
			|| error.find("cannot find the path specified") != std::string_view::npos;
	}

	std::string file_name_in(const char* prefix)
	{
		static std::atomic<std::uint64_t> counter{ 0 };
		std::random_device entropy;
		return std::format("{}_{:08x}{:08x}_{}", prefix, entropy(), entropy(), ++counter);
	}

	struct curl_state
	{
		edt::forge::curl_options options;
	};

	edt::forge::http_response perform(const curl_state& state, const edt::forge::http_request& request, std::stop_token stop)
	{
		using edt::forge::forge_error_code;
		const edt::forge::curl_options& options = state.options;

		if (auto refused = refuse(request, options.allowed_hosts)) return std::move(*refused);
		if (stop.stop_requested()) return failure(forge_error_code::network, "the request was cancelled");

		request_folder folder;
		if (options.scratch_dir) {
			folder.path = options.scratch_dir();
#ifndef _WIN32
			// Only what can be told cheaply: a link or a file in its place is refused. Its rights are the caller's.
			struct stat info{};
			if (!folder.path.empty() && (::lstat(folder.path.c_str(), &info) != 0 || !S_ISDIR(info.st_mode))) folder.path.clear();
#endif
		} else {
			folder.path = make_request_folder();
			folder.owned = !folder.path.empty();
		}
		const fs::path& scratch = folder.path;
		if (scratch.empty()) return failure(forge_error_code::unavailable, "no private temp folder for the request files");

		file_guard config_file;
		file_guard body_file;
		config_file.path = scratch / file_name_in("curl_cfg");
		config_file.wipe = true;

		// Reserved for the worst case (every character escaped), so the buffer is never reallocated and no stale copy of
		// the token is left behind; wiped on every path.
		std::size_t estimate = 1024 + 2 * (request.url.size() + request.method.size());
		for (const auto& [name, value] : request.headers) estimate += 2 * (name.size() + value.size()) + 32;
		if (request.bearer_token != nullptr) estimate += 2 * request.bearer_token->reveal().size() + 64;
		std::string config;
		config.reserve(estimate);
		const wipe_on_exit config_wipe{ config };
		add_option(config, "url", request.url);
		add_option(config, "request", request.method);
		for (const auto& [name, value] : request.headers) add_option(config, "header", name + ": " + value);
		if (request.bearer_token != nullptr) {
			std::string authorization;
			authorization.reserve(request.bearer_token->reveal().size() + 32);
			const wipe_on_exit authorization_wipe{ authorization };
			authorization += "Authorization: Bearer ";
			authorization += request.bearer_token->reveal();
			add_option(config, "header", authorization);
		}
		if (!request.body.empty()) {
			body_file.path = scratch / file_name_in("curl_body");
			if (auto error = write_private_file(body_file.path, request.body); !error.empty()) {
				return failure(forge_error_code::unavailable, error);
			}
			add_option(config, "data-binary", "@" + edt::utf8_of(body_file.path));
		}
		if (auto error = write_private_file(config_file.path, config); !error.empty()) {
			return failure(forge_error_code::unavailable, error);
		}
		wipe_string(config);

		const std::vector<std::string> args = {
			"-q",
			"-K", edt::utf8_of(config_file.path),
			"--proto", "=https",
			"--proto-redir", "=https",
			"--max-redirs", "0",
			"--max-time", std::to_string(std::max(1, options.max_time_seconds)),
			"--silent", "--show-error",
			"--globoff",
			"--tlsv1.2",
			"--max-filesize", std::to_string(options.max_response_bytes),
			"--write-out", std::string(k_status_marker) + "%{http_code}\n",
		};

		platform::process_options process;
		process.working_dir = scratch;
		process.raw_output = true;
		std::vector<std::string> environment = options.environment ? *options.environment : own_environment();
		std::erase_if(environment, is_dropped_variable);
		process.environment = std::move(environment);
		std::string error_tail;
		process.on_stderr_line = [&error_tail](std::string_view line) {
			error_tail.append(line);
			error_tail += '\n';
			if (error_tail.size() > k_stderr_keep) error_tail.erase(0, error_tail.size() - k_stderr_keep);
		};

		std::stop_source own_stop;
		const std::stop_callback forward{ stop, [&own_stop] { own_stop.request_stop(); } };
		// The outer deadline: --max-time bounds curl itself, this bounds a curl that hangs past it.
		const auto deadline = std::chrono::seconds(std::max(1, options.max_time_seconds)) + std::max(options.deadline_grace, std::chrono::seconds(0));
		std::atomic<bool> timed_out{ false };
		std::mutex watchdog_mutex;
		std::condition_variable_any watchdog_wake;
		std::string output;
		bool too_large = false;
		const auto on_output = [&](std::string_view chunk) {
			if (too_large) return;
			output.append(chunk);
			if (output.size() > options.max_response_bytes + k_status_marker.size() + 16) {
				too_large = true;
				own_stop.request_stop();
			}
		};

		const fs::path program = options.curl.empty()
#ifdef _WIN32
			? fs::path("curl.exe")
#else
			? fs::path("curl")
#endif
			: options.curl;

		platform::process_result result;
		std::jthread watchdog{ [&](std::stop_token done) {
			std::unique_lock lock{ watchdog_mutex };
			watchdog_wake.wait_for(lock, done, deadline, [] { return false; });
			if (done.stop_requested()) return;
			timed_out = true;
			own_stop.request_stop();
		} };
		try {
			result = options.run(program, args, process, on_output, own_stop.get_token());
		} catch (const std::exception& e) {
			return failure(forge_error_code::network, std::format("curl could not be run: {}", e.what()));
		} catch (...) {
			return failure(forge_error_code::network, "curl could not be run");
		}

		watchdog.request_stop();
		watchdog.join();

		if (too_large) return failure(forge_error_code::invalid_response, "the response is too large");
		if (timed_out) {
			return failure(forge_error_code::network, std::format("the request timed out after {} s (curl did not end)", options.max_time_seconds));
		}
		if (!result.error.empty()) {
			if (is_not_found_error(result.error)) {
				return failure(forge_error_code::unavailable, "curl was not found (install curl or set forge/curl)");
			}
			return failure(forge_error_code::unavailable, std::format("curl could not be started: {}", edt::forge::sanitize_text(result.error, 200)));
		}
		if (result.cancelled) return failure(forge_error_code::network, "the request was cancelled");

		if (result.exit_code != 0) {
			if (result.exit_code == 28) {
				return failure(forge_error_code::network, std::format("the request timed out after {} s", options.max_time_seconds));
			}
			if (result.exit_code == 63) return failure(forge_error_code::invalid_response, "the response is too large");
			if (request.bearer_token != nullptr) edt::forge::scrub_secret(error_tail, *request.bearer_token);
			while (!error_tail.empty() && std::isspace(static_cast<unsigned char>(error_tail.back())) != 0) error_tail.pop_back();
			if (error_tail.size() > k_stderr_shown) error_tail.erase(0, error_tail.size() - k_stderr_shown);
			return failure(forge_error_code::network, std::format("curl failed (exit {}): {}", result.exit_code, error_tail));
		}

		const std::size_t marker = output.rfind(k_status_marker);
		if (marker == std::string::npos) return failure(forge_error_code::invalid_response, "curl gave no HTTP status");
		std::string_view status_text = std::string_view(output).substr(marker + k_status_marker.size());
		while (!status_text.empty() && std::isspace(static_cast<unsigned char>(status_text.back())) != 0) status_text.remove_suffix(1);
		int status = 0;
		const auto parsed = std::from_chars(status_text.data(), status_text.data() + status_text.size(), status);
		if (parsed.ec != std::errc{} || parsed.ptr != status_text.data() + status_text.size() || status < 100 || status > 599) {
			return failure(forge_error_code::invalid_response, "curl gave an unreadable HTTP status");
		}
		output.resize(marker);
		if (output.size() > options.max_response_bytes) return failure(forge_error_code::invalid_response, "the response is too large");
		if (status >= 300 && status < 400) {
			return failure(forge_error_code::network, std::format("the server answered with a redirect (HTTP {}); redirects are not followed", status));
		}

		egLOG_DEBUG("editor/forge", "{} {} -> HTTP {}", request.method, edt::forge::https_url_host(request.url).value_or(""), status);
		edt::forge::http_response response;
		response.status = status;
		response.body = std::move(output);
		return response;
	}

}

edt::forge::curl_options edt::forge::configured_curl_options()
{
	curl_options options;
	options.curl = *cfg_forge_curl;
	options.allowed_hosts = *cfg_forge_hosts;
	return options;
}

edt::forge::http_transport edt::forge::make_curl_transport(curl_options options)
{
	auto state = std::make_shared<curl_state>();
	state->options = std::move(options);
	return [state](const http_request& request, std::stop_token stop) {
		return perform(*state, request, std::move(stop));
	};
}
