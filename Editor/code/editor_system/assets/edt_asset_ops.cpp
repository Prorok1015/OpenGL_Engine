#include "edt_asset_ops.h"
#include "edt_asset_database.h"
#include "edt_reference_rewrite.h"
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>
#include <snk/editor/edt_document.h>
#include <snk/model_codec/res_asset_id.h>
#include <snk/model_codec/res_desc_binary_codec.h>
#include <snk/platform/plt_beneath.h>
#include <snk/project/prj_manifest.h>
#include <snk/resource/res_asset_meta.h>
#include <snk/resource/res_asset_walk.h>
#include <snk/resource/res_path_sanitize.h>
#include <snk/resource/res_stamp.h>
#include <boost/json.hpp>
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <format>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace json = boost::json;

namespace
{
	constexpr std::string_view k_journal_file = "journal.json";
	constexpr std::size_t k_max_id_length = 64;
	constexpr std::size_t k_steps_per_file = 64;
	// The step files one journal may name: more is no journal an operation wrote.
	constexpr std::uint64_t k_max_step_files = 1024;
	// What one step of a journal takes on the disk besides its root and paths (keys, hashes, quotes).
	constexpr std::uint64_t k_step_overhead = 192;

	std::string_view view_of(json::string_view text)
	{
		return std::string_view(text.data(), text.size());
	}

	std::string utf8(const fs::path& path)
	{
		return fs_paths::generic_utf8_of(path);
	}

	fs::path path_of(const std::string& relative)
	{
		return fs_paths::path_from_utf8(relative);
	}

	std::string parent_of(std::string_view relative)
	{
		const std::size_t slash = relative.find_last_of('/');
		return slash == std::string_view::npos ? std::string{} : std::string(relative.substr(0, slash));
	}

	std::string join(std::string_view folder, std::string_view name)
	{
		return folder.empty() ? std::string(name) : std::format("{}/{}", folder, name);
	}

	std::string hash_of(std::span<const std::byte> bytes)
	{
		res::content_hasher hasher;
		hasher.update(bytes);
		return hasher.text();
	}

	std::string hash_of(const std::string& text)
	{
		return hash_of(std::as_bytes(std::span<const char>(text.data(), text.size())));
	}

	platform::beneath::read_request read_request_of(const fs::path& root, const std::string& relative, std::uint64_t max_bytes)
	{
		const fs::path path = path_of(relative);
		platform::beneath::read_request request;
		request.root = root;
		request.folder = path.parent_path();
		request.leaf = path.filename();
		request.max_bytes = max_bytes;
		return request;
	}

	std::string read_error(const platform::beneath::read_result& result)
	{
		if (result.status == platform::beneath::read_status::ok) return {};
		return result.error.empty() ? std::string{ "it cannot be read" } : result.error;
	}

	// Empty `error`: the hash of the file's bytes, read in chunks.
	std::string hash_file(const fs::path& root, const std::string& relative, std::string& error)
	{
		res::content_hasher hasher;
		const auto result = platform::beneath::read_beneath_stream(read_request_of(root, relative, (std::numeric_limits<std::uint64_t>::max)()),
			[&hasher](std::span<const std::byte> chunk) {
				hasher.update(chunk);
				return true;
			});
		error = read_error(result);
		return error.empty() ? hasher.text() : std::string{};
	}

	std::string read_file(const fs::path& root, const std::string& relative, std::uint64_t max_bytes, std::vector<std::byte>& bytes)
	{
		bytes.clear();
		return read_error(platform::beneath::read_beneath(read_request_of(root, relative, max_bytes), bytes));
	}

	fs::path temp_leaf_for(const fs::path& leaf)
	{
		std::random_device random;
		fs::path out{ "." };
		out += leaf;
		out += std::format(".{:08x}.tmp", random());
		return out;
	}

	std::string write_file(const fs::path& root, const std::string& relative, std::span<const std::byte> bytes, bool replace)
	{
		const fs::path path = path_of(relative);
		platform::beneath::pinned_root pinned{ root };
		platform::beneath::write_request request;
		request.folder = path.parent_path();
		request.leaf = path.filename();
		request.tmp_leaf = temp_leaf_for(request.leaf);
		request.data = bytes;
		request.replace = replace;
		request.tmp_path = root / request.folder / request.tmp_leaf;
		return platform::beneath::write_beneath(pinned, request);
	}

	std::string move_file(const fs::path& root, const std::string& from, const std::string& to)
	{
		const fs::path source = path_of(from);
		const fs::path target = path_of(to);
		platform::beneath::pinned_root pinned{ root };
		platform::beneath::rename_request request;
		request.from_folder = source.parent_path();
		request.from_leaf = source.filename();
		request.to_folder = target.parent_path();
		request.to_leaf = target.filename();
		return platform::beneath::rename_beneath(pinned, request);
	}

	std::string remove_file(const fs::path& root, const std::string& relative)
	{
		const fs::path path = path_of(relative);
		platform::beneath::pinned_root pinned{ root };
		return platform::beneath::remove_beneath(pinned, path.parent_path(), path.filename(), true);
	}

	bool exists_on_disk(const fs::path& root, const std::string& relative)
	{
		std::error_code ec;
		return fs::symlink_status(root / path_of(relative), ec).type() != fs::file_type::not_found;
	}

	// Like exists_on_disk, but where the project's names ignore letter case (`fold`) a path that differs from `relative` in case
	// only, in any of its names, is the same file or folder, whatever the disk under the folder says.
	bool exists_named(const fs::path& root, const std::string& relative, bool fold)
	{
		if (exists_on_disk(root, relative)) return true;
		if (!fold) return false;
		fs::path at = root;
		std::size_t begin = 0;
		while (true) {
			const std::size_t end = relative.find('/', begin);
			const bool last = end == std::string::npos;
			const std::string wanted = res::asset_name_key(std::string_view(relative).substr(begin, last ? std::string::npos : end - begin), true);
			std::optional<fs::path> found;
			std::error_code ec;
			fs::directory_iterator it(at, fs::directory_options::skip_permission_denied, ec);
			if (ec) return false;
			for (const fs::directory_iterator done; it != done; it.increment(ec)) {
				if (ec) return false;
				if (res::asset_name_key(utf8(it->path().filename()), true) != wanted) continue;
				if (!last && fs::symlink_status(it->path(), ec).type() != fs::file_type::directory) continue;
				found = it->path();
				break;
			}
			if (!found) return false;
			if (last) return true;
			at = *found;
			begin = end + 1;
		}
	}

	bool is_regular(const fs::path& root, const std::string& relative)
	{
		std::error_code ec;
		return fs::is_regular_file(fs::symlink_status(root / path_of(relative), ec));
	}

	// `folder` below `root`, made where missing, each folder from the one above it and never through a link (#1294).
	std::string make_folders(const fs::path& root, const fs::path& folder)
	{
		platform::beneath::pinned_root pinned{ root };
		return platform::beneath::make_folders_beneath(pinned, folder);
	}

	// The folder `relative` below `root`, made by this call: it must not be there yet, nor be reached through a link.
	std::string make_new_folder(const fs::path& root, const std::string& relative)
	{
		platform::beneath::pinned_root pinned{ root };
		return platform::beneath::make_folders_beneath(pinned, path_of(relative), true);
	}

	// Whether the folder below `root` holds nothing; `error` set when it cannot be listed (not there, a link on the way).
	bool folder_is_empty(const fs::path& root, const std::string& relative, std::string& error)
	{
		platform::beneath::pinned_root pinned{ root };
		const platform::beneath::list_result listed = platform::beneath::list_beneath(pinned, path_of(relative), 1);
		if (listed.missing) error = "it is not there";
		else error = listed.error;
		return error.empty() && listed.entries.empty();
	}

	// Removes the empty folder below `root`; a folder with anything in it stays (the removal fails).
	std::string remove_empty_folder(const fs::path& root, const std::string& relative)
	{
		std::error_code ec;
		if (fs::symlink_status(root / path_of(relative), ec).type() != fs::file_type::directory) return "it is not a folder";
		const fs::path path = path_of(relative);
		platform::beneath::pinned_root pinned{ root };
		return platform::beneath::remove_beneath(pinned, path.parent_path(), path.filename());
	}

	bool is_folder(const fs::path& root, const std::string& relative)
	{
		std::error_code ec;
		return fs::symlink_status(root / path_of(relative), ec).type() == fs::file_type::directory;
	}

	// The journal's files are reached from the folder that holds journal_root (the project's cache), so a link at
	// journal_root itself is refused like one below it (#1294): journal_base is that root, in_journal the path below it.
	fs::path journal_folder(const fs::path& journal_root)
	{
		const fs::path normal = journal_root.lexically_normal();
		// "x/asset_ops/" has no filename: it is the folder "x/asset_ops".
		return normal.has_filename() || !normal.has_relative_path() ? normal : normal.parent_path();
	}

	fs::path journal_base(const fs::path& journal_root)
	{
		return journal_folder(journal_root).parent_path();
	}

	std::string in_journal(const fs::path& journal_root, const std::string& id, std::string_view relative)
	{
		return std::format("{}/{}/{}", utf8(journal_folder(journal_root).filename()), id, relative);
	}

	// What a test may take over (asset_ops_ports::fail_step, force_copy); both empty in production.
	struct move_seams
	{
		std::function<std::string(std::string_view)> fail;
		std::function<bool(std::string_view)> force_copy;
		std::function<std::string(std::string_view)> rename_error;
	};

	move_seams seams_of(const edt::asset_ops_ports& ports)
	{
		return move_seams{ ports.fail_step, ports.force_copy, ports.rename_error };
	}

	struct rename_outcome
	{
		std::string error;
		// Only the move between two volumes: the one failure a copy may take over from.
		bool across_volumes = false;
	};

	std::string injected_failure(const move_seams& seams, const std::string& label)
	{
		return seams.fail ? seams.fail(label) : std::string{};
	}

	bool copy_forced(const move_seams& seams, const std::string& label)
	{
		return seams.force_copy && seams.force_copy(label);
	}

	fs::path without_trailing_separator(const fs::path& path)
	{
		fs::path out = path.lexically_normal();
		if (!out.empty() && !out.has_filename()) out = out.parent_path();
		return out;
	}

	// The deepest folder both lie in, by name; empty when they share none (another drive).
	fs::path common_folder(const fs::path& first, const fs::path& second)
	{
		const fs::path left = without_trailing_separator(first);
		const fs::path right = without_trailing_separator(second);
		fs::path common;
		auto l = left.begin();
		auto r = right.begin();
		for (; l != left.end() && r != right.end() && *l == *r; ++l, ++r) common /= *l;
		return common;
	}

	fs::path relative_to(const fs::path& folder, const fs::path& common)
	{
		fs::path out = without_trailing_separator(folder).lexically_relative(common);
		if (out == ".") out.clear();
		return out;
	}

	// A rename from one root to another: both lie in a folder the move is made from, and every folder below it is opened
	// without following a link, as a rename inside one root is. An error when it cannot be made, the file staying where it
	// was; `across_volumes` says the cause was another volume (or another drive: no shared folder), which is not a refusal.
	rename_outcome rename_between(const fs::path& from_root, const std::string& from, const fs::path& to_root, const std::string& to)
	{
		const fs::path common = common_folder(from_root, to_root);
		if (common.empty()) return rename_outcome{ "the two folders share no parent folder", true };
		const fs::path source = (relative_to(from_root, common) / path_of(from)).lexically_normal();
		const fs::path target = (relative_to(to_root, common) / path_of(to)).lexically_normal();
		platform::beneath::pinned_root pinned{ common };
		platform::beneath::rename_request request;
		request.from_folder = source.parent_path();
		request.from_leaf = source.filename();
		request.to_folder = target.parent_path();
		request.to_leaf = target.filename();
		rename_outcome outcome;
		request.across_volumes = &outcome.across_volumes;
		outcome.error = platform::beneath::rename_beneath(pinned, request);
		return outcome;
	}

