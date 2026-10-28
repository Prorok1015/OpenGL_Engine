#include "edt_game_builder.h"
#include "edt_build_diagnostics.h"
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_dialog_service.h>
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

namespace json = boost::json;
namespace fs = std::filesystem;

namespace {
	constexpr const char* k_popup = "Building##edt_build";
	constexpr const char* k_presets = "CMakePresets.json";

	std::string utf8_of(const fs::path& path)
	{
		const std::u8string text = path.u8string();
		return std::string(text.begin(), text.end());
	}

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
		std::string configuration;
		edt::process_runner run;
		std::function<std::variant<edt::toolchain, std::string>(std::stop_token)> find_toolchain;
	};

	edt::game_build_result run_build(const build_input& in, ds::task_context& ctx)
	{
		edt::game_build_result result;
		std::error_code ec;
		if (in.sdk_dir.empty() || !fs::is_regular_file(in.sdk_dir / "snake_sdkConfig.cmake", ec)) {
			result.setup_error = std::format("The engine SDK was not found: no snake_sdkConfig.cmake in '{}' (set build/sdk_dir)",
				utf8_of(in.sdk_dir));
			return result;
		}

		ctx.set_progress(0.f, "Looking for the toolchain");
		auto found = in.find_toolchain(ctx.stop);
		ctx.throw_if_stopped();
		if (auto* error = std::get_if<std::string>(&found)) {
			result.setup_error = std::move(*error);
			return result;
		}
		const edt::toolchain& tools = std::get<edt::toolchain>(found);

		platform::process_options options;
		options.working_dir = in.root / "code";
		options.environment = tools.environment;
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
			const auto outcome = in.run(tools.cmake, args, options, on_line, ctx.stop);
			if (outcome.cancelled) {
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

		ctx.set_progress(0.f, "Configuring");
		if (step("configure", { "--preset", in.configuration })) {
			ctx.set_progress(0.f, "Building");
			step("build", { "--build", "--preset", in.configuration });
		}

		result.problems = parser.take();
		result.tail.assign(tail.begin(), tail.end());
		for (const auto& module : in.modules) {
			result.modules.push_back(in.root / "build" / "bin" / edt::module_file_name(module));
		}
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

std::string edt::module_file_name(std::string_view module)
{
#ifdef _WIN32
	return std::format("{}.dll", module);
#else
	return std::format("lib{}.so", module);
#endif
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

	m_action = actions.add({ .id = k_action_id, .label = "Build Game", .menu_path = "Build", .shortcut = "Ctrl+B",
		.disabled_tooltip = std::move(tooltip),
		.execute = [this](const json::value& args) {
			if (is_building()) return action_result::failure("busy", "A build is already running");
			if (const std::string reason = disabled_reason(); !reason.empty()) return action_result::failure("disabled", reason);
			const game_project project = *m_options.project();

			build_input input;
			input.root = project.root;
			input.modules = project.modules;
			input.sdk_dir = m_options.sdk_dir;
			input.configuration = m_options.default_configuration;
			if (const json::value* chosen = args.as_object().if_contains("configuration")) input.configuration = std::string(chosen->as_string());
			input.run = m_options.run;
			input.find_toolchain = m_options.find_toolchain;
			m_configuration = input.configuration;

			m_problems.clear("build");
			egLOG_INFO("editor/build", "building the game module ({}) in '{}'", input.configuration, utf8_of(project.root / "code"));
			m_job = m_jobs.start(ds::lane::io, { "Build game", "build" }, m_lifetime,
				[input = std::move(input)](ds::task_context& ctx) { return run_build(input, ctx); },
				[this](ds::task_result<game_build_result>& result) {
					if (!result.ok()) {
						egLOG_WARN("editor/build", "the build job failed: {}", result.error);
						return;
					}
					finish(*result.value);
				});
			if (m_options.show_progress) {
				m_spinner_t = 0.f;
				m_options.show_progress("edt/build_game", [this, alive = std::weak_ptr<int>(m_alive)] {
					return !alive.expired() && draw_progress();
				});
			}
			return action_result::success(json::object{ { "started", true }, { "job", m_job } });
		},
		.args_schema = args_schema(),
		.result_schema = result_schema(),
		.expose_as_tool = true,
		.mutates = true,
		.activate = [this, &actions] {
			const auto outcome = actions.invoke(k_action_id, json::object{});
			if (!outcome.ok) m_dialogs.message(message_level::warning, std::format("The game was not built: {}", outcome.error));
		},
		.enabled = [this] { return disabled_reason().empty(); } });
}

edt::game_builder::~game_builder() = default;

bool edt::game_builder::is_building() const
{
	return m_job != 0 && m_jobs.is_running(m_job);
}

void edt::game_builder::cancel()
{
	if (is_building()) m_jobs.cancel(m_job);
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

void edt::game_builder::finish(game_build_result& result)
{
	if (!result.tail.empty()) {
		std::string text;
		for (const auto& line : result.tail) {
			text += '\n';
			text += line;
		}
		egLOG_DEBUG("editor/build", "build output (last {} lines):{}", result.tail.size(), text);
	}

	if (!result.setup_error.empty()) {
		egLOG_WARN("editor/build", "the game module was not built: {}", result.setup_error);
		m_dialogs.message(message_level::error, result.setup_error);
		return;
	}

	const std::size_t errors = static_cast<std::size_t>(std::count_if(result.problems.begin(), result.problems.end(),
		[](const problem& item) { return item.severity == problem_severity::error; }));
	m_problems.set("build", std::move(result.problems));

	if (result.ok()) {
		std::string built;
		for (const auto& module : result.modules) {
			std::error_code ec;
			if (!fs::exists(module, ec)) continue;
			built += built.empty() ? "" : ", ";
			built += utf8_of(module);
		}
		egLOG_INFO("editor/build", "game module built: {}", built.empty() ? std::string{ "(no module file of the manifest found in build/bin)" } : built);
		return;
	}

	std::string summary;
	if (!result.process_error.empty()) {
		summary = std::format("The game build failed: CMake could not be started ({})", result.process_error);
	} else if (errors > 0) {
		summary = std::format("The game build failed with {} error{} (see Problems)", errors, errors == 1 ? "" : "s");
	} else {
		summary = std::format("The game build failed: the {} step exited with code {} (see the log)", result.failed_step, result.exit_code);
	}
	egLOG_WARN("editor/build", "{}", summary);
	m_dialogs.message(message_level::error, std::move(summary));
}

bool edt::game_builder::draw_progress()
{
	if (!is_building()) {
		if (ImGui::BeginPopupModal(k_popup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
		return false;
	}

	ImGui::OpenPopup(k_popup);
	if (ImGui::BeginPopupModal(k_popup, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar)) {
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
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
	return true;
}
