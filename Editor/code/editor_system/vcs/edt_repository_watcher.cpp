#include "edt_repository_watcher.h"
#include "edt_path_text.h"
#include "edt_vcs_service.h"
#include <snk/common/logger/engine_log.h>
#include <format>
#include <initializer_list>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

std::optional<edt::vcs::file_stamp> edt::vcs::repository_watcher::default_stat(const fs::path& path)
{
	std::error_code ec;
	const fs::file_status status = fs::status(path, ec);
	if (ec || !fs::exists(status)) return std::nullopt;
	const fs::file_time_type time = fs::last_write_time(path, ec);
	if (ec) return std::nullopt;
	file_stamp out;
	out.mtime = static_cast<std::int64_t>(time.time_since_epoch().count());
	if (fs::is_regular_file(status)) {
		const std::uintmax_t size = fs::file_size(path, ec);
		if (!ec) out.size = static_cast<std::uint64_t>(size);
	}
	return out;
}

std::vector<edt::vcs::dir_entry> edt::vcs::repository_watcher::default_list_dir(const fs::path& path)
{
	std::vector<dir_entry> out;
	std::error_code ec;
	fs::directory_iterator it(path, fs::directory_options::skip_permission_denied, ec);
	const fs::directory_iterator end;
	while (!ec && it != end) {
		std::error_code type_ec;
		// A link is not followed: a link to a folder above would loop.
		const bool is_dir = it->symlink_status(type_ec).type() == fs::file_type::directory;
		out.push_back(dir_entry{ it->path().filename(), is_dir && !type_ec });
		it.increment(ec);
	}
	return out;
}

edt::vcs::repository_watcher::repository_watcher(ports ports)
	: m_ports(std::move(ports))
{
}

void edt::vcs::repository_watcher::follow(const vcs_service& service)
{
	const std::uint64_t generation = service.state_generation();
	if (m_followed_generation == generation) return;
	m_followed_generation = generation;
	const vcs_service::state& state = service.cached_state();
	if (!state.available || !state.git_dir || !state.common_dir) {
		if (m_armed) set_paths({}, {}, {}, {});
		return;
	}
	const std::vector<fs::path> arm{ *state.git_dir, *state.common_dir, state.root.value_or(fs::path{}),
		service.probed_project().value_or(fs::path{}) };
	// The same repository probed again: re-arming would drop a change not polled yet.
	if (m_armed && arm == m_armed_with) return;
	set_paths(arm[0], arm[1], arm[2], arm[3]);
}

void edt::vcs::repository_watcher::set_paths(const fs::path& git_dir, const fs::path& common_dir, const fs::path& repo_root,
	const fs::path& project_root)
{
	m_armed = !git_dir.empty();
	m_armed_with = { git_dir, common_dir, repo_root, project_root };
	m_git_dir = git_dir;
	m_common_dir = common_dir.empty() ? git_dir : common_dir;
	m_work_roots.clear();
	if (!repo_root.empty()) m_work_roots.push_back(repo_root);
	if (!project_root.empty() && project_root.lexically_normal() != repo_root.lexically_normal()) m_work_roots.push_back(project_root);
	m_index.clear();
	m_fixed_refs.clear();
	m_walked_refs.reset();
	m_slow_walks = 0;
	m_ref_folders.clear();
	m_refs_folders_only = false;
	m_warned = false;
	m_own_write = false;
	m_pending_index = false;
	m_last_poll.reset();
	if (!m_armed) return;
	m_index = index_stamps();
	m_fixed_refs = fixed_refs_stamps();
	bool switched = false;
	m_walked_refs = walked_refs_stamps(switched);
	m_last_poll = m_ports.now();
}

void edt::vcs::repository_watcher::begin_own_write()
{
	if (!m_armed) return;
	if (!m_own_write) {
		stamps index = index_stamps();
		if (index != m_index) {
			m_pending_index = true;
			m_index = std::move(index);
		}
	}
	m_own_write = true;
}