	// Copies a file from one root to another in chunks (never held whole), flushed to the disk and moved into place only when
	// every byte was read and its hash is `hash`: nothing is at `to` before that, and a failure leaves no temp file. The step
	// `before_commit` of the seams fails it after the check.
	std::string copy_verified(const fs::path& from_root, const std::string& from, const fs::path& to_root, const std::string& to,
		const std::string& hash, const std::string& before_commit, const move_seams& seams, const std::string& mismatch)
	{
		std::string problem;
		const platform::beneath::chunk_source source = [&](const platform::beneath::chunk_sink& sink) {
			res::content_hasher hasher;
			bool sink_failed = false;
			const platform::beneath::read_result result = platform::beneath::read_beneath_stream(
				read_request_of(from_root, from, (std::numeric_limits<std::uint64_t>::max)()), [&](std::span<const std::byte> chunk) {
					hasher.update(chunk);
					sink_failed = !sink(chunk);
					return !sink_failed;
				});
			problem = read_error(result);
			if (!problem.empty() || sink_failed) return false;
			if (hasher.text() != hash) {
				problem = mismatch;
				return false;
			}
			problem = injected_failure(seams, before_commit);
			return problem.empty();
		};
		const fs::path target = path_of(to);
		platform::beneath::pinned_root pinned{ to_root };
		platform::beneath::write_request request;
		request.folder = target.parent_path();
		request.leaf = target.filename();
		request.tmp_leaf = temp_leaf_for(request.leaf);
		request.stream = &source;
		request.replace = false;
		request.tmp_path = to_root / request.folder / request.tmp_leaf;
		const std::string error = platform::beneath::write_beneath(pinned, request);
		if (error.empty()) return {};
		return problem.empty() ? error : problem;
	}

	// The file of the project goes into the journal folder (below `journal_root`): moved by a rename, which copies nothing and
	// is atomic, or, only where the two lie on different volumes, copied, flushed and checked before the original is removed.
	// Any other reason the rename cannot be made (a link in the way, an escape, a denied access) refuses the step and leaves
	// the file where it is: a copy must not go where the rename was not allowed. At no moment is there no complete copy.
	std::string stash_file(const fs::path& root, const std::string& relative, const fs::path& journal_root_base,
		const std::string& journal_relative, const std::string& hash, const move_seams& seams)
	{
		const fs::path target = path_of(journal_relative);
		if (std::string error = make_folders(journal_root_base, target.parent_path()); !error.empty()) return error;
		const std::string changed = std::format("'{}' changed while it was removed", relative);
		if (!copy_forced(seams, "stash:" + relative)) {
			rename_outcome moved{ seams.rename_error ? seams.rename_error("stash:" + relative) : std::string{}, false };
			if (moved.error.empty()) moved = rename_between(root, relative, journal_root_base, journal_relative);
			if (moved.error.empty()) {
				std::string error;
				const std::string now = hash_file(journal_root_base, journal_relative, error);
				if (error.empty() && now == hash) return {};
				if (error.empty()) error = changed;
				const rename_outcome back = rename_between(journal_root_base, journal_relative, root, relative);
				if (!back.error.empty()) {
					error += std::format(" and could not be put back: {}", back.error);
				}
				return error;
			}
			if (!moved.across_volumes) return std::format("'{}' cannot be moved into the journal: {}", relative, moved.error);
			egLOG_INFO("asset_ops", "'{}' is copied into the journal, not moved: {}", relative, moved.error);
		}
		std::string copied = copy_verified(root, relative, journal_root_base, journal_relative, hash,
			"stash_copied:" + relative, seams, changed);
		if (!copied.empty()) {
			return copied;
		}
		return remove_file(root, relative);
	}

	// The kept file comes back to `relative` below `root`, without replacing anything: moved by a rename, or, only where the two
	// lie on different volumes, copied beside it under a temporary name, flushed, checked against `hash` and moved in; any other
	// reason the rename cannot be made fails the step. A file that came back by rename is checked too, and goes back to the journal
	// when it is not `hash`. The kept file is removed only once the restored one is complete, so a failure leaves the kept one.
	std::string restore_file(const fs::path& journal_root_base, const std::string& journal_relative,
		const fs::path& root, const std::string& relative, const std::string& hash, const move_seams& seams)
	{
		if (std::string error = injected_failure(seams, "unstash:" + relative); !error.empty()) return error;
		const fs::path target = path_of(relative);
		if (std::string error = make_folders(root, target.parent_path()); !error.empty()) return error;
		if (!copy_forced(seams, "unstash:" + relative)) {
			rename_outcome moved{ seams.rename_error ? seams.rename_error("unstash:" + relative) : std::string{}, false };
			if (moved.error.empty()) moved = rename_between(journal_root_base, journal_relative, root, relative);
			if (moved.error.empty()) {
				std::string error;
				const std::string now = hash_file(root, relative, error);
				if (error.empty() && now == hash) return {};
				if (error.empty()) error = "the kept copy is not the file that was removed";
				const rename_outcome back = rename_between(root, relative, journal_root_base, journal_relative);
				if (!back.error.empty()) {
					error += std::format(" and could not be put back into the journal: {}", back.error);
				}
				return error;
			}
			if (!moved.across_volumes) return std::format("'{}' cannot be moved back from the journal: {}", relative, moved.error);
			egLOG_INFO("asset_ops", "'{}' is copied back from the journal, not moved: {}", relative, moved.error);
		}
		if (std::string error = copy_verified(journal_root_base, journal_relative, root, relative, hash,
			"unstash_copied:" + relative, seams,
			"the kept copy is not the file that was removed"); !error.empty()) {
			return error;
		}
		if (const std::string kept = remove_file(journal_root_base, journal_relative); !kept.empty()) {
			egLOG_WARN("asset_ops", "the kept copy of '{}' stays in the journal: {}", relative, kept);
		}
		return {};
	}

	edt::refusal stop(edt::refusal_code code, std::string reason)
	{
		edt::refusal out;
		out.code = code;
		out.reason = std::move(reason);
		return out;
	}

	// The first names of a list, for a reason: the rest is counted.
	std::string first_names(const std::vector<std::string>& names)
	{
		constexpr std::size_t k_shown = 12;
		std::string out;
		for (std::size_t i = 0; i < names.size() && i < k_shown; ++i) {
			if (i > 0) out += ", ";
			out += names[i];
		}
		if (names.size() > k_shown) out += std::format(" and {} more", names.size() - k_shown);
		return out;
	}

	edt::refusal too_many(std::string_view files, std::uint64_t bytes, const edt::asset_ops_limits& limits)
	{
		return stop(edt::refusal_code::too_many_files, std::format("{} file(s) and folder(s) of {} byte(s) in one operation: at most {} files and folders and {} bytes are done at once",
			files, bytes, limits.max_files, limits.max_bytes));
	}

	// A folder of a layer as the walk of a request finds it, no link followed.
	struct folder_listing
	{
		std::vector<std::string> files;
		// The folder and every folder below it, each after its parent.
		std::vector<std::string> folders;
		// Links (any reparse point on Windows) and entries that are neither a file nor a folder.
		std::vector<std::string> odd;
		// More files than the walk was allowed: it stopped.
		bool truncated = false;
	};

	// Every entry below `folder` in `root`, each folder opened from the one above it without following a link.
	std::string list_folder(const fs::path& root, const std::string& folder, std::size_t max_files, folder_listing& out)
	{
		platform::beneath::pinned_root pinned{ root };
		out.folders.push_back(folder);
		for (std::size_t next = 0; next < out.folders.size(); ++next) {
			const std::string current = out.folders[next];
			const std::size_t depth = static_cast<std::size_t>(std::ranges::count(std::string_view(current).substr(folder.size()), '/'));
			if (depth >= static_cast<std::size_t>(res::asset_walk_max_depth)) {
				return std::format("'{}' is nested deeper than {} folders", current, res::asset_walk_max_depth);
			}
			const platform::beneath::list_result listed = platform::beneath::list_beneath(pinned, path_of(current), max_files + 1);
			if (listed.missing) return std::format("'{}' is not there any more", current);
			if (!listed.error.empty()) return std::format("'{}' cannot be listed: {}", current, listed.error);
			if (listed.truncated) {
				out.truncated = true;
				return {};
			}
			std::vector<std::pair<std::string, platform::beneath::entry_kind>> entries;
			entries.reserve(listed.entries.size());
			for (const platform::beneath::listed_entry& entry : listed.entries) entries.emplace_back(utf8(entry.name), entry.kind);
			std::ranges::sort(entries);
			for (const auto& [name, kind] : entries) {
				std::string relative = join(current, name);
				switch (kind) {
				case platform::beneath::entry_kind::file: out.files.push_back(std::move(relative)); break;
				case platform::beneath::entry_kind::folder: out.folders.push_back(std::move(relative)); break;
				case platform::beneath::entry_kind::link:
				case platform::beneath::entry_kind::other: out.odd.push_back(std::move(relative)); break;
				}
			}
			if (out.files.size() + out.folders.size() > max_files) {
				out.truncated = true;
				return {};
			}
		}
		return {};
	}

	// The size of a regular file below `root` as its open handle sees it, read without following a link.
	std::optional<std::uint64_t> size_of(const fs::path& root, const std::string& relative)
	{
		platform::beneath::read_request request = read_request_of(root, relative, (std::numeric_limits<std::uint64_t>::max)());
		request.stamp_only = true;
		std::vector<std::byte> none;
		const platform::beneath::read_result result = platform::beneath::read_beneath(request, none);
		if (result.status != platform::beneath::read_status::ok || !result.stamp) return std::nullopt;
		return result.stamp->size;
	}

	// The file a .resource or a desc's binary file (or its .resource) belongs to; nullopt for any other name.
	std::optional<std::string> companion_owner(std::string_view relative)
	{
		std::string_view file = relative;
		if (res::is_asset_meta_sidecar(file)) file = res::asset_meta_owner(file);
		const std::size_t slash = file.find_last_of('/');
		const std::string_view name = slash == std::string_view::npos ? file : file.substr(slash + 1);
		if (const std::optional<std::string> desc = res::desc_binary_owner_name(name)) return join(parent_of(file), *desc);
		if (file.size() != relative.size()) return std::string(file);
		return std::nullopt;
	}

	// A file below the folder `base` that a walk of the layer never lists as an asset: a name on the way it skips (hidden,
	// temporary, system), a .resource (of nothing, or of such a file) or a desc's binary file (a .bin.new too). Any other file
	// without a record is one the asset database has not read yet.
	bool never_listed(std::string_view relative, std::string_view base)
	{
		const std::string_view rest = relative.substr(base.size() + 1);
		std::size_t begin = 0;
		while (true) {
			const std::size_t end = rest.find('/', begin);
			const bool last = end == std::string_view::npos;
			const std::string_view segment = rest.substr(begin, last ? std::string_view::npos : end - begin);
			if (res::is_skipped_asset_name(segment, !last)) return true;
			if (last) return res::is_asset_meta_sidecar(segment) || res::desc_binary_owner_name(segment).has_value();
			begin = end + 1;
		}
	}

	std::string_view step_kind_name(edt::journal_step_kind kind)
	{
		switch (kind) {
		case edt::journal_step_kind::move: return "move";
		case edt::journal_step_kind::rewrite: return "rewrite";
		case edt::journal_step_kind::stash: return "stash";
		case edt::journal_step_kind::make_folder: return "make_folder";
		case edt::journal_step_kind::remove_folder: return "remove_folder";
		}
		return "move";
	}

	bool is_folder_step(const edt::journal_step& step)
	{
		return step.kind == edt::journal_step_kind::make_folder || step.kind == edt::journal_step_kind::remove_folder;
	}

	std::optional<edt::journal_step_kind> step_kind_of(std::string_view name)
	{
		for (const edt::journal_step_kind kind : { edt::journal_step_kind::move, edt::journal_step_kind::rewrite, edt::journal_step_kind::stash,
			edt::journal_step_kind::make_folder, edt::journal_step_kind::remove_folder }) {
			if (step_kind_name(kind) == name) return kind;
		}
		return std::nullopt;
	}

	std::string step_label(const edt::journal_step& step)
	{
		return std::format("{} {}", step_kind_name(step.kind), step.from);
	}

	bool is_safe_id(std::string_view id)
	{
		if (id.empty() || id.size() > k_max_id_length) return false;
		return std::ranges::all_of(id, [](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '-' || ch == '_'; });
	}

	// A path that stays below the folder it is read against: no root, no drive, no '..', no empty or odd names.
	bool is_safe_relative(std::string_view path)
	{
		if (path.empty() || path.front() == '/' || path.find('\\') != std::string_view::npos) return false;
		return res::portable_path_problem(path).empty();
	}

	// The one spelling a root has in a journal: normalized, generic separators, no trailing slash. A journal's root is compared
	// to a root the ports name byte for byte in this spelling, so an alias of it (`..`, a link, another case) is another root.
	std::string canonical_root(const fs::path& root)
	{
		std::string text = utf8(root.lexically_normal());
		while (text.size() > 1 && text.back() == '/' && !text.ends_with(":/")) text.pop_back();
		return text;
	}

	bool is_canonical_root(const std::string& root)
	{
		if (root.empty() || root.find('\\') != std::string::npos || canonical_root(path_of(root)) != root) return false;
		return std::ranges::none_of(path_of(root), [](const fs::path& part) { return part == ".."; });
	}

