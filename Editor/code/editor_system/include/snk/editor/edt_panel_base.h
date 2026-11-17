#pragma once
#include "edt_api.h"
#include <cstdint>
#include <string>
#include <string_view>
// ImGuiWindowFlags is an imgui integer typedef (int). Re-declared here to keep <imgui.h>
// out of this header, which leaks into every panel (EPIC-49 US-49-4). Same-type redefinition
// is legal alongside <imgui.h>; the .cpp includes the full header for the ImGui API.
using ImGuiWindowFlags = int;

namespace gui { struct theme_semantic_colors; }

namespace edt
{
	// +1 on any change to the virtual functions below: the editor layout key mixes it in, tools/ci/vtable_pins.json pins them (#969).
	inline constexpr std::uint32_t panel_base_abi_revision = 1;

	class SNK_EDITOR_API panel_base
	{
	public:
		panel_base(std::string_view id, std::string_view title);
		virtual ~panel_base() = default;

		void render();

		bool is_open() const { return m_open; }
		void set_open(bool open) { m_open = open; }
		const std::string& get_id() const { return m_id; }
		const std::string& get_title() const { return m_title; }
		// The editor's semantic colours, followed while the panel lives (#1136): panel_registry sets them on add.
		// Null: the defaults. Must outlive the panel or be reset.
		void set_colors(const gui::theme_semantic_colors* colors);

	protected:
		const gui::theme_semantic_colors& colors() const;

		virtual void on_before_begin() {}
		virtual void on_render() = 0;
		// Called instead of on_render() when the panel is closed, collapsed or in a hidden tab.
		virtual void on_hidden() {}

		std::string m_id;
		std::string m_title;
		bool m_open = true;
		ImGuiWindowFlags m_flags = 0;
		const gui::theme_semantic_colors* m_colors = nullptr;
	};
}
