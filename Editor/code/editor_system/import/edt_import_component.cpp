#include "edt_import_component.h"
#include "edt_agent_model_import.h"
#include "edt_asset_export_dialog.h"
#include "edt_asset_exporter.h"
#include "edt_decoder_sandbox_notice.h"
#include "edt_editor_layer.h"
#include "edt_image_import.h"
#include "edt_import_problems.h"
#include "edt_model_importer.h"
#include "edt_play_session.h"
#include "edt_process_decoder.h"
#if SNK_HAS_ASSIMP
#include "edt_decoded_model_adapter.h"
#include "edt_person_model_import.h"
#include <snk/model_import/mdl_model_adapter.h>
#endif
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>
#include <snk/common/text_escape.h>
#include <snk/config/cfg_variables.h>
#include <snk/editor/edt_dialog_service.h>
#include <snk/editor/edt_editor_context.h>
#include <snk/editor/edt_job_service.h>
#include <snk/platform/plt_process.h>
#include <snk/resource/res_system.h>
#include <imgui.h>
#include <format>
#include <utility>

namespace
{
	// The memory the decoder process may use for an agent's model (#647), in MiB; 0: 2048. Kept between 256 and 8192.
	CFG_VAR_DEF_INT(cfg_import_decoder_memory_mb, "import/decoder_memory_mb", 0);
	// The decoder's OS sandbox (#1043) for every import, a person's setting: on (run without it where the system has
	// none, with one WARN), off, or required (CI: refuse to decode without it). An agent's import never changes it.
	CFG_VAR_DEF_STRING(cfg_import_decoder_sandbox, "import/decoder_sandbox", "on");
}

edt::import_component::import_component(ports from)
	: m_ports(std::move(from))
#if SNK_HAS_ASSIMP
	, m_importer(std::make_unique<model_importer>(m_ports.desc, m_ports.resources))
#endif
	, m_exporter(std::make_unique<asset_exporter>(m_ports.resources))
{
	// A throw below must not leave the adapter registered: it holds the decoder, and a destructor does not run for this object.
	try {
		build();
	}
	catch (...) {
		shutdown();
		throw;
	}
}

