#include "res_resource_text.h"
#include "res_system.h"

res::text_resource::text_resource(const tag& tag, const std::vector<std::byte>& string)
	: resource_entry(tag)
{
	content.resize(string.size());
	std::memcpy(content.data(), string.data(), string.size());
}