#include "res_system.h"
#include "resources/res_resource_picture.h"
#include "resources/res_resource_text.h"
#include "resolvers/res_tag_resolver.h"
#include "adapters/res_pct_adapter.h"
#include "adapters/res_text_adapter.h"
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
	: memory_resolver_(registrate_resolver<memory_resolver>(res::tag::memory))
{
	ASSERT_MSG(std::filesystem::exists(cfg_res_path), "Resources path '{0}' does not exist!", cfg_res_path->string());

	auto& res_resolver = registrate_resolver<resource_resolver>(tag::default_protocol(), std::vector<std::string>{ cfg_res_path->string() });
	res_resolver.set_resource_changed_callback([this](const res::tag& tag) { signal_changed(tag); });

	registrate_adapter(res::raw_image_adapter::INFO, 
		std::bind(res::raw_image_adapter{},
			std::placeholders::_1,
			std::placeholders::_2));

	registrate_adapter(res::pct_adapter::INFO,
	std::bind(res::pct_adapter{},
			std::placeholders::_1,
			std::placeholders::_2)
		);

	registrate_adapter(res::text_adapter::INFO,
	std::bind(res::text_adapter{},
			std::placeholders::_1,
			std::placeholders::_2)
		);
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
