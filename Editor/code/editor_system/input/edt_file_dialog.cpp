#include "edt_file_dialog.h"
#include <snk/gui/gui_theme.h>
#include <snk/gui/gui_ui_font.h>
#include "edt_ui_colors.h"
#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"
#include "edt_ui_metrics.h"

#include <string>

namespace {
    struct dialog_icons {
        std::string folder;
        std::string file;
        std::string search;
        std::string back;
    };

    dialog_icons icons_for_current_font()
    {
        if (gui::ui_font_has_icons()) {
            return {gui::icon::folder, gui::icon::file, gui::icon::search, gui::icon::back};
        }
        return {"[]", "-", "?", "<"};
    }
}

bool edt::file_dialog::show(const char* title, bool* p_open, const gui::theme_semantic_colors& colors)
{
    bool result = false;
    const dialog_icons icons = icons_for_current_font();
    closed_by_request = false;

    // Create title with extension filter info
    std::string window_title = title;
    if (!extension_filters.empty()) {
        window_title += " (";
        for (size_t i = 0; i < extension_filters.size(); ++i) {
            window_title += "*" + extension_filters[i];
            if (i < extension_filters.size() - 1) {
                window_title += ", ";
            }
        }
        window_title += ")";
    } else {
        window_title += " (*.*)";
    }
    
    // Once: a popup closed from outside (Esc) must stay closed instead of being reopened next frame.
    if (!opened) {
        ImGui::OpenPopup(window_title.c_str());
        opened = true;
    }

    // Set fixed button width and spacing
    const float button_width = em(120.0f);
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float total_width = button_width * 2 + spacing;
    
    // Minimum window size should be slightly larger than buttons width
    const float padding = em(40.0f); // Additional padding
    const ImVec2 min_size(total_width + padding, em(450.f));
    
    ImGui::SetNextWindowSizeConstraints(min_size, ImVec2(FLT_MAX, FLT_MAX));
    ImGui::SetNextWindowSize(ImVec2(em(600.f), em(450.f)), ImGuiCond_FirstUseEver);
    
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
    if (ImGui::BeginPopupModal(window_title.c_str(), p_open, flags)) {
		if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
			ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
			selected_path.clear();
			*p_open = false;
			opened = false;
			close_requested = false;
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return false;
		}
        // Path navigation bar with better styling
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 6));
        
        // Back button
        if (ImGui::Button((icons.back + "##back").c_str())) {
            if (current_path.has_parent_path()) {
                current_path = current_path.parent_path();
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Go back");
            
        ImGui::SameLine();
        
        // Current path with background
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::GetStyle().Colors[ImGuiCol_MenuBarBg]);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
        std::string path_str = current_path.string();
        // Reserve space for filter
        float filter_width = em(200.0f);
        float path_width = ImGui::GetContentRegionAvail().x - filter_width - ImGui::GetStyle().ItemSpacing.x;
        ImGui::PushItemWidth(path_width);
        ImGui::InputText("##path", &path_str, ImGuiInputTextFlags_ReadOnly);
        ImGui::PopItemWidth();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        
        ImGui::SameLine();
        // Filter with icon
        ImGui::PushItemWidth(filter_width);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
        ImGui::TextUnformatted(icons.search.c_str());
        ImGui::SameLine();
        ImGui::InputTextWithHint("##filter", "Filter...", &filter);
        ImGui::PopStyleVar(2);

        // File list with better styling
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyle().Colors[ImGuiCol_FrameBg]);
        if (ImGui::BeginChild("Files", ImVec2(0, -em(80.f)), true)) {
            float item_height = ImGui::GetTextLineHeightWithSpacing() * 1.2f;
            
            // Directories first
            for (const auto& entry : std::filesystem::directory_iterator(current_path)) {
                if (entry.is_directory()) {
                    std::string filename = entry.path().filename().string();
                    
                    // Apply filter
                    std::string lower_filename = filename;
                    std::string lower_filter = filter;
                    std::transform(lower_filename.begin(), lower_filename.end(), lower_filename.begin(), ::tolower);
                    std::transform(lower_filter.begin(), lower_filter.end(), lower_filter.begin(), ::tolower);
                    
                    if (!filter.empty() && lower_filename.find(lower_filter) == std::string::npos)
                        continue;

                    bool is_selected = selected_path == entry.path();
                    if (ImGui::Selectable((icons.folder + " " + filename).c_str(), is_selected, 0, ImVec2(0, item_height))) {
                        selected_path = entry.path();
                    }
                    
                    // Handle double click to enter directory
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                        current_path = entry.path();
                        selected_path.clear(); // Clear selected path when entering directory
                    }
                }
            }

            // Then files
            for (const auto& entry : std::filesystem::directory_iterator(current_path)) {
                if (entry.is_regular_file()) {
                    std::string filename = entry.path().filename().string();
                    
                    // Check file extension if filters are set
                    if (!extension_filters.empty()) {
                        std::string file_ext = entry.path().extension().string();
                        std::transform(file_ext.begin(), file_ext.end(), file_ext.begin(), ::tolower);
                        
                        // Skip files if extension doesn't match any filter
                        bool ext_match = false;
                        for (const auto& ext_filter : extension_filters) {
                            if (file_ext == ext_filter) {
                                ext_match = true;
                                break;
                            }
                        }
                        if (!ext_match)
                            continue;
                    }
                    
                    // Apply name filter
                    std::string lower_filename = filename;
                    std::string lower_filter = filter;
                    std::transform(lower_filename.begin(), lower_filename.end(), lower_filename.begin(), ::tolower);
                    std::transform(lower_filter.begin(), lower_filter.end(), lower_filter.begin(), ::tolower);
                    
                    if (!filter.empty() && lower_filename.find(lower_filter) == std::string::npos)
                        continue;

                    bool is_selected = selected_path == entry.path();
                    if (ImGui::Selectable((icons.file + " " + filename).c_str(), is_selected, 0, ImVec2(0, item_height))) {
                        selected_path = entry.path();
                    }
                }
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();

        // Selected path display with background
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::GetStyle().Colors[ImGuiCol_MenuBarBg]);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 6));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
		std::string selected_path_str = selected_path.string();
        ImGui::InputText("##selected", &selected_path_str, ImGuiInputTextFlags_ReadOnly);
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();

        // Check if OK button can be activated
        bool can_accept = false;
        if (!selected_path.empty()) {
            switch (select_mode) {
                case SELECT_MODE::FILES_ONLY:
                    can_accept = std::filesystem::is_regular_file(selected_path);
                    break;
                case SELECT_MODE::FOLDERS_ONLY:
                    can_accept = std::filesystem::is_directory(selected_path);
                    break;
                case SELECT_MODE::ANY:
                    can_accept = true;
                    break;
            }
        }

        // Buttons
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8);

        // Center buttons
        ImGui::SetCursorPosX((ImGui::GetWindowSize().x - total_width) * 0.5f);

        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);

        // A button that cannot be pressed keeps the style's plain button colour, hovered or not.
        if (!can_accept) {
            const ImVec4 plain = ImGui::GetStyleColorVec4(ImGuiCol_Button);
            ImGui::PushStyleColor(ImGuiCol_Button, plain);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, plain);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, plain);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, shade(colors.success, 0.6f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, shade(colors.success, 0.75f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, shade(colors.success, 0.5f));
        }

        if (ImGui::Button("OK", ImVec2(button_width, em(30.f))) && can_accept) {
            result = true;
            *p_open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(3);
        
        ImGui::SameLine();
        
        ImGui::PushStyleColor(ImGuiCol_Button, shade(colors.error, 0.5f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, shade(colors.error, 0.6f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, shade(colors.error, 0.45f));
        if (ImGui::Button("Cancel", ImVec2(button_width, em(30.f)))) {
            selected_path.clear();
            *p_open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();

        // Only when the user did nothing this frame: an OK or Cancel click already ended the dialog (*p_open is false)
        // and wins over the request, so the selected path of an OK click is kept.
        if (close_requested && *p_open) {
            selected_path.clear();
            *p_open = false;
            closed_by_request = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    } else {
        *p_open = false; // closed from outside: a cancel
    }
    if (!*p_open) {
        opened = false; // the next show() opens it again
        close_requested = false;
    }
    return result;
}

