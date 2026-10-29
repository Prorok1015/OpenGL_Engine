#include <snk/resource/res_resource_service_init.h>
#include <snk/resource/res_system.h>
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/project/prj_project.h>
#include <filesystem>
#include <format>
#include <system_error>

void core::res::resource_init(ds::app_data_storage& data)
{
	auto& tasks = data.require<ds::task_system>();
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
}