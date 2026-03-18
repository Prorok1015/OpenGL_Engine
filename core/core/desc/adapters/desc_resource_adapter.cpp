#include "desc_resource_adapter.h"
#include "resources/desc_resource.h"

std::shared_ptr<desc::desc_resource> desc::desc_adapter::operator()(res::tag tag, const std::vector<std::byte>& data) const
{
	std::string_view json_data(reinterpret_cast<const char*>(data.data()), data.size());
	json::parse_options opts;
	opts.max_depth = 128;
	auto json = json::parse(json_data, {}, opts);
	return std::make_shared<desc::desc_resource>(tag, json);
}

std::vector<std::byte> desc::desc_adapter::serialize(const res::tag& tag, const std::shared_ptr<res::resource_entry>& resource) const
{
	auto desc_res = std::static_pointer_cast<desc::desc_resource>(resource);
	std::string data = json::serialize(desc_res->body);
	std::vector<std::byte> byte_data;
	byte_data.resize(data.size());
	std::memcpy(byte_data.data(), data.data(), data.size());
	return byte_data;
}
