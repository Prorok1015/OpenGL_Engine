#pragma once
#include <snk/editor/edt_panel_base.h>
#include <snk/editor/edt_problem_list.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace edt
{
	// `file` relative to `root` when it is an absolute path under it, else as printed. Never throws: a file that is
	// not valid UTF-8 (cl prints in the console code page) is returned as printed.
	std::string problem_display_file(std::string_view file, const std::filesystem::path& root);
	// "file(line,column): severity code: message" with the file as printed, then the notes on their own lines;
	// the parts a problem lacks are left out.
	std::string problem_copy_text(const problem& item);

	// The editor's problems (problem_list) as a table: severity, file:line, message; the notes are in the row's
	// tooltip so that every row is one line high and the table clips. Counts per severity in the header double as
	// filters; a row's context menu copies it. With an activate callback a double click, Enter or "Go to" on a row
	// hands it over with its source. A row's fixes (set_actions) head its context menu; set_undo shows "Undo: <label>"
	// beside the filters while there is something to undo.
	class problems_panel : public panel_base
	{
	public:
		using activate_fn = std::function<void(const std::string& source, const problem& item)>;
		using actions_fn = std::function<std::vector<std::pair<std::string, std::function<void()>>>(const std::string& source, const problem& item)>;

		// `problems` must outlive the panel. `project_root` (empty: none) shortens the paths that lie under it.
		problems_panel(const problem_list& problems, std::filesystem::path project_root);

		void set_on_activate(activate_fn on_activate) { m_on_activate = std::move(on_activate); }
		void set_actions(actions_fn actions) { m_actions = std::move(actions); }
		// `enabled` (null: always) greys the button out, e.g. while a dialog waits or an edit is in progress.
		void set_undo(std::function<std::optional<std::string>()> label, std::function<void()> run, std::function<bool()> enabled = {})
		{
			m_undo_label = std::move(label);
			m_undo = std::move(run);
			m_undo_enabled = std::move(enabled);
		}

	protected:
		void on_render() override;

	private:
		struct row
		{
			std::string source;
			problem item;
			std::string location;
			std::string tooltip;
		};

		void refresh();
		void filter();

		const problem_list& m_problems;
		std::filesystem::path m_root;
		std::vector<row> m_rows;
		// Indices into m_rows of the rows the severity filters let through.
		std::vector<std::size_t> m_visible;
		// Indexed by problem_severity.
		std::array<std::size_t, 3> m_counts{};
		std::array<bool, 3> m_shown{ true, true, true };
		std::uint64_t m_generation = 0;
		activate_fn m_on_activate;
		actions_fn m_actions;
		std::function<std::optional<std::string>()> m_undo_label;
		std::function<void()> m_undo;
		std::function<bool()> m_undo_enabled;
	};
}
