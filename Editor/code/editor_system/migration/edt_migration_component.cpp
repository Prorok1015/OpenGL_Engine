#include "edt_migration_component.h"
#include "edt_agent_policy.h"
#include "edt_asset_open_component.h"
#include "edt_data_protocol.h"
#include "edt_editor_layer.h"
#include "edt_level_controller.h"
#include "edt_level_load_component.h"
#include "edt_migration_commands.h"
#include "edt_migration_panel.h"
#include "edt_migration_window_model.h"
#include "edt_prefab_editor_component.h"
#include "edt_scene_document.h"
#include "edt_ui_metrics.h"
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>
#include <snk/config/cfg_variables.h>
#include <snk/desc/desc_system.h>
#include <snk/editor/edt_dialog_service.h>
#include <snk/editor/edt_document_manager.h>
#include <snk/editor/edt_editor_context.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/editor/edt_panel_registry.h>
#include <snk/project/prj_json_text.h>
#include <snk/project/prj_project.h>
#include <snk/project_migration/pmg_apply.h>
#include <snk/project_migration/pmg_asset_steps.h>
#include <snk/project_migration/pmg_assets.h>
#include <snk/resource/res_system.h>
#include <boost/json/value.hpp>
#include <imgui.h>
#include <format>
#include <fstream>
#include <iterator>
#include <utility>

namespace
{
	// --open-migration (#761): once the project is open and the start level settled, the Project Migration window opens.
	CFG_VAR_DEF_BOOL(cfg_open_migration, "editor/open_migration", false);
}

edt::migration_component::migration_component(ports from)
	: m_ports(std::move(from))
{
}

edt::migration_component::~migration_component() = default;

