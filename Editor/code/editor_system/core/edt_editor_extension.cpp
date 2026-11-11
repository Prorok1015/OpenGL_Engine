#include <snk/editor/edt_editor_extension.h>
#include <snk/common/engine_assert.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
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

	// The document a module's preview factory made, seen by the editor through this proxy: every call into the module's code
	// (open, close, save, undo, the reads, the event binding) is counted. Not the destructor: it runs at unload, after the
	// module's tasks stopped.
	class counted_preview final : public edt::previewable_document
	{
	public:
		counted_preview(edt::callback_gate& gate, std::unique_ptr<edt::previewable_document> inner)
			: m_gate(gate), m_inner(std::move(inner)) {}

		std::string_view kind() const override { return run([&] { return m_inner->kind(); }); }
		const std::string& title() const override { return run([&]() -> const std::string& { return m_inner->title(); }); }
		const res::tag& source() const override { return run([&]() -> const res::tag& { return m_inner->source(); }); }
		bool is_dirty() const override { return run([&] { return m_inner->is_dirty(); }); }
		std::uint64_t generation() const override { return run([&] { return m_inner->generation(); }); }
		bool can_save() const override { return run([&] { return m_inner->can_save(); }); }
		edt::save_result save() override { return run([&] { return m_inner->save(); }); }
		edt::save_result save_as(const res::tag& target) override { return run([&] { return m_inner->save_as(target); }); }
		bool undo() override { return run([&] { return m_inner->undo(); }); }
		bool redo() override { return run([&] { return m_inner->redo(); }); }
		std::optional<edt::history_entry> next_undo_entry() const override { return run([&] { return m_inner->next_undo_entry(); }); }
		std::optional<edt::history_entry> next_redo_entry() const override { return run([&] { return m_inner->next_redo_entry(); }); }
		bool has_agent_changes() const override { return run([&] { return m_inner->has_agent_changes(); }); }
		bool edits_locked() const override { return run([&] { return m_inner->edits_locked(); }); }
		void accept_agent_changes() override { run([&] { m_inner->accept_agent_changes(); }); }
		void bind_events(edt::editor_events* events) override { run([&] { m_inner->bind_events(events); }); }
		void open(const res::tag& source) override { run([&] { m_inner->open(source); }); }
		void close() override { run([&] { m_inner->close(); }); }
		bool is_open() const override { return run([&] { return m_inner->is_open(); }); }

	private:
		// The manager gives the id to this proxy (assign_id is not virtual); the document announces its changes with it.
		template<class F>
		auto run(F&& body) const -> std::invoke_result_t<F&&>
		{
			const edt::callback_gate::guard running(m_gate);
			if (m_inner->id() != id()) m_inner->assign_id(id());
			return std::forward<F>(body)();
		}

		edt::callback_gate& m_gate;
		std::unique_ptr<edt::previewable_document> m_inner;
	};

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

void edt::editor_module_context::add_asset_kind(module_asset_kind_desc desc)
{
	asset_kind_desc kind;
	kind.id = std::move(desc.id);
	kind.suffix = std::move(desc.suffix);
	kind.icon = std::move(desc.icon);
	const auto bind = [this](std::function<void(editor_module_context&, const res::tag&)> action) {
		std::function<void(editor_context&, const res::tag&)> bound;
		if (action) {
			bound = [this, action = std::move(action)](editor_context&, const res::tag& tag) { action(*this, tag); };
		}
		return counted(m_scope.callbacks(), std::move(bound));
	};
	kind.open = bind(std::move(desc.open));
	kind.drop_into_scene = bind(std::move(desc.drop_into_scene));
	m_scope.keep(m_editor.asset_kinds().add(std::move(kind)));
}

void edt::editor_module_context::add_asset_preview(module_asset_preview_desc desc)
{
	asset_preview_desc preview;
	preview.id = std::move(desc.id);
	preview.suffix = std::move(desc.suffix);
	if (desc.make_preview) {
		preview.make_preview = [this, &gate = m_scope.callbacks(), make = std::move(desc.make_preview)](edt::editor_services&)
			-> std::unique_ptr<edt::previewable_document> {
			const edt::callback_gate::guard running(gate);
			std::unique_ptr<edt::previewable_document> made = make(*this);
			if (!made) return nullptr;
			return std::make_unique<counted_preview>(gate, std::move(made));
		};
	}
	m_scope.keep(m_editor.asset_kinds().add_preview(std::move(preview)));
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
	const std::uint32_t module_major = app::modules::api_major(version);
	const std::uint32_t host_major = app::modules::api_major(host);
	if (module_major < host_major) {
		return "built against editor api " + std::to_string(module_major) + "." + std::to_string(app::modules::api_minor(version))
			+ ", this editor serves " + std::to_string(host_major) + ".x: rebuild the project against the current SDK";
	}
	if (module_major > host_major) {
		return "built against editor api " + std::to_string(module_major) + "." + std::to_string(app::modules::api_minor(version))
			+ ", this editor serves " + std::to_string(host_major) + ".x: update the editor, or rebuild the project against this SDK";
	}
	if (app::modules::api_minor(version) > app::modules::api_minor(host)) {
		return "module needs editor api minor " + std::to_string(app::modules::api_minor(version))
			+ ", host provides " + std::to_string(app::modules::api_minor(host));
	}
	return {};
}
