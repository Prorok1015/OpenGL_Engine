#include "edt_person_model_import.h"
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>
#include <snk/common/text_escape.h>
#include <snk/platform/plt_process.h>
#include <snk/resource/res_path_sanitize.h>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <fstream>
#include <functional>
#include <optional>
#include <set>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string mib_text(std::uintmax_t bytes)
{
	return std::format("{:.1f} MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
}

// The whole file, read in chunks so a stop request is not kept waiting; empty with `error` set when it cannot be.
std::vector<std::byte> read_model(const fs::path& path, std::string& error, ds::task_context& ctx)
{
	std::error_code ec;
	const std::uintmax_t size = fs::file_size(path, ec);
	if (ec) {
		error = std::format("the file cannot be read: {}", ec.message());
		return {};
	}
	if (size > edt::k_max_person_source_bytes) {
		error = std::format("the file is too big ({}), the limit is {}", mib_text(size), mib_text(edt::k_max_person_source_bytes));
		return {};
	}
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		error = "the file cannot be opened";
		return {};
	}
	std::vector<std::byte> bytes(static_cast<std::size_t>(size));
	constexpr std::size_t chunk = 8ull * 1024ull * 1024ull;
	for (std::size_t done = 0; done < bytes.size(); done += chunk) {
		ctx.throw_if_stopped();
		const std::size_t part = (std::min)(chunk, bytes.size() - done);
		if (!in.read(reinterpret_cast<char*>(bytes.data() + done), static_cast<std::streamsize>(part))) {
			error = "the file cannot be read to its end";
			return {};
		}
	}
	return bytes;
}

}

std::string edt::person_import_folder(const fs::path& path, std::int64_t process_id, std::uint64_t serial)
{
	return std::format("__imports/{}/{}/{}/", process_id, serial, res::sanitize_relative_path(fs_paths::utf8_of(path.stem())));
}

void edt::erase_imports_of_ended_runs(res::resource_system& resources, std::int64_t current_process_id)
{
	constexpr std::string_view imports = "__imports/";
	const std::optional<std::vector<res::tag>> listed = resources.files_under(res::tag::memory, imports);
	if (!listed) {
		egLOG_WARN("Editor/Import", "memory:// cannot list '{}': the imports of earlier runs stay", imports);
		return;
	}
	std::set<std::string, std::less<>> seen;
	std::vector<std::string> ended;
	for (const res::tag& tag : *listed) {
		if (!tag.relative().starts_with(imports)) continue;
		const std::string_view below = tag.relative().substr(imports.size());
		const std::size_t slash = below.find('/');
		if (slash == std::string_view::npos) continue;
		const std::string_view run = below.substr(0, slash);
		if (!seen.emplace(run).second) continue;
		std::int64_t process_id = 0;
		const auto [end, error] = std::from_chars(run.data(), run.data() + run.size(), process_id);
		// Not a run's folder (no process id): nothing tells whether an editor still uses it, so it stays.
		if (error != std::errc{} || end != run.data() + run.size() || process_id <= 0) continue;
		if (process_id == current_process_id || platform::process_is_running(process_id)) continue;
		ended.emplace_back(run);
	}
	for (const std::string& run : ended) {
		if (const std::optional<res::tag> left = erase_memory_folder(resources, std::format("{}{}/", imports, run))) {
			egLOG_WARN("Editor/Import", "'{}' of an earlier run's import cannot be removed", left->view());
		}
	}
}

edt::import_intermediate edt::person_model_import(const fs::path& path, const std::string& memory_folder,
	const decoder_steps& decoder, res::resource_system& resources, ds::task_context& ctx)
{
	import_intermediate result;
	result.source_path = path;
	if (const std::string why = decoder.unavailable_reason ? decoder.unavailable_reason() : std::string{}; !why.empty()) {
		result.error = std::format("the model decoder cannot run, and a model is never read in the editor itself: {}", why);
		return result;
	}

	ctx.throw_if_stopped();
	std::string read_error;
	const std::vector<std::byte> bytes = read_model(path, read_error, ctx);
	if (!read_error.empty()) {
		result.error = read_error;
		return result;
	}
	const std::string size = mib_text(bytes.size());
	const auto failed = [&](std::string_view step, std::string_view why) {
		result.error = std::format("{} ({}) failed: {}", step, size, why);
		return result;
	};

	import_source source;
	source.path = path;
	source.preloaded = &bytes;
	source.memory_folder = memory_folder;

	constexpr std::string_view listing = "listing the files the model reads";
	model_file_scan found;
	try {
		ctx.throw_if_stopped();
		source.deadline = std::chrono::steady_clock::now() + k_person_parse_time_limit;
		found = decoder.scan(source, &ctx);
	} catch (const std::exception& e) {
		return failed(listing, e.what());
	}
	if (!found.ok()) return failed(listing, found.error);
	egLOG_INFO("Editor/Import", "'{}' ({}) reads {} other file(s), {} in all",
		text_escape::escaped_text(fs_paths::utf8_of(path.filename()), 256), size,
		found.files.size(), mib_text(found.total_bytes));

	constexpr std::string_view importing = "importing the model";
	ctx.throw_if_stopped();
	if (const std::optional<res::tag> left = erase_memory_folder(resources, memory_folder)) {
		return failed(importing, std::format("'{}', left in memory:// by an earlier import, cannot be removed", left->view()));
	}
	source.allowed = &found.files;
	import_intermediate imported;
	try {
		source.deadline = std::chrono::steady_clock::now() + k_person_parse_time_limit;
		imported = decoder.import(source, &ctx);
	} catch (const std::exception& e) {
		return failed(importing, e.what());
	}
	if (!imported.error.empty()) return failed(importing, imported.error);
	// Stopped while the decoder imported: the job reports nothing, so what it stored goes now.
	if (ctx.stop.stop_requested()) {
		for (const res::tag& tag : imported.created_tags) resources.erase(tag);
		throw ds::task_cancelled{};
	}
	imported.source_path = path;
	return imported;
}
