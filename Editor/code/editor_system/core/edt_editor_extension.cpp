#include <snk/editor/edt_editor_extension.h>
#include <snk/common/engine_assert.h>
#include <functional>
#include <string>
#include <utility>

namespace
{
	// The module's callback, counted while it runs: editor_context::module_unload_refusal refuses an unload meanwhile.
	template<class R, class... Args>
	std::function<R(Args...)> counted(edt::callback_gate& gate, std::function<R(Args...)> fn)
	{
		if (!fn) return fn;
		return [&gate, fn = std::move(fn)](Args... args) -> R {
			const edt::callback_gate::guard running(gate);
			return fn(std::forward<Args>(args)...);
		};
	}

	void count_action_callbacks(edt::callback_gate& gate, edt::action_desc& desc)
	{
		desc.disabled_reason = counted(gate, std::move(desc.disabled_reason));
		desc.checked = counted(gate, std::move(desc.checked));
		desc.dynamic_label = counted(gate, std::move(desc.dynamic_label));
		desc.execute = counted(gate, std::move(desc.execute));
		desc.execute_call = counted(gate, std::move(desc.execute_call));
		desc.activate = counted(gate, std::move(desc.activate));
		desc.enabled = counted(gate, std::move(desc.enabled));
		desc.describe = counted(gate, std::move(desc.describe));
	}
}

edt::editor_module_context::editor_module_context(editor_context& editor, registration_scope& scope)
	: m_editor(editor)
	, m_scope(scope)
{
}

void edt::editor_module_context::set_problems(std::string source, std::vector<problem> items)
{
	m_editor.problems().set(source, std::move(items));
	problem_list& list = m_editor.problems();
	m_scope.keep_cleanup([&list, source = std::move(source)] { list.clear(source); });
}

void edt::editor_module_context::add_panel(panel_desc desc)
{
	m_scope.keep(m_editor.panels().add(std::move(desc), m_scope.callbacks()));
}

void edt::editor_module_context::add_menu(std::string name)
{
	m_scope.keep(m_editor.actions().add_menu(std::move(name)));
}

void edt::editor_module_context::add_action(action_desc desc)
{
	count_action_callbacks(m_scope.callbacks(), desc);
	m_scope.keep(m_editor.actions().add(std::move(desc)));
}

void edt::editor_module_context::add_action(cmd::command_desc command, std::optional<action_ui> ui)
{
	callback_gate& gate = m_scope.callbacks();
	command.category_of = counted(gate, std::move(command.category_of));
	command.describe = counted(gate, std::move(command.describe));
	command.invoke = counted(gate, std::move(command.invoke));
	command.enabled = counted(gate, std::move(command.enabled));
	command.disabled_reason = counted(gate, std::move(command.disabled_reason));
	if (ui) {
		ui->disabled_reason = counted(gate, std::move(ui->disabled_reason));
		ui->checked = counted(gate, std::move(ui->checked));
		ui->dynamic_label = counted(gate, std::move(ui->dynamic_label));
		ui->enabled = counted(gate, std::move(ui->enabled));
		ui->activate = counted(gate, std::move(ui->activate));
	}
	m_scope.keep(m_editor.actions().add(std::move(command), std::move(ui)));
}

void edt::editor_module_context::add_inspector(std::string type, inspector_renderer renderer)
{
	m_scope.keep(m_editor.inspectors().add(std::move(type), counted(m_scope.callbacks(), std::move(renderer))));
}

void edt::editor_module_context::add_asset_kind(asset_kind_desc desc)
{
	desc.open = counted(m_scope.callbacks(), std::move(desc.open));
	desc.drop_into_scene = counted(m_scope.callbacks(), std::move(desc.drop_into_scene));
	m_scope.keep(m_editor.asset_kinds().add(std::move(desc)));
}

void edt::editor_module_context::add_asset_preview(asset_preview_desc desc)
{
	desc.make_preview = counted(m_scope.callbacks(), std::move(desc.make_preview));
	m_scope.keep(m_editor.asset_kinds().add_preview(std::move(desc)));
}

edt::editor_extension::editor_extension() = default;

edt::editor_extension::~editor_extension()
{
	ASSERT_MSG(!m_scope, "editor_extension destroyed without shutdown_services: the module's own members are gone and its queued callbacks may still run");
}

void edt::editor_extension::register_services(ds::app_data_storage& data)
{
	ASSERT_MSG(!m_scope, "editor_extension: registered twice without a shutdown");
	auto& editor = data.require<editor_context>();
	m_scope = std::make_unique<registration_scope>(editor.tasks());
	m_context = std::make_unique<editor_module_context>(editor, *m_scope);
	m_editor = &editor;
	editor.track_module_scope(*m_scope);
	register_editor(*m_context);
}

void edt::editor_extension::initialize_services(ds::app_data_storage&)
{
}

void edt::editor_extension::shutdown_services(ds::app_data_storage&)
{
	if (!m_scope) return;
	// Whatever shutdown_editor does (it may throw), the scope is cleared and untracked before this returns: the host
	// unloads the module next, and the editor must not keep a pointer into it.
	struct finish_unload
	{
		editor_extension& self;
		~finish_unload()
		{
			self.m_scope->clear();
			self.m_editor->untrack_module_scope(*self.m_scope);
			self.m_editor = nullptr;
			self.m_context.reset();
			self.m_scope.reset();
		}
	} finish{ *this };
	m_scope->stop_tasks();
	// From here the host's refusal check must not look at this module's callbacks any more.
	m_editor->untrack_module_scope(*m_scope);
	shutdown_editor();
}

std::string edt::check_editor_api_version(std::uint32_t (*module_api_version)())
{
	if (!module_api_version) {
		return "not a game_editor module (snk_editor_api_version is missing)";
	}
	const std::uint32_t version = module_api_version();
	const std::uint32_t host = SNK_EDITOR_API_VERSION;
	if (app::modules::api_major(version) != app::modules::api_major(host)) {
		return "editor api major version " + std::to_string(app::modules::api_major(version))
			+ " does not match host major " + std::to_string(app::modules::api_major(host));
	}
	if (app::modules::api_minor(version) > app::modules::api_minor(host)) {
		return "module needs editor api minor " + std::to_string(app::modules::api_minor(version))
			+ ", host provides " + std::to_string(app::modules::api_minor(host));
	}
	return {};
}
