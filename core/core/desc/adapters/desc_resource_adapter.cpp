#include "desc_resource_adapter.h"
#include "resources/desc_resource.h"

std::shared_ptr<desc::desc_resource> desc::desc_adapter::operator()(res::tag tag, const std::vector<std::byte>& data) const
{
	std::string_view json_data(reinterpret_cast<const char*>(data.data()), data.size());
	auto json = json::parse(json_data);
	return std::make_shared<desc::desc_resource>(tag, json);
}
