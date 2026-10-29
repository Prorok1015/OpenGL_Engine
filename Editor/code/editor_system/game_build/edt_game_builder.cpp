#include "edt_game_builder.h"
#include "edt_build_diagnostics.h"
#include "edt_path_text.h"
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_dialog_service.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_job_service.h>
#include <snk/common/logger/engine_log.h>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <imgui.h>
#include <algorithm>
#include <charconv>
#include <cstddef>
#include <deque>
#include <format>
#include <system_error>
#include <utility>

namespace json = boost::json;
namespace fs = std::filesystem;
using edt::utf8_of;

namespace {
	constexpr const char* k_window = "Building the game##edt_build";
	constexpr const char* k_presets = "CMakePresets.json";

	json::value args_schema()
	{
		return json::parse(R"({"type":"object","properties":{"configuration":{"type":"string","enum":["debuggame","release"]}},"additionalProperties":false})");
	}

	json::value result_schema()
	{
		return json::parse(R"({"type":"object","properties":{"started":{"type":"boolean"},"job":{"type":"integer"}},"required":["started","job"],"additionalProperties":false})");
	}

	struct build_input
	{
		fs::path root;
		std::vector<std::string> modules;
		fs::path sdk_dir;
		edt::build_request request;
		edt::process_runner run;
		std::function<std::variant<edt::toolchain, std::string>(std::stop_token)> find_toolchain;
		std::shared_ptr<std::stop_source> restart;
	};

	edt::game_build_result restarted_result()
	{
		egLOG_DEBUG("editor/build", "build stopped for a newer request");
		edt::game_build_result result;
		result.restarted = true;
		return result;
	}

	std::vector<edt::game_build_result::module_file> find_modules(const fs::path& root, const std::vector<std::string>& modules)
	{
		std::error_code ec;
		fs::path dir = fs::absolute(root / "build" / "bin", ec);
		if (ec) dir = root / "build" / "bin";
		std::vector<edt::game_build_result::module_file> out;
		for (const auto& module : modules) {
			const auto names = edt::module_file_names(module);
			edt::game_build_result::module_file file{ module, dir / names.front(), false };
			for (const auto& name : names) {
				if (fs::exists(dir / name, ec)) {
					file.path = dir / name;
					file.exists = true;
					break;
				}
			}
			out.push_back(std::move(file));
		}
		return out;
	}

	edt::game_build_result run_build(const build_input& in, ds::task_context& ctx)
	{
		// The steps see one token: the job's cancel and the builder's restart both stop them.
		std::stop_source stop_source = *in.restart;
		const std::stop_callback forward_cancel(ctx.stop, [stop_source]() mutable { stop_source.request_stop(); });
		const std::stop_token stop = stop_source.get_token();
		if (stop.stop_requested()) {
			ctx.throw_if_stopped();
			return restarted_result();
		}

		edt::game_build_result result;
		std::error_code ec;
		if (in.sdk_dir.empty() || !fs::is_regular_file(in.sdk_dir / "snake_sdkConfig.cmake", ec)) {
			result.setup_error = edt::sdk_not_found_message(in.sdk_dir);
			return result;
		}

		ctx.set_progress(0.f, "Looking for the toolchain");
		auto found = in.find_toolchain(stop);
		ctx.throw_if_stopped();
		if (stop.stop_requested()) return restarted_result();
		if (auto* error = std::get_if<std::string>(&found)) {
			result.setup_error = std::move(*error);
			return result;
		}
		const edt::toolchain& tools = std::get<edt::toolchain>(found);

		platform::process_options options;
		options.working_dir = in.root / "code";
		options.environment = tools.environment;
		options.low_priority = in.request.priority == edt::build_priority::low;
		edt::set_variable(*options.environment, "SNAKE_SDK_DIR", utf8_of(in.sdk_dir));

		edt::build_output_parser parser;
		std::deque<std::string> tail;
		const auto on_line = [&](std::string_view line) {
			parser.feed(line);
			tail.emplace_back(line);
			if (tail.size() > edt::game_builder::k_tail_lines) tail.pop_front();
			if (const auto step = edt::parse_ninja_progress(line); step && step->second > 0) {
				ctx.set_progress(static_cast<float>(step->first) / static_cast<float>(step->second),
					std::format("Building {}/{}", step->first, step->second));
			}
		};

		const auto step = [&](const char* name, std::vector<std::string> args) {
			const auto outcome = in.run(tools.cmake, args, options, on_line, stop);
			if (outcome.cancelled) {
				if (!ctx.stop.stop_requested()) {
					result.restarted = true;
					return false;
				}
				egLOG_INFO("editor/build", "build cancelled");
				throw ds::task_cancelled{};
			}
			if (!outcome.error.empty() || outcome.exit_code != 0) {
				result.failed_step = name;
				result.exit_code = outcome.exit_code;
				result.process_error = outcome.error;
				return false;
			}
			return true;
		};

		const std::string& preset = in.request.preset.empty() ? in.request.configuration : in.request.preset;
		std::vector<std::string> build_args{ "--build", "--preset", preset };
		if (in.request.jobs > 0) {
			build_args.push_back("--parallel");
			build_args.push_back(std::to_string(in.request.jobs));
		}
		if (in.request.verbosity == edt::log_verbosity::verbose) build_args.push_back("--verbose");

		std::vector<std::string> configure_args{ "--preset", preset };
		if (in.request.fresh) configure_args.push_back("--fresh");

		ctx.set_progress(0.f, "Configuring");
		if (step("configure", std::move(configure_args))) {
			result.configured = true;
			ctx.set_progress(0.f, "Building");
			step("build", std::move(build_args));
		}
		if (result.restarted) return restarted_result();

		result.problems = parser.take();
		result.tail.assign(tail.begin(), tail.end());
		result.modules = find_modules(in.root, in.modules);
		return result;
	}
}

