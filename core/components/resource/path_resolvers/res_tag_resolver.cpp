#include "res_tag_resolver.h"
#include "common.h"
#include <fstream>
#include "logger/engine_log.h"

std::future<std::vector<std::byte>> res::resource_resolver::operator()(const tag& tag) const
{
	auto path = resolve_tag(tag);
	return std::async(std::launch::async, [path = std::move(path)]()
		{
			std::vector<std::byte> data;
			if (!path.empty()) {
				std::ifstream file(path, std::ios::binary | std::ios::ate);

				if (file.is_open()) {
					auto size = file.tellg();
					file.seekg(0, std::ios::beg);
					data.resize(size);
					file.read(reinterpret_cast<char*>(data.data()), size);
				}
			}
			return data;
		});
}

std::filesystem::path res::resource_resolver::resolve_tag(const tag& tag) const
{
	for (auto& entry : entry_points)
	{
		std::filesystem::path path = entry + std::string{ tag.path() } + std::string{ tag.name() };
		
		if (std::filesystem::exists(path)) {
			return path;
		}
	}
	egLOG("resource/resolve", "Tag '{0}' is not resolved!", tag.get_full());
	return {};
}