#include "res_system.h"
#include "resolvers/res_vfs_resolver.h"
#include "adapters/res_pct_adapter.h"
#include "adapters/res_text_adapter.h"
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

	auto& res_resolver = registrate_resolver<vfs_resolver>(tag::default_protocol(), std::vector<fs::path>{ cfg_res_path->string() });
	res_resolver.set_resource_changed_callback([this](const res::tag& tag) { signal_changed(tag); });
	fs::path memory_path = fs::path{ cfg_res_path->string() } / "../sandbox"; // TODO: make configurable
	auto& memory_resolver = registrate_resolver<vfs_resolver>(res::tag::memory, std::vector<fs::path>{ memory_path });
	memory_resolver.set_resource_changed_callback([this](const res::tag& tag) { signal_changed(tag); });

	registrate_adapter<raw_image_adapter>(res::raw_image_adapter::INFO);
	registrate_adapter<pct_adapter>(res::pct_adapter::INFO);
	registrate_adapter<text_adapter>(res::text_adapter::INFO);
}

void res::resource_system::post_adapter_work(std::function<void()> cb)
{
	if (m_adapter_worker)
		m_adapter_worker->post(cb);
}

void res::resource_system::register_alias(const res::tag& original, const res::tag& alias)
{
	m_aliases[original] = alias;

	{
		std::unique_lock lock(m_cache_mutex);
		if (auto it = m_cache.find(original); it != m_cache.end()) {
			m_cache[alias] = it->second;
		}
	}

	{
		std::unique_lock lock(m_pinning_mutex);
		if (auto it = m_pinned_resources.find(original); it != m_pinned_resources.end()) {
			m_pinned_resources[alias] = it->second;
		}
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

	egLOG("resource/absolut_path", "Broken tag {}", tag.view());
	return std::string{};
}