std::optional<std::pair<int, int>> edt::parse_ninja_progress(std::string_view line)
{
	if (line.size() < 5 || line.front() != '[') return std::nullopt;
	const std::size_t slash = line.find('/');
	const std::size_t close = line.find(']');
	if (slash == std::string_view::npos || close == std::string_view::npos || slash > close) return std::nullopt;
	int done = 0;
	int total = 0;
	const char* first = line.data() + 1;
	const char* middle = line.data() + slash;
	const char* last = line.data() + close;
	const auto a = std::from_chars(first, middle, done);
	const auto b = std::from_chars(middle + 1, last, total);
	if (a.ec != std::errc{} || a.ptr != middle || b.ec != std::errc{} || b.ptr != last) return std::nullopt;
	if (done < 0 || total <= 0 || done > total) return std::nullopt;
	return std::pair{ done, total };
}

std::string edt::sdk_not_found_message(const fs::path& tried)
{
	constexpr std::string_view order = "looked up in order: cfg build/sdk_dir, <editor>/../lib/cmake/snake_sdk, "
		"the engine build tree's snake_sdk";
	if (tried.empty()) return std::format("The engine SDK (snake_sdkConfig.cmake) was not found; {}", order);
	return std::format("The engine SDK was not found: no snake_sdkConfig.cmake in '{}'; {}", utf8_of(tried), order);
}

std::string edt::module_file_name(std::string_view module)
{
#ifdef _WIN32
	return std::format("{}.dll", module);
#else
	return std::format("lib{}.so", module);
#endif
}

std::vector<std::string> edt::module_file_names(std::string_view module)
{
#ifdef _WIN32
	return { module_file_name(module) };
#else
	return { module_file_name(module), std::format("{}.so", module) };
#endif
}

edt::build_request edt::make_manual_build_request(const build_settings& settings, std::string configuration,
	const game_builder_options& options)
{
	build_request out;
	out.configuration = !configuration.empty() ? std::move(configuration) : settings.configuration.value_or(options.default_configuration);
	out.preset = options.preset_for ? options.preset_for(out.configuration) : out.configuration;
	if (settings.jobs_mode == jobs_mode::auto_jobs && options.auto_jobs) {
		out.jobs = std::max(1, options.auto_jobs());
	}
	out.priority = build_priority::normal;
	out.cancel_on_restart = false;
	out.verbosity = settings.log_verbosity;
	out.reload_after = settings.reload_after_build;
	out.fresh = std::find(settings.fresh_configure.begin(), settings.fresh_configure.end(), out.configuration)
		!= settings.fresh_configure.end();
	out.fresh_generation = settings.fresh_generation;
	return out;
}

edt::build_request edt::make_live_build_request(const build_settings& settings, const game_builder_options& options)
{
	build_request out = make_manual_build_request(settings, {}, options);
	if (settings.live_jobs > 0) {
		out.jobs = settings.live_jobs;
	} else {
		// Few jobs: a live build runs while the person works in the editor.
		out.jobs = std::max(1, std::min(2, options.auto_jobs ? options.auto_jobs() : 2));
	}
	out.priority = build_priority::low;
	out.cancel_on_restart = true;
	out.report = build_report::problems_only;
	out.show_progress = false;
	out.reload_after = false;
	// The person's build goes first (owner decision, #445).
	out.preemptible = true;
	return out;
}

