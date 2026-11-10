#include "edt_decode_runner.h"
#include <snk/common/logger/engine_log.h>
#include <snk/platform/plt_process.h>
#include <snk/resource/res_shipping_dirs.h>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <format>
#include <cstdlib>
#include <mutex>
#include <string_view>
#include <thread>

namespace fs = std::filesystem;

namespace {

// One folder per run, snk-model_decoder-<pid>-<random> in the private scratch base of the user.
constexpr const char* k_scratch_name = "model_decoder";

struct folder_guard
{
	fs::path path;
	~folder_guard()
	{
		std::error_code ec;
		fs::remove_all(path, ec);
	}
};

std::optional<std::string> environment_value(const char* name)
{
#if defined(_WIN32)
	// std::getenv is C4996 under /WX; the wide value keeps a path with other letters intact.
	const std::wstring wide_name(name, name + std::char_traits<char>::length(name));
	wchar_t* value = nullptr;
	std::size_t size = 0;
	if (_wdupenv_s(&value, &size, wide_name.c_str()) != 0 || !value) {
		return std::nullopt;
	}
	const std::u8string text = fs::path(value).u8string();
	std::free(value);
	return std::string(text.begin(), text.end());
#else
	const char* value = std::getenv(name);
	return value ? std::optional<std::string>(value) : std::nullopt;
#endif
}

std::vector<std::string> clean_environment()
{
	std::vector<std::string> env;
	const auto keep = [&env](const char* name) {
		if (const auto value = environment_value(name)) {
			env.push_back(std::string(name) + "=" + *value);
		}
	};
#if defined(_WIN32)
	keep("SystemRoot");
	keep("PATH");
#else
	// A dev build finds its shared engine through these; nothing else of the editor's environment reaches the child.
	keep("LD_LIBRARY_PATH");
	env.push_back("PATH=/usr/bin:/bin");
#endif
	return env;
}

std::string utf8_text(const fs::path& path)
{
	const auto text = path.u8string();
	return std::string(text.begin(), text.end());
}

} // namespace

edt::decode_runner::decode_runner(decode_runner_config config)
	: m_config(std::move(config))
{
	if (m_config.decoder.empty()) {
		const fs::path self = platform::current_executable();
#if defined(_WIN32)
		m_config.decoder = self.parent_path() / "snk_model_decoder.exe";
#else
		m_config.decoder = self.parent_path() / "snk_model_decoder";
#endif
	}
}

edt::decode_outcome edt::decode_runner::run(const decode_request& request, const std::vector<std::byte>& source, std::stop_token stop) const
{
	decode_outcome outcome;
	// A folder this call made itself, private to this user (exclusive, random name, 0700 / owner-only): the same helper
	// the shipped game uses for memory://. Never an existing one.
	const core::res::scratch_folder_result made = core::res::make_scratch_dir(k_scratch_name);
	if (made.path.empty()) {
		outcome.error = "cannot make the run folder: " + made.error;
		return outcome;
	}
	const fs::path folder = made.path;
	folder_guard guard{ folder };

	decode_request sent = request;
	const auto timeout_ms = static_cast<std::uint64_t>(m_config.timeout.count());
	if (sent.deadline_ms == 0 || sent.deadline_ms > timeout_ms) sent.deadline_ms = timeout_ms;
	sent.memory_limit_bytes = m_config.memory_limit_bytes;

	decode_bundle request_bundle;
	request_bundle.manifest = to_json(sent);
	request_bundle.blobs.push_back({ k_decode_source_tag, source });
	const fs::path request_file = folder / "request.bundle";
	const fs::path out_file = folder / "out.bundle";
	std::string write_error;
	if (!write_bundle(request_file, request_bundle, write_error)) {
		outcome.error = "cannot write the request: " + write_error;
		return outcome;
	}

	std::stop_source child_stop;
	std::stop_callback forward_stop(stop, [&child_stop] { child_stop.request_stop(); });
	std::atomic<bool> timed_out{ false };
	std::mutex timer_mutex;
	std::condition_variable timer_wake;
	bool finished = false;
	std::thread timer([&] {
		std::unique_lock lock(timer_mutex);
		if (!timer_wake.wait_for(lock, m_config.timeout, [&] { return finished; })) {
			timed_out.store(true);
			child_stop.request_stop();
		}
	});

	platform::process_options options;
	options.working_dir = folder;
	options.environment = clean_environment();
	options.low_priority = true;
	options.memory_limit_bytes = m_config.memory_limit_bytes;
	decode_log_budget budget;
	options.on_stderr_line = [&budget](std::string_view line) {
		if (budget.take(line.size())) egLOG_WARN("edt/decoder", "{}", decode_log_text(line));
	};
	// The child compares its parent with this at start: an editor that died before the child looked is not mistaken for
	// a live parent (getppid() would then be 1 or a subreaper).
	std::vector<std::string> arguments{ "--request", utf8_text(request_file), "--out", utf8_text(out_file),
		"--parent-pid", std::to_string(platform::current_process_id()) };
	arguments.insert(arguments.end(), m_config.extra_arguments.begin(), m_config.extra_arguments.end());
	const platform::process_result result = platform::run_process(m_config.decoder, arguments,
		options, [&budget](std::string_view line) {
			if (budget.take(line.size())) egLOG_INFO("edt/decoder", "{}", decode_log_text(line));
		}, child_stop.get_token());

	{
		std::lock_guard lock(timer_mutex);
		finished = true;
	}
	timer_wake.notify_all();
	timer.join();

	if (timed_out.load()) {
		outcome.timed_out = true;
		outcome.error = "the decoder did not finish in time and was stopped";
		return outcome;
	}
	if (result.cancelled) {
		outcome.cancelled = true;
		outcome.error = "cancelled";
		return outcome;
	}
	if (!result.error.empty()) {
		outcome.error = "the decoder could not run: " + result.error;
		return outcome;
	}
	if (result.exit_code != 0) {
		outcome.error = "the decoder failed (exit code " + std::to_string(result.exit_code) + ")";
		return outcome;
	}

	auto read = read_bundle(out_file, m_config.answer_limits);
	if (!read.ok()) {
		outcome.error = "the decoder's answer cannot be used: " + read.error;
		return outcome;
	}
	outcome.bundle = std::move(read.bundle);
	return outcome;
}

