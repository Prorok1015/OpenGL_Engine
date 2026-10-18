#include "rnd_validation_driver.h"

#include <array>
#include <format>
#include <optional>

namespace {
	using namespace rnd::driver;

	texture_interface* unwrap(texture_interface* wrapped)
	{
		return wrapped ? &static_cast<validation::texture*>(wrapped)->inner() : nullptr;
	}
}

rnd::driver::validation::driver::driver(std::unique_ptr<driver_interface> inner, limits budget)
	: driver(std::move(inner), std::make_shared<validator>(budget))
{
}

rnd::driver::validation::driver::driver(std::unique_ptr<driver_interface> inner, std::shared_ptr<validator> state)
	: m_inner(std::move(inner))
	, m_state(std::move(state))
{
}

void rnd::driver::validation::driver::push_frame_buffer()
{
	m_state->push_target();
	m_inner->push_frame_buffer();
}

void rnd::driver::validation::driver::pop_frame_buffer()
{
	if (m_state->pop_target()) {
		m_inner->pop_frame_buffer();
	}
}

bool rnd::driver::validation::driver::check_targets(std::span<texture_interface* const> colors, texture_interface* depth_stencil)
{
	const bool on_backbuffer = m_state->target_depth() == 1;
	if (on_backbuffer) {
		m_state->report(rule::render_target_without_push, "set_render_target(s) with only the backbuffer on the stack");
	}

	bool alive = true;
	for (texture_interface* color : colors) {
		alive = (!color || m_state->require_live(color, object_kind::texture, "color attachment")) && alive;
	}
	alive = (!depth_stencil || m_state->require_live(depth_stencil, object_kind::texture, "depth attachment")) && alive;
	if (!alive) {
		return false;
	}

	std::optional<glm::uvec2> extent;
	bool size_mismatch = false;
	const auto visit = [&](const texture_interface& attachment, bool is_depth) {
		const bool usage_ok = is_depth
			? attachment.has_usage(TEXTURE_USAGE::DEPTH_TARGET) || attachment.has_usage(TEXTURE_USAGE::STENCIL_TARGET)
			: attachment.has_usage(TEXTURE_USAGE::COLOR_TARGET);
		if (!usage_ok) {
			m_state->report(rule::attachment_usage, std::format("{} attachment of {}x{} has usage {}, expected {}",
				is_depth ? "depth" : "color", attachment.width(), attachment.height(), static_cast<int>(attachment.get_usage()),
				is_depth ? "DEPTH_TARGET or STENCIL_TARGET" : "COLOR_TARGET"));
		}
		const glm::uvec2 size{ attachment.width(), attachment.height() };
		if (!extent) {
			extent = size;
		} else if (*extent != size) {
			size_mismatch = true;
		}
	};
	for (const texture_interface* color : colors) {
		if (color) {
			visit(*color, false);
		}
	}
	if (depth_stencil) {
		visit(*depth_stencil, true);
	}
	if (size_mismatch) {
		m_state->report(rule::attachment_size_mismatch, "attachments of one render target differ in size");
	}

	if (!on_backbuffer) {
		target_info& target = m_state->current_target();
		target.has_extent = extent.has_value();
		target.width = extent ? extent->x : 0;
		target.height = extent ? extent->y : 0;
	}
	m_state->note(trace_op::set_targets, static_cast<std::uint32_t>(colors.size()), depth_stencil ? 1u : 0u);
	return true;
}

void rnd::driver::validation::driver::set_render_target(texture_interface* color, texture_interface* depth_stencil)
{
	const std::array<texture_interface*, 1> colors{ color };
	if (check_targets(colors, depth_stencil)) {
		m_inner->set_render_target(unwrap(color), unwrap(depth_stencil));
	}
}

void rnd::driver::validation::driver::set_render_targets(std::vector<texture_interface*> colors, texture_interface* depth_stencil)
{
	if (!check_targets(colors, depth_stencil)) {
		return;
	}
	for (texture_interface*& color : colors) {
		color = unwrap(color);
	}
	m_inner->set_render_targets(std::move(colors), unwrap(depth_stencil));
}

void rnd::driver::validation::driver::check_rect(rule kind, const char* what, glm::ivec4 rect)
{
	const target_info& target = m_state->current_target();
	if (!target.has_extent) {
		return;
	}
	const bool inside = rect.x >= 0 && rect.y >= 0 && rect.z >= 0 && rect.w >= 0
		&& static_cast<std::int64_t>(rect.x) + rect.z <= target.width
		&& static_cast<std::int64_t>(rect.y) + rect.w <= target.height;
	if (!inside) {
		m_state->report(kind, std::format("{} {{{}, {}, {}, {}}} is outside the {}x{} render target",
			what, rect.x, rect.y, rect.z, rect.w, target.width, target.height));
	}
}

