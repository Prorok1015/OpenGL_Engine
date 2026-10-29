#pragma once
#include <snk/resource/res_tag.h>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace res
{
	enum class change_kind
	{
		created,
		modified,
		deleted,
		// `old_tag` held the file before. Reported only when one file disappeared and one appeared in the same poll with
		// the same size and modification time; otherwise a deleted and a created follow, never a guess.
		renamed,
	};

	// One change below a watched mount (code://, #450), as resource_system::subscribe_changes delivers it. A whole poll
	// (or a whole editor operation) arrives as one batch, in the order it was found; the subscriber coalesces further.
	struct file_change
	{
		res::tag tag;
		// Set for `renamed` only.
		res::tag old_tag;
		change_kind kind = change_kind::modified;
		bool is_directory = false;
		// Done through this resource_system (store, remove, rename) rather than found on the disk by the watcher.
		bool self = false;
	};

	// What a mount's watcher reports and skips (code://, #450). Without report_changes the watcher only signals the
	// files whose resource changed on disk, as it always did.
	struct watch_options
	{
		// Also created/deleted/renamed files and folders, as batches (resolver changes_callback).
		bool report_changes = false;
		// A folder name (not a path) whose whole subtree is neither scanned nor reported; none ignored when unset.
		std::function<bool(std::string_view)> ignore_dir;
		// A file name that is reported; every file when unset.
		std::function<bool(std::string_view)> accept_file;
	};

	// What a remove or rename through a resolver came to: `error` says why it did not happen.
	struct file_operation
	{
		bool ok = false;
		std::string error;

		explicit operator bool() const { return ok; }
		static file_operation success() { return { true, {} }; }
		static file_operation failure(std::string why) { return { false, std::move(why) }; }
	};
}