	// Whether the journal of `plan` could grow past what is read back (k_journal_read_limit, with room to spare).
	std::optional<edt::refusal> journal_too_long(const edt::operation_plan& plan)
	{
		const std::uint64_t root = canonical_root(plan.root).size() + k_step_overhead;
		std::uint64_t bytes = 0;
		std::size_t steps = 0;
		const auto add = [&](std::size_t paths) {
			bytes += root + paths;
			++steps;
		};
		for (const edt::plan_item& item : plan.items) {
			for (const edt::planned_move& move : item.moves) add(move.from.size() + move.to.size());
			for (const edt::planned_rewrite& rewrite : item.rewrites) add(rewrite.relative.size() + canonical_root(rewrite.root).size());
		}
		for (const edt::planned_move& move : plan.carried) add(move.from.size() + move.to.size());
		for (const std::string& folder : plan.make_folders) add(folder.size());
		for (const std::string& folder : plan.remove_folders) add(folder.size());
		if (bytes <= edt::k_journal_read_limit / 2 && steps / k_steps_per_file < k_max_step_files) return std::nullopt;
		return stop(edt::refusal_code::too_many_files, std::format("the journal of the operation would take about {} bytes for {} steps: at most {} bytes of journal are read back",
			bytes, steps, edt::k_journal_read_limit / 2));
	}

	// A file at the step's source counts as put back only when it is the file the step moved (#1309): another file there
	// fails the step, so the moved or kept file is never given up for it. Empty: it is the step's file.
	std::string source_in_the_way(const edt::journal_step& step, const fs::path& root, std::string_view kept_where)
	{
		std::string error;
		const std::string now = hash_file(root, step.from, error);
		if (!error.empty()) return error;
		if (now == step.hash) return {};
		return std::format("a different file is at '{}' now: rename or delete that file, then undo again ({})",
			step.from, kept_where);
	}

	// Takes one step back, and tolerates a step that was written ahead but not (or only half) done: what is already as
	// it was before the step is left alone. Empty: done.
	std::string undo_step(const edt::journal_step& step, const fs::path& root, const fs::path& journal_root, const std::string& id, const move_seams& seams,
		std::vector<std::string>& warnings)
	{
		std::vector<std::byte> bytes;
		if (step.kind == edt::journal_step_kind::move || step.kind == edt::journal_step_kind::stash) {
			if (std::string error = injected_failure(seams, "before_undo:" + step.from); !error.empty()) return error;
		}
		switch (step.kind) {
		case edt::journal_step_kind::make_folder: {
			if (!exists_on_disk(root, step.from)) return {};
			std::string error;
			const bool empty = folder_is_empty(root, step.from, error);
			if (!error.empty()) return error;
			if (!empty) {
				warnings.push_back(std::format("the folder '{}' the operation made is not empty, so it stays", step.from));
				return {};
			}
			return remove_empty_folder(root, step.from);
		}
		case edt::journal_step_kind::remove_folder:
			if (is_folder(root, step.from)) return {};
			return make_folders(root, path_of(step.from));
		case edt::journal_step_kind::move:
			if (exists_on_disk(root, step.from)) {
				const std::string kept_where = std::format("the moved file stays at '{}'", step.to);
				if (std::string error = source_in_the_way(step, root, kept_where); !error.empty()) return error;
				// The source is back as it was, so a file still at the target is not the moved one: it stays.
				if (exists_on_disk(root, step.to)) {
					return std::format("'{}' is back but '{}' is there too: rename or delete '{}', then undo again",
						step.from, step.to, step.to);
				}
				return {};
			}
			return move_file(root, step.to, step.from);
		case edt::journal_step_kind::rewrite: {
			std::string error;
			const std::string now = hash_file(root, step.from, error);
			if (!error.empty()) return error;
			if (now == step.hash) return {};
			if (now != step.after_hash) return "it changed meanwhile, so the original is not put over it";
			const std::string original = in_journal(journal_root, id, step.original);
			std::string kept = read_file(journal_base(journal_root), original, edt::k_level_file_limit, bytes);
			if (!kept.empty()) return kept;
			if (hash_of(bytes) != step.hash) return "the kept original is not the file that was replaced";
			return write_file(root, step.from, bytes, true);
		}
		case edt::journal_step_kind::stash:
			if (exists_on_disk(root, step.from)) {
				return source_in_the_way(step, root, "the deleted file is kept in the journal");
			}
			return restore_file(journal_base(journal_root), in_journal(journal_root, id, step.to), root, step.from,
				step.hash, seams);
		}
		return {};
	}

	enum class step_state : std::uint8_t
	{
		// Recorded ahead and never done (a crash between the record and the action): the files are as before the step.
		not_done,
		// Done, and the files are as the step left them.
		done,
		// Something else changed them since: `file` names it.
		changed,
	};

	step_state state_of_step(const edt::journal_step& step, const fs::path& root, const fs::path& journal_root, const std::string& id, std::string& file)
	{
		std::string error;
		switch (step.kind) {
		case edt::journal_step_kind::make_folder:
		case edt::journal_step_kind::remove_folder: {
			std::error_code ec;
			const fs::file_type type = fs::symlink_status(root / path_of(step.from), ec).type();
			if (type == fs::file_type::not_found) {
				return step.kind == edt::journal_step_kind::make_folder ? step_state::not_done : step_state::done;
			}
			if (type == fs::file_type::directory) {
				return step.kind == edt::journal_step_kind::make_folder ? step_state::done : step_state::not_done;
			}
			file = step.from;
			return step_state::changed;
		}
		case edt::journal_step_kind::move: {
			const bool from_there = exists_on_disk(root, step.from);
			if (!exists_on_disk(root, step.to)) {
				if (from_there && hash_file(root, step.from, error) == step.hash && error.empty()) return step_state::not_done;
				file = from_there ? step.from : step.to;
				return step_state::changed;
			}
			if (from_there || hash_file(root, step.to, error) != step.hash || !error.empty()) {
				file = step.to;
				return step_state::changed;
			}
			return step_state::done;
		}
		case edt::journal_step_kind::rewrite: {
			const std::string now = hash_file(root, step.from, error);
			if (error.empty() && now == step.hash) return step_state::not_done;
			if (error.empty() && now == step.after_hash) return step_state::done;
			file = step.from;
			return step_state::changed;
		}
		case edt::journal_step_kind::stash:
			if (exists_on_disk(root, step.from)) {
				if (hash_file(root, step.from, error) == step.hash && error.empty()) return step_state::not_done;
				file = step.from;
				return step_state::changed;
			}
			const std::string kept = hash_file(journal_base(journal_root), in_journal(journal_root, id, step.to), error);
			if (kept != step.hash || !error.empty()) {
				file = step.to;
				return step_state::changed;
			}
			return step_state::done;
		}
		return step_state::changed;
	}

	json::object step_json(const edt::journal_step& step)
	{
		return json::object{ { "kind", std::string(step_kind_name(step.kind)) }, { "root", step.root }, { "from", step.from }, { "to", step.to },
			{ "hash", step.hash }, { "after_hash", step.after_hash }, { "original", step.original } };
	}

	std::string steps_file(std::size_t index)
	{
		return std::format("steps.{}.json", index);
	}

	// Writes the journal so that each step is on the disk before its action. journal.json holds the first k_steps_per_file steps,
	// each further run of them is a steps.<n>.json, so a step rewrites one bounded file, not the whole journal (`last_only`: only
	// what the newest step changed; the step file before journal.json, so journal.json never names a file not yet there).
	std::string persist(const edt::operation_journal& journal, const fs::path& journal_root, bool last_only = false)
	{
		const std::size_t count = journal.steps.size();
		const std::size_t more = count > k_steps_per_file ? (count - 1) / k_steps_per_file : 0;
		const auto steps_of = [&](std::size_t index) {
			json::array out;
			for (std::size_t i = index * k_steps_per_file; i < count && i < (index + 1) * k_steps_per_file; ++i) out.push_back(step_json(journal.steps[i]));
			return out;
		};
		const auto write = [&](const std::string& name, const json::value& value) {
			const std::string text = json::serialize(value);
			return write_file(journal_base(journal_root), in_journal(journal_root, journal.id, name),
				std::as_bytes(std::span<const char>(text.data(), text.size())), true);
		};
		const auto header = [&] {
			json::object out{ { "id", journal.id }, { "operation", std::string(edt::asset_operation_name(journal.operation)) },
				{ "directory", journal.directory }, { "steps", steps_of(0) } };
			if (more > 0) out["more_steps"] = static_cast<std::uint64_t>(more);
			return write(std::string(k_journal_file), out);
		};
		std::string error;
		if (!last_only) {
			for (std::size_t index = 1; index <= more && error.empty(); ++index) error = write(steps_file(index), steps_of(index));
			if (error.empty()) error = header();
		} else if (count <= k_steps_per_file) {
			error = header();
		} else {
			error = write(steps_file(more), steps_of(more));
			if (error.empty() && (count - 1) % k_steps_per_file == 0) error = header();
		}
		if (!error.empty()) egLOG_WARN("asset_ops", "the journal of '{}' was not written: {}", journal.id, error);
		return error.empty() ? std::string{} : std::format("the journal was not written: {}", error);
	}

	// Why a name the operation would give a file is not one a project lists: hidden, temporary or system names, a .resource, a
	// name that would be taken for a desc's binary file (`leaf_changes`: the file's own name is new).
	std::string destination_problem(std::string_view dest, const fs::path& root, bool leaf_changes)
	{
		const bool project_layer = res::is_project_folder_layer(root);
		std::size_t begin = 0;
		while (true) {
			const std::size_t end = dest.find('/', begin);
			const bool last = end == std::string_view::npos;
			const std::string_view segment = dest.substr(begin, last ? std::string_view::npos : end - begin);
			if (res::is_skipped_asset_entry(segment, !last, begin == 0 && project_layer)) {
				return std::format("'{}' is a name the project does not list (hidden, temporary, system or the project's own)", segment);
			}
			if (last) {
				if (leaf_changes && res::is_asset_meta_name(segment)) return std::format("'{}' is a .resource name: those belong to the file before it", segment);
				if (leaf_changes && res::desc_binary_owner_name(segment)) return std::format("'{}' is the name of a desc's binary file, which no one names by hand", segment);
				break;
			}
			begin = end + 1;
		}
		const std::string folder = parent_of(dest);
		if (project_layer && !folder.empty() && res::is_in_project_own_folder(path_of(folder))) {
			return std::format("'{}' is in a folder of the project's own files", folder);
		}
		return {};
	}

	// A file the project does not list (hidden, temporary) moved along with its folder: `from` is <folder>/<rest> and `to`
	// <other folder>/<rest>, both folders names the project lists.
	bool carried_along(std::string_view from, std::string_view to, const fs::path& root)
	{
		if (!is_safe_relative(from) || !is_safe_relative(to)) return false;
		for (std::size_t slash = from.find('/'); slash != std::string_view::npos; slash = from.find('/', slash + 1)) {
			const std::string_view rest = from.substr(slash);
			if (to.size() <= rest.size() || !to.ends_with(rest)) continue;
			const std::string_view to_folder = to.substr(0, to.size() - rest.size());
			if (destination_problem(from.substr(0, slash), root, false).empty() && destination_problem(to_folder, root, false).empty()) return true;
		}
		return false;
	}

	// A folder step names a safe path below its root whose first folder is one the project lists (a hidden folder below it
	// moves with it).
	bool is_step_folder(std::string_view folder, const fs::path& root)
	{
		if (!is_safe_relative(folder)) return false;
		return destination_problem(folder.substr(0, folder.find('/')), root, false).empty();
	}

	// What a step may do under the root it is read against: names the project lists, as a live operation checks them.
	std::string step_problem(const edt::journal_step& step, const fs::path& root)
	{
		const auto listed = [&](const std::string& relative) { return is_safe_relative(relative) && destination_problem(relative, root, false).empty(); };
		switch (step.kind) {
		case edt::journal_step_kind::make_folder:
		case edt::journal_step_kind::remove_folder:
			if (!is_step_folder(step.from, root) || !step.to.empty() || !step.hash.empty() || !step.after_hash.empty() || !step.original.empty()) {
				return std::format("the {} of '{}' is not of a folder below its root, or carries what a folder step has not", step_kind_name(step.kind), step.from);
			}
			break;
		case edt::journal_step_kind::move:
			if ((!(listed(step.from) && listed(step.to)) && !carried_along(step.from, step.to, root)) || !step.original.empty()) {
				return std::format("the move of '{}' is not between two names of the project's files", step.from);
			}
			break;
		case edt::journal_step_kind::rewrite: {
			// An operation rewrites descs and the project manifest only.
			const bool manifest = step.from == prj::k_manifest_file_name;
			const bool desc = step.from.ends_with(".desc") && listed(step.from);
			if ((!manifest && !desc) || !step.to.empty() || !step.original.starts_with("originals/") || !is_safe_relative(step.original)) {
				return std::format("the rewrite of '{}' is not of a desc or the manifest, or has a path that is not below its root or the journal folder", step.from);
			}
			break;
		}
		case edt::journal_step_kind::stash:
			if (!listed(step.from) || step.to != join("files", step.from) || !step.original.empty()) {
				return std::format("the removal of '{}' is not of a file of the project, or its copy is not where it is kept", step.from);
			}
			break;
		}
		return {};
	}
}

