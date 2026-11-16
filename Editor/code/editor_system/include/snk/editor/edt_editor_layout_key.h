#pragma once
#include <cstdint>
#include <type_traits>
#include <imgui.h>
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_asset_kind_registry.h>
#include <snk/editor/edt_callback_gate.h>
#include <snk/editor/edt_dialog_service.h>
#include <snk/editor/edt_document.h>
#include <snk/editor/edt_document_manager.h>
#include <snk/editor/edt_editor_context.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_editor_extension.h>
#include <snk/editor/edt_editor_layout.h>
#include <snk/editor/edt_inspector_registry.h>
#include <snk/editor/edt_job_service.h>
#include <snk/editor/edt_panel_base.h>
#include <snk/editor/edt_panel_registry.h>
#include <snk/editor/edt_problem_list.h>
#include <snk/editor/edt_registration.h>
#include <snk/editor/edt_registration_scope.h>
#include <snk/editor/edt_selection_service.h>
#include <snk/game/gs_module_layout_key.h>

// The type list of the editor layout key (edt_editor_layout.h says what belongs in it). Its includes are heavy (ImGui,
// every editor header): edt_editor_extension.h, where SNK_DECLARE_EDITOR_MODULE is, includes it at its end, and the
// editor's own check (edt_editor_extension.cpp) evaluates it as the editor was built.
namespace edt
{
	namespace detail
	{
		// Every editor interface whose vtable module code fills with an override or calls inline. A change of its bases or
		// virtual functions raises its revision: tools/ci/check_vtable_pins.py ("key": "editor") fails the change that does not.
		using editor_interface_abi_revisions = gs::detail::layout_values<editor_extension_abi_revision,
			panel_base_abi_revision, document_abi_revision, previewable_document_abi_revision>;

		// A module draws its panels with the ImGui of the headers it was built with, against the editor's context.
		using imgui_version = gs::detail::layout_values<IMGUI_VERSION_NUM>;

		// The one list of the key, in order.
		using editor_layout_types = scn::type_list<
			// What a module builds and hands to editor_module_context (by value or moved in).
			panel_desc, action_desc, action_ui, shortcut, module_asset_kind_desc, module_asset_preview_desc, problem,
			// What it gets back or copies out: the selection, the problems, the jobs, a document's history and save.
			selection, sourced_problem, job_progress, job_service::job_info, save_result, history_entry,
			// Handles it keeps, and the signal machinery subscribe() instantiates in module code. The members of signal,
			// slot and signal_state do not depend on the arguments: <int> stands for every one.
			registration, connection, signal<int>, detail::slot<int>, detail::signal_state<int>, callback_gate,
			// Payloads of editor_events a module's handler reads inline.
			game_module_build_result, agent_action_info,
			// The bases a module derives from: its extension, its panels, its preview documents.
			editor_extension, panel_base, document, previewable_document,
			// Host types whose fields module inline code reads (editor_module_context's templates and accessors, the
			// registries' size()/generation()/running_count()/has_selection()); editor_context holds the rest by value.
			editor_module_context, registration_scope, editor_context, editor_events, document_manager, selection_service,
			dialog_service, job_service, problem_list, panel_registry, action_registry, inspector_registry,
			asset_kind_registry,
			// ImGui's shared state a module's drawing code reads and writes inline.
			ImGuiStyle, ImGuiIO,
			editor_interface_abi_revisions, imgui_version>;

		// The interface revisions and ImGui must stay in the key: dropping one would pass every other test.
		static_assert(gs::detail::list_contains<editor_interface_abi_revisions, editor_layout_types>::value
			&& gs::detail::list_contains<imgui_version, editor_layout_types>::value
			&& gs::detail::list_contains<ImGuiStyle, editor_layout_types>::value
			&& gs::detail::list_contains<ImGuiIO, editor_layout_types>::value,
			"the editor layout key must mix in the editor interface ABI revisions, the ImGui version and its types");
	}

	// Evaluated in whichever binary includes this header: SNK_DECLARE_EDITOR_MODULE exports it as snk_editor_layout_key.
	constexpr std::uint64_t editor_layout_key() noexcept
	{
		return detail::editor_layout_of<detail::editor_layout_types>();
	}
}
