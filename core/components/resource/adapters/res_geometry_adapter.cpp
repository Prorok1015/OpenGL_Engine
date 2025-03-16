#include "res_geometry_adapter.h"
#include "common.h"
#include <boost/json.hpp>

std::shared_ptr<res::Resource> res::geometry_adapter::operator()(res::tag, const std::vector<std::byte>& data) const
{
	
	return std::shared_ptr<res::Resource>();
}
