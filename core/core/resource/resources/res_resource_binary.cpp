#include <snk/resource/resources/res_resource_binary.h>

res::binary_resource::binary_resource(const tag& tag, std::vector<std::byte> bytes)
	: resource_entry(tag)
	, m_bytes(std::move(bytes))
{
}
