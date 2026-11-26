#include <boost/test/unit_test.hpp>
#if SNK_HAS_ASSIMP
#include "null_engine_fixture.hpp"
#include "test_temp_dir.hpp"
#include "edt_editor_layer.h"
#include "edt_file_menu.h"
#include "edt_import_component.h"
#include "edt_asset_export_dialog.h"
#include "edt_file_identity.h"
#include "edt_import_problems.h"
#include "edt_model_import_limits.h"
#include "edt_process_decoder.h"
#include "edt_scene_document.h"
#include <snk/command/cmd_registry.h>
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log_sinks.h>
#include <snk/desc/desc_system.h>
#include <snk/ecs/ecs_system.h>
#include <snk/editor/edt_dialog_service.h>
#include <snk/editor/edt_editor_context.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/editor/edt_job_service.h>
#include <snk/editor/edt_problem_list.h>
#include <snk/gui/gui_system.h>
#include <snk/platform/plt_process.h>
#include <snk/render/rnd_render_system.h>
#include <snk/resource/res_asset_meta.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/scn_ecs_assembler.h>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <imgui.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// The person's import as the editor runs it (#987, #1044): import_component over a real editor_context, with the decoder's
// steps faked through the ports, memory:// on the disk as in a project, and the "Importing" popup driven by bare ImGui frames.

namespace {

namespace fs = std::filesystem;

bool contains(std::string_view text, std::string_view part)
{
	return text.find(part) != std::string_view::npos;
}

std::vector<std::byte> bytes_of(std::string_view text)
{
	const std::span<const std::byte> bytes = std::as_bytes(std::span(text));
	return { bytes.begin(), bytes.end() };
}

// Built before the engine, so its memory:// can be mounted on the folder.
struct memory_on_disk
{
	test_util::temp_dir dir{ "edt_import_component" };
	fs::path memory = dir.path() / "project" / ".cache" / "memory";

	memory_on_disk() { fs::create_directories(memory); }
};

// Headless ImGui context: the popup of the import is drawn into it.
struct imgui_ctx
{
	imgui_ctx()
	{
		ctx = ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.DisplaySize = ImVec2(800.f, 600.f);
		unsigned char* px = nullptr;
		int w = 0, h = 0;
		io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
	}
	~imgui_ctx() { ImGui::DestroyContext(ctx); }

	ImGuiContext* ctx = nullptr;
};

struct import_component_fixture : memory_on_disk, test_util::null_engine_fixture
{
	import_component_fixture()
		: null_engine_fixture(false, [this](res::resource_system& rs) {
			rs.unregistrate_resolver(res::tag::memory);
			rs.registrate_resolver<res::vfs_resolver>(res::tag::memory, std::vector<fs::path>{ memory },
				res::mount_access::read_write, res::write_policy::allow_all());
		})
		, gui(nullptr, nullptr)
		, services{ .ctx_manager = storage.require_shared<scn::context_manager>(),
			.base_descs = &storage.require<desc::desc_system>(), .base_resources = &storage.require<res::resource_system>(),
			.base_assembler = &storage.require<scn::ecs_assembler>(), .base_sfactory = &storage.require<ecs::system_factory>() }
		, context(std::make_unique<edt::editor_context>(
			storage.require<res::resource_system>(), storage.require<rnd::render_system>(),
			gui, services, tasks, commands, scene))
		, layer(std::make_shared<edt::editor_layer>())
	{
		// Nothing draws the dialogs: they wait in the queue, where list() shows them.
		context->dialogs().set_registrar([](std::string_view, std::function<bool()>) {});
		model = dir.write("models/robot.glb", "glb-bytes");
	}

	~import_component_fixture()
	{
		component.reset();
		context.reset();
	}

	res::resource_system& rs() { return storage.require<res::resource_system>(); }
	desc::desc_system& descs() { return storage.require<desc::desc_system>(); }

	// A decoder that lists nothing (or fails the listing with `scan_error`) and imports one prefab into the import's folder.
	edt::decoder_steps decoder()
	{
		edt::decoder_steps steps;
		steps.unavailable_reason = [] { return std::string{}; };
		steps.scan = [this](const edt::import_source&, ds::task_context*) {
			++scans;
			edt::model_file_scan found;
			found.error = scan_error;
			return found;
		};
		steps.import = [this](const edt::import_source& source, ds::task_context*) {
			// As the decoder process does past its deadline: the limit the import gave it, by name.
			if (import_times_out) throw std::runtime_error(edt::time_limit_refusal("decoding the model", source.time_limit));
			++imports;
			import_settings.push_back(source.settings);
			edt::import_intermediate result;
			// Named after the model, as the importer names them: the folder of the import is "<...>/<stem>/".
			const std::string folder = source.memory_folder;
			const std::string stem = fs::path(folder.substr(0, folder.size() - 1)).filename().string();
			result.prefab_tag = res::tag{ "memory://" + folder + stem + ".prefab.desc" };
			const std::string bytes = prefab_text.empty() ? std::string(4, '{') : prefab_text;
			BOOST_TEST(rs().store_sync(result.prefab_tag, bytes_of(bytes)));
			result.created_tags = { result.prefab_tag };
			for (const auto& [suffix, text] : extra_files) {
				const res::tag extra{ "memory://" + folder + stem + suffix };
				BOOST_TEST(rs().store_sync(extra, bytes_of(text)));
				result.created_tags.push_back(extra);
			}
			result.all_tags = result.created_tags;
			result.prefab_json = boost::json::parse(R"({ "__type": "prefab_desc" })").as_object();
			descs().migrate_tree(result.prefab_json, "test");
			return result;
		};
		return steps;
	}

