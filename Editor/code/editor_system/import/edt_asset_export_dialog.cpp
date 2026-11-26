#include "edt_asset_export_dialog.h"
#include "edt_ui_metrics.h"
#include "edt_model_import_meta.h"
#include <snk/editor/edt_job_service.h>
#include <snk/resource/res_system.h>
#include <snk/common/fs_paths.h>
#include <snk/gui/gui_theme.h>
#include <snk/platform/plt_beneath.h>
#include <imgui.h>
#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Copies `text` into a fixed ImGui text buffer, truncating it, and always terminates it.
template<std::size_t N>
void copy_to_buffer(char (&dst)[N], std::string_view text)
{
	const std::size_t count = std::min(text.size(), N - 1);
	std::copy_n(text.data(), count, dst);
	dst[count] = '\0';
}

constexpr std::string_view k_prefab_suffix = ".prefab.desc";

// The prefab of the project's own layer as it is now; nullopt when it cannot be read.
std::optional<std::string> prefab_text(const std::filesystem::path& top, const res::tag& prefab)
{
	const std::filesystem::path path = fs_paths::path_from_utf8(std::string(prefab.relative()));
	platform::beneath::read_request request;
	request.root = top;
	request.folder = path.parent_path();
	request.leaf = path.filename();
	request.max_bytes = 64ull * 1024 * 1024;
	std::vector<std::byte> bytes;
	const platform::beneath::read_result read = platform::beneath::read_beneath(request, bytes);
	if (read.status != platform::beneath::read_status::ok) return std::nullopt;
	return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// What a failed export leaves in the project, for the person to keep or delete.
std::string written_before_text(const std::vector<res::tag>& written)
{
	if (written.empty()) return "\nNo file was written.";
	constexpr std::size_t shown = 8;
	std::string text = std::format("\n{} file(s) were written before and stay in the project:", written.size());
	for (std::size_t i = 0; i < written.size() && i < shown; ++i) {
		text += std::format("\n  {}", written[i].view());
	}
	if (written.size() > shown) text += std::format("\n  ... and {} more (see the log)", written.size() - shown);
	return text;
}

}

edt::asset_export_dialog::asset_export_dialog(asset_exporter& exporter, job_service& jobs,
	res::resource_system& resources, const gui::theme_semantic_colors& colors)
	: m_exporter(exporter)
	, m_colors(colors)
	, m_jobs(jobs)
	, m_resources(resources)
{
}

edt::asset_export_dialog::~asset_export_dialog() = default;

void edt::asset_export_dialog::open(import_result result, res::tag reimport)
{
	m_import = std::move(result);
	m_is_open = true;
	m_exported_tag = res::tag::null;
	m_exporting = false;
	m_export_error.clear();
	m_reimport = std::move(reimport);
	m_left_over.clear();
	m_left_over_note.clear();
	m_show_report = false;
	m_scan.reset();
	m_made.reset();
	m_scanning = false;
	m_overwrite_ticked = false;

	if (m_reimport.is_valid()) {
		// The prefab's own name and folder: the files land where the earlier import put them.
		const std::string relative(m_reimport.relative());
		const std::size_t slash = relative.rfind('/');
		std::string file = slash == std::string::npos ? relative : relative.substr(slash + 1);
		if (file.ends_with(k_prefab_suffix)) file.resize(file.size() - k_prefab_suffix.size());
		copy_to_buffer(m_asset_name, file);
		copy_to_buffer(m_target_folder, slash == std::string::npos ? std::string{} : relative.substr(0, slash));
		start_check();
	} else {
		std::string name = m_import.source_path.stem().string();
		copy_to_buffer(m_asset_name, name);
		std::string folder = "assets/" + name;
		copy_to_buffer(m_target_folder, folder);
	}

	m_browse_folder = false;
	m_needs_open = true;
	m_folder_dialog.set_current_path(m_resources.get_resources_path());
	m_folder_dialog.set_select_mode(file_dialog::SELECT_MODE::FOLDERS_ONLY);
	m_folder_dialog.clear_extension_filters();
}

