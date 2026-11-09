#include "edt_decode_runner.h"
#include <snk/common/logger/engine_log.h>
#include <snk/platform/plt_process.h>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string_view>
#include <thread>

namespace fs = std::filesystem;

namespace {

std::atomic<unsigned> g_run_counter{ 0 };

struct folder_guard
{
	fs::path path;
	~folder_guard()
	{
		std::error_code ec;
		fs::remove_all(path, ec);
	}
};

std::vector<std::string> clean_environment()
{
	std::vector<std::string> env;
	const auto keep = [&env](const char* name) {
		if (const char* value = std::getenv(name)) {
			env.push_back(std::string(name) + "=" + value);
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

std::optional<std::int64_t> pid_of_folder(const std::string& name)
{
	const std::size_t dash = name.find('-');
	if (dash == std::string::npos || dash == 0) return std::nullopt;
	std::int64_t pid = 0;
	const auto [end, error] = std::from_chars(name.data(), name.data() + dash, pid);
	if (error != std::errc{} || end != name.data() + dash) return std::nullopt;
	return pid;
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
	std::error_code ec;
	fs::create_directories(m_config.scratch_root, ec);
	if (ec) {
		outcome.error = "cannot create the scratch folder: " + ec.message();
		return outcome;
	}
	fs::permissions(m_config.scratch_root, fs::perms::owner_all, fs::perm_options::replace, ec);

	const fs::path folder = m_config.scratch_root
		/ (std::to_string(platform::current_process_id()) + "-" + std::to_string(g_run_counter.fetch_add(1)));
	folder_guard guard{ folder };
	fs::create_directories(folder, ec);
	if (ec) {
		outcome.error = "cannot create the run folder: " + ec.message();
		return outcome;
	}
	fs::permissions(folder, fs::perms::owner_all, fs::perm_options::replace, ec);

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
	options.on_stderr_line = [](std::string_view line) { egLOG_WARN("edt/decoder", "{}", line); };
	std::vector<std::string> arguments{ "--request", utf8_text(request_file), "--out", utf8_text(out_file) };
	arguments.insert(arguments.end(), m_config.extra_arguments.begin(), m_config.extra_arguments.end());
	const platform::process_result result = platform::run_process(m_config.decoder, arguments,
		options, [](std::string_view line) { egLOG_INFO("edt/decoder", "{}", line); }, child_stop.get_token());

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

std::size_t edt::decode_runner::remove_stale_folders(const fs::path& scratch_root)
{
	std::error_code ec;
	if (!fs::is_directory(scratch_root, ec)) return 0;
	std::size_t removed = 0;
	std::vector<fs::path> stale;
	for (const auto& entry : fs::directory_iterator(scratch_root, ec)) {
		if (!entry.is_directory(ec)) continue;
		const auto pid = pid_of_folder(entry.path().filename().string());
		if (pid && !platform::process_is_running(*pid)) {
			stale.push_back(entry.path());
		}
	}
	for (const fs::path& path : stale) {
		std::error_code remove_ec;
		if (fs::remove_all(path, remove_ec) != static_cast<std::uintmax_t>(-1) && !remove_ec) ++removed;
	}
	return removed;
}
