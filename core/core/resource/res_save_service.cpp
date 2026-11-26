#include <snk/resource/res_save_service.h>
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>
#include <snk/platform/plt_beneath.h>
#include <snk/resource/res_path_sanitize.h>
#include <snk/resource/res_system.h>
#include <algorithm>
#include <format>
#include <memory>
#include <random>
#include <utility>

namespace {
	namespace fs = std::filesystem;

	constexpr std::string_view k_saves_folder = "saves";
	constexpr std::size_t k_max_leaf_bytes = 100;
	constexpr std::string_view k_reserved_suffixes[] = { ".bak", ".tmp", ".corrupt" };

	res::save_result failure(res::save_status status, std::string error)
	{
		res::save_result result;
		result.status = status;
		result.error = std::move(error);
		return result;
	}

	std::string temp_leaf_of(const fs::path& leaf)
	{
		thread_local std::mt19937_64 random{ std::random_device{}() };
		return std::format(".{}.{:016x}.tmp", fs_paths::utf8_of(leaf), random());
	}

	std::string write_one(platform::beneath::pinned_root& pinned, const fs::path& folder, const fs::path& leaf, std::span<const std::byte> data)
	{
		platform::beneath::write_request request;
		request.folder = folder;
		request.leaf = leaf;
		request.tmp_leaf = fs_paths::path_from_utf8(temp_leaf_of(leaf));
		request.data = data;
		request.durable = true;
		request.replace = true;
		request.tmp_path = pinned.path() / folder / request.tmp_leaf;
		return platform::beneath::write_beneath(pinned, request);
	}
}

res::save_service::save_service(ds::task_system& tasks, resource_system& resources)
	: m_tasks(tasks), m_resources(resources)
{
}

res::save_service::~save_service()
{
	m_tasks.wait_idle(m_life);
}

std::string res::save_service::slot_problem(std::string_view slot)
{
	if (std::string problem = portable_path_problem(slot); !problem.empty()) return problem;
	if (slot.size() > k_max_slot_bytes) return std::format("the name is over {} bytes", k_max_slot_bytes);
	if (slot.find('\\') != std::string_view::npos) return "a backslash is not a separator of a slot";
	const std::size_t slash = slot.rfind('/');
	const std::string_view leaf = slash == std::string_view::npos ? slot : slot.substr(slash + 1);
	if (leaf.size() > k_max_leaf_bytes) return std::format("the file name is over {} bytes", k_max_leaf_bytes);
	if (leaf.front() == '.') return "a file name starting with a dot is hidden";
	for (const std::string_view suffix : k_reserved_suffixes) {
		if (leaf.ends_with(suffix)) return std::format("a name ending in {} is a file of the service itself", suffix);
	}
	return {};
}

void res::save_service::set_stage_hook_for_tests(std::function<void(save_stage)> hook)
{
	m_stage_hook = std::move(hook);
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

	std::uint64_t ticket = 0;
	{
		std::lock_guard lock{ m_ticket_mutex };
		ticket = ++m_issued[slot];
	}
	auto data = std::make_shared<const std::vector<std::byte>>(std::move(bytes));
	return m_tasks.run(ds::lane::io, desc, m_life,
		[this, root = roots.back(), slot = std::move(slot), ticket, data, options = std::move(options)](ds::task_context&) {
			return write_now(root, slot, ticket, *data, options);
		});
}

res::save_result res::save_service::write_now(const fs::path& root, const std::string& slot, std::uint64_t ticket,
	std::span<const std::byte> bytes, const save_options& options)
{
	std::lock_guard io{ m_io_mutex };
	{
		std::lock_guard lock{ m_ticket_mutex };
		if (const auto it = m_applied.find(slot); it != m_applied.end() && it->second > ticket) {
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
		if (std::string error = platform::beneath::make_folders_beneath(pinned, folder); !error.empty()) {
			egLOG_WARN("save", "'{}' not saved: {}", slot, error);
			return failure(save_status::io_error, std::move(error));
		}

		stage(save_stage::before_backup);
		std::vector<std::byte> current;
		platform::beneath::read_request request;
		request.root = root;
		request.folder = folder;
		request.leaf = leaf;
		request.max_bytes = k_max_bytes;
		const platform::beneath::read_result found = platform::beneath::read_beneath(request, current);
		bool backup_made = false;
		switch (found.status) {
		case platform::beneath::read_status::ok: {
			bool valid = !current.empty();
			if (valid && options.is_valid) {
				try {
					valid = options.is_valid(current);
				} catch (...) {
					valid = false;
				}
			}
			if (!valid) {
				egLOG_WARN("save", "'{}': the file now at the slot is not a valid save, it is not kept as the backup", slot);
				break;
			}
			fs::path backup_leaf = leaf;
			backup_leaf += ".bak";
			if (std::string error = write_one(pinned, folder, backup_leaf, current); !error.empty()) {
				egLOG_WARN("save", "'{}' not saved, its backup could not be made: {}", slot, error);
				return failure(save_status::io_error, std::format("the backup could not be made: {}", error));
			}
			backup_made = true;
			break;
		}
		case platform::beneath::read_status::missing:
			break;
		case platform::beneath::read_status::too_large:
			return failure(save_status::refused, std::format("the file at the slot is over {} bytes", k_max_bytes));
		default:
			return failure(save_status::refused, found.error.empty() ? std::string{ "what is at the slot is not a regular file" } : found.error);
		}

		stage(save_stage::after_backup);
		stage(save_stage::before_replace);
		if (std::string error = write_one(pinned, folder, leaf, bytes); !error.empty()) {
			egLOG_WARN("save", "'{}' not saved: {}", slot, error);
			return failure(save_status::io_error, std::move(error));
		}

		{
			std::lock_guard lock{ m_ticket_mutex };
			std::uint64_t& applied = m_applied[slot];
			applied = std::max(applied, ticket);
		}
		save_result result;
		result.bytes = bytes.size();
		result.backup_made = backup_made;
		return result;
	} catch (const std::exception& problem) {
		egLOG_WARN("save", "'{}' not saved: {}", slot, problem.what());
		return failure(save_status::io_error, std::format("interrupted: {}", problem.what()));
	} catch (...) {
		return failure(save_status::io_error, "interrupted");
	}
}
