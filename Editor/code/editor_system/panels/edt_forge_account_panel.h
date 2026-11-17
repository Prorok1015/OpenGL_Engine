#pragma once
#include <snk/editor/edt_panel_base.h>
#include "edt_forge_account_model.h"
#include <array>
#include <string>
#include <vector>

namespace edt
{
	// ImGui keeps its own copies of what a text field held: the active field's text and its revert copy, the copy made when
	// the field is deactivated, and the undo stack (which records edits even with NoUndoRedo). When `field_id` (an ImGuiID,
	// the ID the token field was drawn with) is the one ImGui holds, drops the active item and overwrites all of those
	// (volatile writes, the allocations kept) so no token is left in the ImGui context. Needs the current ImGui context;
	// does nothing without one.
	void wipe_imgui_token_state(unsigned int field_id);

	// Git Account (#458 PR-4): the token of each code host the forge commands may talk to. Floating, closed by default; opened
	// by the Git panel's "Account..." button and by the action forge.account.open (not for agents). It only draws
	// forge_account_model: a row shows "set (OS keychain)", "set (this session only)" or "not set", never the token; the token is
	// typed into a masked field whose buffer, and ImGui's own copies of it (wipe_imgui_token_state), are wiped when Save or
	// Clear is pressed and whenever the window is closed or hidden.
	class forge_account_panel final : public panel_base
	{
	public:
		static constexpr const char* k_id = "forge_account";
		static constexpr const char* k_title = "Git Account";

		forge_account_panel(editor_events& events, std::vector<std::string> hosts, forge_account_model::ports ports);
		~forge_account_panel() override;
		forge_account_panel(const forge_account_panel&) = delete;
		forge_account_panel& operator=(const forge_account_panel&) = delete;

		forge_account_model& model() { return m_model; }
		// Brings the window to the front on its next frame.
		void focus() { m_focus = true; }

	protected:
		void on_before_begin() override;
		void on_render() override;
		void on_hidden() override;

	private:
		using buffer = std::array<char, forge_account_model::k_max_token + 1>;

		void wipe_field(std::size_t index);
		void wipe_all();

		forge_account_model m_model;
		// One masked field per host row.
		std::vector<buffer> m_buffers;
		// The ImGui ID each field was last drawn with (0: not drawn yet).
		std::vector<unsigned int> m_field_ids;
		bool m_focus = false;
	};
}