std::string_view edt::asset_operation_name(asset_operation operation)
{
	switch (operation) {
	case asset_operation::rename: return "rename";
	case asset_operation::move: return "move";
	case asset_operation::remove: return "delete";
	}
	return "rename";
}

std::string_view edt::refusal_code_name(refusal_code code)
{
	switch (code) {
	case refusal_code::one_asset_only: return "one_asset_only";
	case refusal_code::no_database: return "no_database";
	case refusal_code::database_not_ready: return "database_not_ready";
	case refusal_code::not_res_path: return "not_res_path";
	case refusal_code::not_found: return "not_found";
	case refusal_code::engine_layer: return "engine_layer";
	case refusal_code::not_top_layer: return "not_top_layer";
	case refusal_code::meta_in_other_layer: return "meta_in_other_layer";
	case refusal_code::bad_name: return "bad_name";
	case refusal_code::no_such_folder: return "no_such_folder";
	case refusal_code::unportable_path: return "unportable_path";
	case refusal_code::no_change: return "no_change";
	case refusal_code::case_only_rename: return "case_only_rename";
	case refusal_code::target_exists: return "target_exists";
	case refusal_code::binary_name_lost: return "binary_name_lost";
	case refusal_code::usage_scan_skipped: return "usage_scan_skipped";
	case refusal_code::referrer_open: return "referrer_open";
	case refusal_code::has_referrers: return "has_referrers";
	case refusal_code::stale_plan: return "stale_plan";
	case refusal_code::io_error: return "io_error";
	case refusal_code::binary_pair_blocked: return "binary_pair_blocked";
	case refusal_code::unrewritable_references: return "unrewritable_references";
	case refusal_code::mixed_operations: return "mixed_operations";
	case refusal_code::overlapping_requests: return "overlapping_requests";
	case refusal_code::into_itself: return "into_itself";
	case refusal_code::folder_in_lower_layer: return "folder_in_lower_layer";
	case refusal_code::folder_has_foreign_entries: return "folder_has_foreign_entries";
	case refusal_code::too_many_files: return "too_many_files";
	}
	return "io_error";
}

json::value edt::journal_to_json(const operation_journal& journal)
{
	json::array steps;
	for (const journal_step& step : journal.steps) steps.push_back(step_json(step));
	return json::object{ { "id", journal.id }, { "operation", std::string(asset_operation_name(journal.operation)) },
		{ "directory", journal.directory }, { "steps", std::move(steps) } };
}

std::optional<json::value> edt::read_journal(const fs::path& journal_root, std::string_view id, std::string& reason)
{
	if (!is_safe_id(id)) {
		reason = "the id is not one plain name";
		return std::nullopt;
	}
	std::uint64_t budget = k_journal_read_limit;
	const auto read = [&](const std::string& name) -> std::optional<json::value> {
		std::vector<std::byte> bytes;
		if (const std::string error = read_file(journal_base(journal_root), in_journal(journal_root, std::string(id), name), budget, bytes); !error.empty()) {
			reason = std::format("'{}' cannot be read (or is bigger than {} bytes with the rest of the journal): {}", name, k_journal_read_limit, error);
			return std::nullopt;
		}
		budget -= bytes.size();
		json::error_code ec;
		json::value value = json::parse(json::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), ec);
		if (ec) {
			reason = std::format("'{}' is not JSON", name);
			return std::nullopt;
		}
		return value;
	};
	std::optional<json::value> journal = read(std::string(k_journal_file));
	if (!journal) return std::nullopt;
	json::object* object = journal->if_object();
	json::value* steps = object ? object->if_contains("steps") : nullptr;
	const json::value* more = object ? object->if_contains("more_steps") : nullptr;
	if (!more) return journal;
	const std::optional<std::uint64_t> files = more->is_int64() && more->as_int64() > 0 ? std::optional<std::uint64_t>(static_cast<std::uint64_t>(more->as_int64()))
		: more->is_uint64() ? std::optional<std::uint64_t>(more->as_uint64()) : std::nullopt;
	if (!steps || !steps->is_array() || !files || *files == 0 || *files > k_max_step_files) {
		reason = "the journal names its other step files wrongly";
		return std::nullopt;
	}
	for (std::uint64_t index = 1; index <= *files; ++index) {
		std::optional<json::value> part = read(steps_file(static_cast<std::size_t>(index)));
		if (!part) return std::nullopt;
		if (!part->is_array()) {
			reason = std::format("'{}' is not a list of steps", steps_file(static_cast<std::size_t>(index)));
			return std::nullopt;
		}
		for (json::value& step : part->as_array()) steps->as_array().push_back(std::move(step));
	}
	object->erase("more_steps");
	return journal;
}

std::string edt::journal_problem(const operation_journal& journal)
{
	if (!is_safe_id(journal.id)) return "the id is not one plain name";
	for (const journal_step& step : journal.steps) {
		if (step.root.empty()) return "a step has no root";
		if (!is_canonical_root(step.root)) return std::format("the root '{}' is not written plainly (normalized, no '..', no trailing slash)", step.root);
		if (std::string problem = step_problem(step, path_of(step.root)); !problem.empty()) return problem;
	}
	return {};
}

std::optional<edt::operation_journal> edt::journal_from_json(const json::value& value)
{
	std::string reason;
	return journal_from_json(value, reason);
}

std::optional<edt::operation_journal> edt::journal_from_json(const json::value& value, std::string& reason)
{
	const json::object* object = value.if_object();
	if (!object) {
		reason = "not a JSON object";
		return std::nullopt;
	}
	const auto text = [](const json::object& from, std::string_view key) -> std::optional<std::string> {
		const json::value* found = from.if_contains(json::string_view(key.data(), key.size()));
		if (!found || !found->is_string()) return std::nullopt;
		return std::string(view_of(found->as_string()));
	};
	const std::optional<std::string> id = text(*object, "id");
	const std::optional<std::string> operation = text(*object, "operation");
	const std::optional<std::string> directory = text(*object, "directory");
	const json::value* steps = object->if_contains("steps");
	if (!id || !operation || !directory || !steps || !steps->is_array()) {
		reason = "a field of the journal is missing";
		return std::nullopt;
	}
	if (object->contains("more_steps")) {
		reason = "only the first steps of the journal are here: read it with read_journal";
		return std::nullopt;
	}

	operation_journal out;
	out.id = *id;
	out.directory = *directory;
	const auto named = { asset_operation::rename, asset_operation::move, asset_operation::remove };
	const auto kind = std::ranges::find_if(named, [&](asset_operation candidate) { return asset_operation_name(candidate) == *operation; });
	if (kind == named.end()) {
		reason = "an operation the journal does not know";
		return std::nullopt;
	}
	out.operation = *kind;
	for (const json::value& item : steps->as_array()) {
		const json::object* step = item.if_object();
		if (!step) {
			reason = "a step is not a JSON object";
			return std::nullopt;
		}
		const std::optional<std::string> step_kind = text(*step, "kind");
		const std::optional<std::string> root = text(*step, "root");
		const std::optional<std::string> from = text(*step, "from");
		const std::optional<std::string> to = text(*step, "to");
		const std::optional<std::string> hash = text(*step, "hash");
		const std::optional<std::string> after_hash = text(*step, "after_hash");
		const std::optional<std::string> original = text(*step, "original");
		if (!step_kind || !root || !from || !to || !hash || !after_hash || !original) {
			reason = "a field of a step is missing";
			return std::nullopt;
		}
		const std::optional<journal_step_kind> parsed = step_kind_of(*step_kind);
		if (!parsed) {
			reason = "a step of a kind the journal does not know";
			return std::nullopt;
		}
		out.steps.push_back(journal_step{ *parsed, *root, *from, *to, *hash, *after_hash, *original });
	}
	reason = journal_problem(out);
	if (!reason.empty()) return std::nullopt;
	return out;
}

bool edt::is_engine_layer(const std::vector<fs::path>& layers, std::size_t layer, const fs::path& engine_base)
{
	if (engine_base.empty() || layer >= layers.size()) return false;
	std::error_code ec;
	const fs::path a = fs::weakly_canonical(layers[layer], ec);
	const fs::path b = ec ? fs::path{} : fs::weakly_canonical(engine_base, ec);
	if (ec) return layers[layer].lexically_normal() == engine_base.lexically_normal();
	return a == b;
}

bool edt::is_open_with_unsaved_changes(const document* open, const document& scene, bool is_editor_copy)
{
	return is_editor_copy || open == &scene || (open != nullptr && open->is_dirty());
}

edt::asset_ops::asset_ops(asset_ops_ports ports)
	: m_ports(std::move(ports))
{
}

edt::operation_plan edt::asset_ops::plan(std::span<const asset_request> requests) const
{
	operation_plan out;
	out.refused = plan_into(requests, out);
	if (out.refused) {
		out.items.clear();
		out.carried.clear();
		out.make_folders.clear();
		out.remove_folders.clear();
	}
	return out;
}

