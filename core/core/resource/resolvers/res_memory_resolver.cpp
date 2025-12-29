#include "res_memory_resolver.h"
#include "res_control_block.hpp"

core::res::res_resolver_interface::async_raw_data res::memory_resolver::resolve(const res::tag& tag) const
{
	auto cb = std::make_shared<core::res::res_control_block<std::vector<std::byte>>>();
	auto cdata = m_memory.at(tag);
	cb->set_ready(std::move(cdata));
	return cb;
}