void rnd::driver::validation::driver::set_viewport(glm::ivec4 rect)
{
	m_state->note(trace_op::set_viewport);
	check_rect(rule::viewport_outside_target, "viewport", rect);
	m_inner->set_viewport(rect);
}

void rnd::driver::validation::driver::set_scissor(glm::ivec4 rect)
{
	m_state->note(trace_op::set_scissor);
	if (rect.z >= 1 && rect.w >= 1) {
		check_rect(rule::scissor_outside_target, "scissor", rect);
	}
	m_inner->set_scissor(rect);
}

void rnd::driver::validation::driver::clear(clear_flags flags)
{
	m_state->note(trace_op::clear);
	m_inner->clear(flags);
}

void rnd::driver::validation::driver::clear(clear_flags flags, glm::vec4 color)
{
	m_state->note(trace_op::clear);
	m_inner->clear(flags, color);
}

void rnd::driver::validation::driver::clear(clear_flags flags, const std::vector<glm::vec4>& colors)
{
	m_state->note(trace_op::clear);
	m_inner->clear(flags, colors);
}

const rnd::driver::validation::vertex_array* rnd::driver::validation::driver::check_draw(
	const vertex_array_interface* va, unsigned int count, std::size_t base_index)
{
	if (!m_state->require_live(va, object_kind::vertex_array, "draw vertex array")) {
		return nullptr;
	}
	const auto* wrapped = static_cast<const vertex_array*>(va);
	m_state->note(trace_op::draw, count);
	m_state->on_draw();
	if (!m_state->program_used()) {
		m_state->report(rule::draw_without_use, "draw before any shader use() in this frame");
	}
	if (!wrapped->has_layout()) {
		m_state->report(rule::draw_without_layout, "draw with a vertex array that has no vertex buffer layout");
	}
	if (count + base_index > wrapped->index_capacity()) {
		m_state->report(rule::draw_index_range, std::format("draw of {} indices from index {} exceeds the index buffer of {} indices",
			count, base_index, wrapped->index_capacity()));
	}
	return wrapped;
}

void rnd::driver::validation::driver::draw_indices(const std::unique_ptr<vertex_array_interface>& vertices, RENDER_MODE render_mode, unsigned int count, unsigned int offset)
{
	if (const vertex_array* wrapped = check_draw(vertices.get(), count, 0)) {
		m_inner->draw_indices(wrapped->inner(), render_mode, count, offset);
	}
}

void rnd::driver::validation::driver::draw_indices(const vertex_array_interface* va, RENDER_MODE rm, unsigned int count, unsigned int base_vertex, unsigned int base_index)
{
	if (const vertex_array* wrapped = check_draw(va, count, base_index)) {
		m_inner->draw_indices(wrapped->inner().get(), rm, count, base_vertex, base_index);
	}
}

std::unique_ptr<rnd::driver::shader_interface> rnd::driver::validation::driver::create_shader(const std::vector<shader_header>& headers)
{
	auto created = m_inner->create_shader(headers);
	if (!created) {
		return nullptr;
	}
	m_state->on_program_created();
	return std::make_unique<shader>(m_state, std::move(created));
}

std::unique_ptr<rnd::driver::texture_interface> rnd::driver::validation::driver::create_texture(const texture_header& header)
{
	auto created = m_inner->create_texture(header);
	if (!created) {
		return nullptr;
	}
	m_state->on_texture_created();
	return std::make_unique<texture>(m_state, std::move(created), header);
}

std::unique_ptr<rnd::driver::vertex_array_interface> rnd::driver::validation::driver::create_vertex_array()
{
	return std::make_unique<vertex_array>(m_state, m_inner->create_vertex_array());
}

std::unique_ptr<rnd::driver::buffer_interface> rnd::driver::validation::driver::create_buffer()
{
	return std::make_unique<buffer>(m_state, std::shared_ptr<buffer_interface>(m_inner->create_buffer()));
}

std::unique_ptr<rnd::driver::uniform_buffer_interface> rnd::driver::validation::driver::create_uniform_buffer(std::size_t size, std::size_t binding)
{
	return std::make_unique<uniform_buffer>(m_state, m_inner->create_uniform_buffer(size, binding), size);
}

std::unique_ptr<rnd::driver::ssbo_buffer_interface> rnd::driver::validation::driver::create_ssbo_buffer(std::size_t size, std::size_t binding)
{
	return std::make_unique<ssbo_buffer>(m_state, m_inner->create_ssbo_buffer(size, binding), size);
}

rnd::driver::validation::swapchain::swapchain(std::unique_ptr<swapchain_interface> inner, limits budget)
	: m_inner(std::move(inner))
	, m_state(std::make_shared<validator>(budget))
{
}

std::unique_ptr<rnd::driver::driver_interface> rnd::driver::validation::swapchain::create_driver()
{
	return std::make_unique<driver>(m_inner->create_driver(), m_state);
}

void rnd::driver::validation::swapchain::present()
{
	m_state->on_present();
	m_inner->present();
}
