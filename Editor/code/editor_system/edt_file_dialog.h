#pragma once
#include "common.h"

namespace edt {

class file_dialog {
public:
    enum class SELECT_MODE {
        FILES_ONLY,      // Files only
        FOLDERS_ONLY,    // Folders only
        ANY            // Any item
    };

    file_dialog() {
        current_path = std::filesystem::current_path();
        base_path = current_path;
        selected_path = "";
        filter = "";
        select_mode = SELECT_MODE::FILES_ONLY;
    }

    bool show(const char* title, bool* p_open);

    const std::filesystem::path& get_selected_path() const { return selected_path; }
	const std::filesystem::path& get_base_path() const { return base_path; }
    void clear_selection() { selected_path.clear(); }
	void set_current_path(const std::filesystem::path& path) { current_path = path; base_path = path; }
    void set_select_mode(SELECT_MODE mode) { select_mode = mode; }
    
    // New methods for extension filters
    void add_extension_filter(const std::string& ext) { extension_filters.push_back(ext); }
    void clear_extension_filters() { extension_filters.clear(); }

private:
    std::filesystem::path base_path;
    std::filesystem::path current_path;
    std::filesystem::path selected_path;
    std::string filter;
    std::vector<std::string> extension_filters;  // Changed to vector
    SELECT_MODE select_mode;
};
}
