#include "desc_desc_service_init.h"
#include "res_system.h"
#include "adapters/desc_resource_adapter.h"
#include "adapters/desc_adapter_t.h"
#include "desc_system.h"
#include "desc_load_job.h"

desc::desc_load_job load_job;

void core::desc::desc_init(ds::app_data_storage& data)
{
	auto& res_system = data.require<::res::resource_system>();
	auto& desc_system = data.construct<::desc::desc_system>(std::ref(res_system));
	res_system.registrate_adapter<::desc::desc_adapter>(::desc::desc_adapter::INFO);
	res_system.registrate_adapter<core::desc::desc_adapter_t>(core::desc::desc_adapter_t::INFO, std::ref(res_system), std::ref(desc_system));
	
	load_job.internal_init(&desc_system);
}

void core::desc::desc_term(ds::app_data_storage& data)
{
	load_job.internal_init(nullptr);
	data.destruct<::desc::desc_system>();
	auto& res_system = data.require<::res::resource_system>();
	res_system.unregistrate_adapter(::desc::desc_adapter::INFO);
}