void edt::asset_export_dialog::start_check()
{
	const std::uint64_t serial = ++m_scan_serial;
	m_scan.reset();
	m_overwrite_ticked = false;
	m_scanning = false;
	std::unordered_map<res::tag, res::tag> remap;
	if (!form_problem(remap).empty() && remap.empty()) return;
	m_scanning = true;
	struct checked_files
	{
		std::optional<made_files> made{};
		reimport_scan scan{};
	};
	const std::filesystem::path top = m_resources.get_resources_path();
	m_scan_job = m_jobs.start(ds::lane::io, { "Check " + std::string(m_reimport.view()), "export" }, m_lifetime,
		[exporter = &m_exporter, remap = std::move(remap), top, folder = std::string(m_target_folder),
		 prefab = m_reimport](ds::task_context& ctx) {
			checked_files out;
			if (const model_import_record record = read_model_import_record(top, prefab); record.ok()) {
				out.made = record.made;
			}
			out.scan = exporter->scan_reimport(remap, top, folder, out.made, &ctx);
			return out;
		},
		[this, serial](ds::task_result<checked_files>& result) {
			if (serial != m_scan_serial) return;
			m_scanning = false;
			if (result.ok()) {
				m_made = std::move(result.value->made);
				m_scan = std::move(result.value->scan);
			} else {
				m_scan = reimport_scan{};
				m_scan->error = result.error.empty() ? std::string("the files could not be checked") : result.error;
			}
		});
}

bool edt::asset_export_dialog::needs_overwrite_tick() const
{
	if (!m_scan) return false;
	return std::ranges::any_of(m_scan->files, [](const reimport_file_check& file) {
		return file.exists && !file.same_as_new && file.state != edit_state::unchanged;
	});
}

std::string edt::asset_export_dialog::reimport_blocker(bool checking, const reimport_scan* scan, bool ticked)
{
	if (checking || !scan) return "checking the files...";
	if (!scan->error.empty()) return scan->error + ": nothing is overwritten";
	const bool edits = std::ranges::any_of(scan->files, [](const reimport_file_check& file) {
		return file.exists && !file.same_as_new && file.state != edit_state::unchanged;
	});
	if (edits && !ticked) return "tick \"Overwrite the files listed above, losing their edits\" to export";
	return {};
}

std::string edt::asset_export_dialog::form_problem(std::unordered_map<res::tag, res::tag>& remap) const
{
	const std::string asset_name(m_asset_name);
	const std::string target_folder(m_target_folder);
	// build_remap asserts on a path holding "://", so the name and the folder are checked before it runs.
	std::string problem = asset_exporter::asset_name_problem(asset_name);
	if (problem.empty()) problem = asset_exporter::folder_problem(target_folder);
	if (!problem.empty()) return problem;
	remap = m_exporter.build_remap(m_import, asset_name, target_folder);
	problem = asset_exporter::destination_problem(remap, asset_name, target_folder);
	if (problem.empty() && m_reimport.is_valid()) {
		const auto root = remap.find(m_import.root_tag);
		if (root == remap.end() || root->second != m_reimport) {
			problem = std::format("the model's prefab would be written as {}, not over {}",
				root == remap.end() ? std::string("nothing") : std::string(root->second.view()), m_reimport.view());
		}
	}
	if (problem.empty() && m_open_with_unsaved_changes) {
		for (const auto& [mem_tag, res_tag] : remap) {
			if (m_resources.exists(res_tag) && m_open_with_unsaved_changes(res_tag)) {
				problem = std::format("{} is open with unsaved changes: save or close it first", res_tag.view());
				break;
			}
		}
	}
	return problem;
}

std::string edt::asset_export_dialog::export_blocker() const
{
	std::unordered_map<res::tag, res::tag> remap;
	std::string problem = form_problem(remap);
	if (problem.empty() && m_reimport.is_valid()) {
		problem = reimport_blocker(m_scanning, m_scan ? &*m_scan : nullptr, m_overwrite_ticked);
	}
	return problem;
}

bool edt::asset_export_dialog::render()
{
	if (!m_is_open)
		return false;

	if (m_needs_open) {
		ImGui::OpenPopup("Export to Project##edt_export");
		m_needs_open = false;
	}

	// Handle folder browser sub-dialog
	if (m_browse_folder) {
		bool folder_open = true;
		if (m_folder_dialog.show("Select Target Folder", &folder_open, m_colors)) {
			auto selected = m_folder_dialog.get_selected_path();
			auto base = m_resources.get_resources_path();
			auto rel = selected.lexically_relative(base);
			// A folder outside res:// keeps its own path, so the form names the problem instead of exporting somewhere else.
			// The root of res:// itself is the empty folder.
			copy_to_buffer(m_target_folder, rel == "." ? std::string{} : fs_paths::generic_utf8_of(rel.empty() ? selected : rel));
			m_browse_folder = false;
			m_needs_open = true; // re-open the export popup (ImGui closed it while folder dialog was active)
		}
		if (!folder_open) {
			m_browse_folder = false;
			m_needs_open = true;
		}
		return true;
	}

	// A check whose job was dropped unrun leaves nothing to wait for.
	if (m_scanning && !m_jobs.is_running(m_scan_job)) {
		m_scanning = false;
		m_scan = reimport_scan{};
		m_scan->error = "the files could not be checked";
	}

	bool is_open = true;

	ImGui::SetNextWindowSize(ImVec2(em(500.f), em(400.f)), ImGuiCond_FirstUseEver);
	if (ImGui::BeginPopupModal("Export to Project##edt_export", nullptr, ImGuiWindowFlags_None))
	{
		if (m_show_report)
			render_reimport_report();
		else if (m_exporting)
			render_export_progress();
		else
			render_export_form();

		ImGui::EndPopup();
	} else {
		// Popup was closed externally (only if not exporting)
		if (!m_exporting) {
			m_is_open = false;
			is_open = false;
		}
	}

	return is_open;
}

