#pragma once
#include "rnd_validation_validator.h"
#include <snk/driver/rnd_shader_interface.h>
#include <snk/driver/rnd_texture_interface.h>
#include <snk/driver/rnd_vertex_array_interface.h>
#include <snk/driver/rnd_buffer_interface.h>
#include <snk/common/snk_api.h>

#include <cstddef>
#include <memory>
#include <vector>

// Wrappers the validating driver hands out instead of the wrapped driver's objects. Each registers itself
// as live for as long as it exists and forwards to the object it wraps.
namespace rnd::driver::validation
{
	namespace detail
	{
		// Keeps one object in the live registry.
		class registration
		{
		public:
			registration(std::shared_ptr<validator> owner, const void* object, object_kind kind)
				: m_owner(std::move(owner)), m_object(object)
			{
				m_owner->add_object(object, kind);
				m_owner->note(trace_op::create_object, static_cast<std::uint32_t>(kind));
			}
			registration(const registration&) = delete;
			registration& operator=(const registration&) = delete;
			~registration() { m_owner->remove_object(m_object); }

		private:
			std::shared_ptr<validator> m_owner;
			const void* m_object;
		};
	}

	class SNK_API shader final : public shader_interface
	{
	public:
		shader(std::shared_ptr<validator> owner, std::unique_ptr<shader_interface> inner);

		const shader_interface& inner() const { return *m_inner; }

	private:
		std::unique_ptr<shader_interface> m_inner;
		detail::registration m_registration;
	};

	class SNK_API texture final : public texture_interface
	{
	public:
		// `inner` is the texture of the wrapped driver, which owns it; `inner_handle` names it there. The wrapper
		// never outlives it: the validation driver destroys the wrapper first and then the inner texture.
		texture(std::shared_ptr<validator> owner, texture_interface& inner, texture_handle inner_handle, const texture_header& hdr);

		texture_interface* native() override { return m_inner->native(); }
		texture_interface& inner() { return *m_inner; }
		// The handle the wrapped driver gave the texture.
		texture_handle handle_in_inner() const { return m_inner_handle; }

		void bind(uint32_t position) override;
		void unbind(uint32_t position) override { m_inner->unbind(position); }
		void set_filtering(texture_header::FILTERING min, texture_header::FILTERING mag) override;
		void set_wrapping(texture_header::WRAPPING wrap_s, texture_header::WRAPPING wrap_t, texture_header::WRAPPING wrap_r) override
		{
			m_inner->set_wrapping(wrap_s, wrap_t, wrap_r);
		}
		void update_data(const void* data, uint32_t x_offset, uint32_t y_offset, uint32_t z_offset,
			uint32_t width, uint32_t height, uint32_t depth, uint32_t level, uint32_t layer, uint32_t face) override
		{
			m_inner->update_data(data, x_offset, y_offset, z_offset, width, height, depth, level, layer, face);
		}
		void generate_mipmaps() override { m_inner->generate_mipmaps(); }

	private:
		std::shared_ptr<validator> m_owner;
		texture_interface* m_inner;
		texture_handle m_inner_handle;
		detail::registration m_registration;
	};

	class SNK_API buffer final : public buffer_interface
	{
	public:
		buffer(std::shared_ptr<validator> owner, std::shared_ptr<buffer_interface> inner);

		void set_data(const void* data, size_t size, BUFFER_BINDING binding) override;
		const BufferLayout& get_layout() const override { return m_inner->get_layout(); }
		void set_layout(const BufferLayout& layout_) override { m_inner->set_layout(layout_); }

		// Bytes allocated, with the wrapped buffer's rule: the first call or a null `data` allocates, later calls overwrite.
		std::size_t size_bytes() const { return m_size; }
		// Shared with the wrapped vertex array, which keeps its buffers alive.
		const std::shared_ptr<buffer_interface>& inner() const { return m_inner; }

	private:
		std::shared_ptr<validator> m_owner;
		std::shared_ptr<buffer_interface> m_inner;
		std::size_t m_size = 0;
		bool m_allocated = false;
		detail::registration m_registration;
	};

	class SNK_API vertex_array final : public vertex_array_interface
	{
	public:
		// Indices are 32-bit, as every driver draws them.
		static constexpr std::size_t index_size = sizeof(std::uint32_t);

		vertex_array(std::shared_ptr<validator> owner, std::unique_ptr<vertex_array_interface> inner);

		void bind() const override { m_inner->bind(); }
		void unbind() const override { m_inner->unbind(); }
		void add_vertex_buffer(const std::shared_ptr<buffer_interface>& vertexBuffer) override;
		void set_index_buffer(const std::shared_ptr<buffer_interface>& indexBuffer) override;

		std::size_t index_capacity() const;
		// A vertex buffer with a non-empty layout was attached (the wrapped driver reads the layout on attach).
		bool has_layout() const { return m_has_layout; }
		const std::unique_ptr<vertex_array_interface>& inner() const { return m_inner; }

	private:
		std::shared_ptr<validator> m_owner;
		std::unique_ptr<vertex_array_interface> m_inner;
		std::vector<std::shared_ptr<buffer>> m_vertex_buffers;
		std::shared_ptr<buffer> m_index_buffer;
		bool m_has_layout = false;
		detail::registration m_registration;
	};
}
