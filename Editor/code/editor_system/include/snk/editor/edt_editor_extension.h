#pragma once
#include "edt_api.h"
#include "edt_editor_context.h"
#include "edt_registration_scope.h"
#include <snk/application/app_module_entry.h>
#include <snk/application/app_module_interface.h>
#include <snk/game/gs_module_layout.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// The version of the editor API a game_editor module is built against (major << 16 | minor), exported by the module as
// snk_editor_api_version (SNK_DECLARE_EDITOR_MODULE) and checked by the host before it creates the module: the major
// must match, the module's minor must not exceed the host's. Raise the major for a change that breaks a module's
// build or binary, the minor for an addition.
// 1.1: editor_module_context::add_asset_preview (the preview document of an asset suffix, asset_preview_desc).
// 1.2: the module's callbacks are counted while they run (registration_scope::callbacks, #663): a reload asked from inside
// one is refused. The inline editor_module_context::subscribe reads the new member of the scope, so a 1.2 module needs a 1.2 host.
// 2.0 (#663): nothing the API offers registers past the module's scope. editor_module_context::services() is gone,
// events() is const (a module subscribes through subscribe() only), and the asset kinds and previews of a module are
// module_asset_kind_desc / module_asset_preview_desc: their callbacks get the editor_module_context, not the editor_context
// or the editor_services. A 1.x module is refused by the host (check_editor_api_version): rebuild the project against the new SDK.
#define SNK_EDITOR_API_VERSION 0x0002'0000u

namespace edt
{
	class editor_module_context;

	// What the editor does with one kind of asset a module registers (editor_module_context::add_asset_kind). The callbacks get
	// the module's own context, not the editor's: what the module registers from them goes into its scope.
	struct module_asset_kind_desc
	{
		std::string id{};     // "prefab"
		// Matches the assets whose res-relative path ends with this text (case-sensitive); empty: every asset.
		std::string suffix{};
		std::string icon{};
		// Double-click in the asset browser. Null: no open action.
		std::function<void(editor_module_context&, const res::tag&)> open{};
		// Drop onto the viewport. Null: cannot be dropped into the scene.
		std::function<void(editor_module_context&, const res::tag&)> drop_into_scene{};
	};

	// The document that previews the assets of a suffix (editor_module_context::add_asset_preview), as asset_preview_desc but
	// its factory gets the module's context.
	struct module_asset_preview_desc
	{
		std::string id{};     // unique among the previews
		std::string suffix{};
		// Runs on the editor's main thread when the preview first opens. Required.
		std::function<std::unique_ptr<previewable_document>(editor_module_context&)> make_preview{};
	};

	// What a game_editor module (#588) gets: the editor's registries, jobs and problems only through functions that keep
	// what they register in the module's scope, and the state it works with (documents, selection) and reads (events). Not
	// handed out (editor API 2.0, #663): the engine's services (editor_services: its registries take a desc or a spawner
	// past the scope; a module reaches them through its game layer), dialog_service (its callbacks would outlive the module;
	// a wrapper comes with the first module that needs a dialog) and the editor_context itself. Unloading the module undoes
	// all of it, so the editor is as it was before the load (and a reload registers again without clashing with the old
	// entries). The wording of the guarantee: no function the API offers registers past the scope; a module that includes
	// the private headers and reaches the editor by other means (the host's storage) is outside it, the language has no way to stop it.
	class SNK_EDITOR_API editor_module_context
	{
	public:
		editor_module_context(editor_context& editor, registration_scope& scope);
		editor_module_context(const editor_module_context&) = delete;
		editor_module_context& operator=(const editor_module_context&) = delete;

		void add_panel(panel_desc desc);
		void add_menu(std::string name);
		void add_action(action_desc desc);
		void add_action(cmd::command_desc command, std::optional<action_ui> ui);
		void add_inspector(std::string type, inspector_renderer renderer);
		void add_asset_kind(module_asset_kind_desc desc);
		// The module's callbacks (panels, actions, inspector renderers, asset kinds, event handlers) are counted while they run:
		// editor_context::module_unload_refusal refuses an unload meanwhile, so a callback cannot reload its own module.
		// The document that previews the assets of a suffix (editor API 1.1, #589). Its make_preview runs on the editor's main
		// thread when the preview opens; the document it makes must run its tasks on tasks() (not on jobs(), which the module
		// does not own), and its close() and destructor run at unload after shutdown_editor(): they must not rely on
		// anything the module's shutdown_editor() released.
		void add_asset_preview(module_asset_preview_desc desc);

		// The only way to subscribe (events() is const): the connection goes into the scope. The signal comes from events().
		template<class... Args, class F>
		void subscribe(const signal<Args...>& event, F&& handler)
		{
			callback_gate& gate = m_scope.callbacks();
			// const_cast: events() hands the module const signals so that it cannot subscribe by hand; the scope's connection
			// is the one made here. The signal is the host's non-const object.
			m_scope.keep(const_cast<signal<Args...>&>(event).subscribe(std::function<void(Args...)>(
				[&gate, fn = std::forward<F>(handler)](Args... args) mutable {
					const callback_gate::guard running(gate);
					fn(args...);
				})));
		}

