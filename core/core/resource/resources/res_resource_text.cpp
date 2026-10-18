#include <snk/resource/resources/res_resource_text.h>
#include <snk/resource/res_system.h>

res::text_resource::text_resource(const tag& tag, const std::vector<std::byte>& string)
	: resource_entry(tag)
{
	content.resize(string.size());
	if (!string.empty()) std::memcpy(content.data(), string.data(), string.size());
}