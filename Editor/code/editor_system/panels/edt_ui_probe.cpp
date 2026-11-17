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

std::string edt::ui_probe::format_ambiguous(std::string_view area, std::string_view name)
{
	return std::format("item {} '{}' ambiguous", area, name);
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

void edt::ui_probe::take(item& entry, std::string line)
{
	if (!m_open_passes.contains(entry.area)) {
		entry.seen = true;
		write(entry, std::move(line));
		return;
	}
	entry.pending = entry.seen ? format_ambiguous(entry.area, entry.name) : std::move(line);
	entry.seen = true;
}

void edt::ui_probe::report(std::string_view area, std::string_view name, float min_x, float min_y, float max_x, float max_y)
{
	take(entry_of(area, name), format(area, name, min_x, min_y, max_x, max_y));
}

void edt::ui_probe::report_hidden(std::string_view area, std::string_view name)
{
	take(entry_of(area, name), format_hidden(area, name));
}

void edt::ui_probe::begin_pass(std::string_view area)
{
	m_open_passes.emplace(area);
	for (auto& [key, entry] : m_items) {
		if (entry.area == area) {
			entry.seen = false;
		}
	}
}

void edt::ui_probe::end_pass(std::string_view area)
{
	m_open_passes.erase(std::string(area));
	for (auto& [key, entry] : m_items) {
		if (entry.area == area) {
			write(entry, entry.seen ? std::move(entry.pending) : format_hidden(entry.area, entry.name));
			entry.pending.clear();
		}
	}
}

edt::ui_probe::scoped_pass::scoped_pass(ui_probe* probe, std::initializer_list<std::string_view> areas)
	: m_probe(probe)
	, m_areas(areas)
{
	if (!m_probe) return;
	for (const std::string_view area : m_areas) {
		m_probe->begin_pass(area);
	}
}

edt::ui_probe::scoped_pass::~scoped_pass()
{
	if (!m_probe) return;
	for (const std::string_view area : m_areas) {
		m_probe->end_pass(area);
	}
}
