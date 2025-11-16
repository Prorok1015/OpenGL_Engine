#pragma once
#include <rnd_vertex_array_interface.h>
#include <rnd_gl_buffer.h>
#include <vector>
#include <memory>

namespace rnd::driver::gl
{
	class vertex_array : public rnd::driver::vertex_array_interface
	{
	public:
		vertex_array();
		virtual ~vertex_array() override;

		virtual void bind() const override;
		virtual void unbind() const override;

		virtual void add_vertex_buffer(const std::shared_ptr<rnd::driver::buffer_interface>& vertexBuffer) override;
		virtual void remove_vertex_buffer(const std::shared_ptr<rnd::driver::buffer_interface>& vertexBuffer) override;
		virtual void set_index_buffer(const std::shared_ptr<rnd::driver::buffer_interface>& indexBuffer) override;

		const std::vector<std::shared_ptr<buffer>>& get_vertex_buffers() const { return m_VertexBuffers; }
		const std::shared_ptr<buffer>& get_index_buffer() const { return m_IndexBuffer; }
		uint32_t vertex_array_id;
	private:
		uint32_t m_VertexBufferIndex = 0;
		std::vector<std::shared_ptr<buffer>> m_VertexBuffers;
		std::shared_ptr<buffer> m_IndexBuffer;
	};
}