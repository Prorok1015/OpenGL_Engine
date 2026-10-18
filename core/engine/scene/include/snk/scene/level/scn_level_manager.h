#pragma once
#include <snk/scene/level/scn_level.h>
#include <snk/resource/res_system.h>
#include <snk/scene/level/scn_hot_reload_manager.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <snk/common/snk_api.h>

namespace scn
{
	class SNK_API level_manager
	{
	public:
		level_manager(res::resource_system& resource, 
					  scn::ecs_assembler& assembler, 
					  ecs::system_factory& system_factory, 
					  std::shared_ptr<scn::hot_reload_manager> hot_manager = nullptr)
			: m_resource(resource)
			, m_assembler(assembler)
			, m_system_factory(system_factory)
			, active_lvl(hot_manager)
			, m_hot_reload_manager(hot_manager)
		{}
		~level_manager();
		level_manager(const level_manager&) = delete;
		level_manager(level_manager&&) = default;
		level_manager& operator=(const level_manager&) = delete;
		level_manager& operator=(level_manager&&) = default;

		void update(std::chrono::duration<float> dt);

		scn::level& get_level() { return active_lvl; }

		bool load(const res::tag& level_tag);

		// Called after every successful hot reload (world list or only the systems changed), so owners
		// of per-world data (the editor) can resync. Worlds created at runtime survive the reload.
		void set_on_reload(std::function<void()> cb) { m_on_reload = std::move(cb); }
	private:
		std::shared_ptr<scn::hot_reload_manager> m_hot_reload_manager;
		scn::ecs_assembler& m_assembler;
		ecs::system_factory& m_system_factory;
		res::resource_system& m_resource;
		scn::level active_lvl;
		// Worlds that came from the level desc; any other world in active_lvl was created at runtime.
		std::vector<std::string> m_desc_world_names;
		std::function<void()> m_on_reload;
	};
}