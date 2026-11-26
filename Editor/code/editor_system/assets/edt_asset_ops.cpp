#include "edt_asset_ops.h"
#include "edt_asset_database.h"
#include "edt_reference_rewrite.h"
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>
#include <snk/editor/edt_document.h>
#include <snk/editor/edt_document_manager.h>
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

	// Like exists_on_disk, but where the project's names ignore letter case (`fold`) a file that differs from `relative` in case
	// only is the same file, whatever the disk under the folder says.
	bool exists_named(const fs::path& root, const std::string& relative, bool fold)
	{
		if (exists_on_disk(root, relative)) return true;
		if (!fold) return false;
		const fs::path path = path_of(relative);
		const std::string wanted = res::asset_name_key(utf8(path.filename()), true);
		std::error_code ec;
		fs::directory_iterator it(root / path.parent_path(), fs::directory_options::skip_permission_denied, ec);
		if (ec) return false;
		for (const fs::directory_iterator end; it != end; it.increment(ec)) {
			if (ec) return false;
			if (res::asset_name_key(utf8(it->path().filename()), true) == wanted) return true;
		}
		return false;
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

	std::string_view step_kind_name(edt::journal_step_kind kind)
	{
		switch (kind) {
		case edt::journal_step_kind::move: return "move";
		case edt::journal_step_kind::rewrite: return "rewrite";
		case edt::journal_step_kind::stash: return "stash";
		}
		return "move";
	}

	std::optional<edt::journal_step_kind> step_kind_of(std::string_view name)
	{
		for (const edt::journal_step_kind kind : { edt::journal_step_kind::move, edt::journal_step_kind::rewrite, edt::journal_step_kind::stash }) {
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
	std::string undo_step(const edt::journal_step& step, const fs::path& root, const fs::path& journal_root, const std::string& id, const move_seams& seams)
	{
		std::vector<std::byte> bytes;
		if (step.kind != edt::journal_step_kind::rewrite) {
			if (std::string error = injected_failure(seams, "before_undo:" + step.from); !error.empty()) return error;
		}
		switch (step.kind) {
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

	std::string persist(const edt::operation_journal& journal, const fs::path& journal_root)
	{
		const std::string text = json::serialize(edt::journal_to_json(journal));
		const std::string error = write_file(journal_base(journal_root),
			in_journal(journal_root, journal.id, k_journal_file),
			std::as_bytes(std::span<const char>(text.data(), text.size())), true);
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

	// What a step may do under the root it is read against: names the project lists, as a live operation checks them.
	std::string step_problem(const edt::journal_step& step, const fs::path& root)
	{
		const auto listed = [&](const std::string& relative) { return is_safe_relative(relative) && destination_problem(relative, root, false).empty(); };
		switch (step.kind) {
		case edt::journal_step_kind::move:
			if (!listed(step.from) || !listed(step.to) || !step.original.empty()) {
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
	}
	return "io_error";
}

json::value edt::journal_to_json(const operation_journal& journal)
{
	json::array steps;
	for (const journal_step& step : journal.steps) {
		steps.push_back(json::object{ { "kind", std::string(step_kind_name(step.kind)) }, { "root", step.root }, { "from", step.from }, { "to", step.to },
			{ "hash", step.hash }, { "after_hash", step.after_hash }, { "original", step.original } });
	}
	return json::object{ { "id", journal.id }, { "operation", std::string(asset_operation_name(journal.operation)) },
		{ "directory", journal.directory }, { "steps", std::move(steps) } };
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

bool edt::is_open_with_unsaved_changes(document_manager& documents, const res::tag& tag, bool is_editor_copy)
{
	if (is_open_with_unsaved_changes(documents.find_by_source(tag), documents.scene(), is_editor_copy)) return true;
	return !unsaved_edits_holder(documents, tag).empty();
}

std::string edt::unsaved_edits_holder(document_manager& documents, const res::tag& tag)
{
	std::vector<document_id> linked;
	documents.for_each([&](document& open) {
		if (open.source() == tag && documents.has_link(open.id())) linked.push_back(open.id());
	});
	for (const document_id link : linked) {
		std::vector<document_id> family = documents.chain_of(link);
		const std::vector<document_id> below = documents.descendants_of(link);
		family.insert(family.end(), below.begin(), below.end());
		for (const document_id id : family) {
			const document* open = documents.find(id);
			if (open == nullptr || !open->is_dirty()) continue;
			if (open->kind() == "scene") return std::format("the level {}", open->source().view());
			return open->source().is_valid() ? std::string(open->source().view()) : std::format("'{}'", open->title());
		}
	}
	return {};
}

edt::asset_ops::asset_ops(asset_ops_ports ports)
	: m_ports(std::move(ports))
{
}

edt::operation_plan edt::asset_ops::plan(std::span<const asset_request> requests) const
{
	operation_plan out;
	if (requests.size() != 1) {
		out.refused = stop(refusal_code::one_asset_only, "one asset at a time: folders and several files are not done yet");
		return out;
	}
	plan_item item;
	item.request = requests.front();
	out.refused = plan_one(item);
	if (!out.refused) out.items.push_back(std::move(item));
	return out;
}

std::optional<edt::refusal> edt::asset_ops::plan_one(plan_item& item) const
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
		if (request.operation == asset_operation::rename) {
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
		if (const std::string problem = destination_problem(dest, item.root, request.operation == asset_operation::rename); !problem.empty()) {
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
			const usage_report binary_usages = find_usages(*db, m_ports.descs ? m_ports.descs() : nullptr, usage_target{ *binary, std::nullopt });
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
	usage_report usages = find_usages(*db, m_ports.descs ? m_ports.descs() : nullptr, usage_target{ record->path, record->id },
		manifest ? std::string_view(manifest->file) : std::string_view{}, manifest_references);
	std::erase_if(usages.items, [&](const asset_usage& usage) { return usage.file == record->path.view(); });
	item.referrers = usages.items;

	std::optional<refusal> scan_problem;
	if (!usages.skipped.empty()) {
		scan_problem = stop(refusal_code::usage_scan_skipped, "some desc files are too big to be read for references, so who refers to the asset is not known");
		scan_problem->skipped = usages.skipped;
	} else if (!usages.complete) {
		scan_problem = stop(refusal_code::database_not_ready, "the asset database is still reading the files");
	}
	if (removing) {
		item.scan_problem = std::move(scan_problem);
		item.needs_force = !item.referrers.empty();
		return std::nullopt;
	}
	if (scan_problem) return scan_problem;

	std::map<std::string, std::vector<const asset_usage*>> groups;
	for (const asset_usage& usage : item.referrers) groups[usage.file].push_back(&usage);
	if (m_ports.is_open_with_unsaved_changes) {
		for (const auto& entry : groups) {
			const std::string& file = entry.first;
			const std::vector<const asset_usage*>& group = entry.second;
			if (group.front()->kind == usage_kind::manifest || !m_ports.is_open_with_unsaved_changes(res::tag{ file })) continue;
			refusal open = stop(refusal_code::referrer_open, std::format("'{}' refers to the asset and is open with unsaved changes", file));
			for (const asset_usage* usage : group) open.referrers.push_back(*usage);
			return open;
		}
	}

	const std::string target_guid = record->id ? res::to_text(*record->id) : std::string{};
	for (const auto& entry : groups) {
		const std::string& file = entry.first;
		const std::vector<const asset_usage*>& group = entry.second;
		const bool by_path = std::ranges::any_of(group, [](const asset_usage* usage) { return !usage->by_guid; });
		const auto reject = [&](std::string code, std::string reason) {
			item.could_not_rewrite.push_back(rewrite_failure{ file, std::move(code), std::move(reason) });
			for (const asset_usage* usage : group) {
				if (!usage->by_guid) item.broken_references.push_back(*usage);
			}
		};
		fs::path root;
		std::string relative;
		if (group.front()->kind == usage_kind::manifest) {
			if (!manifest) {
				reject("unknown_file", "the project manifest is not known");
				continue;
			}
			root = manifest->root;
			relative = manifest->file;
		} else {
			const asset_record* holder = db->find_any_case(res::tag{ file });
			if (!holder) {
				reject("unknown_file", "the asset database has no such file any more");
				continue;
			}
			if (holder->layer != db->top_layer()) {
				const bool engine = is_engine_layer(layers, holder->layer, m_ports.engine_base);
				if (by_path) {
					reject(engine ? "engine_layer" : "lower_layer", engine ? "not rewritten: a file of the engine is not changed" : "not rewritten: lower layer");
				} else {
					item.warnings.push_back(std::format("'{}' is in a lower layer and is not rewritten: it refers by guid, which stays valid", file));
				}
				continue;
			}
			root = layers[holder->layer];
			relative = std::string(holder->path.relative());
		}
		std::vector<std::byte> bytes;
		if (const std::string error = read_file(root, relative, k_level_file_limit, bytes); !error.empty()) {
			reject("unreadable", error);
			continue;
		}
		std::vector<std::string> pointers;
		pointers.reserve(group.size());
		for (const asset_usage* usage : group) pointers.push_back(usage->json_pointer);
		const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		rewrite_outcome outcome = rewrite_references(text, pointers, record->path.view(), item.target.view(), db->case_insensitive(), target_guid);
		if (outcome.status == rewrite_status::nothing_to_do) continue;
		if (outcome.status != rewrite_status::rewritten) {
			reject(std::string(rewrite_status_name(outcome.status)), std::move(outcome.reason));
			continue;
		}
		item.rewrites.push_back(planned_rewrite{ file, std::move(root), std::move(relative), hash_of(bytes), std::move(outcome.text), std::move(outcome.pointers) });
	}
	if (!item.could_not_rewrite.empty()) {
		refusal why = stop(refusal_code::unrewritable_references, std::format("{} file(s) that refer to '{}' cannot be rewritten, so its references would break: nothing was changed", item.could_not_rewrite.size(), record->path.view()));
		why.unrewritable = item.could_not_rewrite;
		why.referrers = item.broken_references;
		return why;
	}
	return std::nullopt;
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
		}
	}
	if (!changes.empty()) m_ports.report_self_change(changes);
}

edt::operation_report edt::asset_ops::execute(const operation_plan& plan, const execute_options& options) const
{
	operation_report report;
	try {
		execute_into(plan, options, report);
	} catch (const std::exception& error) {
		report.ok = false;
		report.error = error.what();
		if (!report.journal.steps.empty()) roll_back(report);
	} catch (...) {
		report.ok = false;
		report.error = "an unexpected error";
		if (!report.journal.steps.empty()) roll_back(report);
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
			undone = undo_step(*step, path_of(step->root), m_ports.journal_root, journal.id, seams_of(m_ports));
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

void edt::asset_ops::execute_into(const operation_plan& plan, const execute_options& options, operation_report& report) const
{
	if (plan.refused) {
		report.refused = *plan.refused;
		return;
	}
	if (plan.items.size() != 1) {
		report.refused = stop(refusal_code::one_asset_only, "one asset at a time");
		return;
	}
	const plan_item& item = plan.items.front();
	const bool removing = item.request.operation == asset_operation::remove;
	report.warnings = item.warnings;
	report.could_not_rewrite = item.could_not_rewrite;
	report.broken_references = item.broken_references;

	if (removing) {
		if (item.scan_problem) {
			if (!options.force) {
				report.refused = *item.scan_problem;
				return;
			}
			report.warnings.push_back(std::format("forced: {}", item.scan_problem->reason));
		}
		if (item.needs_force) {
			if (!options.force) {
				refusal why = stop(refusal_code::has_referrers, std::format("{} place(s) refer to '{}'", item.referrers.size(), item.request.asset.view()));
				why.referrers = item.referrers;
				report.refused = std::move(why);
				return;
			}
			report.broken_references = item.referrers;
			report.warnings.push_back(std::format("'{}' was removed while {} place(s) still refer to it: they are left as they are", item.request.asset.view(), item.referrers.size()));
		}
	}
	if (m_ports.journal_root.empty()) {
		report.refused = stop(refusal_code::io_error, "there is no folder for the operation journal");
		return;
	}

	// Nothing is touched when a file is not what the plan read, or has unsaved edits open since.
	if (m_ports.is_open_with_unsaved_changes) {
		if (m_ports.is_open_with_unsaved_changes(item.request.asset)) {
			report.refused = stop(refusal_code::referrer_open, std::format("'{}' is open with unsaved changes", item.request.asset.view()));
			return;
		}
		for (const planned_rewrite& rewrite : item.rewrites) {
			if (!rewrite.file.starts_with("res://") || !m_ports.is_open_with_unsaved_changes(res::tag{ rewrite.file })) continue;
			report.refused = stop(refusal_code::referrer_open, std::format("'{}' refers to the asset and is open with unsaved changes", rewrite.file));
			return;
		}
	}
	std::vector<std::vector<std::byte>> originals(item.rewrites.size());
	for (const planned_move& move : item.moves) {
		std::string error;
		if (hash_file(item.root, move.from, error) != move.hash || !error.empty()) {
			report.refused = stop(refusal_code::stale_plan, std::format("'{}' changed since it was planned", res::tag::make(move.from).view()));
			return;
		}
		if (!removing && exists_on_disk(item.root, move.to)) {
			report.refused = stop(refusal_code::target_exists, std::format("'{}' is taken", res::tag::make(move.to).view()));
			return;
		}
	}
	for (std::size_t i = 0; i < item.rewrites.size(); ++i) {
		const planned_rewrite& rewrite = item.rewrites[i];
		const std::string error = read_file(rewrite.root, rewrite.relative, k_level_file_limit, originals[i]);
		if (!error.empty() || hash_of(originals[i]) != rewrite.before_hash) {
			report.refused = stop(refusal_code::stale_plan, std::format("'{}' changed since it was planned", rewrite.file));
			return;
		}
	}

	operation_journal& journal = report.journal;
	journal.id = res::to_text(res::generate_asset_id());
	journal.operation = item.request.operation;
	journal.directory = utf8(m_ports.journal_root / journal.id);
	report.operation_id = journal.id;
	const fs::path& journal_root = m_ports.journal_root;
	std::vector<std::string> original_names(item.rewrites.size());
	for (std::size_t i = 0; i < item.rewrites.size(); ++i) {
		original_names[i] = std::format("originals/{}", i);
		const std::string kept = in_journal(journal_root, journal.id, original_names[i]);
		const std::string error = write_file(journal_base(journal_root), kept, originals[i], false);
		if (!error.empty()) {
			report.refused = stop(refusal_code::io_error, std::format("the original of '{}' was not kept: {}", item.rewrites[i].file, error));
			return;
		}
	}
	originals.clear();

	const std::shared_ptr<void> hold = m_ports.hold_changes ? m_ports.hold_changes() : nullptr;
	const std::string root = canonical_root(item.root);
	const auto injected = [this](const std::string& label) { return m_ports.fail_step ? m_ports.fail_step(label) : std::string{}; };
	const move_seams seams = seams_of(m_ports);
	// The step is in the journal on the disk before what it describes is done: a crash in between leaves a journal that
	// says what may have happened, and undo_step takes a step that did not happen for done.
	const auto record = [&](journal_step step) -> std::string {
		journal.steps.push_back(std::move(step));
		return persist(journal, journal_root);
	};

	const auto run = [&]() -> std::string {
		for (const planned_move& move : item.moves) {
			if (removing) {
				if (std::string error = injected("stash:" + move.from); !error.empty()) return error;
				if (std::string error = record(journal_step{ journal_step_kind::stash, root, move.from, move.to, move.hash, {}, {} }); !error.empty()) return error;
				const std::string kept = in_journal(journal_root, journal.id, move.to);
				std::string error = stash_file(item.root, move.from, journal_base(journal_root), kept, move.hash, seams);
				if (!error.empty()) return error;
			} else {
				if (std::string error = injected("move:" + move.from); !error.empty()) return error;
				if (std::string error = record(journal_step{ journal_step_kind::move, root, move.from, move.to, move.hash, {}, {} }); !error.empty()) return error;
				if (std::string error = move_file(item.root, move.from, move.to); !error.empty()) return error;
			}
		}
		for (std::size_t i = 0; i < item.rewrites.size(); ++i) {
			const planned_rewrite& rewrite = item.rewrites[i];
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

	for (const planned_move& move : item.moves) {
		const std::string to = removing ? std::format("cache://asset_ops/{}/{}", journal.id, move.to) : std::string(res::tag::make(move.to).view());
		report.moved.push_back(file_move{ std::string(res::tag::make(move.from).view()), to });
	}
	for (const planned_rewrite& rewrite : item.rewrites) report.rewritten.push_back(rewritten_file{ rewrite.file, rewrite.pointers });
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

		// A crash can leave the last step recorded and not done: that step is not a change, it is skipped.
		std::vector<step_state> states(journal.steps.size(), step_state::done);
		for (std::size_t i = 0; i < journal.steps.size(); ++i) {
			std::string file;
			states[i] = state_of_step(journal.steps[i], roots[i], journal_root, journal.id, file);
			if (states[i] == step_state::changed) report.changed_since.push_back(file);
		}
		if (!report.changed_since.empty()) {
			report.error = "files changed since the operation, so nothing was put back";
			return report;
		}
		if (m_ports.is_open_with_unsaved_changes) {
			for (std::size_t i = 0; i < journal.steps.size(); ++i) {
				const journal_step& step = journal.steps[i];
				if (states[i] == step_state::not_done || (step.kind == journal_step_kind::rewrite && !step.from.ends_with(".desc"))) continue;
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
			if (std::string error = undo_step(step, roots[i], journal_root, journal.id, seams_of(m_ports)); !error.empty()) {
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