	void make_component()
	{
		component = std::make_unique<edt::import_component>(edt::import_component::ports{ .context = *context, .resources = rs(),
			.desc = descs(), .tasks = tasks, .layer = *layer,
			.exported = [this](const res::tag&) { ++exported_to_scene; },
			.decoder = [this] { return decoder(); },
			.open_with_unsaved_changes = [this](const res::tag& tag) { return tag.view() == open_dirty; },
			.unsaved_edits_holder = [this](const res::tag& tag) {
				return tag.view() == open_dirty ? dirty_holder : std::string{};
			} });
	}

	// One editor frame: the import's popup (its implicit callback), then the jobs and the main-thread queue.
	void frame()
	{
		ImGui::NewFrame();
		layer->process_implicit();
		ImGui::EndFrame();
		test_util::pump(tasks);
	}

	// A frame in which no job runs: the import stays queued. `escape` presses Esc in it.
	void frame_without_jobs(bool escape)
	{
		ImGuiIO& io = ImGui::GetIO();
		if (escape) io.AddKeyEvent(ImGuiKey_Escape, true);
		ImGui::NewFrame();
		layer->process_implicit();
		ImGui::EndFrame();
		if (escape) io.AddKeyEvent(ImGuiKey_Escape, false);
	}

	// Esc in the "Importing" popup while the job is still queued.
	void cancel_with_escape()
	{
		frame_without_jobs(false);
		frame_without_jobs(true);
		BOOST_REQUIRE_MESSAGE(!component->person_import_running(), "Esc reached the popup and cancelled the import");
	}

	// What a cancelled job could still store after the cancel (it is cooperative).
	res::tag store_late_file()
	{
		const res::tag late{ std::format("memory://{}1/robot/late.bin", run_folder()) };
		BOOST_REQUIRE(rs().store_sync(late, std::vector<std::byte>(4, std::byte{ 1 })));
		return late;
	}

	void frames_until(const std::function<bool()>& done)
	{
		for (int i = 0; i < 20 && !done(); ++i) frame();
	}

	std::string run_folder() const { return std::format("__imports/{}/", platform::current_process_id()); }

	std::size_t files_of_this_run()
	{
		const std::optional<std::vector<res::tag>> listed = rs().files_under(res::tag::memory, run_folder());
		BOOST_REQUIRE(listed.has_value());
		return listed->size();
	}

