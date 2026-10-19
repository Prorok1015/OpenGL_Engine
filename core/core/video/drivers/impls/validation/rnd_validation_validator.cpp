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
		case trace_op::set_viewport: return "viewport";
		case trace_op::begin_pass: return "begin_pass";
		case trace_op::end_pass: return "end_pass";
		case trace_op::bind_pipeline: return "bind_pipeline";
		case trace_op::bind_texture: return "bind_texture";
		case trace_op::bind_group: return "bind_group";
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
	case rule::unbalanced_at_present: return "unbalanced_at_present";
	case rule::attachment_usage: return "attachment_usage";
	case rule::texture_usage_flags: return "texture_usage_flags";
	case rule::texture_not_sampled: return "texture_not_sampled";
	case rule::attachment_size_mismatch: return "attachment_size_mismatch";
	case rule::nested_render_pass: return "nested_render_pass";
	case rule::end_without_begin: return "end_without_begin";
	case rule::invalid_render_pass: return "invalid_render_pass";
	case rule::render_area_outside_target: return "render_area_outside_target";
	case rule::viewport_outside_target: return "viewport_outside_target";
	case rule::draw_outside_render_pass: return "draw_outside_render_pass";
	case rule::draw_without_pipeline: return "draw_without_pipeline";
	case rule::bind_pipeline_outside_pass: return "bind_pipeline_outside_pass";
	case rule::invalid_pipeline: return "invalid_pipeline";
	case rule::pipeline_format_mismatch: return "pipeline_format_mismatch";
	case rule::draw_index_range: return "draw_index_range";
	case rule::draw_without_layout: return "draw_without_layout";
	case rule::buffer_write_range: return "buffer_write_range";
	case rule::dead_object: return "dead_object";
	case rule::budget_frame_buffers: return "budget_frame_buffers";
	case rule::budget_programs: return "budget_programs";
	case rule::budget_textures: return "budget_textures";
	case rule::budget_pipelines: return "budget_pipelines";
	case rule::invalid_buffer: return "invalid_buffer";
	case rule::bind_group_outside_pass: return "bind_group_outside_pass";
	case rule::invalid_bind_group: return "invalid_bind_group";
	case rule::bind_group_layout_mismatch: return "bind_group_layout_mismatch";
	case rule::binding_outside_table: return "binding_outside_table";
	case rule::transient_group_expired: return "transient_group_expired";
	case rule::dynamic_offset_alignment: return "dynamic_offset_alignment";
	case rule::draw_missing_bind_group: return "draw_missing_bind_group";
	case rule::budget_bind_groups: return "budget_bind_groups";
	case rule::binding_offset_alignment: return "binding_offset_alignment";
	}
	return "unknown";
}

std::string rnd::driver::validation::usage_text(texture_usage usage)
{
	struct named
	{
		texture_usage flag;
		const char* name;
	};
	constexpr named names[] = {
		{ texture_usage::sampled, "sampled" },
		{ texture_usage::storage, "storage" },
		{ texture_usage::color_target, "color_target" },
		{ texture_usage::depth_target, "depth_target" },
		{ texture_usage::stencil_target, "stencil_target" },
	};
	std::string text;
	for (const named& entry : names) {
		if (has_usage(usage, entry.flag)) {
			text += text.empty() ? "" : "|";
			text += entry.name;
		}
	}
	return text.empty() ? "none" : text;
}

std::string rnd::driver::validation::format_trace_entry(const trace_entry& entry)
{
	switch (entry.op) {
	case trace_op::begin_pass:
		return std::format("#{} begin_pass({} color{})", entry.frame, entry.a, entry.b ? " + depth" : "");
	case trace_op::draw:
		return std::format("#{} draw({})", entry.frame, entry.a);
	case trace_op::bind_pipeline:
	case trace_op::bind_texture:
		return std::format("#{} {}({})", entry.frame, trace_op_name(entry.op), entry.a);
	case trace_op::bind_group:
		return std::format("#{} bind_group(set {}, group {})", entry.frame, entry.a, entry.b);
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

void rnd::driver::validation::validator::begin_pass(std::string name, target_info target, pass_formats formats, std::uint32_t colors, bool depth)
{
	m_pass_active = true;
	m_pass_name = std::move(name);
	m_pass_target = target;
	m_pass_formats = std::move(formats);
	m_pipeline_bound = false;
	note(trace_op::begin_pass, colors, depth ? 1u : 0u);
	if (crosses_budget(++m_current.frame_buffers, m_budget.max_frame_buffers)) {
		report(rule::budget_frame_buffers, std::format("more than {} render passes begun in one frame", m_budget.max_frame_buffers));
	}
}

bool rnd::driver::validation::validator::end_pass()
{
	if (!m_pass_active) {
		return false;
	}
	m_pass_active = false;
	m_pass_name.clear();
	m_pass_target = {};
	m_pass_formats = {};
	m_pipeline_bound = false;
	note(trace_op::end_pass);
	return true;
}

bool rnd::driver::validation::validator::consume_dropped_begin()
{
	if (m_dropped_begins == 0) {
		return false;
	}
	--m_dropped_begins;
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

void rnd::driver::validation::validator::on_pipeline_created()
{
	if (crosses_budget(++m_current.pipelines, m_budget.max_pipelines)) {
		report(rule::budget_pipelines, std::format("more than {} pipelines created in one frame", m_budget.max_pipelines));
	}
}

void rnd::driver::validation::validator::on_bind_group_created(bool transient)
{
	if (transient) {
		++m_current.transient_bind_groups;
		return;
	}
	if (crosses_budget(++m_current.bind_groups, m_budget.max_bind_groups)) {
		report(rule::budget_bind_groups, std::format("more than {} persistent bind groups created in one frame", m_budget.max_bind_groups));
	}
}

void rnd::driver::validation::validator::on_present()
{
	note(trace_op::present);
	if (m_pass_active) {
		report(rule::unbalanced_at_present, std::format("render pass '{}' is still open at present", m_pass_name));
	}
	m_last = m_current;
	m_current = {};
	++m_frame;
}
