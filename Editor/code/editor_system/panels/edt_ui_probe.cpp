#include "edt_ui_probe.h"
#include <snk/common/logger/engine_log.h>
#include <cmath>
#include <format>

edt::ui_probe::ui_probe(sink out)
	: m_out(std::move(out))
{
}

std::string edt::ui_probe::format(std::string_view area, std::string_view name, float min_x, float min_y, float max_x, float max_y)
{
	const auto pixels = [](float value) { return static_cast<long long>(std::lround(value)); };
	return std::format("item {} '{}' center {} {} size {} {}", area, name,
		pixels((min_x + max_x) / 2.f), pixels((min_y + max_y) / 2.f), pixels(max_x - min_x), pixels(max_y - min_y));
}

std::string edt::ui_probe::format_hidden(std::string_view area, std::string_view name)
{
	return std::format("item {} '{}' hidden", area, name);
}

edt::ui_probe::item& edt::ui_probe::entry_of(std::string_view area, std::string_view name)
{
	item& entry = m_items[std::format("{}/{}", area, name)];
	if (entry.name.empty()) {
		entry.area = area;
		entry.name = name;
	}
	return entry;
}

void edt::ui_probe::write(item& entry, std::string line)
{
	if (entry.line == line) {
		return;
	}
	entry.line = std::move(line);
	if (m_out) {
		m_out(entry.line);
	} else {
		egLOG(k_category, "{}", entry.line);
	}
}

void edt::ui_probe::report(std::string_view area, std::string_view name, float min_x, float min_y, float max_x, float max_y)
{
	item& entry = entry_of(area, name);
	entry.seen = true;
	write(entry, format(area, name, min_x, min_y, max_x, max_y));
}

void edt::ui_probe::report_hidden(std::string_view area, std::string_view name)
{
	item& entry = entry_of(area, name);
	entry.seen = true;
	write(entry, format_hidden(area, name));
}

void edt::ui_probe::begin_pass(std::string_view area)
{
	for (auto& [key, entry] : m_items) {
		if (entry.area == area) {
			entry.seen = false;
		}
	}
}

void edt::ui_probe::end_pass(std::string_view area)
{
	for (auto& [key, entry] : m_items) {
		if (entry.area == area && !entry.seen) {
			write(entry, format_hidden(entry.area, entry.name));
		}
	}
}
