#include "edt_document_parts.h"
#include <snk/resource/res_system.h>

void edt::disk_state::flag(const std::string& disk_version)
{
	m_changed = true;
	switch (m_serial_policy) {
	case disk_serial::none:
		break;
	case disk_serial::every_flag:
		++m_serial;
		break;
	case disk_serial::per_version:
		if (disk_version != m_flagged_version) {
			m_flagged_version = disk_version;
			++m_serial;
		}
		break;
	}
}

void edt::disk_state::clear()
{
	m_changed = false;
	m_flagged_version.clear();
}

void edt::disk_state::on_source_changed(std::string_view on_disk_version, std::string_view read_version, bool dirty, const std::function<bool()>& locked,
	const std::function<bool()>& reload, const std::function<void()>& announce)
{
	if (on_disk_version == read_version) {
		if (m_changed) {
			m_changed = false;
			announce();
		}
		return;
	}
	if (!dirty && !locked() && reload()) {
		return;
	}
	if (!m_changed) {
		m_changed = true;
		announce();
	}
}

edt::save_result edt::disk_state::overwrite(std::string& version, std::string on_disk_version,
	const std::function<save_result()>& save, const std::function<bool(const save_result&)>& failed)
{
	const std::string previous_version = std::exchange(version, std::move(on_disk_version));
	const save_result saved = save();
	if (failed(saved)) {
		version = previous_version;
		m_changed = true;
	}
	return saved;
}

std::optional<std::string> edt::write_gate::blocker(const desc_file_layout& files)
{
	return files.write_blocker ? files.write_blocker() : std::nullopt;
}

edt::file_write_check edt::write_gate::check(res::resource_system& reader, const desc::desc_system& descs, const desc_file_layout& files,
	const res::tag& tag, std::string_view disk_version, disk_state& state, const std::function<void()>& announce)
{
	file_write_check allowed = check_file_write(reader, descs, files, tag, disk_version);
	if (allowed.changed_on_disk && !state.changed()) {
		state.set_changed(true);
		announce();
	}
	return allowed;
}

void edt::source_watch::watch(res::resource_system* resources, const res::tag& tag, void* owner, std::function<void(const res::tag&)> changed)
{
	unwatch();
	m_resources = resources;
	if (m_resources) {
		m_tag = tag;
		m_owner = owner;
		m_resources->watch(m_tag, m_owner, std::move(changed));
	}
}

void edt::source_watch::unwatch()
{
	if (m_resources) {
		m_resources->unwatch(m_tag, m_owner);
		m_resources = nullptr;
	}
}
