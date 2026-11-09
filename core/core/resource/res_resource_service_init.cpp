#include <snk/resource/res_resource_service_init.h>
#include <snk/resource/res_system.h>
#include <snk/resource/res_shipping_dirs.h>
#include <snk/config/cfg_variables.h>
#include <snk/platform/plt_process.h>
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/project/prj_project.h>
#include <snk/common/logger/engine_log.h>
#include <filesystem>
#include <format>
#include <system_error>
#include <utility>

// user:// of a read-only project (res/read_only_project) instead of the player's data folder: CI, a portable build.
CFG_VAR_DEF_PATH(cfg_res_user_path, "resource/user_path", "");

namespace {
	// memory:// of a read-only project: this run's own folder, removed by resource_term.
	struct scratch_folder
	{
		explicit scratch_folder(std::filesystem::path folder) : path(std::move(folder)) {}
		std::filesystem::path path;
	};

	void create_folder(const prj::project_context& project, const std::filesystem::path& dir)
	{
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		if (ec) {
			throw prj::project_error(std::format("project '{}': cannot create '{}': {}", project.data.name, dir.string(), ec.message()));
		}
	}

	// A shipped game (#593): nothing is created or written in the project; user:// and memory:// live outside it.
	void mount_read_only_project(ds::app_data_storage& data, const prj::project_context& project,
		const std::filesystem::path& engine_base)
	{
		std::filesystem::path user_dir = cfg_res_user_path->empty()
			? core::res::player_data_dir(project.data.name)
			: std::filesystem::absolute(cfg_res_user_path->string());
		if (user_dir.empty()) {
			throw prj::project_error(std::format("project '{}': no folder for user:// (LOCALAPPDATA, XDG_DATA_HOME and HOME "
				"are not set); set resource/user_path", project.data.name));
		}
		if (const std::size_t stale = core::res::remove_stale_scratch_dirs(project.data.name); stale > 0) {
			egLOG_INFO("resource", "removed {} scratch folder(s) of ended runs", stale);
		}
		const std::filesystem::path scratch = core::res::scratch_dir_for(project.data.name, platform::current_process_id());
		create_folder(project, user_dir);
		create_folder(project, scratch);
		data.construct<scratch_folder>(scratch);
		data.construct<::res::resource_system>(std::ref(data.require<ds::task_system>()),
			::res::resource_system::shipping_mount_table(engine_base, project.content_roots, user_dir, scratch));
		egLOG_INFO("resource", "project '{}' mounted read-only; user:// in {}", project.data.name, user_dir.string());
	}
}

void core::res::resource_init(ds::app_data_storage& data)
{
	auto& tasks = data.require<ds::task_system>();
	if (!::res::resource_system::watches_files()) {
		egLOG_INFO("resource", "file watching is off (res/watch_files)");
	}
	if (data.has_value<prj::project_context>()) {
		const auto& project = data.require<prj::project_context>();
		const auto engine_base = ::res::resource_system::configured_engine_base();
		std::error_code ec;
		if (!engine_base.empty() && !std::filesystem::is_directory(engine_base, ec)) {
			throw prj::project_error(std::format("project '{}': the engine resources '{}' (resource/base_path) are not a directory",
				project.data.name, engine_base.string()));
		}

		// The directories may have changed since the project was opened: nothing outside it gets mounted. Checked
		// before the writable folders are created (a symlinked .cache must not get folders made through it) and after.
		const auto verify = [&project, &engine_base] {
			if (auto problem = prj::verify_project_mounts(project, engine_base)) {
				throw prj::project_error(std::format("project '{}': {}", project.data.name, *problem));
			}
		};
		verify();
		if (::res::resource_system::read_only_project()) {
			mount_read_only_project(data, project, engine_base);
			return;
		}
		for (const auto& dir : project.writable_dirs()) {
			std::filesystem::create_directories(dir, ec);
			if (ec) {
				throw prj::project_error(std::format("project '{}': cannot create '{}': {}", project.data.name, dir.string(), ec.message()));
			}
		}
		verify();

		std::filesystem::path code_dir = project.code_dir();
		if (!std::filesystem::is_directory(code_dir, ec)) {
			code_dir.clear();
		}
		data.construct<::res::resource_system>(std::ref(tasks),
			::res::resource_system::project_mount_table(engine_base, project.content_roots, project.cache_dir(),
				project.user_profile_dir(), code_dir));
		return;
	}
	data.construct<::res::resource_system>(std::ref(tasks));
}

void core::res::resource_term(ds::app_data_storage& data)
{
	data.destruct<::res::resource_system>();
	if (data.has_value<scratch_folder>()) {
		std::error_code ec;
		std::filesystem::remove_all(data.require<scratch_folder>().path, ec);
		if (ec) {
			egLOG_WARN("resource", "cannot remove the scratch folder '{}': {}", data.require<scratch_folder>().path.string(), ec.message());
		}
		data.destruct<scratch_folder>();
	}
}