#include <snk/editor/edt_editor_extension.h>
#include <snk/common/engine_assert.h>
#include <string>

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
	m_scope.keep(m_editor.panels().add(std::move(desc)));
}

void edt::editor_module_context::add_menu(std::string name)
{
	m_scope.keep(m_editor.actions().add_menu(std::move(name)));
}

void edt::editor_module_context::add_action(action_desc desc)
{
	m_scope.keep(m_editor.actions().add(std::move(desc)));
}

void edt::editor_module_context::add_action(cmd::command_desc command, std::optional<action_ui> ui)
{
	m_scope.keep(m_editor.actions().add(std::move(command), std::move(ui)));
}

void edt::editor_module_context::add_inspector(std::string type, inspector_renderer renderer)
{
	m_scope.keep(m_editor.inspectors().add(std::move(type), std::move(renderer)));
}

void edt::editor_module_context::add_asset_kind(asset_kind_desc desc)
{
	m_scope.keep(m_editor.asset_kinds().add(std::move(desc)));
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
	m_scope->stop_tasks();
	shutdown_editor();
	m_scope->clear();
	m_editor->untrack_module_scope(*m_scope);
	m_editor = nullptr;
	m_context.reset();
	m_scope.reset();
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
