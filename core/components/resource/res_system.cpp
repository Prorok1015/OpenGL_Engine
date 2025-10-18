#include "res_system.h"
#include "res_picture.h"
#include "res_resource_text_file.h"
#include "res_resource_model.h"
#include "path_resolvers/res_tag_resolver.h"
#include "adapters/res_pct_adapter.h"
#include <boost/json.hpp>
#include "cfg_api.h"

res::resource_system* p_res_system = nullptr;

CFG_VAR_DEF_PATH(cfg_res_path, "resource/path", "./res");

res::resource_system& res::get_system()
{
	ASSERT_MSG(p_res_system, "Resource system is nullptr!");
	return *p_res_system;
}

res::resource_system::resource_system()
{
	ASSERT_MSG(std::filesystem::exists(cfg_res_path), "Resources path '{0}' does not exist!", cfg_res_path->string());

	registrate_resolver(tag::memory, 
		std::bind(&memory_resolver::operator(),
			std::addressof(memory_resolver_),
			std::placeholders::_1));

	registrate_resolver(tag::default_protocol(),
		std::bind(resource_resolver{ { cfg_res_path->string() }}, std::placeholders::_1));

	registrate_adapter(res::raw_image_adapter::EXTENSION, 
		std::bind(res::raw_image_adapter{},
			std::placeholders::_1,
			std::placeholders::_2));

	for (auto& ext : res::pct_adapter::EXTENSIONS) {
		registrate_adapter(ext,
		std::bind(res::pct_adapter{},
				std::placeholders::_1,
				std::placeholders::_2)
			);
	}
}

std::filesystem::path res::resource_system::get_resources_path()
{
	return cfg_res_path;
}

std::string res::resource_system::get_absolut_path(const res::tag& tag)
{
	if (tag.protocol() == res::tag::default_protocol()) {
		std::string path{ tag.path() };
		std::string name{ tag.name() };
		return cfg_res_path->string() + path + name;
	}

	egLOG("resource/absolut_path", "Broken tag {}", tag.get_full());
	return std::string{};
}

std::shared_ptr<res::Resource> res::resource_system::find_cache(const res::tag& tag) const
{
	auto it = std::find_if(cache_.begin(), cache_.end(), [&tag](auto res) { return *res == tag; });
	if (it != cache_.end()) {
		return *it;
	}

	return nullptr;
}
