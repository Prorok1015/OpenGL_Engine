#pragma once
#include "edt_desc_file_commands.h"
#include "edt_file_write_check.h"
#include <snk/editor/edt_document.h>
#include <snk/model_codec/res_tag.h>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace desc { class desc_system; }
namespace res { class resource_system; }

namespace edt
{
	// The parts the editor's file documents (scene, data, prefab, mesh) share (#1400, EPIC-82 step a): what each of them did
	// for itself about "the file changed on disk". Editor-internal: nothing here is exported or named by a pinned interface.

	// When disk_state::flag moves the serial the documents expose (disk_change_serial).
	enum class disk_serial
	{
		// No serial: the document has none (prefab, mesh).
		none,
		// Every flag: the level controller flags once per outside version it has judged (scene).
		every_flag,
		// Only when the version flagged is not the one flagged last: a watcher may signal the same outside version again (data).
		per_version,
	};

	// "The file differs from what the document read": the flag, the serial a banner put off comes back for, and the shared
	// decision of a document that watches one file.
	class disk_state
	{
	public:
		explicit disk_state(disk_serial serial = disk_serial::none) : m_serial_policy(serial) {}

		bool changed() const { return m_changed; }
		std::uint64_t serial() const { return m_serial; }

		// The file differs now. `disk_version` is the version found, used only by disk_serial::per_version.
		void flag(const std::string& disk_version = {});
		// Sets the flag as it is, moving no serial and forgetting no version (a document opened, reverted, written).
		void set_changed(bool changed) { m_changed = changed; }
		// The flag off and the version flagged last forgotten: the next outside version is a new one again.
		void clear();

		// What a document that watches its file does when the file was signalled (prefab, mesh): the flag follows the
		// comparison of the version found with the one the document read, in this order and no other:
		// - the same version: a flag that was set is cleared and announced;
		// - another version and the document is clean and not locked (`locked` is asked only for a clean one): `reload()` takes it
		//   (true: it did, nothing more);
		// - otherwise the flag is set and announced when it was not.
		void on_source_changed(std::string_view on_disk_version, std::string_view read_version, bool dirty, const std::function<bool()>& locked,
			const std::function<bool()>& reload, const std::function<void()>& announce);

		// An overwrite (prefab, mesh): `version` is the one the document read; it is taken as `on_disk_version` while `save`
		// runs (the save compares it with the file), and put back with the flag set again when `failed(saved)` says the
		// document was not written.
		save_result overwrite(std::string& version, std::string on_disk_version, const std::function<save_result()>& save,
			const std::function<bool(const save_result&)>& failed);

	private:
		disk_serial m_serial_policy;
		bool m_changed = false;
		std::string m_flagged_version;
		std::uint64_t m_serial = 0;
	};

	// What a document that writes a desc file asks before it does (prefab, mesh, data; #1400 PR 2).
	struct write_gate
	{
		// Why nothing may be written into the project now (the layout's `write_blocker` port: a migration that runs or stopped
		// half way); nullopt: it may. The one place the port is read.
		static std::optional<std::string> blocker(const desc_file_layout& files);

		// check_file_write for a document that keeps `state`: when the file turns out to differ from `disk_version` the flag is
		// set and `announce` called (once: not when it was set already), though the watcher may not have said so yet (Save All's
		// question depends on the flag). The result is check_file_write's, whole: the caller refuses on its `error`.
		static file_write_check check(res::resource_system& reader, const desc::desc_system& descs, const desc_file_layout& files,
			const res::tag& tag, std::string_view disk_version, disk_state& state, const std::function<void()>& announce);
	};

	// One watched file of a document, registered on the writable resources and given back with the document (prefab, mesh;
	// data watches several files and keeps its own list).
	class source_watch
	{
	public:
		// Watches `tag` on `resources` for `owner` (nothing when null); a watch that was there is dropped first.
		void watch(res::resource_system* resources, const res::tag& tag, void* owner, std::function<void(const res::tag&)> changed);
		void unwatch();

	private:
		res::resource_system* m_resources = nullptr;
		res::tag m_tag;
		void* m_owner = nullptr;
	};
}
