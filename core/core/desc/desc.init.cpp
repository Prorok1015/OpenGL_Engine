#include "desc_init.h"
#include "res_system.h"
#include "adapters/desc_resource_adapter.h"
#include "desc_system.h"
#include "desc_load_job.h"

desc::desc_load_job load_job;

void components::desc_init(ds::app_data_storage& data)
{
	auto& res_system = data.require<res::resource_system>();
	res_system.registrate_adapter(desc::desc_adapter::INFO, 
		std::bind(desc::desc_adapter{},
			std::placeholders::_1,
			std::placeholders::_2
		));

	load_job.internal_init(&data.construct<desc::desc_system>(res_system));
}

void components::desc_term(ds::app_data_storage& data)
{
	load_job.internal_init(nullptr);
	data.destruct<desc::desc_system>();
	auto& res_system = data.require<res::resource_system>();
	res_system.unregistrate_adapter(desc::desc_adapter::INFO);
}
