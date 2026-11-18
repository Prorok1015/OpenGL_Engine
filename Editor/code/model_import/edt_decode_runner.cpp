#include "edt_decode_runner.h"
#include "edt_environment.h"
#include <snk/common/logger/engine_log.h>
#include <snk/common/text_escape.h>
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
#include <utility>

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
		core::res::remove_tree_without_following(path, ec);
	}
};

std::string utf8_text(const fs::path& path)
{
	const auto text = path.u8string();
	return std::string(text.begin(), text.end());
}

std::vector<std::string> clean_environment(const fs::path& folder)
{
	std::vector<std::string> env;
	const auto keep = [&env](const char* name) {
		if (const auto value = edt::environment_utf8(name)) {
			env.push_back(std::string(name) + "=" + *value);
		}
	};
#if defined(_WIN32)
	keep("SystemRoot");
	keep("PATH");
	// CreateProcessW rewrites these for an AppContainer and fails (ERROR_ENVVAR_NOT_FOUND) without them (#1054).
	keep("LOCALAPPDATA");
	keep("APPDATA");
	keep("USERPROFILE");
	// The one place a confined child may write (#1054); unconfined, still the run folder that is removed after it.
	env.push_back("TEMP=" + utf8_text(folder));
	env.push_back("TMP=" + utf8_text(folder));
#else
	// A dev build finds its shared engine through these; nothing else of the editor's environment reaches the child.
	keep("LD_LIBRARY_PATH");
	env.push_back("PATH=/usr/bin:/bin");
	// Whatever asks for a temporary file uses the run folder, the one place the sandbox lets the child write (#1043).
	env.push_back("TMPDIR=" + utf8_text(folder));
#endif
	return env;
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

edt::decode_outcome edt::decode_runner::run(const decode_request& request, const std::vector<std::byte>& source, const std::vector<bundle_blob>& files,
	std::stop_token stop) const
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
	request_bundle.blobs.reserve(files.size() + 1);
	request_bundle.blobs.push_back({ k_decode_source_tag, source });
	request_bundle.blobs.insert(request_bundle.blobs.end(), files.begin(), files.end());
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
	options.environment = clean_environment(folder);
	options.low_priority = true;
	options.memory_limit_bytes = m_config.memory_limit_bytes;
#if defined(_WIN32)
	if (m_config.confine) {
		options.confinement = platform::process_confinement{ m_config.container, { folder }, m_config.confine_read_execute };
	}
#endif
	decode_log_budget budget;
	// The child's own line when it refuses to run without its sandbox: the reason of the failure (its first 256 bytes).
	std::mutex refusal_mutex;
	std::string refusal;
	sandbox_status_reader sandbox_status;
	options.on_stderr_line = [&budget, &refusal_mutex, &refusal, &sandbox_status](std::string_view line) {
		{
			const std::lock_guard lock(refusal_mutex);
			if (sandbox_status.line(line)) return;
			if (line.starts_with(k_decoder_sandbox_refusal_prefix) && refusal.empty()) {
				refusal = text_escape::escaped_text(line.substr(k_decoder_sandbox_refusal_prefix.size()), 256);
			}
		}
		if (budget.take(line.size())) egLOG_WARN("edt/decoder", "{}", decode_log_text(line));
	};
	// The child compares its parent with this at start: an editor that died before the child looked is not mistaken for
	// a live parent (getppid() would then be 1 or a subreaper).
	// The child limits every file it writes to the largest answer the editor reads (RLIMIT_FSIZE), and enforces the sandbox
	// mode itself (#1043).
	std::vector<std::string> arguments{ "--request", utf8_text(request_file), "--out", utf8_text(out_file),
		"--parent-pid", std::to_string(platform::current_process_id()),
		"--sandbox", std::string(sandbox_mode_name(m_config.sandbox)),
		"--max-output-bytes", std::to_string(largest_bundle_bytes(m_config.answer_limits)) };
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
	{
		const std::lock_guard lock(refusal_mutex);
		outcome.sandbox = sandbox_status.status();
	}
	outcome.started = result.error.empty();
	outcome.exit_code = result.exit_code;

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
		outcome.confinement_failed = result.confinement_failed;
		outcome.error = "the decoder could not run: " + result.error;
		return outcome;
	}
	if (result.exit_code == k_decoder_sandbox_exit_code) {
		outcome.sandbox_refused = true;
		const std::lock_guard lock(refusal_mutex);
		outcome.error = "the model decoder cannot run in its sandbox: " + (refusal.empty() ? std::string("no reason given") : refusal);
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

bool edt::sandbox_status_reader::line(std::string_view text)
{
	const bool first = std::exchange(m_first, false);
	if (!first || !text.starts_with(k_decoder_sandbox_status_prefix)) return false;
	text.remove_prefix(k_decoder_sandbox_status_prefix.size());
	const auto take = [&text](std::string_view word) {
		if (!text.starts_with(word)) return false;
		text.remove_prefix(word.size());
		return true;
	};
	decoder_sandbox_status read;
	if (take("fs=") && take(k_decoder_sandbox_fs_name)) read.fs = true;
	else if (!take("none")) return true;
	if (take(" syscalls=") && take(k_decoder_sandbox_syscalls_name)) read.syscalls = true;
	else if (!take("none")) return true;
	if (take(" why=")) read.why = text_escape::escaped_text(text, 256);
	else if (!text.empty()) return true;
	read.reported = true;
	m_status = std::move(read);
	return true;
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
		egLOG_WARN("edt/decoder", "the decoder's output is cut here: it exceeds {} lines or {} ({}); the rest is dropped", k_max_lines, limit_text(k_max_bytes), k_step_decoder_log);
		return false;
	}
	return true;
}
