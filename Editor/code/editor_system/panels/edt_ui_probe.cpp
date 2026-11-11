#include "edt_ui_probe.h"
#include <snk/common/logger/engine_log.h>
#include <cmath>
#include <format>

edt::ui_probe::ui_probe(bool enabled, sink out)
	: m_enabled(enabled)
	, m_out(std::move(out))
{
}

std::string edt::ui_probe::format(std::string_view area, std::string_view name, float min_x, float min_y, float max_x, float max_y)
{
	const auto pixels = [](float value) { return static_cast<long long>(std::lround(value)); };
	return std::format("item {} '{}' center {} {} size {} {}", area, name,
		pixels((min_x + max_x) / 2.f), pixels((min_y + max_y) / 2.f), pixels(max_x - min_x), pixels(max_y - min_y));
}

void edt::ui_probe::report(std::string_view area, std::string_view name, float min_x, float min_y, float max_x, float max_y)
{
	if (!m_enabled) {
		return;
	}
	std::string line = format(area, name, min_x, min_y, max_x, max_y);
	std::string& last = m_last_line[std::format("{}/{}", area, name)];
	if (last == line) {
		return;
	}
	last = line;
	if (m_out) {
		m_out(line);
	} else {
		egLOG(k_category, "{}", line);
	}
}
