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

std::optional<res::tag> res::resource_resolver::path_mapper(const std::filesystem::path& path) const
{
	for (auto& entry : entry_points)
	{
		std::filesystem::path full_entry = std::filesystem::absolute(entry);
		std::filesystem::path full_path = std::filesystem::absolute(path);
		if (full_path.string().find(full_entry.string()) == 0) {
			std::string relative_path = full_path.string().substr(full_entry.string().length());
			return tag::make(relative_path);
		}
	}
	return std::nullopt;
}

core::res::res_resolver_interface::async_raw_data res::resource_resolver::resolve(const tag& tag) const
{
	auto data_sp = std::make_shared<core::res::res_control_block<std::vector<std::byte>>>();
	auto path = resolve_tag(tag);

	boost::asio::post(io.get_executor(), [data_sp, path]() {
		try {
			egLOG("resource/resolver", "loading file '{}'", path.string());

			if (!std::filesystem::exists(path)) {
				data_sp->set_error("File does not exist: " + path.string());
				return;
			}

			std::ifstream file(path, std::ios::binary | std::ios::ate);
			if (!file.is_open()) {
				data_sp->set_error("Failed to open file: " + path.string());
				return;
			}

			auto size = file.tellg();
			std::vector<std::byte> data;

			if (size > 0) {
				file.seekg(0, std::ios::beg);
				data.resize(static_cast<size_t>(size));
				if (!file.read(reinterpret_cast<char*>(data.data()), size)) {
					data_sp->set_error("Failed to read data: " + path.string());
					return;
				}
			}

			data_sp->set_ready(std::move(data));

		}
		catch (const std::exception& e) {
			data_sp->set_error(std::string("Internal IO error: ") + e.what());
			egLOG("resource/error", "Critical error loading {}: {}", path.string(), e.what());
		}
		catch (...) {
			data_sp->set_error("Unknown critical error");
		}
	});
	return data_sp;
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