std::optional<edt::refusal> edt::asset_ops::plan_into(std::span<const asset_request> requests, operation_plan& out) const
{
	if (requests.empty()) return stop(refusal_code::bad_name, "nothing was asked: name a file or a folder");
	const asset_operation operation = requests.front().operation;
	out.operation = operation;
	if (std::ranges::any_of(requests, [operation](const asset_request& request) { return request.operation != operation; })) {
		return stop(refusal_code::mixed_operations, "one operation at a time: a plan renames, moves or deletes, never a mix of them");
	}
	if (operation == asset_operation::rename && requests.size() != 1) {
		return stop(refusal_code::bad_name, "a rename takes one file or one folder");
	}
	const asset_database* db = m_ports.assets ? m_ports.assets() : nullptr;
	if (!db) return stop(refusal_code::no_database, "no project is open: there is no asset database");
	// A file the database has not read yet would be taken for one it never lists, and moved without its pair and its references.
	if (!db->walked_once() || db->pending_hashes() > 0) {
		return stop(refusal_code::database_not_ready, "the asset database is still reading the project's files: try again when it is done");
	}
	const bool removing = operation == asset_operation::remove;
	const bool fold = db->case_insensitive();
	const std::vector<fs::path>& layers = db->layers();
	const std::size_t top = db->top_layer();
	const bool has_top = top < layers.size();
	if (has_top) out.root = layers[top];
	const asset_ops_limits& limits = m_ports.limits;
	const auto key_of = [fold](std::string_view relative) { return res::asset_name_key(relative, fold); };
	const auto shown = [](std::string_view relative) { return std::string(res::tag::make(relative).view()); };

	// The files of the requested folders, by key: what each becomes and which request it came with.
	struct walked_file
	{
		std::string from;
		std::string to;
		std::string folder;
		std::string request_to;
		// The requested folder it was found in, below the layer.
		std::string base;
	};
	std::vector<walked_file> walked;
	std::map<std::string, std::size_t> walked_at;
	std::map<std::string, std::uint64_t> sizes;
	std::uint64_t walked_bytes = 0;
	// Every folder of the requested folders, by key: where it goes ("" for a delete).
	std::map<std::string, std::string> folder_to;
	std::vector<std::string> source_folders;
	std::vector<std::string> odd;
	std::vector<const asset_request*> file_requests;
	// Keys of the requested folders, and of the folders a move puts something into: one may not lie in the other.
	std::vector<std::string> moving_folders;
	std::vector<std::pair<std::string, std::string>> targets;
	const auto into_folder = [](std::string_view folder) {
		if (folder.starts_with("res://")) folder.remove_prefix(6);
		while (folder.starts_with('/')) folder.remove_prefix(1);
		while (folder.ends_with('/')) folder.remove_suffix(1);
		return std::string(folder);
	};

	for (const asset_request& request : requests) {
		if (request.asset.protocol() != res::tag::default_protocol()) {
			return stop(refusal_code::not_res_path, std::format("only res:// assets are changed, not '{}'", request.asset.view()));
		}
		std::string relative(request.asset.relative());
		while (relative.ends_with('/')) relative.pop_back();
		if (relative.empty()) return stop(refusal_code::bad_name, "res:// itself is not renamed, moved or deleted");
		if (operation == asset_operation::move) targets.emplace_back(key_of(into_folder(request.to)), relative);
		if (!has_top || db->find_any_case(request.asset)) {
			file_requests.push_back(&request);
			continue;
		}
		// Only a name that passed these checks is looked at on the disk, and never through a link.
		const bool safe = is_safe_relative(relative) && destination_problem(relative, out.root, false).empty();
		std::error_code ec;
		const fs::file_type type = safe ? fs::symlink_status(out.root / path_of(relative), ec).type() : fs::file_type::not_found;
		if (type == fs::file_type::symlink) {
			return stop(refusal_code::folder_has_foreign_entries, std::format("'{}' is a link: a link is never followed nor moved", shown(relative)));
		}
		if (type != fs::file_type::directory) {
			for (std::size_t layer = 0; safe && layer < layers.size(); ++layer) {
				if (layer == top || !is_folder(layers[layer], relative)) continue;
				const bool engine = is_engine_layer(layers, layer, m_ports.engine_base);
				return stop(engine ? refusal_code::engine_layer : refusal_code::not_top_layer,
					std::format("'{}' is a folder of {} only, not of the project's own folder", shown(relative), engine ? "the engine" : "a lower layer"));
			}
			// plan_one says what it is (not_found).
			file_requests.push_back(&request);
			continue;
		}

		std::string dest;
		if (!removing) {
			std::string into;
			if (operation == asset_operation::rename) {
				if (request.to.empty() || request.to.find_first_of("/\\") != std::string::npos) {
					return stop(refusal_code::bad_name, "a new name is one folder name, without a folder");
				}
				dest = join(parent_of(relative), request.to);
			} else {
				std::string_view folder = request.to;
				if (folder.starts_with("res://")) folder.remove_prefix(6);
				while (folder.starts_with('/')) folder.remove_prefix(1);
				while (folder.ends_with('/')) folder.remove_suffix(1);
				if (folder.find('\\') != std::string_view::npos) return stop(refusal_code::bad_name, "a folder is written with '/'");
				const std::string source_key = key_of(relative);
				const std::string into_key = key_of(folder);
				if (into_key == source_key || into_key.starts_with(source_key + "/")) {
					return stop(refusal_code::into_itself, std::format("'{}' cannot go into itself or a folder inside it", shown(relative)));
				}
				into = std::string(folder);
				const std::size_t slash = relative.find_last_of('/');
				dest = join(into, slash == std::string::npos ? std::string_view(relative) : std::string_view(relative).substr(slash + 1));
			}
			if (const std::string problem = res::portable_path_problem(dest); !problem.empty()) {
				return stop(refusal_code::unportable_path, std::format("'{}': {}", dest, problem));
			}
			if (const std::string problem = destination_problem(dest, out.root, operation == asset_operation::rename); !problem.empty()) {
				return stop(refusal_code::bad_name, std::format("'{}': {}", dest, problem));
			}
			if (!into.empty() && !is_folder(out.root, into)) {
				return stop(refusal_code::no_such_folder, std::format("'res://{}' is not a folder of the project's own folder", into));
			}
			if (dest == relative) return stop(refusal_code::no_change, "the folder is there already under that name");
			if (fold && key_of(dest) == key_of(relative)) {
				return stop(refusal_code::case_only_rename, "names are the same folder in any letter case here, so a change of case alone is not done");
			}
			if (exists_named(out.root, dest, fold) || db->find_any_case(res::tag::make(dest))) {
				return stop(refusal_code::target_exists, std::format("'{}' is taken", shown(dest)));
			}
			// The folder would show the lower layer's files under it too, and its own would cover theirs.
			for (std::size_t layer = 0; layer < layers.size(); ++layer) {
				if (layer == top || !is_folder(layers[layer], dest)) continue;
				return stop(refusal_code::folder_in_lower_layer, std::format("'{}' is a folder of {}: the folder would mix with its files there",
					shown(dest), is_engine_layer(layers, layer, m_ports.engine_base) ? "the engine" : "a lower layer"));
			}
		}
		moving_folders.push_back(key_of(relative));

		for (std::size_t layer = 0; layer < layers.size(); ++layer) {
			if (layer == top || !is_folder(layers[layer], relative)) continue;
			refusal why = stop(refusal_code::folder_in_lower_layer, {});
			const std::string prefix = key_of(relative) + "/";
			for (const auto& entry : db->records()) {
				const asset_record& record = entry.second;
				if (!key_of(record.path.relative()).starts_with(prefix)) continue;
				if (record.layer == top && record.shadowed_layers.empty()) continue;
				why.entries.emplace_back(record.path.view());
			}
			why.reason = std::format("'{}' is also a folder of {}: {} would stay visible under the old path", shown(relative),
				is_engine_layer(layers, layer, m_ports.engine_base) ? "the engine" : "a lower layer",
				why.entries.empty() ? std::string("the folder") : first_names(why.entries));
			return why;
		}

		folder_listing listing;
		if (const std::string error = list_folder(out.root, relative, limits.max_files, listing); !error.empty()) {
			return stop(refusal_code::io_error, std::format("'{}': {}", shown(relative), error));
		}
		if (listing.truncated) return too_many(std::format("more than {}", limits.max_files), walked_bytes, limits);
		const auto mapped = [&](const std::string& path) { return removing ? std::string{} : dest + path.substr(relative.size()); };
		for (const std::string& folder : listing.folders) {
			const auto [at, added] = folder_to.emplace(key_of(folder), mapped(folder));
			if (!added) {
				if (at->second != mapped(folder)) {
					return stop(refusal_code::overlapping_requests, std::format("'{}' is asked to go to two places", shown(folder)));
				}
				continue;
			}
			source_folders.push_back(folder);
			if (!removing) out.make_folders.push_back(mapped(folder));
		}
		for (const std::string& file : listing.files) {
			const auto [at, added] = walked_at.emplace(key_of(file), walked.size());
			if (!added) {
				if (walked[at->second].to != mapped(file)) {
					return stop(refusal_code::overlapping_requests, std::format("'{}' is asked to go to two places", shown(file)));
				}
				continue;
			}
			walked.push_back(walked_file{ file, mapped(file), shown(relative), request.to, relative });
			if (walked.size() + source_folders.size() > limits.max_files) return too_many(std::format("more than {}", limits.max_files), walked_bytes, limits);
			const std::optional<std::uint64_t> size = size_of(out.root, file);
			if (!size) return stop(refusal_code::io_error, std::format("'{}' cannot be read", shown(file)));
			sizes[key_of(file)] = *size;
			walked_bytes += *size;
		}
		odd.insert(odd.end(), listing.odd.begin(), listing.odd.end());
	}
	if (!odd.empty()) {
		refusal why = stop(refusal_code::folder_has_foreign_entries, std::format("{} link(s) or entries that are no file or folder: {}; they are never followed nor moved",
			odd.size(), first_names(odd)));
		for (const std::string& entry : odd) why.entries.push_back(shown(entry));
		return why;
	}
	if (walked_bytes > limits.max_bytes) return too_many(std::to_string(walked.size() + source_folders.size()), walked_bytes, limits);
	// What goes into a folder that moves itself would end up in a place neither request named.
	for (const auto& [into, asked_for] : targets) {
		for (const std::string& folder : moving_folders) {
			if (into != folder && !into.starts_with(folder + "/")) continue;
			return stop(refusal_code::overlapping_requests, std::format("'{}' goes into '{}', which is moved itself", shown(asked_for), shown(into)));
		}
	}

	// What to plan as one asset each: the folders' files that are assets, then the files asked by themselves.
	struct candidate
	{
		asset_request request;
		std::optional<std::string> to;
		std::string folder;
	};
	std::vector<candidate> candidates;
	for (const walked_file& file : walked) {
		const res::tag tag = res::tag::make(file.from);
		const asset_record* record = db->find_any_case(tag);
		if (!record || record->layer != top || key_of(record->path.relative()) != key_of(file.from)) continue;
		candidates.push_back(candidate{ asset_request{ operation, tag, file.request_to }, removing ? std::nullopt : std::optional<std::string>(file.to), file.folder });
	}
	std::set<std::string> asked;
	for (const asset_request* request : file_requests) {
		if (const asset_record* record = db->find_any_case(request->asset)) asked.insert(key_of(record->path.relative()));
	}
	for (const asset_request* request : file_requests) {
		// A .resource or a desc's binary file asked with its file (or its folder) goes with it: nothing more to plan.
		if (!db->find_any_case(request->asset)) {
			if (const std::optional<std::string> owner = companion_owner(request->asset.relative())) {
				const std::string owner_key = key_of(*owner);
				if (asked.contains(owner_key) || walked_at.contains(owner_key)) continue;
			}
		}
		candidates.push_back(candidate{ *request, std::nullopt, {} });
	}
	if (walked.size() + source_folders.size() + file_requests.size() > limits.max_files) {
		return too_many(std::format("at least {}", walked.size() + source_folders.size() + file_requests.size()), walked_bytes, limits);
	}

	usage_index usages;
	std::vector<item_facts> facts;
	std::map<std::string, std::size_t> planned_at;
	for (candidate& next : candidates) {
		plan_item item;
		item.request = std::move(next.request);
		item.folder = std::move(next.folder);
		item_facts fact;
		if (std::optional<refusal> why = plan_one(item, next.to, usages, fact)) return why;
		const auto [at, added] = planned_at.emplace(key_of(fact.relative), out.items.size());
		if (!added) {
			if (out.items[at->second].target.view() != item.target.view()) {
				return stop(refusal_code::overlapping_requests, std::format("'{}' is asked to go to two places", fact.path));
			}
			continue;
		}
		out.items.push_back(std::move(item));
		facts.push_back(std::move(fact));
	}

	std::set<std::string> covered;
	for (const plan_item& item : out.items) {
		for (const planned_move& move : item.moves) covered.insert(key_of(move.from));
	}
	std::vector<std::string> foreign;
	std::vector<std::string> unread;
	for (const walked_file& file : walked) {
		if (covered.contains(key_of(file.from))) continue;
		if (!never_listed(file.from, file.base)) {
			unread.push_back(shown(file.from));
			continue;
		}
		if (removing) {
			foreign.push_back(shown(file.from));
			continue;
		}
		std::string error;
		std::string hash = hash_file(out.root, file.from, error);
		if (!error.empty()) return stop(refusal_code::io_error, std::format("'{}': {}", shown(file.from), error));
		out.carried.push_back(planned_move{ file.from, file.to, std::move(hash) });
	}
	if (!unread.empty()) {
		refusal why = stop(refusal_code::database_not_ready, std::format("the asset database has not read {} file(s) of the folder yet: {}; try again when it has",
			unread.size(), first_names(unread)));
		why.entries = std::move(unread);
		return why;
	}
	// What the project does not list is not known to be nobody's: a delete never removes it.
	if (!foreign.empty()) {
		refusal why = stop(refusal_code::folder_has_foreign_entries, std::format("{} file(s) of the folder are no asset of the project: {}; a delete removes only what it knows",
			foreign.size(), first_names(foreign)));
		why.entries = std::move(foreign);
		return why;
	}

	std::size_t count = out.carried.size() + source_folders.size();
	std::uint64_t bytes = 0;
	const auto add_size = [&](const planned_move& move) -> bool {
		const auto known = sizes.find(key_of(move.from));
		const std::optional<std::uint64_t> size = known != sizes.end() ? std::optional<std::uint64_t>(known->second) : size_of(out.root, move.from);
		if (!size) return false;
		bytes += *size;
		return true;
	};
	for (const plan_item& item : out.items) {
		count += item.moves.size();
		for (const planned_move& move : item.moves) {
			if (!add_size(move)) return stop(refusal_code::io_error, std::format("'{}' cannot be read", shown(move.from)));
		}
	}
	for (const planned_move& move : out.carried) {
		if (!add_size(move)) return stop(refusal_code::io_error, std::format("'{}' cannot be read", shown(move.from)));
	}
	if (count > limits.max_files || bytes > limits.max_bytes) return too_many(std::to_string(count), bytes, limits);

	// Parents first for the folders made, children first for the folders removed.
	const auto depth = [](const std::string& path) { return std::ranges::count(path, '/'); };
	std::ranges::stable_sort(out.make_folders, {}, depth);
	out.remove_folders = std::move(source_folders);
	std::ranges::stable_sort(out.remove_folders, std::ranges::greater{}, depth);

	if (removing) {
		// A file the delete takes away does not count as a referrer: it goes too.
		for (plan_item& item : out.items) {
			std::erase_if(item.referrers, [&](const asset_usage& usage) {
				return usage.kind != usage_kind::manifest && covered.contains(key_of(res::tag{ usage.file }.relative()));
			});
			item.needs_force = !item.referrers.empty();
		}
		return journal_too_long(out);
	}
	std::set<std::string> taken;
	const auto check_target = [&](const planned_move& move) -> std::optional<refusal> {
		if (taken.insert(key_of(move.to)).second) return std::nullopt;
		return stop(refusal_code::target_exists, std::format("two files would go to '{}'", shown(move.to)));
	};
	for (const plan_item& item : out.items) {
		for (const planned_move& move : item.moves) {
			if (std::optional<refusal> why = check_target(move)) return why;
		}
	}
	for (const planned_move& move : out.carried) {
		if (std::optional<refusal> why = check_target(move)) return why;
	}
	if (std::optional<refusal> why = plan_rewrites(out.items, facts)) return why;
	return journal_too_long(out);
}