edt::game_builder::game_builder(job_service& jobs, problem_list& problems, dialog_service& dialogs, action_registry& actions,
	game_builder_options options)
	: m_jobs(jobs)
	, m_problems(problems)
	, m_dialogs(dialogs)
	, m_options(std::move(options))
{
	if (m_options.default_configuration != "debuggame" && m_options.default_configuration != "release") {
		egLOG_WARN("editor/build", "build/configuration '{}' is not a preset of the template (debuggame, release): debuggame is used",
			m_options.default_configuration);
		m_options.default_configuration = "debuggame";
	}
	if (!m_options.settings) m_options.settings = [] { return build_settings{}; };
	if (!m_options.find_toolchain) {
		m_options.find_toolchain = [cache = m_toolchains, settings = m_options.toolchain, run = m_options.run](std::stop_token stop) {
			return cache->get(settings, run, stop);
		};
	}

	// A project is opened by restarting the editor, so only the presets can change while it runs.
	std::string tooltip = "The project has no code/CMakePresets.json";
	if (!m_options.engine_shared) {
		tooltip = "Game modules need the shared engine build (SNK_ENGINE_SHARED)";
	} else if (!m_options.project || !m_options.project()) {
		tooltip = "Open a project first";
	}

	cmd::command_desc command;
	command.id = k_action_id;
	command.summary = "Build Game";
	command.category = cmd::agent_category::build;
	command.args_schema = args_schema();
	command.result_schema = result_schema();
	command.expose_as_tool = true;
	command.mutates = true;
	// It runs the project's CMake code, i.e. anything: an agent never gets it without the person's approval
	// (edt::agent_may_use refuses any risk; the highest level, for when approvals exist).
	command.risk = cmd::danger::deny_by_default;
	command.enabled = [this] { return disabled_reason().empty(); };
	command.invoke = [this](const cmd::call& request) {
		std::string configuration;
		if (const json::value* chosen = request.args.as_object().if_contains("configuration")) {
			configuration = std::string(chosen->as_string());
		}
		return start(make_manual_build_request(m_options.settings(), std::move(configuration), m_options));
	};

	action_ui ui;
	ui.label = "Build Game";
	ui.menu_path = "Build";
	ui.shortcut = "Ctrl+B";
	ui.disabled_tooltip = std::move(tooltip);
	ui.activate = [this, &actions] {
		const auto outcome = actions.invoke(k_action_id, json::object{});
		if (!outcome.ok) m_dialogs.message(message_level::warning, std::format("The game was not built: {}", outcome.error));
	};
	m_action = actions.add(std::move(command), std::move(ui));
}

edt::game_builder::~game_builder() = default;

bool edt::game_builder::is_building() const
{
	return m_job != 0 && m_jobs.is_running(m_job);
}

edt::action_result edt::game_builder::start(build_request request)
{
	if (request.configuration.empty()) return action_result::failure("invalid_args", "The build request has no configuration");
	if (is_building()) {
		// The person's build runs, or waits for the live one to stop.
		const bool manual_ahead = !m_running_preemptible || (m_pending && !m_pending->preemptible);
		const bool preempts = !request.preemptible && !manual_ahead;
		// A background request never stops, nor replaces, the person's build.
		const bool allowed = request.preemptible ? request.cancel_on_restart && !manual_ahead
			: request.cancel_on_restart || preempts;
		if (!allowed) return action_result::failure("busy", "A build is already running");
		if (preempts && !m_preempted) {
			egLOG_DEBUG("editor/build", "the live build stops for the build of {}", request.configuration);
			m_preempted = true;
		}
		std::optional<build_request> replaced = std::exchange(m_pending, std::move(request));
		if (replaced) {
			egLOG_WARN("editor/build", "a waiting build request ({}) was replaced by a newer one", replaced->configuration);
			// It never runs: its owner hears so now, not after the running build.
			if (replaced->on_finished) {
				build_outcome dropped;
				dropped.kind = preempts ? build_outcome::preempted : build_outcome::restarted;
				replaced->on_finished(dropped);
			}
		}
		// Not job_service::cancel: that skips on_done, which starts the waiting request. So the job ends as `done`.
		if (m_restart_stop) m_restart_stop->request_stop();
		return action_result::success(json::object{ { "started", false }, { "job", 0 } });
	}
	if (const std::string reason = disabled_reason(); !reason.empty()) return action_result::failure("disabled", reason);
	launch(std::move(request));
	return action_result::success(json::object{ { "started", true }, { "job", m_job } });
}

