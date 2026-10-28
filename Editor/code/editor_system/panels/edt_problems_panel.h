#pragma once
#include "edt_panel_base.h"
#include <snk/editor/edt_problem_list.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace edt
{
	// `file` relative to `root` when it is an absolute path under it, else as printed.
	std::string problem_display_file(std::string_view file, const std::filesystem::path& root);
	// "file(line): severity code: message" with the file as printed; the parts a problem lacks are left out.
	std::string problem_copy_text(const problem& item);

	// The editor's problems (problem_list) as a table: severity, file:line, message with its notes below it.
	// Counts per severity in the header double as filters; a row's context menu copies it.
	class problems_panel : public panel_base
	{
	public:
		// `problems` must outlive the panel. `project_root` (empty: none) shortens the paths that lie under it.
		problems_panel(const problem_list& problems, std::filesystem::path project_root);

	protected:
		void on_render() override;

	private:
		struct row
		{
			problem item;
			std::string location;
		};

		void refresh();

		const problem_list& m_problems;
		std::filesystem::path m_root;
		std::vector<row> m_rows;
		// Indexed by problem_severity.
		std::array<std::size_t, 3> m_counts{};
		std::array<bool, 3> m_shown{ true, true, true };
		std::uint64_t m_generation = 0;
	};
}
