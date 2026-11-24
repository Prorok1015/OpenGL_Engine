#include "edt_shader_markers.h"
#include <algorithm>
#include <map>
#include <utility>

namespace edt
{
	namespace
	{
		// The enum orders error before warning before note: the smaller is the worse.
		problem_severity worse(problem_severity a, problem_severity b)
		{
			return static_cast<int>(a) <= static_cast<int>(b) ? a : b;
		}

		std::string clipped(std::string text)
		{
			if (text.size() > k_max_shader_marker_text) {
				text.resize(k_max_shader_marker_text);
				text += "...";
			}
			return text;
		}
	}

	std::string shader_problem_text(const problem& item)
	{
		std::string text;
		if (!item.code.empty()) {
			text += "[" + item.code + "] ";
		}
		text += item.message;
		if (item.column > 0) {
			text += " (column " + std::to_string(item.column) + ")";
		}
		return clipped(std::move(text));
	}

	shader_marker_set shader_markers_for(const std::vector<sourced_problem>& problems, std::string_view file, int line_count)
	{
		shader_marker_set out;
		std::map<int, shader_line_marker> by_line;
		for (const sourced_problem& row : problems) {
			if (row.source != k_shader_problem_source || row.item.file != file) {
				continue;
			}
			const std::string text = shader_problem_text(row.item);
			if (row.item.line < 1 || line_count < 1) {
				out.unplaced.push_back(text);
				continue;
			}
			const int line = std::min(row.item.line, line_count);
			auto [it, added] = by_line.try_emplace(line);
			shader_line_marker& marker = it->second;
			if (added) {
				marker.line = line;
				marker.severity = row.item.severity;
				marker.text = text;
			} else {
				marker.severity = worse(marker.severity, row.item.severity);
				marker.text = clipped(marker.text + "\n" + text);
			}
		}
		for (auto& [line, marker] : by_line) {
			if (out.markers.size() >= k_max_shader_markers) {
				++out.omitted;
				continue;
			}
			out.markers.push_back(std::move(marker));
		}
		return out;
	}
}
