#include "rnd_validation_validator.h"
#include <snk/common/logger/engine_log.h>

#include <algorithm>

namespace {
	constexpr std::size_t logged_trace_tail = 6;

	// True exactly once per frame: when `used` first passes `limit`.
	bool crosses_budget(std::size_t used, std::size_t limit)
	{
		return limit != rnd::driver::validation::limits::unlimited && used == limit + 1;
	}

	const char* trace_op_name(rnd::driver::validation::trace_op op)
	{
		using rnd::driver::validation::trace_op;
		switch (op) {
		case trace_op::push_target: return "push";
		case trace_op::pop_target: return "pop";
		case trace_op::set_targets: return "targets";
		case trace_op::set_viewport: return "viewport";
		case trace_op::set_scissor: return "scissor";
		case trace_op::clear: return "clear";
		case trace_op::use_program: return "use";
		case trace_op::bind_texture: return "bind_texture";
		case trace_op::bind_ssbo: return "bind_ssbo";
		case trace_op::draw: return "draw";
		case trace_op::create_object: return "create";
		case trace_op::present: return "present";
		}
		return "?";
	}
}

const char* rnd::driver::validation::rule_name(rule r)
{
	switch (r) {
	case rule::pop_without_push: return "pop_without_push";
	case rule::unbalanced_at_present: return "unbalanced_at_present";
	case rule::render_target_without_push: return "render_target_without_push";
	case rule::attachment_usage: return "attachment_usage";
	case rule::attachment_size_mismatch: return "attachment_size_mismatch";
	case rule::viewport_outside_target: return "viewport_outside_target";
	case rule::scissor_outside_target: return "scissor_outside_target";
	case rule::draw_without_use: return "draw_without_use";
	case rule::draw_index_range: return "draw_index_range";
	case rule::draw_without_layout: return "draw_without_layout";
	case rule::buffer_write_range: return "buffer_write_range";
	case rule::dead_object: return "dead_object";
	case rule::budget_frame_buffers: return "budget_frame_buffers";
	case rule::budget_programs: return "budget_programs";
	case rule::budget_textures: return "budget_textures";
	}
	return "unknown";
}

std::string rnd::driver::validation::format_trace_entry(const trace_entry& entry)
{
	switch (entry.op) {
	case trace_op::set_targets:
		return std::format("#{} targets({} color{})", entry.frame, entry.a, entry.b ? " + depth" : "");
	case trace_op::draw:
		return std::format("#{} draw({})", entry.frame, entry.a);
	case trace_op::bind_texture:
	case trace_op::bind_ssbo:
		return std::format("#{} {}({})", entry.frame, trace_op_name(entry.op), entry.a);
	case trace_op::create_object:
		return std::format("#{} create(kind {})", entry.frame, entry.a);
	default:
		return std::format("#{} {}", entry.frame, trace_op_name(entry.op));
	}
}

rnd::driver::validation::validator::validator(limits budget, std::size_t trace_capacity)
	: m_budget(budget)
	, m_trace(std::max<std::size_t>(trace_capacity, 1))
{
}

void rnd::driver::validation::validator::report(rule kind, std::string message)
{
	++m_counts[static_cast<std::size_t>(kind)];
	++m_total;
	egLOG_ERROR("rnd.validate", "{} (frame {}): {} | recent: {}", rule_name(kind), m_frame, message, recent_trace(logged_trace_tail));
	if (m_violations.size() < max_recorded_violations) {
		m_violations.push_back({ kind, m_frame, std::move(message) });
	}
}

void rnd::driver::validation::validator::add_object(const void* object, object_kind kind)
{
	std::lock_guard lock(m_registry_mutex);
	m_objects[object] = kind;
}

void rnd::driver::validation::validator::remove_object(const void* object)
{
	std::lock_guard lock(m_registry_mutex);
	m_objects.erase(object);
}

bool rnd::driver::validation::validator::is_live(const void* object, object_kind kind) const
{
	std::lock_guard lock(m_registry_mutex);
	const auto it = m_objects.find(object);
	return it != m_objects.end() && it->second == kind;
}

std::size_t rnd::driver::validation::validator::live_objects(object_kind kind) const
{
	std::lock_guard lock(m_registry_mutex);
	return static_cast<std::size_t>(std::count_if(m_objects.begin(), m_objects.end(),
		[kind](const auto& entry) { return entry.second == kind; }));
}

bool rnd::driver::validation::validator::require_live(const void* object, object_kind kind, const char* what)
{
	if (is_live(object, kind)) {
		return true;
	}
	report(rule::dead_object, std::format("{} {} is not a live object of this driver (deleted or foreign)", what, object));
	return false;
}

void rnd::driver::validation::validator::note(trace_op op, std::uint32_t a, std::uint32_t b)
{
	m_trace[m_trace_head] = { m_frame, op, a, b };
	m_trace_head = (m_trace_head + 1) % m_trace.size();
	m_trace_size = std::min(m_trace_size + 1, m_trace.size());
}

std::vector<rnd::driver::validation::trace_entry> rnd::driver::validation::validator::trace() const
{
	std::vector<trace_entry> result;
	result.reserve(m_trace_size);
	const std::size_t first = (m_trace_head + m_trace.size() - m_trace_size) % m_trace.size();
	for (std::size_t i = 0; i < m_trace_size; ++i) {
		result.push_back(m_trace[(first + i) % m_trace.size()]);
	}
	return result;
}

std::string rnd::driver::validation::validator::recent_trace(std::size_t count) const
{
	const auto entries = trace();
	std::string text;
	for (std::size_t i = entries.size() - std::min(count, entries.size()); i < entries.size(); ++i) {
		if (!text.empty()) {
			text += " > ";
		}
		text += format_trace_entry(entries[i]);
	}
	return text.empty() ? "(no commands)" : text;
}

void rnd::driver::validation::validator::push_target()
{
	m_targets.emplace_back();
	note(trace_op::push_target);
	if (crosses_budget(++m_current.frame_buffers, m_budget.max_frame_buffers)) {
		report(rule::budget_frame_buffers, std::format("more than {} frame buffers pushed in one frame", m_budget.max_frame_buffers));
	}
}

bool rnd::driver::validation::validator::pop_target()
{
	if (m_targets.size() == 1) {
		report(rule::pop_without_push, "pop_frame_buffer with only the backbuffer on the stack");
		return false;
	}
	m_targets.pop_back();
	note(trace_op::pop_target);
	return true;
}

void rnd::driver::validation::validator::on_texture_created()
{
	if (crosses_budget(++m_current.textures, m_budget.max_textures)) {
		report(rule::budget_textures, std::format("more than {} textures created in one frame", m_budget.max_textures));
	}
}

void rnd::driver::validation::validator::on_program_created()
{
	if (crosses_budget(++m_current.programs, m_budget.max_programs)) {
		report(rule::budget_programs, std::format("more than {} programs created in one frame", m_budget.max_programs));
	}
}

void rnd::driver::validation::validator::on_present()
{
	note(trace_op::present);
	if (m_targets.size() != 1) {
		report(rule::unbalanced_at_present, std::format("frame buffer stack is {} deep at present, expected 1", m_targets.size()));
	}
	m_last = m_current;
	m_current = {};
	m_program_used = false;
	++m_frame;
}
