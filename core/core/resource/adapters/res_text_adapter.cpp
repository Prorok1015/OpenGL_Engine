#include "res_text_adapter.h"

std::shared_ptr<res::text_resource> res::text_adapter::operator()(res::tag tag, const std::vector<std::byte>& data) const
{
	return std::make_shared<res::text_resource>(tag, data);
}

std::vector<std::byte> res::text_adapter::serialize(const res::tag& tag, const std::shared_ptr<res::resource_entry>& resource) const
{
	auto text_res = std::static_pointer_cast<res::text_resource>(resource);
	
	auto& data = text_res->str();
	std::vector<std::byte> byte_data;
	byte_data.resize(data.size());
	std::memcpy(byte_data.data(), data.data(), data.size());
	return byte_data;
}
