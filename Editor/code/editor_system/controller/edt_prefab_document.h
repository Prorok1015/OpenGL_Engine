#pragma once
#include "edt_editor_camera.h"
#include <snk/editor/edt_document.h>
#include <snk/resource/res_tag.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <functional>
#include <entt/entt.hpp>

namespace edt
{
	struct editor_services;

	// Marker component — distinguishes prefab preview camera from editor camera
	struct prefab_camera_tag {};

	// A prefab opened for preview: read-only, and the owner of the set "preview:<tag>" while it is open.
	class prefab_document final : public document
	{
	public:
		explicit prefab_document(editor_services& services);

		void open(const res::tag& prefab_tag);
		void close();

		bool is_open() const { return m_open; }
		bool is_dirty() const override { return m_dirty; }
		std::string_view kind() const override { return "prefab"; }
		const std::string& title() const override { return m_prefab_name; }
		const res::tag& source() const override { return m_source_tag; }
		// Nothing edits the preview yet (EPIC-25).
		std::uint64_t generation() const override { return 0; }

		entt::registry* get_registry();
		const std::string& prefab_name() const { return m_prefab_name; }
		const res::tag& source_tag() const { return m_source_tag; }
		// The preview is the only context of its own set; the panel binds its input to this target.
		const std::string& set_name() const { return m_set_name; }
		static constexpr std::string_view context_name() { return CONTEXT_NAME; }

		static constexpr std::string_view RT_COLOR = "memory://__prefab_camera_rt";
		static constexpr std::string_view RT_DEPTH = "memory://__prefab_depth_rt";
		static constexpr std::string_view RT_TP_ACCUM = "memory://__prefab_tp_accum_rt";
		static constexpr std::string_view RT_TP_REVEAL = "memory://__prefab_tp_reveal_rt";

	private:
		void inject_camera(entt::registry& reg);

		editor_services& m_services;

		res::tag m_source_tag;
		std::string m_prefab_name;
		std::string m_set_name;
		bool m_open = false;
		bool m_dirty = false;
		editor_camera_state m_camera_state;

		static constexpr std::string_view CONTEXT_NAME = "__prefab_preview";
	};
}