	// Windows lists a removed file until the last handle on it closes (the resolver's watcher may hold one a moment), so
	// every check that a file is gone waits for it.
	static bool within(std::chrono::milliseconds limit, const std::function<bool()>& done)
	{
		const auto deadline = std::chrono::steady_clock::now() + limit;
		while (!done()) {
			if (std::chrono::steady_clock::now() >= deadline) return false;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		return true;
	}

	bool no_files_of_this_run_within(std::chrono::milliseconds limit)
	{
		return within(limit, [this] { return files_of_this_run() == 0u; });
	}

	std::vector<std::string> dialog_texts() const
	{
		std::vector<std::string> out;
		for (const edt::dialog_service::dialog_info& info : context->dialogs().list()) out.push_back(info.text);
		return out;
	}

	gui::gui_system gui;
	cmd::command_registry commands;
	edt::editor_services services;
	edt::scene_document scene;
	std::unique_ptr<edt::editor_context> context;
	std::shared_ptr<edt::editor_layer> layer;
	imgui_ctx imgui;
	fs::path model;
	std::string scan_error;
	bool import_times_out = false;
	int scans = 0;
	int imports = 0;
	std::vector<edt::model_import_settings> import_settings;
	// The tag a document holds with unsaved edits; empty: none.
	std::string open_dirty;
	// The other document whose unsaved edits `open_dirty` is open under as a link; empty: its own.
	std::string dirty_holder;
	// What the fake decoder stores: the prefab's bytes (empty: four '{') and other files, "<stem><suffix>" -> bytes.
	std::string prefab_text;
	std::vector<std::pair<std::string, std::string>> extra_files;
	int exported_to_scene = 0;
	std::unique_ptr<edt::import_component> component;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtImportComponentTests, import_component_fixture)

BOOST_AUTO_TEST_CASE(TheSettingsWindow_ClosedWithEsc_ImportsNothing)
{
	make_component();
	int imported = 0;
	component->ask_settings(model, [&imported](const fs::path&, const edt::model_import_settings&) { ++imported; });
	BOOST_REQUIRE(component->settings_open());
	frame_without_jobs(false);
	BOOST_REQUIRE(component->settings_open());
	frame_without_jobs(true);
	BOOST_CHECK(!component->settings_open());
	frames_until([] { return false; });
	BOOST_TEST(imported == 0);
	BOOST_TEST(imports == 0);
	BOOST_CHECK(!component->person_import_running());
}

BOOST_AUTO_TEST_CASE(TheSettingsWindow_IgnoresASecondModel_AndShutdownClosesItWithoutImporting)
{
	make_component();
	std::vector<std::string> imported;
	const auto record = [&imported](const fs::path& source, const edt::model_import_settings&) {
		imported.push_back(source.filename().string());
	};
	component->ask_settings(model, record);
	component->ask_settings(dir.write("models/other.glb", "glb-bytes"), record);
	frame_without_jobs(false);
	BOOST_REQUIRE(component->settings_open());
	component->shutdown();
	BOOST_CHECK(!component->settings_open());
	BOOST_CHECK(imported.empty());
	BOOST_TEST(imports == 0);
}

BOOST_AUTO_TEST_CASE(TheMenusImport_GoesThroughTheSettingsWindow_ToTheJobAndTheExportDialog)
{
	// The chain as file_menu_component wires it: File > Import Model... -> pick -> the window -> file.import_model
	// {path, settings} as the person -> start_import. No agent import is wired: the call must take the dialog branch.
	make_component();
	context->dialogs().set_registrar([this](std::string_view id, std::function<bool()> pump) {
		layer->register_implicit(id, std::move(pump));
	});
	edt::file_menu_hooks hooks;
	hooks.import_model = [this](const fs::path& source, const edt::model_import_settings& settings) {
		component->start_import(source, settings);
	};
	hooks.ask_model_settings = [this](const fs::path& source,
		std::function<void(const edt::model_import_settings&)> import) {
		component->ask_settings(source, [import = std::move(import)](const fs::path&,
			const edt::model_import_settings& settings) { import(settings); });
	};
	auto menu = std::make_unique<edt::file_menu>(context->dialogs(), context->actions(), hooks);
	menu->register_actions();
	edt::dialog_auto_answer answer;
	answer.picked_file = model;
	context->dialogs().set_auto_answer(answer);

	menu->import_model();
	frames_until([this] { return component->settings_open(); });
	BOOST_REQUIRE(component->settings_open());
	BOOST_TEST(imports == 0, "nothing is imported before Import");
	edt::model_import_settings_form& form = component->settings_form();
	form.set_normals(edt::model_normals::flat);
	form.set_tangents(false);
	form.submit();

	frames_until([this] { return component->dialog_open(); });
	BOOST_REQUIRE_MESSAGE(component->dialog_open(), "the person's import ends in the export dialog");
	BOOST_REQUIRE_EQUAL(import_settings.size(), 1u);
	const edt::model_import_settings chosen{ .normals = edt::model_normals::flat, .tangents = false };
	BOOST_CHECK(import_settings[0] == chosen);
	component->shutdown();
	menu.reset();
}

BOOST_AUTO_TEST_CASE(TheSettingsChosen_ReachTheDecoder)
{
	make_component();
	const edt::model_import_settings flat{ .normals = edt::model_normals::flat, .tangents = false };
	component->start_import(model, flat);
	frames_until([this] { return component->dialog_open(); });
	BOOST_REQUIRE(component->dialog_open());
	BOOST_REQUIRE_EQUAL(import_settings.size(), 1u);
	BOOST_CHECK(import_settings[0] == flat);
}

BOOST_AUTO_TEST_CASE(AnImportWaitingInTheExportDialog_LeavesNothingOfThisRunInMemory_AfterShutdown)
{
	make_component();
	component->start_import(model);
	frames_until([this] { return component->dialog_open(); });
	BOOST_REQUIRE(component->dialog_open());
	BOOST_TEST(imports == 1);
	BOOST_TEST(files_of_this_run() > 0u, "the import is in a folder of this run");

	component->shutdown();
	test_util::pump(tasks);
	BOOST_TEST(no_files_of_this_run_within(std::chrono::seconds(5)));
}

BOOST_AUTO_TEST_CASE(ASecondImportWhileTheExportDialogIsOpen_IsRefused_AndTheDialogKeepsItsImport)
{
	make_component();
	component->start_import(model);
	frames_until([this] { return component->dialog_open(); });
	BOOST_REQUIRE(component->dialog_open());
	const std::size_t files_before = files_of_this_run();
	const std::size_t dialogs_before = context->dialogs().pending();

	component->start_import(model);
	BOOST_TEST(!component->person_import_running());
	BOOST_TEST(component->dialog_open());
	BOOST_REQUIRE(context->dialogs().pending() == dialogs_before + 1);
	BOOST_TEST(contains(dialog_texts().back(), "waiting in the editor's export dialog"), dialog_texts().back());
	frame();
	BOOST_TEST(imports == 1, "no second import ran");
	BOOST_TEST(files_of_this_run() == files_before);
}

BOOST_AUTO_TEST_CASE(AShutdownWhileTheImportRuns_StopsTheJobFirst_AndLeavesNothingInMemory)
{
	make_component();
	component->start_import(model);
	BOOST_REQUIRE(component->person_import_running());

	component->shutdown();
	test_util::pump(tasks);
	BOOST_TEST(!component->person_import_running());
	BOOST_TEST(scans == 0);
	BOOST_TEST(imports == 0);
	BOOST_TEST(no_files_of_this_run_within(std::chrono::seconds(5)));
	BOOST_TEST(context->jobs().running_count() == 0u);
}

BOOST_AUTO_TEST_CASE(AnImportCancelledWithEsc_ThenAShutdown_LeavesNothingOfThisRunInMemory)
{
	make_component();
	component->start_import(model);
	cancel_with_escape();
	store_late_file();

	component->shutdown();
	test_util::pump(tasks);
	BOOST_TEST(imports == 0);
	BOOST_TEST(no_files_of_this_run_within(std::chrono::seconds(5)));
}

BOOST_AUTO_TEST_CASE(TheFolderOfAnImportCancelledWithEsc_GoesOnceItsJobStopped)
{
	make_component();
	component->start_import(model);
	cancel_with_escape();
	store_late_file();

	frames_until([this] { return files_of_this_run() == 0u; });
	BOOST_TEST(no_files_of_this_run_within(std::chrono::seconds(5)));
	BOOST_TEST(imports == 0);
}

BOOST_AUTO_TEST_CASE(AnImportPastItsTimeLimit_NamesThePersonsLimitAndTheStep_InTheLogProblemsAndTheDialog)
{
	import_times_out = true;
	std::mutex mutex;
	std::vector<std::string> lines;
	const engine::log_sink_handle sink = engine::add_log_sink([&](std::string_view category, engine::log_level, std::string_view message) {
		if (category != "Editor/Import" || !contains(message, "failed")) return;
		std::lock_guard lock(mutex);
		lines.emplace_back(message);
	});
	make_component();
	component->start_import(model);
	frames_until([this] { return !component->person_import_running(); });
	BOOST_REQUIRE(!component->person_import_running());
	BOOST_TEST(!component->dialog_open());

	const std::string refusal = "decoding the model exceeds 2 min (decoder time)";
	{
		std::lock_guard lock(mutex);
		BOOST_REQUIRE(lines.size() == 1u);
		BOOST_TEST(contains(lines.front(), refusal), lines.front());
	}
	const std::vector<edt::sourced_problem> rows = context->problems().list();
	BOOST_REQUIRE(rows.size() == 1u);
	BOOST_TEST(contains(rows.front().item.message, refusal), rows.front().item.message);
	BOOST_REQUIRE(!dialog_texts().empty());
	BOOST_TEST(contains(dialog_texts().back(), refusal), dialog_texts().back());
	BOOST_TEST(no_files_of_this_run_within(std::chrono::seconds(5)), "a failed import leaves nothing in memory://");
}

BOOST_AUTO_TEST_CASE(AFailedImport_NamesTheModelAndTheSideFile_OnlyEscaped_InTheLogProblemsAndTheDialog)
{
	// U+202E (right-to-left override) and, where a file name may hold them, '\' and ':'; ':' is shown as it is.
	const std::string hostile = "\xE2\x80\xAE";
#if defined(_WIN32)
	const std::string name = "ro" + hostile + "bot.glb";
	const std::string name_shown = "ro\\u{202E}bot.glb";
#else
	const std::string name = "r\\o:" + hostile + "bot.glb";
	const std::string name_shown = "r\\\\o:\\u{202E}bot.glb";
#endif
	model = dir.write(fs_paths::path_from_utf8("models/" + name), "glb-bytes");
	scan_error = "'..\\tex:" + hostile + ".png' is outside the folder of the model";
	const std::string side_shown = "'..\\\\tex:\\u{202E}.png' is outside the folder of the model";

	std::mutex mutex;
	std::vector<std::string> lines;
	const engine::log_sink_handle sink = engine::add_log_sink([&](std::string_view category, engine::log_level, std::string_view message) {
		if (category != "Editor/Import" || !contains(message, "failed")) return;
		std::lock_guard lock(mutex);
		lines.emplace_back(message);
	});
	make_component();
	component->start_import(model);
	frames_until([this] { return !component->person_import_running(); });
	BOOST_REQUIRE(!component->person_import_running());
	BOOST_TEST(!component->dialog_open());

	std::vector<std::string> shown;
	{
		std::lock_guard lock(mutex);
		BOOST_REQUIRE(lines.size() == 1u);
		shown.push_back(lines.front());
	}
	const std::vector<edt::sourced_problem> rows = context->problems().list();
	BOOST_REQUIRE(rows.size() == 1u);
	BOOST_TEST(rows.front().source == edt::k_import_problem_source);
	shown.push_back(rows.front().item.message);
	BOOST_REQUIRE(!dialog_texts().empty());
	shown.push_back(dialog_texts().back());
	for (const std::string& text : shown) {
		BOOST_TEST(!contains(text, hostile), text);
		BOOST_TEST(contains(text, name_shown), text);
		BOOST_TEST(contains(text, side_shown), text);
	}
}

BOOST_AUTO_TEST_CASE(AtStartup_TheImportsOfARunThatEnded_Go_AndThoseOfThisRunStay)
{
	constexpr std::int64_t ended_run = 2147483646;
	BOOST_REQUIRE(!platform::process_is_running(ended_run));
	const std::int64_t this_run = platform::current_process_id();
	dir.write(fs::path("project") / ".cache" / "memory" / "__imports" / std::to_string(ended_run) / "x", "left by a crash");
	dir.write(fs::path("project") / ".cache" / "memory" / "__imports" / std::to_string(this_run) / "x", "this run's");
	const res::tag ended{ std::format("memory://__imports/{}/x", ended_run) };
	const res::tag current{ std::format("memory://__imports/{}/x", this_run) };
	BOOST_REQUIRE(rs().exists(ended));
	BOOST_REQUIRE(rs().exists(current));

	make_component();
	BOOST_TEST(within(std::chrono::seconds(5), [&] { return !rs().exists(ended); }));
	BOOST_TEST(rs().exists(current));
}

BOOST_AUTO_TEST_SUITE_END()

namespace {

// A prefab an earlier import made (#1194): res:// on the disk, an engine layer below the project's own.
struct reimport_fixture : import_component_fixture
{
	reimport_fixture()
	{
		fs::create_directories(engine_res);
		fs::create_directories(project_res);
		rs().unregistrate_resolver(res::tag::default_protocol());
		rs().registrate_resolver<res::vfs_resolver>(res::tag::default_protocol(),
			std::vector<fs::path>{ engine_res, project_res }, res::mount_access::read_write,
			res::write_policy::allow_all());
		prefab_text = R"({"__type":"prefab_desc","v":2})";
		extra_files = { { ".mat.desc", R"({"__type":"material_desc","v":2})" } };
	}