		// The owner of the jobs and task continuations the module starts: they are stopped when it unloads, and none
		// runs after shutdown_editor.
		const ds::lifetime& tasks() const { return m_scope.tasks(); }
		ds::task_system& task_system() const { return m_editor.tasks(); }

		// Jobs of the module: bound to tasks(), so unloading stops them. The job_service is read-only for a module
		// (job.list, job_info), starting goes through here.
		template<class F>
		job_service::job_id start_job(ds::lane where, ds::task_desc desc, F&& work,
			std::function<void(ds::task_result<std::invoke_result_t<F&, ds::task_context&>>&)> on_done)
		{
			return m_editor.jobs().start(where, std::move(desc), m_scope.tasks(), std::forward<F>(work), std::move(on_done));
		}
		template<class F>
		job_service::job_id start_job_with_result(ds::lane where, ds::task_desc desc, F&& work,
			std::function<void(ds::task_result<boost::json::value>&)> on_done = {})
		{
			return m_editor.jobs().start_with_result(where, std::move(desc), m_scope.tasks(), std::forward<F>(work), std::move(on_done));
		}

		// The problems of `source`, cleared when the module unloads.
		void set_problems(std::string source, std::vector<problem> items);

		res::resource_system& resources() const { return m_editor.resources(); }
		rnd::render_system& render() const { return m_editor.render(); }
		gui::gui_system& gui() const { return m_editor.gui(); }
		document_manager& documents() { return m_editor.documents(); }
		const editor_events& events() const { return m_editor.events(); }
		selection_service& selection() { return m_editor.selection(); }
		const job_service& jobs() const { return m_editor.jobs(); }
		const problem_list& problems() const { return m_editor.problems(); }

	private:
		editor_context& m_editor;
		registration_scope& m_scope;
	};

	// The base of a game_editor module. The host puts the editor_context in the storage it hands over; the module only
	// writes register_editor. Declare it with SNK_DECLARE_EDITOR_MODULE. The host always calls shutdown_services before
	// it destroys the module, also when shutdown_editor throws: destroying the module first would run its queued
	// callbacks over members that are already gone. The module's DLL must not touch edt:: from a static initializer.
	// The callbacks registered through the context are counted while they run (editor_context::module_unload_refusal).
	class SNK_EDITOR_API editor_extension : public app::modules::module_interface
	{
	public:
		editor_extension();
		~editor_extension() override;
		// Explicit: dllexport on MSVC would otherwise generate copies of the move-only members.
		editor_extension(const editor_extension&) = delete;
		editor_extension& operator=(const editor_extension&) = delete;

		virtual void register_editor(editor_module_context& context) = 0;
		// Called after the module's tasks are stopped (none runs or delivers from here on) and before what it registered
		// is removed; the module's own state goes here. A task of the module must honour its stop token.
		virtual void shutdown_editor() {}

		// The editor module: it registers into the editor, so it loads after it and goes before it.
		std::vector<std::string_view> depends_on() const override { return { "editor" }; }

		void register_services(ds::app_data_storage& data) final;
		void initialize_services(ds::app_data_storage& data) final;
		void shutdown_services(ds::app_data_storage& data) final;

	private:
		editor_context* m_editor = nullptr;
		std::unique_ptr<registration_scope> m_scope;
		std::unique_ptr<editor_module_context> m_context;
	};

	// Empty when the host can use a module that reports `module_api_version` (the function a module exports as
	// snk_editor_api_version; null when it has none): the major equals SNK_EDITOR_API_VERSION's and the minor is not
	// above it. Otherwise why not. The host checks this before it creates the module.
	[[nodiscard]] SNK_EDITOR_API std::string check_editor_api_version(std::uint32_t (*module_api_version)());
}

// The four module functions (SNK_DECLARE_MODULE) plus snk_editor_api_version.
#define SNK_DECLARE_EDITOR_MODULE_PREFIXED(prefix, Type) \
	SNK_DECLARE_MODULE_PREFIXED(prefix, Type) \
	extern "C" SNK_MODULE_EXPORT std::uint32_t prefix##editor_api_version() \
	{ \
		return SNK_EDITOR_API_VERSION; \
	}

#define SNK_DECLARE_EDITOR_MODULE(Type) SNK_DECLARE_EDITOR_MODULE_PREFIXED(snk_, Type)

// The module_entry of an editor module declared with SNK_DECLARE_EDITOR_MODULE_PREFIXED(prefix, ...): the five functions.
#define SNK_EDITOR_MODULE_ENTRY(prefix) \
	::app::modules::module_entry{ &prefix##module_api_version, &prefix##module_build_info, &prefix##create_module, \
		&prefix##destroy_module, &prefix##editor_api_version, &prefix##module_layout_key }