edt::vcs::repository_changes edt::vcs::repository_watcher::tick()
{
	repository_changes out;
	if (!m_armed) return out;
	if (m_ports.own_writes_running && m_ports.own_writes_running()) {
		m_own_write = true;
	} else if (m_own_write) {
		// The editor's last git write ended: what it did to the index and the work tree is not news.
		m_own_write = false;
		m_index = index_stamps();
	}
	out.index = std::exchange(m_pending_index, false);
	const clock::time_point now = m_ports.now();
	if (m_last_poll && now - *m_last_poll < k_poll_interval) return out;
	m_last_poll = now;
	if (!m_own_write) {
		stamps index = index_stamps();
		if (index != m_index) {
			out.index = true;
			m_index = std::move(index);
		}
	}
	stamps fixed = fixed_refs_stamps();
	if (fixed != m_fixed_refs) {
		out.refs = true;
		m_fixed_refs = std::move(fixed);
	}
	bool switched = false;
	std::optional<stamps> walked = walked_refs_stamps(switched);
	if (!walked) return out;
	// The first finished walk, or the switch to folders only, is a new baseline: not a change by itself.
	if (switched || !m_walked_refs) {
		m_walked_refs = std::move(walked);
		return out;
	}
	if (*walked != *m_walked_refs) {
		out.refs = true;
		if (m_refs_folders_only) {
			// A new folder (refs/heads/feature/) is watched from now on.
			m_ref_folders = walk_ref_folders();
			walked = walked_refs_stamps(switched);
		}
		m_walked_refs = std::move(walked);
	}
	return out;
}

void edt::vcs::repository_watcher::add(stamps& out, const fs::path& path) const
{
	if (const std::optional<file_stamp> stamp = m_ports.stat_file(path)) out.emplace(path, *stamp);
}

edt::vcs::repository_watcher::stamps edt::vcs::repository_watcher::index_stamps() const
{
	stamps out;
	add(out, m_git_dir / "index");
	for (const fs::path& root : m_work_roots) {
		for (const dir_entry& entry : m_ports.list_dir(root)) {
			if (entry.is_dir || entry.name == ".git") continue;
			add(out, root / entry.name);
		}
	}
	return out;
}

edt::vcs::repository_watcher::stamps edt::vcs::repository_watcher::fixed_refs_stamps() const
{
	stamps out;
	for (const char* name : { "HEAD", "FETCH_HEAD", "ORIG_HEAD", "MERGE_HEAD" }) add(out, m_git_dir / name);
	add(out, m_common_dir / "packed-refs");
	return out;
}

std::optional<edt::vcs::repository_watcher::stamps> edt::vcs::repository_watcher::walked_refs_stamps(bool& switched)
{
	switched = false;
	if (!m_refs_folders_only) {
		stamps files;
		const walk_result result = walk_ref_files(files);
		if (result == walk_result::done) {
			m_slow_walks = 0;
			return files;
		}
		// One slow walk may be the disk waking up: only several in a row switch.
		if (result == walk_result::too_slow && ++m_slow_walks < k_slow_walks_to_switch) return std::nullopt;
		m_refs_folders_only = true;
		switched = true;
		if (!m_warned) {
			m_warned = true;
			egLOG_WARN("vcs", "'{}' holds too many refs to watch each file ({}): only its folders are watched",
				edt::utf8_of(m_common_dir / "refs"), result == walk_result::too_many
					? std::format("over {} entries", k_refs_budget)
					: std::format("{} walks in a row over {} us", k_slow_walks_to_switch, k_refs_walk_budget.count()));
		}
		m_ref_folders = walk_ref_folders();
	}
	stamps out;
	for (const fs::path& folder : m_ref_folders) add(out, folder);
	return out;
}

edt::vcs::repository_watcher::walk_result edt::vcs::repository_watcher::walk_ref_files(stamps& out) const
{
	const clock::time_point start = m_ports.now();
	const auto too_slow = [this, start] { return m_ports.now() - start > k_refs_walk_budget; };
	std::vector<fs::path> pending{ m_common_dir / "refs" };
	std::size_t entries = 0;
	while (!pending.empty()) {
		const fs::path folder = std::move(pending.back());
		pending.pop_back();
		for (const dir_entry& entry : m_ports.list_dir(folder)) {
			if (++entries > k_refs_budget) return walk_result::too_many;
			// Every 64 entries: one huge folder cannot overrun the budget.
			if (entries % 64 == 0 && too_slow()) return walk_result::too_slow;
			if (entry.is_dir) pending.push_back(folder / entry.name);
			else add(out, folder / entry.name);
		}
		if (too_slow()) return walk_result::too_slow;
	}
	return walk_result::done;
}

std::vector<fs::path> edt::vcs::repository_watcher::walk_ref_folders() const
{
	std::vector<fs::path> out;
	std::vector<fs::path> pending{ m_common_dir / "refs" };
	while (!pending.empty() && out.size() < k_refs_budget) {
		fs::path folder = std::move(pending.back());
		pending.pop_back();
		for (const dir_entry& entry : m_ports.list_dir(folder)) {
			if (entry.is_dir) pending.push_back(folder / entry.name);
		}
		out.push_back(std::move(folder));
	}
	return out;
}