	static res::tag material() { return res::tag::make("assets/robot/robot.mat.desc"); }

	std::string read_project(const char* relative) const
	{
		std::ifstream in(project_res / relative, std::ios::binary);
		return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	}

	// A person's re-import of `source` over the prefab, up to the export dialog with its check of the files done.
	edt::asset_export_dialog& checked_dialog(const fs::path& source)
	{
		const std::optional<edt::model_import_refusal> refused = component->reimport(prefab(), source, nullptr);
		BOOST_REQUIRE_MESSAGE(!refused.has_value(), refused ? refused->error : std::string{});
		frames_until([this] { return component->dialog_open() && !component->export_dialog().checking(); });
		BOOST_REQUIRE(component->dialog_open());
		return component->export_dialog();
	}

	// Export runs to its end: the dialog closes, or goes back to the form with the reason.
	void export_now(edt::asset_export_dialog& dialog)
	{
		BOOST_REQUIRE_MESSAGE(dialog.start_export(), dialog.export_blocker());
		frames_until([this, &dialog] { return !component->dialog_open() || !dialog.exporting(); });
	}

	// The first re-import over a record of before #1194 (no `made`): every file is unknown, the tick lets it through.
	void reimport_over_an_old_record()
	{
		write_record();
		make_component();
		edt::asset_export_dialog& dialog = checked_dialog(model);
		dialog.set_overwrite_ticked(true);
		export_now(dialog);
		BOOST_REQUIRE(!component->dialog_open());
	}

