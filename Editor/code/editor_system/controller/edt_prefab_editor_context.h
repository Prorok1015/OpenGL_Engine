#pragma once
#include "edt_editor_camera.h"
#include "res_tag.h"
#include <cstdint>
#include <string>
#include <functional>
#include <entt/entt.hpp>

namespace scn { class ecs_assembler; class level_manager; }
namespace ecs { class system_factory; }
namespace res { class resource_system; }
namespace desc { class desc_system; }

namespace edt
{
	struct shared_state;

	// Marker component — distinguishes prefab preview camera from editor camera
	struct prefab_camera_tag {};

	class prefab_editor_context
	{
	public:
		prefab_editor_context(shared_state& state, res::resource_system& res, desc::desc_system& desc);

		void open(const res::tag& prefab_tag);
		void close();

		bool is_open() const { return m_open; }
		bool is_dirty() const { return m_dirty; }

		entt::registry* get_registry();
		const std::string& prefab_name() const { return m_prefab_name; }
		const res::tag& source_tag() const { return m_source_tag; }

		static constexpr std::string_view RT_COLOR = "memory://__prefab_camera_rt";
		static constexpr std::string_view RT_DEPTH = "memory://__prefab_depth_rt";
		static constexpr std::string_view RT_TP_ACCUM = "memory://__prefab_tp_accum_rt";
		static constexpr std::string_view RT_TP_REVEAL = "memory://__prefab_tp_reveal_rt";

	private:
		void inject_camera(entt::registry& reg);

		shared_state& m_state;
		res::resource_system& m_res;
		desc::desc_system& m_desc;

		res::tag m_source_tag;
		std::string m_prefab_name;
		uint32_t m_world_id = UINT32_MAX;
		bool m_open = false;
		bool m_dirty = false;
		editor_camera_state m_camera_state;

		static constexpr std::string_view WORLD_NAME = "__prefab_preview";
	};
}