void edt::asset_export_dialog::render_export_form()
{
	const bool reimport = m_reimport.is_valid();
	if (reimport) {
		const std::string target(m_reimport.view());
		ImGui::Text("Re-import %s", target.c_str());
		ImGui::TextDisabled("Asset name: %s", m_asset_name);
		ImGui::TextDisabled("Target folder: %s", m_target_folder);
	} else {
		ImGui::Text("Asset name:");
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputText("##asset_name", m_asset_name, sizeof(m_asset_name));

		ImGui::Spacing();

		ImGui::Text("Target folder (relative to res://):");
		const ImGuiStyle& style = ImGui::GetStyle();
		float browse_w = ImGui::CalcTextSize("Browse...").x + style.FramePadding.x * 2.f + style.ItemSpacing.x;
		ImGui::SetNextItemWidth(-browse_w);
		ImGui::InputText("##target_folder", m_target_folder, sizeof(m_target_folder));
		ImGui::SameLine();
		if (ImGui::Button("Browse...")) {
			m_browse_folder = true;
		}
	}

	const std::string target_folder(m_target_folder);
	const ImVec4 error_color = m_colors.error;

	std::unordered_map<res::tag, res::tag> remap;
	const std::string problem = form_problem(remap);
	const std::string hold = problem.empty() && reimport
		? reimport_blocker(m_scanning, m_scan ? &*m_scan : nullptr, m_overwrite_ticked) : std::string{};
	if (!problem.empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, error_color);
		ImGui::TextWrapped("%s", problem.c_str());
		ImGui::PopStyleColor();
	} else if (target_folder.empty()) {
		ImGui::TextDisabled("Empty: the root of res://");
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	const bool scanned = reimport && m_scan && !m_scanning;
	const bool tick = scanned && m_scan->error.empty() && needs_overwrite_tick();
	if (problem.empty()) {
		ImGui::Text("%s", reimport ? "Files to be written ([overwrite] keeps its .resource and ID):"
			: "Files to be created:");
		float buttons_height =
			ImGui::GetFrameHeightWithSpacing() * (tick ? 3.f : 2.f) + ImGui::GetStyle().ItemSpacing.y * 3.f;
		if (!m_export_error.empty()) {
			const auto lines = 1 + std::ranges::count(m_export_error, '\n');
			buttons_height += ImGui::GetTextLineHeightWithSpacing() * static_cast<float>(lines + 1);
		}
		if (!hold.empty()) buttons_height += ImGui::GetTextLineHeightWithSpacing();
		float available = ImGui::GetContentRegionAvail().y - buttons_height;
		ImGui::BeginChild("##file_list", ImVec2(0.f, std::max(available, em(80.f))),
			ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
		std::unordered_map<res::tag, const reimport_file_check*> checks;
		if (scanned) {
			for (const reimport_file_check& check : m_scan->files) checks.emplace(check.tag, &check);
		}
		for (const auto& [mem_tag, res_tag] : remap) {
			const std::string rel(res_tag.relative());
			const auto check = checks.find(res_tag);
			const bool exists = check != checks.end() ? check->second->exists : m_resources.exists(res_tag);
			if (!exists) {
				ImGui::Text("%s", rel.c_str());
				continue;
			}
			// What the disk holds already is not overwritten in any way that matters.
			if (check != checks.end() && check->second->same_as_new) continue;
			ImGui::TextColored(m_colors.warning, "%s [overwrite]", rel.c_str());
			if (check == checks.end()) continue;
			if (check->second->state == edit_state::changed) {
				ImGui::TextColored(m_colors.warning, "    %s", "changed after import: edits will be lost");
			} else if (check->second->state == edit_state::unknown) {
				ImGui::TextColored(m_colors.warning, "    %s", "may hold edits made after import: they will be lost");
			}
		}
		ImGui::EndChild();
		if (tick) ImGui::Checkbox("Overwrite the files listed above, losing their edits", &m_overwrite_ticked);
		if (!hold.empty()) ImGui::TextDisabled("%s", hold.c_str());
	}

	if (!m_export_error.empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, error_color);
		ImGui::TextWrapped("Export failed: %s", m_export_error.c_str());
		ImGui::PopStyleColor();
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	const float bw = em(110.f);
	const float avail = ImGui::GetContentRegionAvail().x;
	ImGui::SetCursorPosX((avail - bw * 2.f - ImGui::GetStyle().ItemSpacing.x) * 0.5f);

	const bool can_export = problem.empty() && hold.empty();

	if (!can_export) ImGui::BeginDisabled();
	if (ImGui::Button("Export", { bw, 0.f })) {
		start_export();
	}
	if (!can_export) ImGui::EndDisabled();

	ImGui::SameLine();

	if (ImGui::Button("Cancel", { bw, 0.f }) ||
		(ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
		 ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
		m_is_open = false;
		ImGui::CloseCurrentPopup();
	}
}

bool edt::asset_export_dialog::start_export()
{
	if (!m_is_open || m_exporting || m_show_report) return false;
	std::unordered_map<res::tag, res::tag> remap;
	if (!form_problem(remap).empty()) return false;
	const bool reimport = m_reimport.is_valid();
	if (reimport && !reimport_blocker(m_scanning, m_scan ? &*m_scan : nullptr, m_overwrite_ticked).empty()) {
		return false;
	}

	m_export_progress = std::make_shared<export_progress>();
	m_exporting = true;
	m_export_done = false;
	m_export_error.clear();

	// Capture by value for the background thread
	auto import_copy = m_import;
	auto progress_ptr = m_export_progress;
	auto* exporter_ptr = &m_exporter;
	m_written = std::make_shared<std::vector<res::tag>>();
	auto written = m_written;
	auto left_over = std::make_shared<no_longer_made_list>();
	const std::filesystem::path top = m_resources.get_resources_path();
	const res::tag reimport_target = m_reimport;
	const std::string target_folder(m_target_folder);
	// What the check saw: a file that is something else by the time it would be overwritten stops the export.
	std::optional<std::unordered_map<res::tag, std::string>> seen;
	if (reimport && m_scan) seen = m_scan->seen;
	const std::optional<made_files> made = m_made;

	// io lane: the work is writing files to the project; the exporter reports through export_progress.
	// wait_for_writes: a write the resource system refuses ends the export with its reason instead of a "successful"
	// one.
	m_export_job = m_jobs.start(ds::lane::io, { "Export " + std::string(m_asset_name), "export" }, m_lifetime,
		[exporter_ptr, import_copy = std::move(import_copy), remap = std::move(remap), progress_ptr, written,
		 left_over, top, reimport_target, target_folder, seen = std::move(seen), made](ds::task_context& ctx) {
			ctx.throw_if_stopped();
			// The prefab as the earlier import left it, read before it is overwritten.
			const std::optional<std::string> before =
				reimport_target.is_valid() ? prefab_text(top, reimport_target) : std::nullopt;
			const bool done = exporter_ptr->export_to_project(import_copy, remap, progress_ptr.get(), &ctx,
				written.get(), true, {}, {}, true, seen ? &*seen : nullptr);
			if (done && reimport_target.is_valid()) {
				*left_over = asset_exporter::no_longer_made(made, before, target_folder, remap);
			}
			return done;
		},
		[this, written, left_over](ds::task_result<bool>& result) {
			if (!result.ok()) {
				m_export_error = result.error.empty() ? std::string("the export stopped (see the log)") : result.error;
			} else if (!*result.value || !m_export_progress || !m_export_progress->success.load()) {
				m_export_error = m_export_progress && !m_export_progress->error.empty()
					? m_export_progress->error : std::string("a file could not be written (see the log)");
			} else if (!m_export_progress->skipped.empty()) {
				m_exported_tag = m_export_progress->exported_tag;
				std::string missing;
				for (const res::tag& tag : m_export_progress->skipped) missing += std::format("\n  {}", tag.view());
				egLOG_WARN("edt/export", "Exported {} without {} file(s) the model refers to but the import did not "
					"find:{}", m_exported_tag.view(), m_export_progress->skipped.size(), missing);
			} else {
				m_exported_tag = m_export_progress->exported_tag;
				egLOG("edt/export", "Export successful: {}", m_exported_tag.view());
			}
			if (m_export_error.empty() && m_reimport.is_valid()) {
				for (const res::tag& tag : left_over->tags) {
					if (m_resources.exists(tag)) m_left_over.push_back(tag);
				}
				m_left_over_note = left_over->note;
				std::string listed;
				for (const res::tag& tag : m_left_over) listed += std::format("\n  {}", tag.view());
				if (!m_left_over.empty()) {
					egLOG_WARN("edt/export", "Re-imported {}; no longer made by the model, they stay in the "
						"project:{}", m_reimport.view(), listed);
				}
				if (!m_left_over_note.empty()) {
					egLOG_WARN("edt/export", "Re-imported {}; {}", m_reimport.view(), m_left_over_note);
				}
				m_show_report = !m_left_over.empty() || !m_left_over_note.empty();
			}
			if (!m_export_error.empty()) {
				// Not taken back: a written file may have replaced one the project had, which is gone by now.
				m_export_error += written_before_text(*written);
				egLOG_ERROR("edt/export", "Export failed: {}", m_export_error);
				// What is on the disk now is what the next attempt must be checked against.
				if (m_reimport.is_valid()) start_check();
			}
			m_export_done = true;
		});
	if (reimport) {
		// Also against job.cancel from the job list: past the first file the re-import runs to its end.
		m_jobs.set_cancel_veto(m_export_job, [progress_ptr](bool) -> std::optional<std::string> {
			if (may_cancel_export(true, progress_ptr->done.load())) return std::nullopt;
			return std::string("the re-import has written files: it runs to its end");
		});
	}
	return true;
}

void edt::asset_export_dialog::export_stopped()
{
	m_export_done = true;
	if (!m_reimport.is_valid()) {
		egLOG("edt/export", "Export cancelled");
		return;
	}
	// The job is no longer running: what it wrote is all it will write.
	m_export_error = "The re-import was cancelled part way";
	m_export_error += written_before_text(m_written ? *m_written : std::vector<res::tag>{});
	egLOG_ERROR("edt/export", "{}", m_export_error);
	start_check();
}

void edt::asset_export_dialog::render_export_progress()
{
	int done = m_export_progress->done.load();
	int total = m_export_progress->total.load();

	ImGui::Text("Exporting assets...");
	ImGui::Spacing();

	float fraction = (total > 0) ? static_cast<float>(done) / static_cast<float>(total) : 0.f;
	char overlay[64];
	snprintf(overlay, sizeof(overlay), "%d / %d", done, total);
	ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 0.f), overlay);

	ImGui::Spacing();

	// The job was cancelled and its result discarded: nothing more will arrive.
	if (!m_export_done && !m_jobs.is_running(m_export_job))
		export_stopped();

	const bool reimport = m_reimport.is_valid();
	const bool may_cancel = !m_export_done && may_cancel_export(reimport, done);
	if (!may_cancel) ImGui::BeginDisabled();
	const bool cancel = ImGui::Button("Cancel", { em(110.f), 0.f });
	if (!may_cancel) ImGui::EndDisabled();
	if (may_cancel && (cancel || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
		ImGui::IsKeyPressed(ImGuiKey_Escape, false)))) {
		// A re-import waits for its job to stop, then reports what it wrote.
		if (m_jobs.cancel(m_export_job) && !reimport) {
			egLOG("edt/export", "Export cancelled");
			m_export_done = true;
		}
	}

	if (m_export_done) {
		m_exporting = false;
		m_export_progress.reset();
		// A failed export goes back to the form with the reason; the files it wrote before stay (see export_to_project).
		if (m_export_error.empty() && !m_show_report) {
			m_is_open = false;
			ImGui::CloseCurrentPopup();
		}
	}
}

void edt::asset_export_dialog::render_reimport_report()
{
	const std::string target(m_reimport.view());
	ImGui::Text("Re-imported %s.", target.c_str());
	if (!m_left_over_note.empty()) ImGui::TextColored(m_colors.warning, "%s", m_left_over_note.c_str());
	if (!m_left_over.empty()) ImGui::TextWrapped("%s", "No longer made by the model; they stay in the project:");
	const float list_height = std::max(ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 2.f,
		em(80.f));
	ImGui::BeginChild("##no_longer_made", ImVec2(0.f, list_height), ImGuiChildFlags_Borders,
		ImGuiWindowFlags_HorizontalScrollbar);
	for (const res::tag& tag : m_left_over) {
		const std::string text(tag.view());
		ImGui::TextUnformatted(text.c_str());
	}
	ImGui::EndChild();
	const bool escape = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
		&& ImGui::IsKeyPressed(ImGuiKey_Escape, false);
	if (ImGui::Button("Close", { em(110.f), 0.f }) || escape) {
		m_show_report = false;
		m_is_open = false;
		ImGui::CloseCurrentPopup();
	}
}