std::optional<edt::refusal> edt::asset_ops::plan_one(plan_item& item, const std::optional<std::string>& to, usage_index& usages, item_facts& facts) const
{
	const asset_database* db = m_ports.assets ? m_ports.assets() : nullptr;
	if (!db) return stop(refusal_code::no_database, "no project is open: there is no asset database");
	const asset_request& request = item.request;
	const bool removing = request.operation == asset_operation::remove;
	if (request.asset.protocol() != res::tag::default_protocol()) {
		return stop(refusal_code::not_res_path, std::format("only res:// assets are changed, not '{}'", request.asset.view()));
	}
	const asset_record* record = db->find_any_case(request.asset);
	if (!record) return stop(refusal_code::not_found, std::format("'{}' is not a file of the project", request.asset.view()));
	const std::vector<fs::path>& layers = db->layers();
	if (is_engine_layer(layers, record->layer, m_ports.engine_base)) {
		return stop(refusal_code::engine_layer, std::format("'{}' belongs to the engine and is never changed", record->path.view()));
	}
	if (record->layer != db->top_layer()) {
		return stop(refusal_code::not_top_layer, std::format("'{}' is not in the project's own folder", record->path.view()));
	}
	if (record->has_meta && record->meta_layer != record->layer) {
		return stop(refusal_code::meta_in_other_layer, std::format("the .resource of '{}' is in another layer than the file", record->path.view()));
	}
	item.root = layers[record->layer];
	const std::string source = std::string(record->path.relative());
	const std::string meta_suffix{ res::asset_meta_suffix };

	if (m_ports.is_open_with_unsaved_changes && m_ports.is_open_with_unsaved_changes(record->path)) {
		return stop(refusal_code::referrer_open, std::format("'{}' is open with unsaved changes", record->path.view()));
	}

	std::string dest;
	std::string move_folder;
	if (!removing) {
		if (to) {
			dest = *to;
		} else if (request.operation == asset_operation::rename) {
			if (request.to.empty() || request.to.find_first_of("/\\") != std::string::npos) {
				return stop(refusal_code::bad_name, "a new name is one file name, without a folder");
			}
			dest = join(parent_of(source), request.to);
		} else {
			std::string_view folder = request.to;
			if (folder.starts_with("res://")) folder.remove_prefix(6);
			while (folder.starts_with('/')) folder.remove_prefix(1);
			while (folder.ends_with('/')) folder.remove_suffix(1);
			if (folder.find('\\') != std::string_view::npos) return stop(refusal_code::bad_name, "a folder is written with '/'");
			dest = join(folder, record->path.name());
			move_folder = std::string(folder);
		}
		if (const std::string problem = res::portable_path_problem(dest); !problem.empty()) {
			return stop(refusal_code::unportable_path, std::format("'{}': {}", dest, problem));
		}
		if (const std::string problem = destination_problem(dest, item.root, !to && request.operation == asset_operation::rename); !problem.empty()) {
			return stop(refusal_code::bad_name, std::format("'{}': {}", dest, problem));
		}
		// Only a folder name that passed the checks above is looked at on the disk, and never through a link.
		if (!move_folder.empty()) {
			std::error_code ec;
			if (fs::symlink_status(item.root / path_of(move_folder), ec).type() != fs::file_type::directory) {
				return stop(refusal_code::no_such_folder, std::format("'res://{}' is not a folder of the project's own folder", move_folder));
			}
		}
		if (dest == source) return stop(refusal_code::no_change, "the file is there already under that name");
		if (db->case_insensitive() && res::asset_name_key(dest, true) == res::asset_name_key(source, true)) {
			return stop(refusal_code::case_only_rename, "names are the same file in any letter case here, so a change of case alone is not done");
		}
		item.target = res::tag::make(dest);
		if (db->find_any_case(item.target) || exists_on_disk(item.root, dest)) {
			return stop(refusal_code::target_exists, std::format("'{}' is taken", item.target.view()));
		}
		// A .resource left at the new name would be taken by the new file, whether or not the old one had one.
		if (exists_named(item.root, dest + meta_suffix, db->case_insensitive())) {
			return stop(refusal_code::target_exists, std::format("'{}' is taken", res::tag::make(dest + meta_suffix).view()));
		}
	}

	// The files that go together: the file, its .resource, and for a desc its binary file with that one's .resource.
	struct pair_file { std::string from; std::string to; };
	std::vector<pair_file> pairs;
	const std::optional<res::tag> binary = res::desc_binary::binary_tag(record->path);
	const std::optional<res::tag> dest_binary = removing ? std::nullopt : res::desc_binary::binary_tag(item.target);
	const std::string binary_source = binary ? std::string(binary->relative()) : std::string{};
	const bool binary_here = binary && is_regular(item.root, binary_source);
	if (binary) {
		if (!binary_here) {
			for (std::size_t lower = 0; lower < layers.size(); ++lower) {
				if (lower == record->layer || !is_regular(layers[lower], binary_source)) continue;
				return stop(refusal_code::binary_pair_blocked, std::format("the binary file '{}' of '{}' is in a lower layer, so it cannot go with the desc", binary->view(), record->path.view()));
			}
		}
		if (const std::optional<res::tag> staged = res::desc_binary::staged_tag(record->path); staged && exists_on_disk(item.root, std::string(staged->relative()))) {
			return stop(refusal_code::binary_pair_blocked, std::format("'{}' is there: a save of '{}' is not finished", staged->view(), record->path.view()));
		}
		if (binary_here) {
			const usage_report binary_usages = usages.find(*db, m_ports.descs ? m_ports.descs() : nullptr, usage_target{ *binary, std::nullopt });
			refusal shared = stop(refusal_code::binary_pair_blocked, std::format("something other than the desc refers to its binary file '{}'", binary->view()));
			std::ranges::copy_if(binary_usages.items, std::back_inserter(shared.referrers), [&](const asset_usage& usage) { return usage.file != record->path.view(); });
			if (!shared.referrers.empty()) return shared;
		}
	}
	if (binary_here && !removing && !dest_binary) {
		return stop(refusal_code::binary_name_lost, std::format("the binary file of '{}' goes with a desc named <name>.<kind>.desc, which '{}' is not",
			record->path.view(), item.target.view()));
	}
	const auto add_desc = [&]() {
		pairs.push_back({ source, dest });
		if (is_regular(item.root, source + meta_suffix)) pairs.push_back({ source + meta_suffix, removing ? std::string{} : dest + meta_suffix });
	};
	const auto add_binary = [&]() {
		if (!binary_here) return;
		const std::string binary_dest = removing ? std::string{} : std::string(dest_binary->relative());
		pairs.push_back({ binary_source, binary_dest });
		if (is_regular(item.root, binary_source + meta_suffix)) pairs.push_back({ binary_source + meta_suffix, removing ? std::string{} : binary_dest + meta_suffix });
	};
	// A rename or a move puts the binary file first, then the desc; a delete the desc first, so an interrupted operation never
	// leaves a desc that has lost its binary file by a step of its own. The journal takes either order back.
	if (removing) {
		add_desc();
		add_binary();
	} else {
		add_binary();
		add_desc();
	}
	if (dest_binary) {
		// A binary file, a staged one or a .resource of one left at the new name would be taken by the new desc, with or
		// without a binary file of the old one.
		const std::optional<res::tag> dest_staged = res::desc_binary::staged_tag(item.target);
		std::vector<std::string> strays{ std::string(dest_binary->relative()), std::string(dest_binary->relative()) + meta_suffix };
		if (dest_staged) strays.emplace_back(dest_staged->relative());
		for (const std::string& stray : strays) {
			if (exists_named(item.root, stray, db->case_insensitive())) return stop(refusal_code::target_exists, std::format("'{}' is taken", res::tag::make(stray).view()));
		}
	}
	for (pair_file& pair : pairs) {
		if (!removing && exists_on_disk(item.root, pair.to)) {
			return stop(refusal_code::target_exists, std::format("'{}' is taken", res::tag::make(pair.to).view()));
		}
		std::string error;
		std::string hash = hash_file(item.root, pair.from, error);
		if (!error.empty()) return stop(refusal_code::io_error, std::format("'{}': {}", res::tag::make(pair.from).view(), error));
		item.moves.push_back(planned_move{ pair.from, removing ? "files/" + pair.from : pair.to, std::move(hash) });
	}
	if (removing) {
		const bool covers_engine = std::ranges::any_of(record->shadowed_layers, [&](std::size_t layer) { return is_engine_layer(layers, layer, m_ports.engine_base); });
		if (covers_engine) {
			item.warnings.push_back(std::format("'{}' covered a file of the engine: the engine's file shows again", record->path.view()));
		}
	}

	const std::optional<asset_ops_manifest> manifest = m_ports.manifest ? m_ports.manifest() : std::nullopt;
	const std::span<const manifest_reference> manifest_references = manifest ? std::span<const manifest_reference>(manifest->references) : std::span<const manifest_reference>{};
	usage_report found = usages.find(*db, m_ports.descs ? m_ports.descs() : nullptr, usage_target{ record->path, record->id },
		manifest ? std::string_view(manifest->file) : std::string_view{}, manifest_references);
	std::erase_if(found.items, [&](const asset_usage& usage) { return usage.file == record->path.view(); });
	item.referrers = found.items;
	facts.path = std::string(record->path.view());
	facts.relative = source;
	facts.guid = record->id ? res::to_text(*record->id) : std::string{};

	std::optional<refusal> scan_problem;
	if (!found.skipped.empty()) {
		scan_problem = stop(refusal_code::usage_scan_skipped, "some desc files are too big to be read for references, so who refers to the asset is not known");
		scan_problem->skipped = found.skipped;
	} else if (!found.complete) {
		scan_problem = stop(refusal_code::database_not_ready, "the asset database is still reading the files");
	}
	if (removing) {
		item.scan_problem = std::move(scan_problem);
		item.needs_force = !item.referrers.empty();
		return std::nullopt;
	}
	return scan_problem;
}

