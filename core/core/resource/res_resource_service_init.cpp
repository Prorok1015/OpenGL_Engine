#include <snk/resource/res_resource_service_init.h>
#include <snk/resource/res_system.h>
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/project/prj_project.h>

void core::res::resource_init(ds::app_data_storage& data)
{
	auto& tasks = data.require<ds::task_system>();
	if (data.has_value<prj::project_context>()) {
		const auto& project = data.require<prj::project_context>();
		data.construct<::res::resource_system>(std::ref(tasks),
			::res::resource_system::project_mount_table(project.content_roots, project.cache_dir() / "memory"));
		return;
	}
	data.construct<::res::resource_system>(std::ref(tasks));
}

void core::res::resource_term(ds::app_data_storage& data)
{
	data.destruct<::res::resource_system>();
}