void edt::migration_component::install(std::vector<registration>& out, const std::function<void(const std::shared_ptr<panel_base>&)>& add_panel)
{
	// EPIC-58 S7 (#759): project.migration_plan and project.migrate (in place, on the main thread). Refused while Play, a load,
	// a build or a module reload, an import, the export dialog or a git write runs (busy), and with an uncommitted edit or
	// any dirty document (needs_input). The migration closes every document but the scene, lets go of the level, writes,
	// then attaches the level and loads it again from disk.
	{
		migration_deps migration{ m_ports.context.jobs() };
		migration.geometry_checks = m_geometry_checks;
		migration.log_filter = m_ports.log_filter;
		const prj::project_context* project = m_ports.project;
		migration.project_root = [project]() -> std::optional<std::filesystem::path> {
			if (project == nullptr) return std::nullopt;
			return project->root;
		};
		migration.project_name = [project] { return project != nullptr ? project->data.name : std::string{}; };
		migration.content_roots = [project] { return project != nullptr ? project->content_roots : std::vector<std::filesystem::path>{}; };
		migration.descs = [this]() -> desc::desc_system& { return *m_ports.services.descs(); };
		migration.resources = [this]() -> res::resource_system& { return *m_ports.services.resources(); };
		migration.busy_reason = m_ports.busy_reason;
		migration.index_wait = m_ports.index_wait;
		migration.unsaved_reason = m_ports.unsaved_reason;
		migration.close_documents_and_detach_level = [this] {
			m_ports.prefab.close();
			// Every document but the scene: the prefab editor through its panel, data documents through their host, any other
			// (a module's, an extension's) through document_manager::close, which announces it to whoever shows it.
			document_manager& documents = m_ports.context.documents();
			std::vector<document_id> open_documents;
			documents.for_each([&documents, &open_documents](document& open) {
				if (&open != &documents.scene()) open_documents.push_back(open.id());
			});
			for (const document_id id : open_documents) {
				if (m_ports.asset_open.data_documents().owns(id)) m_ports.asset_open.data_documents().close(id);
				else if (documents.find(id)) documents.close(id);
			}
			const res::tag level = m_ports.state.level_tag;
			// As follow_game_layer_change before a layer change: nothing may hold the level while its files are written.
			m_ports.asset_open.drop_pending();
			if (m_ports.level_load.in_progress()) m_ports.level_load.abandon();
			// A level a stopped migration left detached stays as it was kept (a second detach would keep the empty worlds).
			if (!m_ports.level_ctrl.is_detached()) m_ports.level_ctrl.detach_from_game_layer();
			return level;
		};
		migration.pause_watch = [this] { m_ports.level_ctrl.unwatch_level(); };
		// Through the base system: it reaches the game layer's too.
		migration.signal_changed = [this](const res::tag& tag) { m_ports.resources.signal_changed(tag); };
		migration.rebuild_assets = m_ports.rebuild_assets;
		migration.reopen_level = [this](const res::tag& level, std::function<void(bool)> loaded) -> std::optional<std::string> {
			// The files are still half migrated (a rollback refused): the level stays detached, as the stopped migration left it.
			if (std::optional<std::string> stopped = m_stopped.reason()) return stopped;
			m_ports.level_ctrl.attach_to_game_layer();
			if (!level.is_valid()) return std::nullopt;
			if (!m_ports.level_load.begin_and_watch(level, std::move(loaded))) return "'" + std::string(level.view()) + "' did not start loading again";
			return std::nullopt;
		};
		migration.level_load_in_progress = [this] { return m_ports.level_load.in_progress(); };
		migration.approved_internally = m_ports.approved_internally;
		migration.code_identity = m_ports.game_layer_generation;
		migration.level_left_detached = [this](const res::tag& level, std::function<bool()> rolled_back, const std::string& reason) {
			static_cast<void>(level);
			m_stopped.hold(std::move(rolled_back),
				"a migration of the project stopped half way: roll the migration back first (" + reason + ")");
		};
		migration.current_level_tag = [this] { return m_ports.state.level_tag; };
		migration.register_implicit = [this](std::string_view id, std::function<bool()> fn) { m_ports.layer.register_implicit(id, std::move(fn)); };
		migration.running_changed = [this](bool running) {
			m_running = running;
			if (running) ++m_runs;
		};
		// The save block of a stopped migration goes with its rollback (its rolled_back() sees the journal too).
		migration.migration_rolled_back = [this](const std::string&) {
			static_cast<void>(m_stopped.reason());
		};
		migration.show_progress = [](bool open) {
			constexpr const char* title = "Migrating Project##edt_migrate";
			ImGui::OpenPopup(title);
			if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar)) {
				if (open) {
					ImGui::Text("Migrating project...");
				} else {
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
			}
		};
		for (auto& command : register_migration_commands(m_ports.context.actions().commands(), std::move(migration))) {
			out.push_back(std::move(command));
		}
	}
	// EPIC-58 S8 (#761): Project > Migrate Project... opens the Project Migration window, which runs the two commands above as
	// a person's calls (origin ui) and takes the migration's answer from job_finished.
	{
		migration_window_model::ports window;
		window.invoke = [this](std::string_view id, boost::json::value args) { return m_ports.context.actions().invoke(id, args); };
		const prj::project_context* project = m_ports.project;
		window.open_file = [this, project](const std::string& path) {
			if (project == nullptr) return;
			const std::filesystem::path file = (project->root / fs_paths::path_from_utf8(path)).lexically_normal();
			for (const std::filesystem::path& content : project->content_roots) {
				const std::filesystem::path relative = file.lexically_relative(content.lexically_normal());
				if (relative.empty() || *relative.begin() == std::filesystem::path("..")) continue;
				if (!m_ports.asset_open.open(res::tag::make(fs_paths::generic_utf8_of(relative)), false)) {
					egLOG_INFO("project/migrate", "'{}' is not a file the editor opens", path);
				}
				return;
			}
			egLOG_INFO("project/migrate", "'{}' is not a file the editor opens", path);
		};
		window.listing_of = [project](const std::string& id) {
			if (project == nullptr) return rollback_listing{ "no project is open", {}, {} };
			return rollback_listing_of(project->root, id);
		};
		m_panel = std::make_shared<migration_panel>(m_ports.context.events(), std::move(window));
		add_panel(m_panel);
		out.push_back(register_migration_window_action(m_ports.context.actions(),
			[project] { return project != nullptr; }, [this] { open_window(); }));
	}
}