void edt::game_builder::launch(build_request request)
{
	// Only a request that meets a running build waits; one left by a job cancelled from outside is stale.
	if (m_pending) {
		egLOG_WARN("editor/build", "a waiting build request ({}) was dropped: its build was cancelled", m_pending->configuration);
		m_pending.reset();
	}
	if (request.preset.empty()) request.preset = request.configuration;
	const game_project project = *m_options.project();

	build_input input;
	input.root = project.root;
	input.modules = project.modules;
	input.sdk_dir = m_options.sdk_dir;
	input.request = request;
	input.run = m_options.run;
	input.find_toolchain = m_options.find_toolchain;
	m_restart_stop = std::make_shared<std::stop_source>();
	input.restart = m_restart_stop;
	m_configuration = request.configuration;
	m_running_preemptible = request.preemptible;
	m_preempted = false;

	// A quiet build replaces them when it ends: a background build restarted per keystroke must not blank the list.
	if (request.report != build_report::problems_only) m_problems.clear("build");
	const std::string started = std::format("building the game module ({}, preset '{}', {}{}) in '{}'", request.configuration,
		request.preset, request.jobs > 0 ? std::format("{} jobs", request.jobs) : std::string{ "jobs of the preset" },
		request.fresh ? ", fresh configure" : "", utf8_of(project.root / "code"));
	if (request.report == build_report::problems_only) {
		egLOG_DEBUG("editor/build", "{}", started);
	} else {
		egLOG_INFO("editor/build", "{}", started);
	}
	m_job = m_jobs.start(ds::lane::io, { "Build game", "build" }, m_lifetime,
		[input = std::move(input)](ds::task_context& ctx) { return run_build(input, ctx); },
		[this, request](ds::task_result<game_build_result>& result) {
			build_outcome outcome;
			if (!result.ok()) {
				egLOG_WARN("editor/build", "the build job failed: {}", result.error);
				outcome.message = std::format("The build job failed: {}", result.error);
				outcome.job = m_job;
			} else {
				if (request.fresh && result.value->configured && m_options.on_fresh_configured) {
					m_options.on_fresh_configured(request.configuration, request.fresh_generation);
				}
				outcome = finish(*result.value, request);
			}
			if (request.on_finished) request.on_finished(outcome);
			if (m_pending) {
				build_request next = std::move(*m_pending);
				m_pending.reset();
				const auto report = next.on_finished;
				if (const auto started = start(std::move(next)); !started.ok) {
					egLOG_WARN("editor/build", "the waiting build did not start: {}", started.error);
					// Its owner would otherwise see no build and no outcome, as after a cancel.
					if (report) {
						build_outcome refused;
						refused.not_started = true;
						refused.message = started.error;
						report(refused);
					}
				}
			}
		});
	if (m_options.show_progress && request.show_progress && !m_progress_shown) {
		m_progress_shown = true;
		m_spinner_t = 0.f;
		m_options.show_progress("edt/build_game", [this, alive = std::weak_ptr<int>(m_alive)] {
			return !alive.expired() && draw_progress();
		});
	}
}

void edt::game_builder::cancel()
{
	if (m_pending) {
		egLOG_WARN("editor/build", "a waiting build request ({}) was dropped: the build was cancelled", m_pending->configuration);
		m_pending.reset();
	}
	if (is_building()) m_jobs.cancel(m_job);
}

void edt::game_builder::stop_background()
{
	if (!building_preemptible()) return;
	if (m_pending && m_pending->preemptible) {
		build_request dropped = std::move(*m_pending);
		m_pending.reset();
		if (dropped.on_finished) {
			build_outcome outcome;
			outcome.kind = build_outcome::preempted;
			dropped.on_finished(outcome);
		}
	}
	if (!m_preempted) egLOG_DEBUG("editor/build", "the live build is stopped by the editor");
	m_preempted = true;
	// Not job_service::cancel: that skips on_done, so neither the outcome nor a waiting manual request would follow.
	if (m_restart_stop) m_restart_stop->request_stop();
}

bool edt::game_builder::has_presets() const
{
	const auto now = std::chrono::steady_clock::now();
	if (m_presets_checked == std::chrono::steady_clock::time_point{} || now - m_presets_checked >= std::chrono::seconds(1)) {
		m_presets_checked = now;
		const auto project = m_options.project ? m_options.project() : std::nullopt;
		std::error_code ec;
		m_presets_found = project && fs::is_regular_file(project->root / "code" / k_presets, ec);
	}
	return m_presets_found;
}

std::string edt::game_builder::disabled_reason() const
{
	if (!m_options.engine_shared) return "Game modules need the shared engine build (SNK_ENGINE_SHARED)";
	if (!m_options.project || !m_options.project()) return "Open a project first";
	if (!has_presets()) return "The project has no code/CMakePresets.json";
	return {};
}

