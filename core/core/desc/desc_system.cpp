#include <snk/desc/desc_system.h>

desc::desc_system::desc_system(res::resource_system& res_system_)
	: m_res_system(res_system_)
{
}

desc::desc_system::desc_system(res::resource_system& res_system_, const desc_system* parent)
	: m_res_system(res_system_)
	, m_parent(parent)
	, m_schemas(parent ? &parent->m_schemas : nullptr)
{
}
