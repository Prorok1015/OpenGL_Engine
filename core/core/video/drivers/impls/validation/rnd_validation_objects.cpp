#include "rnd_validation_objects.h"
#include <format>

namespace {
	using namespace rnd::driver;

	bool fits(std::size_t capacity, std::size_t size, std::size_t offset)
	{
		return offset <= capacity && size <= capacity - offset;
	}

	// Reports a write past the end of a fixed-size buffer; true when the write may go ahead.
	bool check_write(validation::validator& owner, const char* what, std::size_t capacity, std::size_t size, std::size_t offset)
	{
		if (fits(capacity, size, offset)) {
			return true;
		}
		owner.report(validation::rule::buffer_write_range,
			std::format("{} set_data of {} bytes at offset {} exceeds its size {}", what, size, offset, capacity));
		return false;
	}
}

rnd::driver::validation::shader::shader(std::shared_ptr<validator> owner, std::unique_ptr<shader_interface> inner)
	: m_owner(owner)
	, m_inner(std::move(inner))
	, m_registration(owner, this, object_kind::shader)
{
}

void rnd::driver::validation::shader::use()
{
	m_owner->on_program_used();
	m_owner->note(trace_op::use_program);
	m_inner->use();
}

rnd::driver::validation::texture::texture(std::shared_ptr<validator> owner, std::unique_ptr<texture_interface> inner, const texture_header& hdr)
	: texture_interface(hdr)
	, m_owner(owner)
	, m_inner(std::move(inner))
	, m_registration(owner, this, object_kind::texture)
{
}

void rnd::driver::validation::texture::bind(uint32_t position)
{
	m_owner->note(trace_op::bind_texture, position);
	if (!has_usage(texture_usage::sampled)) {
		m_owner->report(rule::texture_not_sampled, std::format("{}x{} texture with usage {} bound to unit {} for sampling",
			width(), height(), usage_text(get_usage()), position));
	}
	m_inner->bind(position);
}

void rnd::driver::validation::texture::set_filtering(texture_header::FILTERING min, texture_header::FILTERING mag)
{
	header.min = min;
	header.mag = mag;
	m_inner->set_filtering(min, mag);
}

rnd::driver::validation::buffer::buffer(std::shared_ptr<validator> owner, std::shared_ptr<buffer_interface> inner)
	: m_owner(owner)
	, m_inner(std::move(inner))
	, m_registration(owner, this, object_kind::buffer)
{
}

void rnd::driver::validation::buffer::set_data(const void* data, size_t size, BUFFER_BINDING binding)
{
	if (!(data && m_allocated)) {
		m_allocated = true;
		m_size = size;
	}
	m_inner->set_data(data, size, binding);
}

rnd::driver::validation::vertex_array::vertex_array(std::shared_ptr<validator> owner, std::unique_ptr<vertex_array_interface> inner)
	: m_owner(owner)
	, m_inner(std::move(inner))
	, m_registration(owner, this, object_kind::vertex_array)
{
}

void rnd::driver::validation::vertex_array::add_vertex_buffer(const std::shared_ptr<buffer_interface>& vertexBuffer)
{
	auto wrapped = std::dynamic_pointer_cast<buffer>(vertexBuffer);
	if (!wrapped) {
		m_owner->report(rule::dead_object, "vertex buffer was not created by the validating driver");
		m_inner->add_vertex_buffer(vertexBuffer);
		return;
	}
	m_has_layout = m_has_layout || !wrapped->get_layout().get_elements().empty();
	m_vertex_buffers.push_back(wrapped);
	m_inner->add_vertex_buffer(wrapped->inner());
}

void rnd::driver::validation::vertex_array::set_index_buffer(const std::shared_ptr<buffer_interface>& indexBuffer)
{
	auto wrapped = std::dynamic_pointer_cast<buffer>(indexBuffer);
	if (!wrapped) {
		m_owner->report(rule::dead_object, "index buffer was not created by the validating driver");
		m_inner->set_index_buffer(indexBuffer);
		return;
	}
	m_index_buffer = wrapped;
	m_inner->set_index_buffer(wrapped->inner());
}

std::size_t rnd::driver::validation::vertex_array::index_capacity() const
{
	return m_index_buffer ? m_index_buffer->size_bytes() / index_size : 0;
}

rnd::driver::validation::uniform_buffer::uniform_buffer(std::shared_ptr<validator> owner, std::unique_ptr<uniform_buffer_interface> inner, std::size_t size)
	: m_owner(owner)
	, m_inner(std::move(inner))
	, m_size(size)
	, m_registration(owner, this, object_kind::uniform_buffer)
{
}

void rnd::driver::validation::uniform_buffer::set_data(const void* data, std::size_t size, std::size_t offset)
{
	if (check_write(*m_owner, "uniform buffer", m_size, size, offset)) {
		m_inner->set_data(data, size, offset);
	}
}

rnd::driver::validation::ssbo_buffer::ssbo_buffer(std::shared_ptr<validator> owner, std::unique_ptr<ssbo_buffer_interface> inner, std::size_t size)
	: m_owner(owner)
	, m_inner(std::move(inner))
	, m_size(size)
	, m_registration(owner, this, object_kind::ssbo_buffer)
{
}

void rnd::driver::validation::ssbo_buffer::set_data(const void* data, std::size_t size, std::size_t offset)
{
	if (check_write(*m_owner, "ssbo buffer", m_size, size, offset)) {
		m_inner->set_data(data, size, offset);
	}
}

void rnd::driver::validation::ssbo_buffer::bind(std::size_t index)
{
	m_owner->note(trace_op::bind_ssbo, static_cast<std::uint32_t>(index));
	m_inner->bind(index);
}