std::optional<edt::refusal> edt::asset_ops::plan_rewrites(std::vector<plan_item>& items, const std::vector<item_facts>& facts) const
{
	const asset_database* db = m_ports.assets ? m_ports.assets() : nullptr;
	if (!db) return stop(refusal_code::no_database, "no project is open: there is no asset database");
	const std::vector<fs::path>& layers = db->layers();
	const bool fold = db->case_insensitive();
	const std::optional<asset_ops_manifest> manifest = m_ports.manifest ? m_ports.manifest() : std::nullopt;

	// Where each file the plan moves ends up: a referrer that moves is rewritten there, after the moves.
	std::map<std::string, std::string> moved_to;
	for (const plan_item& item : items) {
		for (const planned_move& move : item.moves) moved_to[res::asset_name_key(move.from, fold)] = move.to;
	}
	struct referrer_group
	{
		std::size_t item = 0;
		std::vector<const asset_usage*> usages;
	};
	std::map<std::string, std::vector<referrer_group>> files;
	for (std::size_t i = 0; i < items.size(); ++i) {
		for (const asset_usage& usage : items[i].referrers) {
			std::vector<referrer_group>& groups = files[usage.file];
			if (groups.empty() || groups.back().item != i) groups.push_back(referrer_group{ i, {} });
			groups.back().usages.push_back(&usage);
		}
	}
	if (m_ports.is_open_with_unsaved_changes) {
		for (const auto& entry : files) {
			const std::string& file = entry.first;
			const std::vector<referrer_group>& groups = entry.second;
			if (groups.front().usages.front()->kind == usage_kind::manifest || !m_ports.is_open_with_unsaved_changes(res::tag{ file })) continue;
			refusal open = stop(refusal_code::referrer_open, std::format("'{}' refers to the asset and is open with unsaved changes", file));
			for (const referrer_group& group : groups) {
				for (const asset_usage* usage : group.usages) open.referrers.push_back(*usage);
			}
			return open;
		}
	}

	for (const auto& entry : files) {
		const std::string& file = entry.first;
		const std::vector<referrer_group>& groups = entry.second;
		const auto reject = [&](const referrer_group& group, std::string code, std::string reason) {
			plan_item& item = items[group.item];
			item.could_not_rewrite.push_back(rewrite_failure{ file, std::move(code), std::move(reason) });
			for (const asset_usage* usage : group.usages) {
				if (!usage->by_guid) item.broken_references.push_back(*usage);
			}
		};
		const auto reject_all = [&](const std::string& code, const std::string& reason) {
			for (const referrer_group& group : groups) reject(group, code, reason);
		};
		fs::path root;
		std::string relative;
		const bool in_manifest = groups.front().usages.front()->kind == usage_kind::manifest;
		if (in_manifest) {
			if (!manifest) {
				reject_all("unknown_file", "the project manifest is not known");
				continue;
			}
			root = manifest->root;
			relative = manifest->file;
		} else {
			const asset_record* holder = db->find_any_case(res::tag{ file });
			if (!holder) {
				reject_all("unknown_file", "the asset database has no such file any more");
				continue;
			}
			if (holder->layer != db->top_layer()) {
				const bool engine = is_engine_layer(layers, holder->layer, m_ports.engine_base);
				for (const referrer_group& group : groups) {
					const bool by_path = std::ranges::any_of(group.usages, [](const asset_usage* usage) { return !usage->by_guid; });
					if (by_path) {
						reject(group, engine ? "engine_layer" : "lower_layer", engine ? "not rewritten: a file of the engine is not changed" : "not rewritten: lower layer");
					} else {
						items[group.item].warnings.push_back(std::format("'{}' is in a lower layer and is not rewritten: it refers by guid, which stays valid", file));
					}
				}
				continue;
			}
			root = layers[holder->layer];
			relative = std::string(holder->path.relative());
		}
		std::vector<std::byte> bytes;
		if (const std::string error = read_file(root, relative, k_level_file_limit, bytes); !error.empty()) {
			reject_all("unreadable", error);
			continue;
		}
		// Every asset the file refers to, in turn, over one text: the pointers stay valid, only values change.
		std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		std::vector<std::string> pointers_changed;
		std::optional<std::size_t> owner;
		bool failed = false;
		for (const referrer_group& group : groups) {
			std::vector<std::string> pointers;
			pointers.reserve(group.usages.size());
			for (const asset_usage* usage : group.usages) pointers.push_back(usage->json_pointer);
			const item_facts& fact = facts[group.item];
			rewrite_outcome outcome = rewrite_references(text, pointers, fact.path, items[group.item].target.view(), fold, fact.guid);
			if (outcome.status == rewrite_status::nothing_to_do) continue;
			if (outcome.status != rewrite_status::rewritten) {
				reject(group, std::string(rewrite_status_name(outcome.status)), std::move(outcome.reason));
				failed = true;
				break;
			}
			text = std::move(outcome.text);
			pointers_changed.insert(pointers_changed.end(), std::make_move_iterator(outcome.pointers.begin()), std::make_move_iterator(outcome.pointers.end()));
			if (!owner) owner = group.item;
		}
		if (failed || !owner) continue;
		std::string shown_file = file;
		std::string target = relative;
		if (!in_manifest) {
			if (const auto moved = moved_to.find(res::asset_name_key(relative, fold)); moved != moved_to.end()) {
				target = moved->second;
				shown_file = std::string(res::tag::make(target).view());
			}
		}
		items[*owner].rewrites.push_back(planned_rewrite{ std::move(shown_file), std::move(root), std::move(target), hash_of(bytes), std::move(text),
			std::move(pointers_changed), std::move(relative) });
	}

	std::vector<rewrite_failure> failures;
	std::vector<asset_usage> broken;
	for (const plan_item& item : items) {
		failures.insert(failures.end(), item.could_not_rewrite.begin(), item.could_not_rewrite.end());
		broken.insert(broken.end(), item.broken_references.begin(), item.broken_references.end());
	}
	if (failures.empty()) return std::nullopt;
	const std::string what = items.size() == 1 ? std::format("'{}'", facts.front().path) : std::string("the assets");
	refusal why = stop(refusal_code::unrewritable_references, std::format("{} file(s) that refer to {} cannot be rewritten, so references would break: nothing was changed",
		failures.size(), what));
	why.unrewritable = std::move(failures);
	why.referrers = std::move(broken);
	return why;
}

void edt::asset_ops::announce(const std::vector<journal_step>& steps, bool undoing) const
{
	const asset_database* db = m_ports.report_self_change && m_ports.assets ? m_ports.assets() : nullptr;
	if (!db) return;
	// Only the files res:// shows: a step in another root (the manifest's folder) is not one of the database's.
	std::vector<std::string> roots;
	roots.reserve(db->layers().size());
	for (const fs::path& layer : db->layers()) roots.push_back(utf8(layer));
	const auto in_layers = [&roots](const journal_step& step) { return std::ranges::find(roots, step.root) != roots.end(); };

	std::vector<res::file_change> changes;
	changes.reserve(steps.size());
	const auto add = [&changes](std::string_view relative, std::string_view old_relative, res::change_kind kind) {
		res::file_change change;
		change.tag = res::tag::make(relative);
		if (!old_relative.empty()) change.old_tag = res::tag::make(old_relative);
		change.kind = kind;
		change.self = true;
		changes.push_back(std::move(change));
	};
	for (const journal_step& step : steps) {
		if (!in_layers(step)) continue;
		switch (step.kind) {
		case journal_step_kind::move:
			if (undoing) add(step.from, step.to, res::change_kind::renamed);
			else add(step.to, step.from, res::change_kind::renamed);
			break;
		case journal_step_kind::rewrite:
			add(step.from, {}, res::change_kind::modified);
			break;
		case journal_step_kind::stash:
			add(step.from, {}, undoing ? res::change_kind::created : res::change_kind::deleted);
			break;
		case journal_step_kind::make_folder:
		case journal_step_kind::remove_folder:
			// The moves of their files say everything; a folder change would make the database walk every layer again.
			break;
		}
	}
	if (!changes.empty()) m_ports.report_self_change(changes);
}

edt::operation_report edt::asset_ops::execute(const operation_plan& plan, const execute_options& options) const
{
	operation_report report;
	bool started = false;
	try {
		execute_into(plan, options, report, started);
	} catch (const std::exception& error) {
		report.ok = false;
		report.error = error.what();
		if (!report.journal.steps.empty()) roll_back(report);
	} catch (...) {
		report.ok = false;
		report.error = "an unexpected error";
		if (!report.journal.steps.empty()) roll_back(report);
	}
	if (started && m_ports.operation_ended) {
		try {
			m_ports.operation_ended(report.journal, report.ok);
		} catch (...) {
			egLOG_WARN("asset_ops", "the history did not take the end of the operation '{}'", report.journal.id);
		}
	}
	return report;
}

void edt::asset_ops::roll_back(operation_report& report) const
{
	operation_journal& journal = report.journal;
	std::vector<journal_step> stuck;
	for (auto step = journal.steps.rbegin(); step != journal.steps.rend(); ++step) {
		std::string undone;
		try {
			undone = undo_step(*step, path_of(step->root), m_ports.journal_root, journal.id, seams_of(m_ports), report.warnings);
		} catch (const std::exception& error) {
			undone = error.what();
		} catch (...) {
			undone = "an unexpected error";
		}
		if (!undone.empty()) {
			report.rollback_failures.push_back(std::format("{}: {}", step_label(*step), undone));
			egLOG_WARN("asset_ops", "'{}' could not be taken back: {}", step_label(*step), undone);
			stuck.push_back(*step);
		}
	}
	// A step taken back leaves the disk as it was; only what stayed done is news to the database.
	if (!stuck.empty()) announce(stuck, false);
	if (report.rollback_failures.empty()) journal.steps.clear();
	try {
		persist(journal, m_ports.journal_root);
	} catch (...) {
		egLOG_WARN("asset_ops", "the journal of '{}' was not written", journal.id);
	}
}

void edt::asset_ops::execute_into(const operation_plan& plan, const execute_options& options, operation_report& report, bool& started) const
{
	if (plan.refused) {
		report.refused = *plan.refused;
		return;
	}
	if (plan.items.empty() && plan.carried.empty() && plan.make_folders.empty() && plan.remove_folders.empty()) {
		report.refused = stop(refusal_code::bad_name, "the plan has nothing to do");
		return;
	}
	const bool removing = plan.operation == asset_operation::remove;
	const fs::path& layer = plan.items.empty() ? plan.root : plan.items.front().root;
	if (layer.empty()) {
		report.refused = stop(refusal_code::io_error, "the plan names no folder of the project");
		return;
	}
	std::vector<const planned_move*> moves;
	std::vector<const planned_rewrite*> rewrites;
	std::vector<asset_usage> referrers;
	const plan_item* scanned = nullptr;
	for (const plan_item& item : plan.items) {
		report.warnings.insert(report.warnings.end(), item.warnings.begin(), item.warnings.end());
		report.could_not_rewrite.insert(report.could_not_rewrite.end(), item.could_not_rewrite.begin(), item.could_not_rewrite.end());
		report.broken_references.insert(report.broken_references.end(), item.broken_references.begin(), item.broken_references.end());
		for (const planned_move& move : item.moves) moves.push_back(&move);
		for (const planned_rewrite& rewrite : item.rewrites) rewrites.push_back(&rewrite);
		if (item.scan_problem && !scanned) scanned = &item;
		if (item.needs_force) referrers.insert(referrers.end(), item.referrers.begin(), item.referrers.end());
	}
	for (const planned_move& move : plan.carried) moves.push_back(&move);

	if (removing) {
		const bool one = plan.items.size() == 1;
		const std::string asked = one ? std::format("'{}'", plan.items.front().request.asset.view()) : std::string("the removed files");
		if (scanned) {
			if (!options.force) {
				report.refused = *scanned->scan_problem;
				return;
			}
			report.warnings.push_back(std::format("forced: {}", scanned->scan_problem->reason));
		}
		if (!referrers.empty()) {
			if (!options.force) {
				refusal why = stop(refusal_code::has_referrers, std::format("{} place(s) refer to {}", referrers.size(), asked));
				why.referrers = referrers;
				report.refused = std::move(why);
				return;
			}
			report.warnings.push_back(std::format("{} {} removed while {} place(s) still refer to {}: they are left as they are", asked, one ? "was" : "were",
				referrers.size(), one ? "it" : "them"));
			report.broken_references = std::move(referrers);
		}
	}
	if (m_ports.journal_root.empty()) {
		report.refused = stop(refusal_code::io_error, "there is no folder for the operation journal");
		return;
	}

	// Nothing is touched when a file is not what the plan read, or has unsaved edits open since.
	if (m_ports.is_open_with_unsaved_changes) {
		for (const plan_item& item : plan.items) {
			if (!m_ports.is_open_with_unsaved_changes(item.request.asset)) continue;
			report.refused = stop(refusal_code::referrer_open, std::format("'{}' is open with unsaved changes", item.request.asset.view()));
			return;
		}
		for (const planned_rewrite* rewrite : rewrites) {
			if (!rewrite->file.starts_with("res://")) continue;
			const res::tag source = res::tag::make(rewrite->source);
			if (!m_ports.is_open_with_unsaved_changes(source)) continue;
			report.refused = stop(refusal_code::referrer_open, std::format("'{}' refers to the asset and is open with unsaved changes", source.view()));
			return;
		}
	}
	for (const planned_move* move : moves) {
		std::string error;
		if (hash_file(layer, move->from, error) != move->hash || !error.empty()) {
			report.refused = stop(refusal_code::stale_plan, std::format("'{}' changed since it was planned", res::tag::make(move->from).view()));
			return;
		}
		if (!removing && exists_on_disk(layer, move->to)) {
			report.refused = stop(refusal_code::target_exists, std::format("'{}' is taken", res::tag::make(move->to).view()));
			return;
		}
	}
	for (const std::string& folder : plan.make_folders) {
		if (!exists_on_disk(layer, folder)) continue;
		report.refused = stop(refusal_code::target_exists, std::format("'{}' is taken", res::tag::make(folder).view()));
		return;
	}
	std::vector<std::vector<std::byte>> originals(rewrites.size());
	for (std::size_t i = 0; i < rewrites.size(); ++i) {
		const planned_rewrite& rewrite = *rewrites[i];
		const std::string error = read_file(rewrite.root, rewrite.source, k_level_file_limit, originals[i]);
		if (!error.empty() || hash_of(originals[i]) != rewrite.before_hash) {
			report.refused = stop(refusal_code::stale_plan, std::format("'{}' changed since it was planned", rewrite.file));
			return;
		}
	}

	operation_journal& journal = report.journal;
	journal.id = res::to_text(res::generate_asset_id());
	journal.operation = plan.operation;
	journal.directory = utf8(m_ports.journal_root / journal.id);
	report.operation_id = journal.id;
	const fs::path& journal_root = m_ports.journal_root;
	std::vector<std::string> original_names(rewrites.size());
	for (std::size_t i = 0; i < rewrites.size(); ++i) {
		original_names[i] = std::format("originals/{}", i);
		const std::string kept = in_journal(journal_root, journal.id, original_names[i]);
		const std::string error = write_file(journal_base(journal_root), kept, originals[i], false);
		if (!error.empty()) {
			report.refused = stop(refusal_code::io_error, std::format("the original of '{}' was not kept: {}", rewrites[i]->file, error));
			return;
		}
	}
	originals.clear();
	if (m_ports.operation_started) {
		m_ports.operation_started(journal);
		started = true;
	}

	const std::shared_ptr<void> hold = m_ports.hold_changes ? m_ports.hold_changes() : nullptr;
	const std::string root = canonical_root(layer);
	const auto injected = [this](const std::string& label) { return m_ports.fail_step ? m_ports.fail_step(label) : std::string{}; };
	const move_seams seams = seams_of(m_ports);
	// The step is in the journal on the disk before what it describes is done: a crash in between leaves a journal that
	// says what may have happened, and undo_step takes a step that did not happen for done.
	const auto record = [&](journal_step step) -> std::string {
		journal.steps.push_back(std::move(step));
		return persist(journal, journal_root, true);
	};

	const auto run = [&]() -> std::string {
		for (const std::string& folder : plan.make_folders) {
			if (std::string error = injected("make_folder:" + folder); !error.empty()) return error;
			// A folder somebody else made meanwhile is never taken for one of the operation's own, which a rollback removes.
			if (exists_on_disk(layer, folder)) return std::format("'{}' is there already", res::tag::make(folder).view());
			if (std::string error = record(journal_step{ journal_step_kind::make_folder, root, folder, {}, {}, {}, {} }); !error.empty()) return error;
			if (std::string error = make_new_folder(layer, folder); !error.empty()) {
				journal.steps.pop_back();
				persist(journal, journal_root);
				return error;
			}
		}
		for (const planned_move* move : moves) {
			if (removing) {
				if (std::string error = injected("stash:" + move->from); !error.empty()) return error;
				if (std::string error = record(journal_step{ journal_step_kind::stash, root, move->from, move->to, move->hash, {}, {} }); !error.empty()) return error;
				const std::string kept = in_journal(journal_root, journal.id, move->to);
				std::string error = stash_file(layer, move->from, journal_base(journal_root), kept, move->hash, seams);
				if (!error.empty()) return error;
			} else {
				if (std::string error = injected("move:" + move->from); !error.empty()) return error;
				if (std::string error = record(journal_step{ journal_step_kind::move, root, move->from, move->to, move->hash, {}, {} }); !error.empty()) return error;
				if (std::string error = move_file(layer, move->from, move->to); !error.empty()) return error;
			}
		}
		for (std::size_t i = 0; i < rewrites.size(); ++i) {
			const planned_rewrite& rewrite = *rewrites[i];
			if (std::string error = injected("rewrite:" + rewrite.relative); !error.empty()) return error;
			// Right before it is replaced: a referrer someone saved since the plan was read is theirs, not overwritten.
			std::string read_problem;
			if (hash_file(rewrite.root, rewrite.relative, read_problem) != rewrite.before_hash || !read_problem.empty()) {
				return std::format("'{}' changed meanwhile, so it was not rewritten", rewrite.file);
			}
			if (std::string error = record(journal_step{ journal_step_kind::rewrite, canonical_root(rewrite.root), rewrite.relative, {}, rewrite.before_hash,
				hash_of(rewrite.after_text), original_names[i] }); !error.empty()) return error;
			const std::span<const std::byte> bytes = std::as_bytes(std::span<const char>(rewrite.after_text.data(), rewrite.after_text.size()));
			if (std::string error = write_file(rewrite.root, rewrite.relative, bytes, true); !error.empty()) return error;
		}
		for (const std::string& folder : plan.remove_folders) {
			if (std::string error = injected("remove_folder:" + folder); !error.empty()) return error;
			// A folder that is not empty (something came in meanwhile) is left, not a failure.
			std::string problem;
			if (!folder_is_empty(layer, folder, problem)) {
				report.warnings.push_back(std::format("the folder '{}' stays: {}", res::tag::make(folder).view(), problem.empty() ? "it is not empty" : problem));
				continue;
			}
			if (std::string error = record(journal_step{ journal_step_kind::remove_folder, root, folder, {}, {}, {}, {} }); !error.empty()) return error;
			if (const std::string error = remove_empty_folder(layer, folder); !error.empty()) {
				report.warnings.push_back(std::format("the folder '{}' stays: {}", res::tag::make(folder).view(), error));
			}
		}
		return {};
	};

	std::string error;
	try {
		error = run();
	} catch (const std::exception& thrown) {
		error = thrown.what();
	} catch (...) {
		error = "an unexpected error";
	}
	if (!error.empty()) {
		report.error = std::move(error);
		roll_back(report);
		return;
	}
	announce(journal.steps, false);

	for (const planned_move* move : moves) {
		const std::string to = removing ? std::format("cache://asset_ops/{}/{}", journal.id, move->to) : std::string(res::tag::make(move->to).view());
		report.moved.push_back(file_move{ std::string(res::tag::make(move->from).view()), to });
	}
	for (const planned_rewrite* rewrite : rewrites) report.rewritten.push_back(rewritten_file{ rewrite->file, rewrite->pointers });
	report.ok = true;
}