void edt::import_component::build()
{
	res::resource_system& resources = m_ports.resources;
	m_export_dialog = std::make_unique<asset_export_dialog>(*m_exporter, m_ports.context.jobs(), resources,
		m_ports.context.colors());
	agent_import_options import_options;
#if SNK_HAS_ASSIMP
	// An agent's model is parsed in a process of its own, with a memory limit and killed at its deadline (#647). There is
	// no fallback to parsing it here: without the decoder the command says so.
	{
		std::string memory_warning;
		decode_runner_config decoder_config;
		decoder_config.memory_limit_bytes = decoder_memory_limit(static_cast<std::int64_t>(*cfg_import_decoder_memory_mb), memory_warning);
		if (!memory_warning.empty()) egLOG_WARN("Editor/Import", "{}", memory_warning);
		std::string sandbox_warning;
		decoder_config.sandbox = decoder_sandbox_mode(*cfg_import_decoder_sandbox, sandbox_warning);
		if (!sandbox_warning.empty()) egLOG_WARN("Editor/Import", "{}", sandbox_warning);
		// Read once, here: what a person set before the editor started is the mode for the whole run (#1076).
		const sandbox_mode sandbox_mode_of_run = decoder_config.sandbox;
		m_sandbox_notice = std::make_shared<decoder_sandbox_notice>(m_ports.context.problems(), sandbox_mode_of_run,
			expected_decoder_sandbox(sandbox_mode_of_run));
		// The decoder runs on worker threads; Problems is the main thread's. The resource system may run the decoder after
		// this component is gone: the notice then is too, and nothing is posted.
		auto on_sandbox = [&tasks = m_ports.tasks, notice = std::weak_ptr(m_sandbox_notice)](const decoder_sandbox_status& status) {
			if (!notice.lock()) return;
			tasks.post_main([notice, status] {
				if (const auto alive = notice.lock()) alive->seen(status);
			});
		};
		if (m_ports.decoder) {
			m_steps = m_ports.decoder();
		} else {
			m_decoder = std::make_shared<process_decoder>(resources, m_ports.desc, std::move(decoder_config), std::move(on_sandbox));
			m_steps = steps_of(m_decoder);
		}
	}
	// A model a desc refers to (a level's res://x.glb) goes through the same decoder, never through assimp in this
	// process (#925); a load without it fails with the reason, in Problems with the rest of the level's.
	if (const std::string why = m_steps.unavailable_reason ? m_steps.unavailable_reason() : std::string{}; !why.empty()) {
		egLOG_WARN("Editor/Import", "{}: .glb/.gltf/.obj/.fbx models will not load", why);
	}
	resources.registrate_adapter<decoded_model_adapter>(mdl::model_adapter::INFO, std::ref(m_ports.desc), std::ref(resources),
		m_steps);
	m_adapter_registered = true;
	resources.warn_on_sync_require(mdl::model_adapter::INFO.extensions);
	// Imports of earlier runs that ended before their folders were erased (a crash, a kill): memory:// is on the disk and
	// shared by every editor on the project, so only the folders of runs that ended go.
	erase_imports_of_ended_runs(resources, platform::current_process_id());
	import_options.import = [steps = m_steps](const import_source& source, ds::task_context* task) {
		return steps.import(source, task);
	};
	import_options.scan = [steps = m_steps](const import_source& source, ds::task_context* task) {
		return steps.scan(source, task);
	};
	import_options.sandbox_status = [notice = std::weak_ptr(m_sandbox_notice)] {
		const auto alive = notice.lock();
		return alive ? alive->current() : decoder_sandbox_status{};
	};
	import_options.unavailable_reason = [steps = m_steps] {
		const std::string why = steps.unavailable_reason ? steps.unavailable_reason() : std::string{};
		return why.empty() ? why : "model import for agents is not available: " + why;
	};
#endif
	import_options.resource_layers = [&resources] { return resources.get_resource_layers(); };
	import_options.resources_path = [&resources] { return resources.get_resources_path(); };
#if SNK_HAS_ASSIMP
	import_options.blocker = [this] { return agent_import_blocker(); };
#else
	import_options.blocker = [this]() -> std::optional<std::string> {
		const play_session* play = m_ports.play ? m_ports.play() : nullptr;
		if (play && play->active()) return std::string{ "Stop Play to import." };
		if (m_dialog_open) return std::string{ "an import is waiting in the editor's export dialog" };
		return std::nullopt;
	};
#endif
	m_agent_import = std::make_unique<agent_model_import>(*m_exporter, m_ports.context.jobs(), resources, std::move(import_options));

	image_import_options image_options;
	image_options.resource_layers = [&resources] { return resources.get_resource_layers(); };
	image_options.resources_path = [&resources] { return resources.get_resources_path(); };
	image_options.blocker = [this]() -> std::optional<std::string> {
		if (m_ports.write_blocker) {
			if (std::optional<std::string> why = m_ports.write_blocker()) return why;
		}
		const play_session* play = m_ports.play ? m_ports.play() : nullptr;
		if (play && play->active()) return std::string{ "Stop Play to import." };
		if (m_dialog_open) return std::string{ "an import is waiting in the editor's export dialog" };
		if (person_import_running()) return std::string{ "a model import is running in the editor" };
		if (agent_import_running()) return std::string{ "a model import asked for by an agent is running" };
		return std::nullopt;
	};
	image_options.written = [this](const std::vector<res::tag>& tags) {
		// What the cache knew of these names (nothing there) goes.
		for (const res::tag& tag : tags) m_ports.resources.signal_changed(tag);
		if (m_ports.assets_written) m_ports.assets_written(tags);
	};
	m_image_import = std::make_unique<image_importer>(m_ports.context.jobs(), std::move(image_options));
}

edt::import_component::~import_component()
{
	shutdown();
}

