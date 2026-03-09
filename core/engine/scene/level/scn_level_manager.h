#pragma once
#include "scn_level.h"
#include "res_system.h"
#include "scn_hot_reload_manager.h"
#include <memory>

namespace scn
{
	class level_manager
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
		~level_manager() = default;
		level_manager(const level_manager&) = delete;
		level_manager(level_manager&&) = default;
		level_manager& operator=(const level_manager&) = delete;
		level_manager& operator=(level_manager&&) = default;

		void update(std::chrono::duration<float> dt);

		scn::level& get_level() { return active_lvl; }

		bool load(const res::tag& level_tag);
		void load_world(const res::tag& world_tag);
	private:
		std::shared_ptr<scn::hot_reload_manager> m_hot_reload_manager;
		scn::ecs_assembler& m_assembler;
		ecs::system_factory& m_system_factory;
		res::resource_system& m_resource;
		scn::level active_lvl;
	};
}