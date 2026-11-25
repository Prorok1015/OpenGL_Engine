#pragma once
#include <snk/editor/edt_document.h>
#include <snk/model_codec/res_tag.h>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

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

		// Sets the flag and calls `announce` when it was not set: the one way a document raises a conflict (a check that found the
		// file changed, a conflict the window raised). Returns whether it did.
		bool raise(const std::function<void()>& announce);

		// What an overwrite does with the flag when the save did not write the document.
		enum class overwrite_flag
		{
			// The flag is set (prefab, mesh: the file is known to differ, and the save did not take it away).
			raise_on_failure,
			// The flag was cleared for the save (a raised conflict stands in the shader document's save) and is put back as it was.
			restore_on_failure,
		};

		// An overwrite: `version` is the one the document read; it is taken as `on_disk_version` while `save` runs (the save
		// compares it with the file), and put back when `failed(saved)` says the document was not written. `flag`: see
		// overwrite_flag; restore_on_failure also clears the flag before the save.
		save_result overwrite(std::string& version, std::string on_disk_version, const std::function<save_result()>& save,
			const std::function<bool(const save_result&)>& failed, overwrite_flag flag = overwrite_flag::raise_on_failure);

	private:
		disk_serial m_serial_policy;
		bool m_changed = false;
		std::string m_flagged_version;
		std::uint64_t m_serial = 0;
	};

	// One watched file of a document, registered on the writable resources and given back with the document (prefab, mesh, shader;
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