edt::build_outcome edt::game_builder::finish(game_build_result& result, const build_request& request)
{
	build_outcome outcome;
	outcome.job = m_job;
	if (result.restarted) {
		outcome.kind = m_preempted ? build_outcome::preempted : build_outcome::restarted;
		return outcome;
	}
	const bool quiet = request.report == build_report::problems_only;
	if (!result.tail.empty() && (!result.ok() || request.verbosity != log_verbosity::errors)) {
		std::string text;
		for (const auto& line : result.tail) {
			text += '\n';
			text += line;
		}
		// A failure's message points at the log, so its output must not hide below the default level; a problems_only
		// build has no message and fails often while the person types.
		if (!result.ok() && quiet) {
			egLOG_DEBUG("editor/build", "build output (last {} lines):{}", result.tail.size(), text);
		} else if (!result.ok()) {
			egLOG_WARN("editor/build", "build output (last {} lines):{}", result.tail.size(), text);
		} else if (request.verbosity == log_verbosity::verbose) {
			egLOG_INFO("editor/build", "build output (last {} lines):{}", result.tail.size(), text);
		} else {
			egLOG_DEBUG("editor/build", "build output (last {} lines):{}", result.tail.size(), text);
		}
	}

	if (!result.setup_error.empty()) {
		egLOG_WARN("editor/build", "the game module was not built: {}", result.setup_error);
		if (!quiet) m_dialogs.message(message_level::error, result.setup_error);
		outcome.message = result.setup_error;
		return outcome;
	}

	const std::size_t errors = static_cast<std::size_t>(std::count_if(result.problems.begin(), result.problems.end(),
		[](const problem& item) { return item.severity == problem_severity::error; }));
	outcome.errors = static_cast<int>(errors);
	outcome.warnings = static_cast<int>(std::count_if(result.problems.begin(), result.problems.end(),
		[](const problem& item) { return item.severity == problem_severity::warning; }));
	m_problems.set("build", std::move(result.problems));

	if (result.ok()) {
		std::string built;
		for (const auto& module : result.modules) {
			if (!module.exists) continue;
			built += built.empty() ? "" : ", ";
			built += utf8_of(module.path);
		}
		if (built.empty()) built = "(no module file of the manifest found in build/bin)";
		if (quiet) {
			egLOG_DEBUG("editor/build", "game module built: {}", built);
		} else {
			egLOG_INFO("editor/build", "game module built: {}", built);
		}
		outcome.kind = build_outcome::succeeded;
		for (const auto& module : result.modules) {
			if (!module.exists) continue;
			outcome.modules.push_back(
				game_module_build_result{ module.name, module.path, request.configuration, outcome.job, request.reload_after });
		}
		if (m_options.events) {
			// A slot may start the next build.
			for (const auto& file : outcome.modules) m_options.events->game_module_built.emit(file);
		}
		return outcome;
	}

	std::string summary;
	if (!result.process_error.empty()) {
		summary = std::format("The game build failed: CMake could not be started ({})", result.process_error);
	} else if (errors > 0) {
		summary = std::format("The game build failed with {} error{} (see Problems)", errors, errors == 1 ? "" : "s");
	} else {
		summary = std::format("The game build failed: the {} step exited with code {} (see the log)", result.failed_step, result.exit_code);
	}
	// Compile errors of a quiet build are in Problems already; a failure without them has only this line.
	if (quiet && errors > 0 && result.process_error.empty()) {
		egLOG_DEBUG("editor/build", "{}", summary);
	} else {
		egLOG_WARN("editor/build", "{}", summary);
	}
	if (!quiet) m_dialogs.message(message_level::error, summary);
	outcome.message = std::move(summary);
	return outcome;
}

bool edt::game_builder::draw_progress()
{
	if (!is_building()) {
		m_progress_shown = false;
		return false;
	}
	// A window, not a modal: a build takes minutes and the editor stays usable meanwhile (#434).
	if (ImGui::Begin(k_window, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse
			| ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar)) {
		const bool escape = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
		m_spinner_t += ImGui::GetIO().DeltaTime;
		const char* spinners[] = { "|", "/", "-", "\\" };
		const int idx = static_cast<int>(m_spinner_t * 8.f) % 4;
		ImGui::Text("%s  Building the game (%s)...", spinners[idx], m_configuration.c_str());
		if (const auto progress = m_jobs.progress(m_job)) {
			ImGui::ProgressBar(progress->fraction, ImVec2(360.f, 0.f), progress->note.c_str());
		}
		if (ImGui::Button("Cancel") || escape) {
			cancel();
		}
	}
	ImGui::End();
	return true;
}
