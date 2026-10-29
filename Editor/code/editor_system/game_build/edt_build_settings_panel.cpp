#include "edt_build_settings_panel.h"
#include "edt_path_text.h"
#include <imgui.h>
#include "misc/cpp/imgui_stdlib.h"
#include <array>
#include <cmath>
#include <format>
#include <utility>

namespace fs = std::filesystem;

namespace {
	const ImVec4 k_error_color{ 1.f, 0.3f, 0.3f, 1.f };
	const ImVec4 k_warning_color{ 1.f, 0.8f, 0.f, 1.f };

	// What build.json's `configuration` accepts (parse_build_settings).
	bool is_build_configuration(std::string_view name)
	{
		return name == "debuggame" || name == "release";
	}

	std::string display_name_of(const edt::build_settings_model& model, std::string_view configuration)
	{
		const edt::build_settings_configuration* entry = model.find(configuration);
		return entry ? entry->display_name : std::string(configuration);
	}
}

edt::build_settings_panel::build_settings_panel(std::function<std::optional<fs::path>()> project_root,
	std::string default_configuration, std::function<bool()> building)
	: panel_base(k_id, k_title)
	, m_project_root(std::move(project_root))
{
	m_model.set_default_configuration(std::move(default_configuration));
	m_model.set_building(std::move(building));
}

void edt::build_settings_panel::reload()
{
	if (!m_estimated) {
		m_model.set_estimate(estimate_jobs());
		m_estimated = true;
	}
	const auto root = m_project_root ? m_project_root() : std::nullopt;
	m_model.load(root.value_or(fs::path{}));
	m_value_edits.clear();
	m_define_error.clear();
	m_changed.clear();
	m_changed_checked = std::chrono::steady_clock::now();
}

void edt::build_settings_panel::on_before_begin()
{
	if (m_reload_on_open) {
		m_reload_on_open = false;
		// Closing the window keeps the edits it had not applied.
		if (!m_model.dirty()) reload();
	}
	ImGui::SetNextWindowSize(ImVec2(620.f, 560.f), ImGuiCond_FirstUseEver);
	if (m_focus) {
		ImGui::SetNextWindowFocus();
		m_focus = false;
	}
}

void edt::build_settings_panel::on_hidden()
{
	if (!is_open()) m_reload_on_open = true;
}

std::string edt::build_settings_panel::relative(const fs::path& file) const
{
	const std::u8string text = file.lexically_relative(m_model.root()).generic_u8string();
	return std::string(text.begin(), text.end());
}

void edt::build_settings_panel::on_render()
{
	if (!m_model.available()) {
		ImGui::TextDisabled("%s", m_model.unavailable_reason().c_str());
		if (ImGui::Button("Reload")) reload();
		return;
	}
	ImGui::TextUnformatted("Project code:");
	ImGui::SameLine();
	ImGui::TextDisabled("%s", utf8_of(m_model.code_dir()).c_str());
	for (const auto& error : m_model.load_errors()) {
		ImGui::PushTextWrapPos(0.f);
		ImGui::TextColored(k_warning_color, "%s", error.c_str());
		ImGui::PopTextWrapPos();
	}

	const auto now = std::chrono::steady_clock::now();
	if (now - m_changed_checked >= std::chrono::seconds(1)) {
		m_changed_checked = now;
		m_model.refresh_fresh_configure();
		m_changed = m_model.changed_on_disk();
	}

	draw_presets();
	draw_module();
	draw_footer();
}