std::vector<fs::path> edt::asset_ops::changeable_roots() const
{
	std::vector<fs::path> roots;
	const asset_database* db = m_ports.assets ? m_ports.assets() : nullptr;
	if (db) {
		const std::vector<fs::path>& layers = db->layers();
		const std::size_t top = db->top_layer();
		if (top < layers.size() && !is_engine_layer(layers, top, m_ports.engine_base)) roots.push_back(layers[top]);
	}
	const std::optional<asset_ops_manifest> manifest = m_ports.manifest ? m_ports.manifest() : std::nullopt;
	if (manifest) roots.push_back(manifest->root);
	return roots;
}

std::string edt::asset_ops::without_host_paths(std::string text) const
{
	// A bare folder is replaced where it is a whole name, not the start of a longer one.
	const auto replace_all = [&text](const std::string& from, std::string_view with, bool whole_name) {
		if (from.empty()) return;
		const auto name_char = [](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_' || ch == '-'; };
		for (std::size_t at = text.find(from); at != std::string::npos; ) {
			const std::size_t end = at + from.size();
			if (whole_name && end < text.size() && name_char(text[end])) {
				at = text.find(from, end);
				continue;
			}
			text.replace(at, from.size(), with);
			at = text.find(from, at + with.size());
		}
	};
	// `root/x` becomes `prefix` + `x`, a bare `root` becomes `bare`.
	const auto hide = [&](const fs::path& root, std::string_view prefix, std::string_view bare) {
		if (root.empty()) return;
		for (const std::string& form : { utf8(root), fs_paths::utf8_of(root) }) {
			replace_all(form + "/", prefix, false);
			replace_all(form, bare, true);
		}
	};
	// The longest first: the journal folder lies inside the project.
	hide(m_ports.journal_root, "cache://asset_ops/", "cache://asset_ops");
	const asset_database* db = m_ports.assets ? m_ports.assets() : nullptr;
	const std::optional<asset_ops_manifest> manifest = m_ports.manifest ? m_ports.manifest() : std::nullopt;
	if (db) {
		const std::vector<fs::path>& layers = db->layers();
		for (std::size_t layer = 0; layer < layers.size(); ++layer) {
			if (is_engine_layer(layers, layer, m_ports.engine_base)) hide(layers[layer], "<engine>/", "<engine>");
			else hide(layers[layer], "res://", "res://");
		}
	}
	if (manifest) hide(manifest->root, "<project>/", "<project>");
	hide(m_ports.engine_base, "<engine>/", "<engine>");
	return text;
}

edt::undo_report edt::asset_ops::undo(const operation_journal& journal) const
{
	undo_report report;
	try {
		// What the journal says is checked against what the ports say, never taken as it stands.
		if (const std::string problem = journal_problem(journal); !problem.empty()) {
			report.error = std::format("the journal is refused: {}", problem);
			return report;
		}
		const fs::path& journal_root = m_ports.journal_root;
		if (journal_root.empty() || utf8(path_of(journal.directory).lexically_normal()) != utf8((journal_root / journal.id).lexically_normal())) {
			report.error = "the journal is refused: its folder is not the journal folder of its id";
			return report;
		}
		const std::vector<fs::path> allowed = changeable_roots();
		const std::optional<asset_ops_manifest> manifest = m_ports.manifest ? m_ports.manifest() : std::nullopt;
		const asset_database* db = m_ports.assets ? m_ports.assets() : nullptr;
		const std::string top = db && db->top_layer() < db->layers().size() ? canonical_root(db->layers()[db->top_layer()]) : std::string{};
		std::vector<fs::path> roots;
		roots.reserve(journal.steps.size());
		for (const journal_step& step : journal.steps) {
			// The root is one the ports name, spelled exactly as they do: an alias of it is not accepted, and what the step may do is
			// judged against that root, not against the string the journal carries.
			const auto found = std::ranges::find_if(allowed, [&](const fs::path& candidate) { return canonical_root(candidate) == step.root; });
			if (found == allowed.end()) {
				report.error = std::format("the journal is refused: '{}' is not the project's own folder or the manifest's", step.root);
				return report;
			}
			if (const std::string problem = step_problem(step, *found); !problem.empty()) {
				report.error = std::format("the journal is refused: {}", problem);
				return report;
			}
			// The manifest's folder is for the manifest alone; the project's own layer takes descs (and the manifest, where they share a folder).
			const bool manifest_file = manifest && step.from == manifest->file;
			const bool in_top = !top.empty() && top == step.root;
			const bool allowed_here = step.kind == journal_step_kind::rewrite ? (manifest_file || (in_top && step.from.ends_with(".desc"))) : in_top;
			if (!allowed_here) {
				report.error = std::format("the journal is refused: '{}' is not something an operation changes in '{}'", step.from, step.root);
				return report;
			}
			roots.push_back(*found);
		}

		// A crash can leave the last step recorded and not done: that step is not a change, it is skipped. Newest first, so a
		// moved file a later step rewrote is judged by the text that rewrite left.
		std::vector<step_state> states(journal.steps.size(), step_state::done);
		for (std::size_t i = journal.steps.size(); i-- > 0;) {
			journal_step judged = journal.steps[i];
			if (judged.kind == journal_step_kind::move) {
				for (std::size_t later = i + 1; later < journal.steps.size(); ++later) {
					const journal_step& step = journal.steps[later];
					if (step.kind != journal_step_kind::rewrite || step.root != judged.root || step.from != judged.to) continue;
					if (states[later] == step_state::done) judged.hash = step.after_hash;
				}
			}
			std::string file;
			states[i] = state_of_step(judged, roots[i], journal_root, journal.id, file);
			if (states[i] == step_state::changed) report.changed_since.push_back(file);
		}
		std::ranges::reverse(report.changed_since);
		if (!report.changed_since.empty()) {
			report.error = "files changed since the operation, so nothing was put back";
			return report;
		}
		if (m_ports.is_open_with_unsaved_changes) {
			for (std::size_t i = 0; i < journal.steps.size(); ++i) {
				const journal_step& step = journal.steps[i];
				if (states[i] == step_state::not_done || is_folder_step(step) || (step.kind == journal_step_kind::rewrite && !step.from.ends_with(".desc"))) continue;
				for (const std::string* file : { &step.from, &step.to }) {
					if (step.kind != journal_step_kind::move && file == &step.to) continue;
					if (m_ports.is_open_with_unsaved_changes(res::tag::make(*file))) report.open_with_unsaved_changes.push_back(*file);
				}
			}
			if (!report.open_with_unsaved_changes.empty()) {
				report.error = "files are open with unsaved changes, so nothing was put back";
				return report;
			}
		}

		const std::shared_ptr<void> hold = m_ports.hold_changes ? m_ports.hold_changes() : nullptr;
		std::vector<journal_step> put_back;
		for (std::size_t i = journal.steps.size(); i-- > 0;) {
			const journal_step& step = journal.steps[i];
			if (states[i] == step_state::not_done) continue;
			if (std::string error = undo_step(step, roots[i], journal_root, journal.id, seams_of(m_ports), report.warnings); !error.empty()) {
				report.failures.push_back(std::format("{}: {}", step_label(step), error));
				egLOG_WARN("asset_ops", "'{}' could not be put back: {}", step_label(step), error);
				continue;
			}
			report.restored.push_back(step.from);
			put_back.push_back(step);
		}
		announce(put_back, true);
		report.ok = report.failures.empty();
		if (!report.ok) report.error = "some files could not be put back";
	} catch (const std::exception& error) {
		report.ok = false;
		report.error = error.what();
	} catch (...) {
		report.ok = false;
		report.error = "an unexpected error";
	}
	return report;
}