bool edt::migration_component::start_level_settled() const
{
	return !(m_ports.level_load.in_progress() || m_ports.level_load.settling() || m_ports.startup_pending()
		|| m_ports.layer.has_implicit_prefix("edt/load_level"));
}

void edt::migration_component::install_startup()
{
	// #761: a migration the project was left with is offered for a rollback once the start level settled; headless nobody
	// is asked (a WARN names the command line), and nothing waits for an answer.
	if (m_ports.project_held || m_ports.project == nullptr) return;
	const std::filesystem::path root = m_ports.project->root;
	migration_startup::ports startup_ports;
	startup_ports.settled = [this] { return start_level_settled(); };
	startup_ports.unfinished = [root] { return prj::migration::unfinished_migrations(root); };
	startup_ports.journal_refusal = [root](const std::string& id) -> std::optional<std::string> {
		rollback_listing listing = rollback_listing_of(root, id);
		if (listing.refused.empty()) return std::nullopt;
		return std::move(listing.refused);
	};
	startup_ports.warn = [](const std::string& text) { egLOG_WARN("project/migrate", "{}", text); };
	startup_ports.ask = [this](const journal_offer& offer) { offer_rollback(offer); };
	startup_ports.start_scan = [this, root] { start_scan(root); };
	auto startup = std::make_shared<migration_startup>(std::move(startup_ports), m_ports.headless);
	// Headless: the WARN only, now in init (no question, no scan). With a person never in init, even with nothing to
	// wait for: the project's game modules load after it (#428), and the scan's types are theirs.
	if (m_ports.headless) static_cast<void>(startup->tick());
	else m_ports.layer.register_implicit("edt/migration_journal", [startup] { return startup->tick(); });
}

void edt::migration_component::install_open_on_start()
{
	if (!cfg_open_migration || m_ports.headless || m_ports.project_held || m_ports.project == nullptr) return;
	m_ports.layer.register_implicit("edt/open_migration", [this] {
		if (!start_level_settled()) return true;
		open_window();
		return false;
	});
}

void edt::migration_component::open_window()
{
	if (m_panel == nullptr) return;
	m_ports.context.panels().set_open(m_panel->get_id(), true);
	m_panel->show_and_plan();
}

void edt::migration_component::open_rollback(const std::string& id)
{
	if (m_panel == nullptr) return;
	m_ports.context.panels().set_open(m_panel->get_id(), true);
	m_panel->show_rollback_question(id);
}

void edt::migration_component::offer_rollback(const journal_offer& offer)
{
	if (offer.what == journal_offer::kind::inform) {
		m_ports.context.dialogs().choose(offer.title, offer.text, { std::string("OK") }, [](std::optional<std::size_t>) {});
		return;
	}
	if (offer.what != journal_offer::kind::ask) return;
	m_ports.context.dialogs().choose(offer.title, offer.text, { std::string(k_journal_offer_roll_back), std::string(k_journal_offer_later) },
		[this, id = offer.id](std::optional<std::size_t> answer) {
			if (answer && *answer == 0) open_rollback(id);
		});
}