void edt::build_settings_panel::draw_presets()
{
	if (!ImGui::CollapsingHeader("Presets", ImGuiTreeNodeFlags_DefaultOpen)) return;
	ImGui::TextDisabled("%s", relative(m_model.user_presets_file()).c_str());

	const build_settings& settings = m_model.settings();
	const std::string selected = m_model.selected_configuration();
	const std::string preview = settings.configuration ? display_name_of(m_model, *settings.configuration)
		: std::format("{} (editor default)", display_name_of(m_model, selected));
	ImGui::SetNextItemWidth(240.f);
	if (ImGui::BeginCombo("Default configuration", preview.c_str())) {
		if (ImGui::Selectable("Editor default", !settings.configuration)) m_model.set_configuration(std::nullopt);
		for (const auto& entry : m_model.configurations()) {
			if (!is_build_configuration(entry.name)) continue;
			const bool chosen = settings.configuration == entry.name;
			if (ImGui::Selectable(entry.display_name.c_str(), chosen)) m_model.set_configuration(entry.name);
		}
		ImGui::EndCombo();
	}

	const build_settings_configuration* current = m_model.find(selected);
	ImGui::TextUnformatted("Generator:");
	ImGui::SameLine();
	ImGui::TextDisabled("%s", current && !current->generator.empty() ? current->generator.c_str() : "(CMake's default)");

	const jobs_estimate& estimate = m_model.estimate();
	const long long gib = std::llround(static_cast<double>(estimate.ram) / static_cast<double>(k_gib));
	const std::string auto_label = std::format("Auto: {}  ({} cores, {} GiB, {})", estimate.jobs, estimate.cores, gib,
		estimate.compiler == compiler_family::msvc ? "MSVC" : "GCC/Clang");
	ImGui::TextUnformatted("Jobs:");
	ImGui::SameLine();
	if (ImGui::RadioButton(auto_label.c_str(), settings.jobs_mode == jobs_mode::auto_jobs)) {
		m_model.set_jobs_mode(jobs_mode::auto_jobs);
	}
	ImGui::SameLine();
	if (ImGui::RadioButton("Manual", settings.jobs_mode == jobs_mode::manual_jobs)) {
		m_model.set_jobs_mode(jobs_mode::manual_jobs);
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(settings.jobs_mode != jobs_mode::manual_jobs);
	int jobs = m_model.manual_jobs();
	ImGui::SetNextItemWidth(110.f);
	if (ImGui::InputInt("##manual_jobs", &jobs)) m_model.set_manual_jobs(jobs);
	ImGui::EndDisabled();

	draw_defines();
}

void edt::build_settings_panel::draw_defines()
{
	ImGui::Spacing();
	ImGui::TextUnformatted("Defines");
	const auto& configurations = m_model.configurations();
	if (configurations.empty()) {
		ImGui::TextDisabled("CMakePresets.json has no configure presets");
		return;
	}
	if (!m_model.find(m_define_configuration)) m_define_configuration = m_model.find(m_model.selected_configuration())
		? m_model.selected_configuration() : configurations.front().name;

	if (ImGui::BeginTabBar("##define_configurations")) {
		for (const auto& entry : configurations) {
			const std::string label = entry.display_name + "##" + entry.name;
			if (ImGui::BeginTabItem(label.c_str())) {
				if (m_define_configuration != entry.name) {
					m_define_configuration = entry.name;
					m_value_edits.clear();
				}
				ImGui::EndTabItem();
			}
		}
		ImGui::EndTabBar();
	}

	const build_settings_configuration* shown = m_model.find(m_define_configuration);
	if (!shown) return;
	// Copies: an edit below changes the model's maps.
	const auto team = shown->team_defines;
	const auto user = shown->user_defines;
	std::optional<std::string> removed;
	const bool building = m_model.is_building();

	constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp;
	if (ImGui::BeginTable("##defines", 3, flags)) {
		ImGui::TableSetupColumn("Name");
		ImGui::TableSetupColumn("Value");
		ImGui::TableSetupColumn("##remove", ImGuiTableColumnFlags_WidthFixed, 24.f);
		ImGui::TableHeadersRow();
		for (const auto& [name, value] : team) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextDisabled("%s", name.c_str());
			const bool name_hovered = ImGui::IsItemHovered();
			ImGui::TableSetColumnIndex(1);
			ImGui::TextDisabled("%s", value.c_str());
			if (name_hovered || ImGui::IsItemHovered()) ImGui::SetTooltip("from CMakePresets.json (team, read-only)");
		}
		for (const auto& [name, value] : user) {
			ImGui::PushID(name.c_str());
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(name.c_str());
			ImGui::TableSetColumnIndex(1);
			std::string& text = m_value_edits.try_emplace(name, value).first->second;
			ImGui::SetNextItemWidth(-1.f);
			ImGui::InputText("##value", &text);
			if (ImGui::IsItemDeactivatedAfterEdit()) {
				if (text != value) {
					if (auto problem = m_model.set_define(m_define_configuration, name, text)) {
						m_define_error = std::move(*problem);
						text = value;
					} else {
						m_define_error.clear();
					}
				}
			} else if (!ImGui::IsItemActive()) {
				text = value;
			}
			ImGui::TableSetColumnIndex(2);
			ImGui::BeginDisabled(building);
			if (ImGui::SmallButton("x")) removed = name;
			ImGui::EndDisabled();
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
				ImGui::SetTooltip("%s", building ? k_building_reason.data() : "Remove (the next build configures from scratch)");
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	if (removed) {
		if (auto problem = m_model.remove_define(m_define_configuration, *removed)) {
			m_define_error = std::move(*problem);
		} else {
			m_value_edits.erase(*removed);
		}
	}

	ImGui::SetNextItemWidth(180.f);
	ImGui::InputTextWithHint("##new_define_name", "NAME", &m_new_name);
	ImGui::SameLine();
	ImGui::SetNextItemWidth(180.f);
	ImGui::InputTextWithHint("##new_define_value", "value", &m_new_value);
	ImGui::SameLine();
	ImGui::Checkbox("all configurations", &m_add_everywhere);
	ImGui::SameLine();
	if (ImGui::Button("+ Add")) {
		auto problem = m_add_everywhere ? m_model.set_define_everywhere(m_new_name, m_new_value)
			: m_model.set_define(m_define_configuration, m_new_name, m_new_value);
		if (problem) {
			m_define_error = std::move(*problem);
		} else {
			m_define_error.clear();
			m_new_name.clear();
			m_new_value.clear();
		}
	}
	if (!m_define_error.empty()) {
		ImGui::PushTextWrapPos(0.f);
		ImGui::TextColored(k_error_color, "%s", m_define_error.c_str());
		ImGui::PopTextWrapPos();
	}
}

void edt::build_settings_panel::draw_module()
{
	if (!ImGui::CollapsingHeader("Module", ImGuiTreeNodeFlags_DefaultOpen)) return;
	const build_settings& settings = m_model.settings();
	bool reload_after = settings.reload_after_build;
	if (ImGui::Checkbox("Reload the module after a successful build", &reload_after)) m_model.set_reload_after_build(reload_after);

	constexpr std::array<std::pair<log_verbosity, const char*>, 3> levels{ {
		{ log_verbosity::errors, "Errors only" }, { log_verbosity::normal, "Normal" }, { log_verbosity::verbose, "Verbose" } } };
	const char* preview = "Normal";
	for (const auto& [level, label] : levels) {
		if (level == settings.log_verbosity) preview = label;
	}
	ImGui::SetNextItemWidth(160.f);
	if (ImGui::BeginCombo("Build log", preview)) {
		for (const auto& [level, label] : levels) {
			if (ImGui::Selectable(label, level == settings.log_verbosity)) m_model.set_log_verbosity(level);
		}
		ImGui::EndCombo();
	}
}

void edt::build_settings_panel::draw_footer()
{
	ImGui::Separator();
	const bool dirty = m_model.dirty();
	const std::optional<std::string> blocked = m_model.apply_blocked_reason();
	ImGui::BeginDisabled(!dirty || blocked.has_value());
	if (ImGui::Button("Apply")) {
		const build_settings_apply_result result = m_model.apply();
		m_apply_errors.clear();
		if (!result.user_presets_error.empty()) m_apply_errors.push_back(result.user_presets_error);
		if (!result.build_settings_error.empty()) m_apply_errors.push_back(result.build_settings_error);
		if (result.ok()) {
			m_value_edits.clear();
			m_define_error.clear();
		}
		m_changed = m_model.changed_on_disk();
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(!dirty);
	if (ImGui::Button("Revert")) {
		m_model.revert();
		m_value_edits.clear();
		m_define_error.clear();
		m_apply_errors.clear();
	}
	ImGui::EndDisabled();

	ImGui::SameLine();
	const auto pending = m_model.pending_files();
	if (pending.empty()) {
		ImGui::TextDisabled("No changes");
	} else {
		std::string files;
		for (const auto& file : pending) {
			files += files.empty() ? "" : ", ";
			files += relative(file);
		}
		ImGui::TextDisabled("Apply writes %s", files.c_str());
	}

	ImGui::PushTextWrapPos(0.f);
	if (blocked) ImGui::TextColored(k_warning_color, "%s", blocked->c_str());
	for (const auto& file : m_changed) {
		ImGui::TextColored(k_warning_color, "%s changed outside the editor - Apply merges onto the current file",
			relative(file).c_str());
	}
	for (const auto& error : m_apply_errors) ImGui::TextColored(k_error_color, "%s", error.c_str());
	for (const auto& configuration : m_model.settings().fresh_configure) {
		ImGui::TextDisabled("Next build of %s reconfigures from scratch (--fresh)", display_name_of(m_model, configuration).c_str());
	}
	ImGui::PopTextWrapPos();
}