	const edt::reimport_file_check* check_of(const edt::asset_export_dialog& dialog, const res::tag& tag) const
	{
		if (!dialog.checked()) return nullptr;
		for (const edt::reimport_file_check& file : dialog.checked()->files) {
			if (file.tag == tag) return &file;
		}
		return nullptr;
	}

	static res::tag prefab() { return res::tag::make("assets/robot/robot.prefab.desc"); }

	// The SHA-256 the import records for the model file.
	std::string model_sha() const
	{
		const std::string bytes = "glb-bytes";
		const edt::content_digest digest = edt::digest_of(std::as_bytes(std::span(bytes)));
		std::string hex;
		for (const std::uint8_t byte : digest) hex += std::format("{:02x}", byte);
		return hex;
	}

	// The prefab and its `.resource` as the import wrote them; `importer` and `newer` spoil it.
	res::asset_meta write_record(std::string importer = "model")
	{
		dir.write("project/res/assets/robot/robot.prefab.desc", R"({"__type":"prefab_desc"})");
		res::asset_meta meta;
		meta.guid = res::generate_asset_id();
		meta.importer = std::move(importer);
		meta.settings = edt::model_settings_json(recorded, edt::model_source_of("robot.glb", model_sha()));
		meta.flags = { "keep" };
		dir.write("project/res/assets/robot/robot.prefab.desc.resource", res::write_asset_meta(meta));
		return meta;
	}

	cmd::result import_call(const cmd::origin& from, boost::json::object args)
	{
		cmd::call request;
		request.args = std::move(args);
		request.from = from;
		return context->actions().invoke("file.import_model", request);
	}