void edt::migration_component::start_scan(const std::filesystem::path& root)
{
	const std::filesystem::path settings_file = migration_notice_file(m_ports.resources.mount_roots("edt"), default_user_data_dir());
	const std::filesystem::path settings_dir = settings_file.parent_path();
	migration_notice_model::ports ports;
	ports.load = [settings_file]() -> std::optional<std::string> {
		std::error_code ec;
		if (!std::filesystem::is_regular_file(settings_file, ec)) return std::nullopt;
		std::ifstream in(settings_file, std::ios::binary);
		if (!in) return std::nullopt;
		return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	};
	ports.save = [settings_file](const std::string& text) -> std::optional<std::string> {
		std::error_code ec;
		std::filesystem::create_directories(settings_file.parent_path(), ec);
		return prj::write_text_atomically(settings_file, text);
	};
	m_notice = std::make_unique<migration_notice_model>(std::move(ports));

	// The types are read here, on the main thread; the scan reads only files.
	const migration_notice_model::scan_ticket ticket{ approved_snapshot_for(settings_dir, root).key, m_ports.game_layer_generation(), m_runs };
	// Stopped by its token (job.cancel, cancel_all_and_wait when the editor quits) between any two files: a stopped scan is
	// dropped, never shown.
	auto work = [root, catalog = prj::migration::type_catalog::from(*m_ports.services.descs()),
					read_only = prj::migration::read_only_layers(m_ports.resources, root), checks = m_geometry_checks](ds::task_context& task) {
		prj::migration::scan_options options;
		options.stop = [&task] { return task.stop.stop_requested(); };
		options.read_only_layers = read_only;
		options.geometry_checks = checks.get();
		const prj::migration::scan_result scan = prj::migration::scan_project(root, catalog, options);
		if (scan.incomplete) throw ds::task_cancelled{};
		prj::migration::project_step_registry steps;
		prj::migration::register_project_steps(steps);
		migration_scan_summary summary = summarize_scan(scan, steps);
		summary.unfinished = prj::migration::unfinished_migrations(root);
		return summary;
	};
	m_scan_job = m_ports.context.jobs().start(ds::lane::io, ds::task_desc{ "Check the project's format", "project", false }, m_scan_lifetime,
		std::move(work), [this, ticket](ds::task_result<migration_scan_summary>& result) {
			if (!result.ok() || result.value == nullptr || m_notice == nullptr) return;
			// The editor never changes its project; the game layer may have (a module loaded or reloaded meanwhile), and a
			// migration or rollback that started since changes the files the scan read.
			const migration_notice_model::scan_ticket now{ ticket.project, m_ports.game_layer_generation(), m_runs };
			if (m_notice->deliver(ticket, now, std::move(*result.value)) && m_notice->visible()) {
				egLOG_INFO("project/migrate", "{} Project > Migrate Project... shows the plan.", m_notice->text());
			}
		});
	m_ports.layer.register_implicit("edt/migration_notice", [this] { return draw_notice(); });
}

bool edt::migration_component::draw_notice()
{
	if (m_notice == nullptr) return false;
	if (m_scan_job != 0 && m_ports.context.jobs().is_running(m_scan_job)) return true;
	if (m_running) m_notice->hide();
	if (!m_notice->visible()) return false;
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	// Above the agent policy's notice, which takes the corner.
	ImGui::SetNextWindowPos(
		ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - em(12.f),
			viewport->WorkPos.y + viewport->WorkSize.y - em(110.f)),
		ImGuiCond_Always, ImVec2(1.f, 1.f));
	const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
		| ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse;
	if (ImGui::Begin("Project format##migration_notice", nullptr, flags)) {
		const std::string text = m_notice->text();
		ImGui::TextUnformatted(text.c_str());
		if (ImGui::Button("Open Migration...")) {
			m_notice->open();
			open_window();
		}
		ImGui::SameLine();
		if (ImGui::Button("Later")) static_cast<void>(m_notice->later());
	}
	ImGui::End();
	return true;
}

std::optional<std::string> edt::migration_component::blocker() const
{
	if (m_running) return std::string{ "a project migration is running" };
	return std::nullopt;
}

std::optional<std::string> edt::migration_component::project_write_blocker()
{
	if (auto migrating = blocker()) return migrating;
	return m_stopped.reason();
}