void edt::import_component::shutdown()
{
	if (m_shut_down) return;
	m_shut_down = true;
#if SNK_HAS_ASSIMP
	// A load still running keeps the adapter, and with it the decoder, alive until it ends.
	if (m_adapter_registered) m_ports.resources.unregistrate_adapter(mdl::model_adapter::INFO);
	if (m_importing) m_ports.context.jobs().cancel(m_job);
	// Jobs cancelled earlier (Esc) may still store into their folders too: every one ends before anything is erased.
	m_ports.tasks.wait_idle(m_lifetime);
	finish_import();
	m_cancelled.clear();
	m_dialog_folder.clear();
	std::string this_run = person_run_folder(platform::current_process_id());
	erase_import_folder(this_run);
#endif
}

bool edt::import_component::person_import_running() const
{
#if SNK_HAS_ASSIMP
	return m_importing;
#else
	return false;
#endif
}

bool edt::import_component::agent_import_running() const
{
	return m_agent_import && m_agent_import->running();
}

bool edt::import_component::poll_pipeline()
{
	if (m_dialog_open) {
		bool still_open = m_export_dialog->render();
		if (!still_open) {
			m_dialog_open = false;
#if SNK_HAS_ASSIMP
			erase_import_folder(m_dialog_folder);
#endif
			auto exported_tag = m_export_dialog->get_exported_tag();
			if (exported_tag.is_valid() && m_ports.exported) {
				m_ports.exported(exported_tag);
			}
			return false;
		}
		return true;
	}

#if SNK_HAS_ASSIMP
	if (m_importing) {
		return poll_progress();
	}
#endif
	return false;
}

#if SNK_HAS_ASSIMP
std::optional<std::string> edt::import_component::agent_import_blocker() const
{
	const play_session* play = m_ports.play ? m_ports.play() : nullptr;
	if (play && play->active()) return std::string{ "Stop Play to import." };
	if (m_dialog_open) return std::string{ "an import is waiting in the editor's export dialog" };
	if (m_importing) return std::string{ "an import is already running in the editor" };
	return std::nullopt;
}

void edt::import_component::start_import(const std::filesystem::path& absolute_path)
{
	if (m_importing) return;
	if (m_dialog_open) {
		m_ports.context.dialogs().message(message_level::info,
			"An import is waiting in the editor's export dialog: export it or close the dialog first.");
		return;
	}
	if (agent_import_running()) {
		m_ports.context.dialogs().message(message_level::info, "An import asked for by an agent is running: wait for it, or cancel it in the job list.");
		return;
	}
	if (const play_session* play = m_ports.play ? m_ports.play() : nullptr; play && play->active()) {
		// Play locks the document: refused where the person sees it, not only in the log.
		egLOG_WARN("play", "{}", "Stop Play to import.");
		m_ports.context.dialogs().message(message_level::warning, "Stop Play to import.");
		return;
	}

	m_importing = true;
	m_finished = false;
	m_spinner_t = 0.f;
	m_result.reset();
	m_failure.clear();
	// The name of a file someone else may have made: shown in the popup, the job list and the log only escaped.
	m_name = text_escape::escaped_text(fs_paths::utf8_of(absolute_path.filename()), 256);
	m_import_folder = person_import_folder(absolute_path, platform::current_process_id(), ++m_serial);
	// The model is parsed in the decoder process (#987), never here: the job reads the file and waits for the decoder.
	m_job = m_ports.context.jobs().start(ds::lane::compute,
		{ "Import " + m_name, "import" }, m_lifetime,
		[decoder = m_steps, &resources = m_ports.resources, absolute_path,
			folder = m_import_folder](ds::task_context& ctx) -> import_intermediate {
			return person_model_import(absolute_path, folder, decoder, resources, ctx);
		},
		[this](ds::task_result<import_intermediate>& result) {
			if (!m_importing) return;
			m_finished = true;
			if (result.ok()) {
				m_result = std::move(*result.value);
			} else {
				m_failure = result.error;
			}
		});
	m_ports.layer.register_implicit("edt/import_model", [this] { return poll_pipeline(); });
}

void edt::import_component::finish_import()
{
	erase_import_folder(m_import_folder);
	m_importing = false;
	m_finished = false;
	m_job = 0;
	m_result.reset();
	m_failure.clear();
	m_name.clear();
}