std::size_t edt::decode_runner::remove_stale_folders()
{
	return core::res::remove_stale_scratch_dirs(k_scratch_name);
}

// What the child printed is the model's doing: a line with a control character must not forge log lines or move the
// cursor of a terminal.
std::string edt::decode_log_text(std::string_view line)
{
	std::string out;
	out.reserve(line.size());
	const auto byte_at = [&line](std::size_t i) -> unsigned { return i < line.size() ? static_cast<unsigned char>(line[i]) : 0u; };
	const auto escape = [&out](unsigned byte) { out += std::format("\\x{:02x}", byte); };
	for (std::size_t i = 0; i < line.size(); ++i) {
		const unsigned byte = byte_at(i);
		if ((byte < 0x20 && byte != '\t') || byte == 0x7f) {
			escape(byte);
			continue;
		}
		// What a terminal or a log viewer treats as a control or reorders text by, in UTF-8: C1 controls (U+0080..U+009F),
		// U+061C, U+200E/F, the bidi embeddings and overrides (U+202A..U+202E), the line and paragraph separators
		// (U+2028, U+2029) and the isolates (U+2066..U+2069).
		std::size_t length = 1;
		if (byte == 0xC2 && byte_at(i + 1) >= 0x80 && byte_at(i + 1) <= 0x9F) length = 2;
		else if (byte == 0xD8 && byte_at(i + 1) == 0x9C) length = 2;
		else if (byte == 0xE2 && byte_at(i + 1) == 0x80 && (byte_at(i + 2) == 0x8E || byte_at(i + 2) == 0x8F || (byte_at(i + 2) >= 0xA8 && byte_at(i + 2) <= 0xAE))) length = 3;
		else if (byte == 0xE2 && byte_at(i + 1) == 0x81 && byte_at(i + 2) >= 0xA6 && byte_at(i + 2) <= 0xA9) length = 3;
		if (length > 1) {
			for (std::size_t k = 0; k < length; ++k) escape(byte_at(i + k));
			i += length - 1;
		} else {
			out.push_back(static_cast<char>(byte));
		}
	}
	return out;
}

bool edt::decode_log_budget::take(std::size_t bytes)
{
	const std::lock_guard lock(m_mutex);
	if (m_done) return false;
	if (++m_lines > k_max_lines || (m_bytes += bytes) > k_max_bytes) {
		m_done = true;
		egLOG_WARN("edt/decoder", "the decoder's output is cut here: more than {} lines or {} KiB; the rest is dropped", k_max_lines, k_max_bytes / 1024);
		return false;
	}
	return true;
}
