#include <snk/resource/res_save_service.h>
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>
#include <snk/platform/plt_beneath.h>
#include <snk/resource/res_path_sanitize.h>
#include <snk/resource/res_system.h>
#include <algorithm>
#include <cctype>
#include <format>
#include <map>
#include <memory>
#include <random>
#include <utility>

struct res::save_service::shared_state
{
	struct slot_state {
		std::uint64_t issued = 0;
		std::uint64_t applied = 0;
		std::uint32_t pending = 0;
	};
	std::mutex mutex;
	std::map<std::string, slot_state> slots;
	std::uint64_t queued_bytes = 0;
	std::uint64_t queue_limit = res::save_service::k_max_queued_bytes;
};

namespace {
	namespace fs = std::filesystem;
	using state_t = res::save_service::shared_state;

	constexpr std::string_view k_saves_folder = "saves";
	constexpr std::string_view k_reserved_suffixes[] = { ".bak", ".tmp", ".corrupt" };
	constexpr std::string_view k_forbidden_characters = "<>\"|?*\\";
	constexpr std::size_t k_max_listed_in_a_folder = 4096;

	res::save_result failure(res::save_status status, std::string error)
	{
		res::save_result result;
		result.status = status;
		result.error = std::move(error);
		return result;
	}

	char lower_of(char c)
	{
		return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
	}

	std::string lowered(std::string_view text)
	{
		std::string out(text);
		std::ranges::transform(out, out.begin(), lower_of);
		return out;
	}

	bool ends_with_ignoring_case(std::string_view text, std::string_view suffix)
	{
		return text.size() >= suffix.size() && lowered(text.substr(text.size() - suffix.size())) == suffix;
	}

	bool valid_utf8(std::string_view text)
	{
		std::size_t i = 0;
		while (i < text.size()) {
			const auto lead = static_cast<unsigned char>(text[i]);
			std::size_t extra = 0;
			std::uint32_t code = 0;
			if (lead < 0x80) {
				++i;
				continue;
			} else if (lead >= 0xC2 && lead <= 0xDF) {
				extra = 1;
				code = lead & 0x1F;
			} else if (lead >= 0xE0 && lead <= 0xEF) {
				extra = 2;
				code = lead & 0x0F;
			} else if (lead >= 0xF0 && lead <= 0xF4) {
				extra = 3;
				code = lead & 0x07;
			} else {
				return false;
			}
			if (i + extra >= text.size()) return false;
			for (std::size_t k = 1; k <= extra; ++k) {
				const auto next = static_cast<unsigned char>(text[i + k]);
				if ((next & 0xC0) != 0x80) return false;
				code = (code << 6) | (next & 0x3F);
			}
			if ((extra == 2 && code < 0x800) || (extra == 3 && code < 0x10000) || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
				return false;
			}
			i += extra + 1;
		}
		return true;
	}

	std::string temp_leaf_of(const fs::path& leaf)
	{
		thread_local std::mt19937_64 random{ std::random_device{}() };
		return std::format(".{}.{:016x}.tmp", fs_paths::utf8_of(leaf), random());
	}