bool edt::import_component::poll_progress()
{
	editor_context& context = m_ports.context;
	// Not running and not finished: the job was cancelled and its result discarded.
	const bool waiting = !m_finished && context.jobs().is_running(m_job);

	if (waiting) {
		ImGui::OpenPopup("Importing##edt_import");
		if (ImGui::BeginPopupModal("Importing##edt_import", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar))
		{
			if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
				ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
				context.jobs().cancel(m_job);
				// A cancel reports nothing: the rows of the last import would read as this one's.
				context.problems().clear(k_import_problem_source);
				defer_erase_until_stopped(m_job, std::exchange(m_import_folder, std::string{}));
				finish_import();
				ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
				return false;
			}
			m_spinner_t += ImGui::GetIO().DeltaTime;
			const char* spinners[] = { "|", "/", "-", "\\" };
			int idx = static_cast<int>(m_spinner_t * 8.f) % 4;
			ImGui::Text("%s  Importing '%s'...", spinners[idx], m_name.c_str());
			ImGui::EndPopup();
		}
		return true;
	}

	ImGui::OpenPopup("Importing##edt_import");
	if (ImGui::BeginPopupModal("Importing##edt_import", nullptr,
		ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar))
	{
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	if (!m_result) {
		if (!m_failure.empty()) {
			egLOG("Editor/Import", "Async import failed: {}", text_escape::escaped_text(m_failure, 1024));
			context.problems().set(k_import_problem_source, import_failure_problems(m_name, m_failure));
		} else {
			context.problems().clear(k_import_problem_source);
		}
		finish_import();
		return false;
	}

	auto intermediate = std::move(*m_result);
	// The dialog takes the folder over; finish_import erases what is left of any other outcome.
	std::string folder = std::exchange(m_import_folder, std::string{});
	const std::string name = m_name;
	finish_import();

	if (!intermediate.error.empty()) {
		erase_import_folder(folder);
		egLOG("Editor/Import", "Import of '{}' failed: {}", name, text_escape::escaped_text(intermediate.error, 1024));
		context.problems().set(k_import_problem_source, import_failure_problems(name, intermediate.error));
		context.dialogs().message(message_level::error,
			std::format("Import of '{}' failed: {}", name, text_escape::escaped_text(intermediate.error, 1024)));
		return false;
	}

	// Every outcome replaces what the last import reported.
	std::vector<problem> rows = import_problems(intermediate, name);
	auto result = m_importer->finalize_on_main(std::move(intermediate));
	if (result.prefab) {
		context.problems().set(k_import_problem_source, std::move(rows));
		egLOG("Editor/Import", "Model imported to memory: {}", result.root_tag.view());
		m_dialog_folder = std::move(folder);
		m_export_dialog->open(std::move(result));
		m_dialog_open = true;
		return true;
	}

	egLOG("Editor/Import", "Failed to finalize import");
	context.problems().set(k_import_problem_source, import_failure_problems(name, "the imported model could not be finalized"));
	erase_import_folder(folder);
	return false;
}

void edt::import_component::defer_erase_until_stopped(std::uint64_t job, std::string folder)
{
	if (folder.empty()) return;
	const bool sweeping = !m_cancelled.empty();
	m_cancelled.emplace_back(job, std::move(folder));
	if (!sweeping) m_ports.layer.register_implicit("edt/import_model/cancelled", [this] { return erase_folders_of_stopped_jobs(); });
}

bool edt::import_component::erase_folders_of_stopped_jobs()
{
	std::erase_if(m_cancelled, [this](std::pair<std::uint64_t, std::string>& cancelled) {
		if (m_ports.context.jobs().is_running(cancelled.first)) return false;
		erase_import_folder(cancelled.second);
		return true;
	});
	return !m_cancelled.empty();
}

void edt::import_component::erase_import_folder(std::string& folder)
{
	if (folder.empty()) return;
	if (const std::optional<res::tag> left = erase_memory_folder(m_ports.resources, folder)) {
		egLOG_WARN("Editor/Import", "'{}' of an import cannot be removed", left->view());
	}
	folder.clear();
}
#endif
