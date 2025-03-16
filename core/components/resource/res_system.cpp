#include "res_system.h"
#include "res_picture.h"
#include "res_resource_text_file.h"
#include "res_resource_model.h"
#include "path_resolvers/res_tag_resolver.h"
#include "adapters/res_pct_adapter.h"
#include <boost/json.hpp>

res::resource_system* p_res_system = nullptr;
std::string res::resource_system::s_res_path = RESOURCE_PATH;

res::resource_system& res::get_system()
{
	ASSERT_MSG(p_res_system, "Resource system is nullptr!");
	return *p_res_system;
}

res::resource_system::resource_system()
{
	if (s_res_path.empty()) {
		ASSERT_MSG(std::filesystem::exists("./res/"), "The './res/' folder should be exist next to your exe");
		s_res_path = std::filesystem::absolute("./res/").generic_string();
	}

	registrate_resolver(tag::memory, 
		std::bind(&memory_resolver::operator(),
			std::addressof(memory_resolver_),
			std::placeholders::_1));

	registrate_resolver(tag::default_protocol(),
		std::bind(resource_resolver{}, std::placeholders::_1));

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

std::string res::resource_system::get_absolut_path(const res::tag& tag)
{
	if (tag.protocol() == res::tag::default_protocol()) {
		std::string path{ tag.path() };
		std::string name{ tag.name() };
		return s_res_path + path + name;
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
