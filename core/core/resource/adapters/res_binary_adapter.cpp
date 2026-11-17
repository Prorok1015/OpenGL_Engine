#include <snk/resource/adapters/res_binary_adapter.h>

std::shared_ptr<res::resource_entry> res::binary_adapter::deserialize(const res::tag& tag, const std::vector<std::byte>& raw_data) const
{
	return std::make_shared<res::binary_resource>(tag, raw_data);
}

std::vector<std::byte> res::binary_adapter::serialize(const res::tag&, const std::shared_ptr<res::resource_entry>& resource) const
{
	return std::static_pointer_cast<res::binary_resource>(resource)->bytes();
}
