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
#include <cstdint>
#include <cstring>
#include <exception>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
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

	// Inside the double quotes of a curl config value.
	std::string config_escape(std::string_view text)
	{
		std::string out;
		out.reserve(text.size() + 8);
		for (const char c : text) {
			if (c == '\\' || c == '"') out += '\\';
			out += c;
		}
		return out;
	}

	void add_option(std::string& config, std::string_view name, std::string_view value)
	{
		config += name;
		config += " = \"";
		config += config_escape(value);
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
		static constexpr std::string_view methods[] = { "GET", "POST", "PATCH", "PUT", "DELETE", "HEAD" };
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

	std::string write_private_file(const fs::path& path, std::string_view content)
	{
#ifdef _WIN32
		// The folder is private to the user (a fresh one under the user's temp folder).
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		file.write(content.data(), static_cast<std::streamsize>(content.size()));
		file.flush();
		if (!file) return "cannot write a private file for curl";
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

	// The private folder under the OS temp folder, made on first use.
	class scratch_folder
	{
	public:
		scratch_folder() = default;
		scratch_folder(const scratch_folder&) = delete;
		scratch_folder& operator=(const scratch_folder&) = delete;
		~scratch_folder()
		{
			if (m_root.empty()) return;
			std::error_code ec;
			fs::remove_all(m_root, ec);
		}

		fs::path get()
		{
			const std::scoped_lock lock{ m_mutex };
			std::error_code ec;
			if (!m_root.empty() && fs::is_directory(m_root, ec)) return m_root;
			const fs::path temp = fs::temp_directory_path(ec);
			if (ec) return {};
			std::random_device entropy;
			for (int attempt = 0; attempt < 16; ++attempt) {
				const std::uint64_t tag = (std::uint64_t{ entropy() } << 32) ^ std::uint64_t{ entropy() }
					^ static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
				const fs::path root = temp / std::format("snk_editor_forge_{:016x}", tag);
				// Fails on a folder that exists: this one is ours alone.
				if (!fs::create_directory(root, ec)) {
					if (ec) return {};
					continue;
				}
#ifndef _WIN32
				fs::permissions(root, fs::perms::owner_all, fs::perm_options::replace, ec);
				if (ec) {
					fs::remove_all(root, ec);
					return {};
				}
#endif
				m_root = root;
				return m_root;
			}
			return {};
		}

	private:
		std::mutex m_mutex;
		fs::path m_root;
	};

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

	bool is_curl_home(const std::string& entry)
	{
		constexpr std::string_view name = "curl_home=";
		return entry.size() >= name.size() && lower(std::string_view(entry).substr(0, name.size())) == name;
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
		std::shared_ptr<scratch_folder> own_scratch;
	};

	edt::forge::http_response perform(const curl_state& state, const edt::forge::http_request& request, std::stop_token stop)
	{
		using edt::forge::forge_error_code;
		const edt::forge::curl_options& options = state.options;

		if (auto refused = refuse(request, options.allowed_hosts)) return std::move(*refused);
		if (stop.stop_requested()) return failure(forge_error_code::network, "the request was cancelled");

		const fs::path scratch = options.scratch_dir ? options.scratch_dir() : state.own_scratch->get();
		if (scratch.empty()) return failure(forge_error_code::unavailable, "no private temp folder for the request files");

		file_guard config_file;
		file_guard body_file;
		config_file.path = scratch / file_name_in("curl_cfg");
		config_file.wipe = true;

		std::string config;
		add_option(config, "url", request.url);
		add_option(config, "request", request.method);
		for (const auto& [name, value] : request.headers) add_option(config, "header", name + ": " + value);
		if (request.bearer_token != nullptr) add_option(config, "header", "Authorization: Bearer " + request.bearer_token->reveal());
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
		config.assign(config.size(), '\0');

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
		std::erase_if(environment, is_curl_home);
		process.environment = std::move(environment);
		std::string error_tail;
		process.on_stderr_line = [&error_tail](std::string_view line) {
			error_tail.append(line);
			error_tail += '\n';
			if (error_tail.size() > k_stderr_keep) error_tail.erase(0, error_tail.size() - k_stderr_keep);
		};

		std::stop_source own_stop;
		const std::stop_callback forward{ stop, [&own_stop] { own_stop.request_stop(); } };
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
		try {
			result = options.run(program, args, process, on_output, own_stop.get_token());
		} catch (const std::exception& e) {
			return failure(forge_error_code::network, std::format("curl could not be run: {}", e.what()));
		} catch (...) {
			return failure(forge_error_code::network, "curl could not be run");
		}

		if (too_large) return failure(forge_error_code::invalid_response, "the response is too large");
		if (!result.error.empty()) {
			return failure(forge_error_code::unavailable, "curl was not found (install curl or set forge/curl)");
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
	state->own_scratch = std::make_shared<scratch_folder>();
	return [state](const http_request& request, std::stop_token stop) {
		return perform(*state, request, std::move(stop));
	};
}