	const fs::path engine_res = dir.path() / "engine_res";
	const fs::path project_res = dir.path() / "project" / "res";
	const edt::model_import_settings recorded{ .normals = edt::model_normals::flat, .tangents = false };
};

}

BOOST_FIXTURE_TEST_SUITE(EdtModelReimportTests, reimport_fixture)

BOOST_AUTO_TEST_CASE(TheWindow_OpensFilledFromTheRecord_AndCancelImportsNothing)
{
	write_record();
	make_component();
	int imported = 0;
	component->ask_reimport_settings(prefab(), model,
		[&imported](const fs::path&, const edt::model_import_settings&) { ++imported; });
	BOOST_REQUIRE(component->settings_open());
	const edt::model_import_settings_form& form = component->settings_form();
	BOOST_CHECK(form.reimport());
	BOOST_CHECK_EQUAL(form.target(), std::string(prefab().view()));
	BOOST_CHECK(form.settings() == recorded);
	// The model is hashed by a job: the note comes when it is done.
	const auto same_file = [&form] {
		for (const std::string& note : form.notes()) {
			if (contains(note, "same file as last time")) return true;
		}
		return false;
	};
	frames_until(same_file);
	BOOST_CHECK(same_file());
	frame_without_jobs(false);
	frame_without_jobs(true);
	BOOST_CHECK(!component->settings_open());
	frames_until([] { return false; });
	BOOST_TEST(imported == 0);
	BOOST_TEST(imports == 0);
}

BOOST_AUTO_TEST_CASE(AnotherFileName_IsNoted_AndBlocksNothing)
{
	write_record();
	make_component();
	component->ask_reimport_settings(prefab(), dir.write("models/robot_v2.glb", "other bytes"),
		[](const fs::path&, const edt::model_import_settings&) {});
	BOOST_REQUIRE(component->settings_open());
	bool other_name = false;
	bool same_file = false;
	for (const std::string& note : component->settings_form().notes()) {
		other_name = other_name || contains(note, "name differs");
		same_file = same_file || contains(note, "same file as last time");
	}
	BOOST_CHECK(other_name);
	BOOST_CHECK(!same_file);
	component->settings_form().cancel();
}

BOOST_AUTO_TEST_CASE(TheRecordedSettings_ReachTheDecoder_AndTheExportDialogIsOverThePrefab)
{
	write_record();
	make_component();
	edt::file_menu_hooks hooks;
	hooks.import_model = [this](const fs::path& source, const edt::model_import_settings& settings) {
		component->start_import(source, settings);
	};
	hooks.reimport_model = [this](const res::tag& tag, const fs::path& source, const boost::json::value& settings) {
		return component->reimport(tag, source, settings);
	};
	auto menu = std::make_unique<edt::file_menu>(context->dialogs(), context->actions(), hooks);
	menu->register_actions();
	const cmd::result started = import_call(cmd::origin::ui(),
		boost::json::object{ { "path", model.string() }, { "reimport", std::string(prefab().view()) } });
	BOOST_REQUIRE_MESSAGE(started.ok, started.error);
	frames_until([this] { return component->dialog_open(); });
	BOOST_REQUIRE(component->dialog_open());
	BOOST_REQUIRE_EQUAL(import_settings.size(), 1u);
	BOOST_CHECK(import_settings[0] == recorded);
	const edt::asset_export_dialog& dialog = component->export_dialog();
	BOOST_CHECK(dialog.reimport_target() == prefab());
	BOOST_CHECK_EQUAL(std::string(dialog.asset_name()), "robot");
	BOOST_CHECK_EQUAL(std::string(dialog.target_folder()), "assets/robot");
	component->shutdown();
	menu.reset();
}

BOOST_AUTO_TEST_CASE(SettingsGiven_ReplaceOnlyTheirKeys)
{
	write_record();
	make_component();
	BOOST_REQUIRE(!component->reimport(prefab(), model, boost::json::object{ { "tangents", true } }).has_value());
	frames_until([this] { return component->dialog_open(); });
	BOOST_REQUIRE_EQUAL(import_settings.size(), 1u);
	const edt::model_import_settings expected{ .normals = edt::model_normals::flat, .tangents = true };
	BOOST_CHECK(import_settings[0] == expected);
	component->shutdown();
}

BOOST_AUTO_TEST_CASE(AnAgentsCallWithReimport_IsDenied_ANameWithItIsRefused)
{
	write_record();
	make_component();
	edt::file_menu_hooks hooks;
	hooks.import_model = [this](const fs::path& source, const edt::model_import_settings& settings) {
		component->start_import(source, settings);
	};
	hooks.reimport_model = [this](const res::tag& tag, const fs::path& source, const boost::json::value& settings) {
		return component->reimport(tag, source, settings);
	};
	auto menu = std::make_unique<edt::file_menu>(context->dialogs(), context->actions(), hooks);
	menu->register_actions();
	const boost::json::object args{ { "path", model.string() }, { "reimport", std::string(prefab().view()) } };
	for (const cmd::origin& from : { cmd::origin::agent("session"), cmd::origin::test(), cmd::origin::game() }) {
		const cmd::result denied = import_call(from, args);
		BOOST_CHECK(!denied.ok);
		BOOST_CHECK_EQUAL(denied.code, cmd::codes::denied);
	}
	boost::json::object named = args;
	named["name"] = "other";
	BOOST_CHECK_EQUAL(import_call(cmd::origin::ui(), named).code, cmd::codes::invalid_args);
	frames_until([] { return false; });
	BOOST_TEST(imports == 0);
	BOOST_CHECK(!component->person_import_running());
	menu.reset();
}

BOOST_AUTO_TEST_CASE(ABrokenRecord_IsRefused_BeforeAnythingHappens)
{
	make_component();
	write_record("texture");
	BOOST_CHECK_EQUAL(component->check_reimport(prefab()).code, cmd::codes::invalid_args);

	const res::asset_meta meta = write_record();
	dir.write("project/res/assets/robot/robot.prefab.desc.resource", res::write_asset_meta(meta) + "\n");
	BOOST_CHECK_EQUAL(component->check_reimport(prefab()).code, cmd::codes::invalid_args);

	boost::json::object newer = boost::json::parse(res::write_asset_meta(meta)).as_object();
	newer["__version"] = 99;
	dir.write("project/res/assets/robot/robot.prefab.desc.resource", boost::json::serialize(newer));
	const edt::model_import_record refused = component->check_reimport(prefab());
	BOOST_CHECK_EQUAL(refused.code, cmd::codes::invalid_args);
	BOOST_CHECK_MESSAGE(contains(refused.error, "newer editor"), refused.error);

	fs::remove(project_res / "assets" / "robot" / "robot.prefab.desc.resource");
	BOOST_CHECK_EQUAL(component->check_reimport(prefab()).code, cmd::codes::not_found);

	// The window does not open; the person is told why.
	component->ask_reimport_settings(prefab(), model, [](const fs::path&, const edt::model_import_settings&) {});
	BOOST_CHECK(!component->settings_open());
	bool told = false;
	for (const std::string& text : dialog_texts()) told = told || contains(text, "Re-import refused");
	BOOST_CHECK(told);
	BOOST_CHECK(component->reimport(prefab(), model, nullptr).has_value());
	frames_until([] { return false; });
	BOOST_TEST(imports == 0);
}

BOOST_AUTO_TEST_CASE(APrefabOfALowerLayer_OrOpenWithUnsavedChanges_IsRefused)
{
	make_component();
	dir.write("engine_res/assets/robot/robot.prefab.desc", R"({"__type":"prefab_desc"})");
	const edt::model_import_record lower = component->check_reimport(prefab());
	BOOST_CHECK_EQUAL(lower.code, cmd::codes::invalid_args);
	BOOST_CHECK_MESSAGE(contains(lower.error, "layer"), lower.error);

	write_record();
	open_dirty = std::string(prefab().view());
	const std::optional<edt::model_import_refusal> busy = component->reimport(prefab(), model, nullptr);
	BOOST_REQUIRE(busy.has_value());
	BOOST_CHECK_EQUAL(busy->code, cmd::codes::busy);
	dirty_holder = "the level res://levels/main.level.desc";
	const edt::model_import_record under_level = component->check_reimport(prefab());
	BOOST_CHECK_EQUAL(under_level.code, cmd::codes::busy);
	BOOST_CHECK_MESSAGE(contains(under_level.error, dirty_holder)
		&& contains(under_level.error, "save or discard the edits of"), under_level.error);
	dirty_holder.clear();
	open_dirty.clear();
	BOOST_CHECK(component->check_reimport(prefab()).ok());
	frames_until([] { return false; });
	BOOST_TEST(imports == 0);
}

BOOST_AUTO_TEST_CASE(AnOldRecord_IsUnknown_ExportWaitsForTheTick_ThenTheRecordSaysWhatWasMade)
{
	write_record();
	make_component();
	edt::asset_export_dialog& dialog = checked_dialog(model);
	const edt::reimport_file_check* prefab_file = check_of(dialog, prefab());
	BOOST_REQUIRE(prefab_file != nullptr);
	BOOST_CHECK(prefab_file->exists);
	BOOST_CHECK(prefab_file->state == edt::edit_state::unknown);
	BOOST_CHECK(!prefab_file->same_as_new);
	BOOST_CHECK(dialog.needs_overwrite_tick());
	BOOST_CHECK_MESSAGE(contains(dialog.export_blocker(), "Overwrite the files listed above"), dialog.export_blocker());
	BOOST_CHECK(!dialog.start_export());
	BOOST_CHECK_EQUAL(read_project("assets/robot/robot.prefab.desc"), R"({"__type":"prefab_desc"})");

	dialog.set_overwrite_ticked(true);
	BOOST_CHECK(dialog.export_blocker().empty());
	export_now(dialog);
	BOOST_REQUIRE(!component->dialog_open());
	BOOST_CHECK(contains(read_project("assets/robot/robot.prefab.desc"), "\"v\""));
	const edt::model_import_record record = edt::read_model_import_record(project_res, prefab());
	BOOST_REQUIRE_MESSAGE(record.ok(), record.error);
	BOOST_REQUIRE(record.made.has_value());
	BOOST_CHECK(record.made->contains("robot.prefab.desc"));
	BOOST_CHECK(record.made->contains("robot.mat.desc"));
}

BOOST_AUTO_TEST_CASE(AReimport_AddsNoNewInstanceToTheOpenScene)
{
	reimport_over_an_old_record();
	BOOST_CHECK_EQUAL(exported_to_scene, 0);
}

BOOST_AUTO_TEST_CASE(EditsToThePrefabAndAMaterialAfterTheImport_AreChanged_AndExportWaitsForTheTick)
{
	reimport_over_an_old_record();
	dir.write("project/res/assets/robot/robot.prefab.desc", R"({"__type":"prefab_desc","edited":true})");
	dir.write("project/res/assets/robot/robot.mat.desc", R"({"__type":"material_desc","edited":true})");

	edt::asset_export_dialog& dialog = checked_dialog(model);
	for (const res::tag& tag : { prefab(), material() }) {
		const edt::reimport_file_check* file = check_of(dialog, tag);
		BOOST_REQUIRE(file != nullptr);
		BOOST_CHECK(file->state == edt::edit_state::changed);
		BOOST_CHECK(!file->same_as_new);
	}
	BOOST_CHECK(dialog.needs_overwrite_tick());
	BOOST_CHECK(!dialog.start_export());
	BOOST_CHECK(contains(read_project("assets/robot/robot.mat.desc"), "edited"));

	dialog.set_overwrite_ticked(true);
	export_now(dialog);
	BOOST_REQUIRE(!component->dialog_open());
	BOOST_CHECK(!contains(read_project("assets/robot/robot.prefab.desc"), "edited"));
	BOOST_CHECK(!contains(read_project("assets/robot/robot.mat.desc"), "edited"));
}

BOOST_AUTO_TEST_CASE(AFileThatHoldsWhatTheExportWouldWrite_IsNotListed_AndNeedsNoTick)
{
	reimport_over_an_old_record();
	edt::asset_export_dialog& dialog = checked_dialog(model);
	BOOST_REQUIRE(dialog.checked().has_value());
	for (const edt::reimport_file_check& file : dialog.checked()->files) {
		BOOST_CHECK(file.exists);
		BOOST_CHECK(file.same_as_new);
		BOOST_CHECK(file.state == edt::edit_state::unchanged);
	}
	BOOST_CHECK(!dialog.needs_overwrite_tick());
	BOOST_CHECK_MESSAGE(dialog.export_blocker().empty(), dialog.export_blocker());
	component->shutdown();
}

BOOST_AUTO_TEST_CASE(AFileChangedAfterTheCheck_FailsTheExport_AndIsLeftAsItIs)
{
	reimport_over_an_old_record();
	prefab_text = R"({"__type":"prefab_desc","v":3})";
	extra_files = { { ".mat.desc", R"({"__type":"material_desc","v":3})" } };
	edt::asset_export_dialog& dialog = checked_dialog(model);
	BOOST_REQUIRE_MESSAGE(dialog.export_blocker().empty(), dialog.export_blocker());
	dir.write("project/res/assets/robot/robot.mat.desc", "edited after the check");
	export_now(dialog);
	BOOST_REQUIRE(component->dialog_open());
	BOOST_CHECK_MESSAGE(contains(dialog.export_error(), "changed after the dialog looked at it"),
		dialog.export_error());
	BOOST_CHECK_EQUAL(read_project("assets/robot/robot.mat.desc"), "edited after the check");
	BOOST_CHECK(!contains(read_project("assets/robot/robot.prefab.desc"), "\"v\": 3"));
	component->shutdown();
}

BOOST_AUTO_TEST_CASE(AMaterialOpenWithUnsavedChanges_BlocksExport)
{
	reimport_over_an_old_record();
	open_dirty = std::string(material().view());
	edt::asset_export_dialog& dialog = checked_dialog(model);
	BOOST_CHECK_MESSAGE(contains(dialog.export_blocker(), "unsaved changes"), dialog.export_blocker());
	BOOST_CHECK(!dialog.start_export());
	component->shutdown();
}

BOOST_AUTO_TEST_CASE(APickedFileWithAnotherStem_LandsOverThePrefabsNames)
{
	write_record();
	make_component();
	const fs::path other = dir.write("models/robot_v2.glb", "glb-bytes");
	edt::asset_export_dialog& dialog = checked_dialog(other);
	dialog.set_overwrite_ticked(true);
	export_now(dialog);
	BOOST_REQUIRE_MESSAGE(!component->dialog_open(), dialog.export_error());
	BOOST_CHECK(contains(read_project("assets/robot/robot.prefab.desc"), "\"v\""));
	BOOST_CHECK(fs::exists(project_res / "assets" / "robot" / "robot.mat.desc"));
	BOOST_CHECK(!fs::exists(project_res / "assets" / "robot" / "robot_v2.prefab.desc"));
	BOOST_CHECK(!fs::exists(project_res / "assets" / "robot" / "robot_v2.mat.desc"));
}

BOOST_AUTO_TEST_CASE(EscBeforeTheFirstFile_StopsTheReimport_AndTheDialogSaysWhatItLeft)
{
	write_record();
	make_component();
	edt::asset_export_dialog& dialog = checked_dialog(model);
	dialog.set_overwrite_ticked(true);
	BOOST_REQUIRE(dialog.start_export());
	frame_without_jobs(false);
	frame_without_jobs(true);
	frames_until([&dialog] { return !dialog.exporting(); });
	BOOST_REQUIRE(component->dialog_open());
	BOOST_CHECK_MESSAGE(contains(dialog.export_error(), "The re-import was cancelled part way"), dialog.export_error());
	BOOST_CHECK_MESSAGE(contains(dialog.export_error(), "No file was written"), dialog.export_error());
	BOOST_CHECK_EQUAL(read_project("assets/robot/robot.prefab.desc"), R"({"__type":"prefab_desc"})");
	component->shutdown();
}

BOOST_AUTO_TEST_CASE(OnceWritingBegan_AReimportCannotBeCancelled_ANewImportCan)
{
	using dialog = edt::asset_export_dialog;
	BOOST_CHECK(dialog::may_cancel_export(true, false));
	BOOST_CHECK(!dialog::may_cancel_export(true, true));
	BOOST_CHECK(dialog::may_cancel_export(false, true));
	edt::reimport_scan scan;
	scan.files.push_back(
		{ .tag = prefab(), .exists = true, .same_as_new = false, .state = edt::edit_state::unchanged });
	BOOST_CHECK(contains(dialog::reimport_blocker(true, &scan, false), "checking the files"));
	BOOST_CHECK(dialog::reimport_blocker(false, &scan, false).empty());
	scan.files.front().state = edt::edit_state::unknown;
	BOOST_CHECK(!dialog::reimport_blocker(false, &scan, false).empty());
	BOOST_CHECK(dialog::reimport_blocker(false, &scan, true).empty());
	scan.files.front().same_as_new = true;
	BOOST_CHECK(dialog::reimport_blocker(false, &scan, false).empty());
	scan.error = "a file could not be read";
	BOOST_CHECK(!dialog::reimport_blocker(false, &scan, true).empty());
}

// The export job's veto (the one start_export sets) against job_service::cancel, job.cancel's path.
BOOST_AUTO_TEST_CASE(JobCancel_MeetsTheVeto_OnceTheReimportBeganWriting)
{
	make_component();
	const auto progress = std::make_shared<edt::export_progress>();
	ds::lifetime owner{ ds::lifetime_option::cancel_on_destroy };
	edt::job_service& jobs = context->jobs();
	const auto job = jobs.start(ds::lane::io, { "Export robot", "export" }, owner,
		[](ds::task_context&) { return true; }, [](ds::task_result<bool>&) {});
	jobs.set_cancel_veto(job, edt::asset_export_dialog::cancel_veto(progress));
	progress->writing.store(true);
	BOOST_CHECK(jobs.cancel_refusal(job).has_value());
	BOOST_CHECK(!jobs.cancel(job));
	BOOST_CHECK(jobs.is_running(job));
	progress->writing.store(false);
	BOOST_CHECK(jobs.cancel(job));
	frames_until([&jobs, job] { return !jobs.is_running(job); });
}

// A cancel taken before the export began writing can meet work that already ran to its end (here: it wrote nothing,
// every file was as the export would leave it): the outcome is shown as any other, not lost with on_done.
BOOST_AUTO_TEST_CASE(ACancelThatMetFinishedWork_ReportsTheOutcome)
{
	reimport_over_an_old_record();
	edt::asset_export_dialog& dialog = checked_dialog(model);
	BOOST_REQUIRE_MESSAGE(dialog.export_blocker().empty(), dialog.export_blocker());
	BOOST_REQUIRE(dialog.start_export());
	// The work runs; its result waits for the main thread.
	while (tasks.run_one(ds::lane::io)) {}
	frame_without_jobs(false);
	frame_without_jobs(true);
	frames_until([this] { return !component->dialog_open(); });
	BOOST_CHECK(!component->dialog_open());
	BOOST_CHECK_MESSAGE(dialog.export_error().empty(), dialog.export_error());
	BOOST_CHECK_EQUAL(exported_to_scene, 0);
}

BOOST_AUTO_TEST_SUITE_END()
#endif