	// What a temp file of the service is called: .<name>.<16 hex digits>.tmp
	bool is_temp_file_name(std::string_view name)
	{
		constexpr std::size_t k_tail = 1 + 16 + 4;
		if (name.size() <= k_tail + 1 || name.front() != '.' || !name.ends_with(".tmp")) return false;
		const std::string_view digits = name.substr(name.size() - 4 - 16, 16);
		return name[name.size() - k_tail] == '.' && std::ranges::all_of(digits, [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
	}

	// Takes its share of the queue on construction and gives it back once: when its write is done, or when the task is
	// dropped without running (cancelled), whichever comes first.
	struct ticket_holder {
		std::shared_ptr<state_t> state;
		std::string key;
		std::uint64_t bytes = 0;
		bool released = false;

		void release()
		{
			std::lock_guard lock{ state->mutex };
			if (std::exchange(released, true)) return;
			state->queued_bytes -= bytes;
			const auto found = state->slots.find(key);
			if (found != state->slots.end() && --found->second.pending == 0) state->slots.erase(found);
		}
		~ticket_holder() { release(); }
	};
}

res::save_service::save_service(ds::task_system& tasks, resource_system& resources)
	: m_tasks(tasks), m_resources(resources), m_state(std::make_shared<shared_state>())
{
}

res::save_service::~save_service()
{
	m_tasks.wait_idle(m_life);
}

void res::save_service::flush()
{
	m_tasks.wait_idle(m_life);
}

std::string res::save_service::slot_problem(std::string_view slot)
{
	if (!valid_utf8(slot)) return "the name is not valid UTF-8";
	if (std::string problem = portable_path_problem(slot); !problem.empty()) return problem;
	if (slot.size() > k_max_slot_bytes) return std::format("the name is over {} bytes", k_max_slot_bytes);
	if (const std::size_t at = slot.find_first_of(k_forbidden_characters); at != std::string_view::npos) {
		return std::format("'{}' is not allowed in a name", slot[at]);
	}
	std::size_t begin = 0;
	while (begin <= slot.size()) {
		const std::size_t end = slot.find('/', begin);
		const std::string_view part = slot.substr(begin, end == std::string_view::npos ? end : end - begin);
		if (part.size() > k_max_name_bytes) return std::format("a part of the name is over {} bytes", k_max_name_bytes);
		if (part.front() == '.') return "a part starting with a dot is hidden";
		for (const std::string_view suffix : k_reserved_suffixes) {
			if (ends_with_ignoring_case(part, suffix)) return std::format("a part ending in {} is a file of the service itself", suffix);
		}
		if (end == std::string_view::npos) break;
		begin = end + 1;
	}
	return {};
}

void res::save_service::set_stage_hook_for_tests(std::function<void(save_stage)> hook)
{
	std::lock_guard lock{ m_io_mutex };
	m_stage_hook = std::move(hook);
}

void res::save_service::set_queue_limit_for_tests(std::uint64_t bytes)
{
	std::lock_guard lock{ m_state->mutex };
	m_state->queue_limit = bytes;
}

void res::save_service::set_write_hook_for_tests(platform::beneath::write_hook hook)
{
	std::lock_guard lock{ m_io_mutex };
	m_write_hook = std::move(hook);
}

ds::task_handle<res::save_result> res::save_service::write(std::string_view slot_view, std::vector<std::byte> bytes, save_options options)
{
	std::string slot{ slot_view };
	const ds::task_desc desc{ .name = std::format("save {}", slot), .category = "save" };

	const auto answer = [&](save_result result) {
		return m_tasks.run(ds::lane::io, desc, m_life, [result = std::move(result)](ds::task_context&) { return result; });
	};
	if (std::string problem = slot_problem(slot); !problem.empty()) {
		return answer(failure(save_status::refused, std::format("'{}' is not a name of a save: {}", slot, problem)));
	}
	if (bytes.size() > k_max_bytes) {
		return answer(failure(save_status::too_large, std::format("{} bytes, the limit is {}", bytes.size(), k_max_bytes)));
	}
	const std::vector<fs::path> roots = m_resources.mount_roots(tag::user);
	if (roots.empty()) {
		return answer(failure(save_status::refused, "user:// is not mounted"));
	}

	// One slot of one folder, whatever the case of its name: a file system may not tell x from X.
	const fs::path& root = roots.back();
	std::string key = lowered(fs_paths::utf8_of(root));
	key += '\n';
	key += lowered(slot);
	std::uint64_t ticket = 0;
	std::uint64_t queued_before = 0;
	std::uint64_t queue_limit = 0;
	bool busy = false;
	{
		std::lock_guard lock{ m_state->mutex };
		queued_before = m_state->queued_bytes;
		queue_limit = m_state->queue_limit;
		busy = queued_before + bytes.size() > m_state->queue_limit;
		if (!busy) {
			m_state->queued_bytes += bytes.size();
			shared_state::slot_state& state = m_state->slots[key];
			ticket = ++state.issued;
			++state.pending;
		}
	}
	if (busy) {
		return answer(failure(save_status::busy, std::format("{} bytes are waiting to be written, the limit is {}", queued_before, queue_limit)));
	}
	auto holder = std::make_shared<ticket_holder>();
	holder->state = m_state;
	holder->key = key;
	holder->bytes = bytes.size();
	auto data = std::make_shared<const std::vector<std::byte>>(std::move(bytes));
	return m_tasks.run(ds::lane::io, desc, m_life,
		[this, root, slot = std::move(slot), key = std::move(key), ticket, data, options = std::move(options), holder](ds::task_context&) {
			save_result result = write_now(root, slot, key, ticket, *data, options);
			holder->release();
			return result;
		});
}

void res::save_service::sweep_stale_temp_files(platform::beneath::pinned_root& pinned, const fs::path& folder)
{
	const platform::beneath::list_result listed = platform::beneath::list_beneath(pinned, folder, k_max_listed_in_a_folder);
	if (!listed.error.empty()) {
		egLOG_WARN("save", "cannot look for the temp files a crash left in '{}': {}", fs_paths::utf8_of(folder), listed.error);
		return;
	}
	for (const platform::beneath::listed_entry& entry : listed.entries) {
		if (entry.kind != platform::beneath::entry_kind::file || !is_temp_file_name(fs_paths::utf8_of(entry.name))) continue;
		if (std::string error = platform::beneath::remove_beneath(pinned, folder, entry.name, true); !error.empty()) {
			egLOG_WARN("save", "the temp file '{}' a crash left was not removed: {}", fs_paths::utf8_of(entry.name), error);
		}
	}
}

res::save_result res::save_service::write_now(const fs::path& root, const std::string& slot, const std::string& key, std::uint64_t ticket,
	std::span<const std::byte> bytes, const save_options& options)
{
	std::lock_guard io{ m_io_mutex };
	{
		std::lock_guard lock{ m_state->mutex };
		if (const auto found = m_state->slots.find(key); found != m_state->slots.end() && found->second.applied > ticket) {
			return failure(save_status::superseded, "a newer write of the slot is already on disk");
		}
	}

	const auto stage = [this](save_stage at) {
		if (m_stage_hook) m_stage_hook(at);
	};
	try {
		const fs::path relative = fs_paths::path_from_utf8(std::format("{}/{}", k_saves_folder, slot));
		const fs::path folder = relative.parent_path();
		const fs::path leaf = relative.filename();
		platform::beneath::pinned_root pinned{ root };
		const auto write_one = [&](const fs::path& name, std::span<const std::byte> data) {
			platform::beneath::write_request request;
			request.folder = folder;
			request.leaf = name;
			request.tmp_leaf = fs_paths::path_from_utf8(temp_leaf_of(name));
			request.data = data;
			request.durable = true;
			request.replace = true;
			request.hook = &m_write_hook;
			request.tmp_path = pinned.path() / folder / request.tmp_leaf;
			return platform::beneath::write_beneath(pinned, request);
		};
		if (std::string error = platform::beneath::make_folders_beneath(pinned, folder); !error.empty()) {
			egLOG_WARN("save", "'{}' not saved: {}", slot, error);
			return failure(save_status::io_error, std::move(error));
		}
		if (m_swept.insert(lowered(fs_paths::utf8_of(root)) + '\n' + lowered(fs_paths::utf8_of(folder))).second) {
			sweep_stale_temp_files(pinned, folder);
		}

		stage(save_stage::before_read);
		std::vector<std::byte> current;
		platform::beneath::read_request request;
		request.root = root;
		request.folder = folder;
		request.leaf = leaf;
		request.max_bytes = k_max_bytes;
		const platform::beneath::read_result found = platform::beneath::read_beneath(request, current);
		bool keep_as_backup = false;
		switch (found.status) {
		case platform::beneath::read_status::ok:
			keep_as_backup = !current.empty();
			if (keep_as_backup && options.is_valid) {
				try {
					keep_as_backup = options.is_valid(current);
				} catch (...) {
					keep_as_backup = false;
				}
			}
			if (!keep_as_backup) {
				egLOG_WARN("save", "'{}': the file now at the slot is not a valid save, it is not kept as the backup", slot);
			}
			break;
		case platform::beneath::read_status::missing:
			break;
		case platform::beneath::read_status::not_regular:
			return failure(save_status::refused, found.error.empty() ? std::string{ "what is at the slot is not a regular file" } : found.error);
		case platform::beneath::read_status::too_large:
		case platform::beneath::read_status::failed:
			// A file nobody can read is no save to keep, and it must not make the slot unwritable for good.
			egLOG_WARN("save", "'{}': the file now at the slot cannot be read ({}), it is not kept as the backup and is replaced", slot,
				found.status == platform::beneath::read_status::too_large ? std::string{ "too large" } : found.error);
			break;
		}

		stage(save_stage::before_replace);
		if (std::string error = write_one(leaf, bytes); !error.empty()) {
			egLOG_WARN("save", "'{}' not saved: {}", slot, error);
			return failure(save_status::io_error, std::move(error));
		}
		stage(save_stage::after_replace);
		{
			std::lock_guard lock{ m_state->mutex };
			shared_state::slot_state& state = m_state->slots[key];
			state.applied = std::max(state.applied, ticket);
		}

		save_result result;
		result.bytes = bytes.size();
		if (keep_as_backup) {
			fs::path backup_leaf = leaf;
			backup_leaf += ".bak";
			if (std::string error = write_one(backup_leaf, current); !error.empty()) {
				egLOG_WARN("save", "'{}' saved, its backup could not be made: {}", slot, error);
				result.backup_error = std::move(error);
			} else {
				result.backup_made = true;
			}
		}
		return result;
	} catch (const std::exception& problem) {
		egLOG_WARN("save", "'{}' not saved: {}", slot, problem.what());
		return failure(save_status::io_error, std::format("interrupted: {}", problem.what()));
	} catch (...) {
		return failure(save_status::io_error, "interrupted");
	}
}
