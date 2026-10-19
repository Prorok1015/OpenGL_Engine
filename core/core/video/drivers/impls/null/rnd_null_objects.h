#pragma once
#include <snk/driver/rnd_shader_interface.h>
#include <snk/driver/rnd_texture_interface.h>
#include <snk/driver/rnd_vertex_array_interface.h>
#include <snk/driver/rnd_buffer_interface.h>
#include <snk/driver/rnd_driver_interface.h>
#include <snk/driver/rnd_framebuffer_cache.h>
#include <snk/common/snk_api.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rnd::driver::null
{
	struct object_counts
	{
		std::size_t created = 0;
		std::size_t destroyed = 0;

		std::size_t live() const { return created - destroyed; }
	};

	// Shared with the driver: objects may outlive it, so they keep the counters alive themselves.
	struct object_stats
	{
		object_counts shaders;
		object_counts textures;
		object_counts vertex_arrays;
		object_counts buffers;
		// The framebuffers begin_render_pass built, by attachment set: the model of what a GPU backend caches.
		framebuffer_cache<std::uint32_t> framebuffers;

		std::size_t live_total() const
		{
			return shaders.live() + textures.live() + vertex_arrays.live() + buffers.live();
		}
	};

	namespace detail
	{
		// Counts one object of a kind for as long as it lives.
		class lifetime_token
		{
		public:
			lifetime_token(std::shared_ptr<object_stats> stats, object_counts object_stats::* kind)
				: m_stats(std::move(stats)), m_kind(kind)
			{
				++((*m_stats).*m_kind).created;
			}
			lifetime_token(const lifetime_token&) = delete;
			lifetime_token& operator=(const lifetime_token&) = delete;
			~lifetime_token() { ++((*m_stats).*m_kind).destroyed; }

		private:
			std::shared_ptr<object_stats> m_stats;
			object_counts object_stats::* m_kind;
		};
	}

	// Empty string when `headers` can make a program; otherwise the reason it cannot.
	SNK_API std::string validate_shader_headers(const std::vector<shader_header>& headers);

	class SNK_API shader final : public shader_interface
	{
	public:
		shader(std::shared_ptr<object_stats> stats, std::vector<shader_header> headers)
			: m_token(std::move(stats), &object_stats::shaders), m_headers(std::move(headers)) {}

		const std::vector<shader_header>& headers() const { return m_headers; }

	private:
		detail::lifetime_token m_token;
		std::vector<shader_header> m_headers;
	};

	class SNK_API texture final : public texture_interface
	{
	public:
		texture(std::shared_ptr<object_stats> stats, const texture_header& hdr)
			: texture_interface(hdr), m_stats(stats), m_token(std::move(stats), &object_stats::textures) {}
		~texture() override
		{
			m_stats->framebuffers.evict(framebuffer_object(), [](std::uint32_t&) {});
		}

		// Identity in a framebuffer cache key.
		std::uint64_t framebuffer_object() const { return reinterpret_cast<std::uintptr_t>(this); }

		void bind(uint32_t /*position*/) override { ++m_bind_calls; }
		void unbind(uint32_t /*position*/) override {}
		void set_filtering(texture_header::FILTERING min, texture_header::FILTERING mag) override
		{
			header.min = min;
			header.mag = mag;
		}
		void set_wrapping(texture_header::WRAPPING /*wrap_s*/, texture_header::WRAPPING /*wrap_t*/,
			texture_header::WRAPPING /*wrap_r*/) override {}
		void update_data(const void* /*data*/, uint32_t /*x_offset*/, uint32_t /*y_offset*/, uint32_t /*z_offset*/,
			uint32_t /*width*/, uint32_t /*height*/, uint32_t /*depth*/, uint32_t /*level*/,
			uint32_t /*layer*/, uint32_t /*face*/) override { ++m_update_calls; }
		void generate_mipmaps() override { ++m_mipmap_calls; }

		const texture_header& get_header() const { return header; }
		std::size_t bind_calls() const { return m_bind_calls; }
		std::size_t update_calls() const { return m_update_calls; }
		std::size_t mipmap_calls() const { return m_mipmap_calls; }

	private:
		std::shared_ptr<object_stats> m_stats;
		detail::lifetime_token m_token;
		std::size_t m_bind_calls = 0;
		std::size_t m_update_calls = 0;
		std::size_t m_mipmap_calls = 0;
	};

	class SNK_API buffer final : public buffer_interface
	{
	public:
		explicit buffer(std::shared_ptr<object_stats> stats)
			: m_token(std::move(stats), &object_stats::buffers) {}

		void set_data(const void* data, size_t size, BUFFER_BINDING binding) override;
		const BufferLayout& get_layout() const override { return m_layout; }
		void set_layout(const BufferLayout& layout_) override { m_layout = layout_; }

		std::size_t size() const { return m_bytes.size(); }
		const std::vector<std::byte>& bytes() const { return m_bytes; }
		BUFFER_BINDING binding() const { return m_binding; }

	private:
		detail::lifetime_token m_token;
		BufferLayout m_layout;
		std::vector<std::byte> m_bytes;
		BUFFER_BINDING m_binding = BUFFER_BINDING::DYNAMIC;
		bool m_allocated = false;
	};

	class SNK_API vertex_array final : public vertex_array_interface
	{
	public:
		explicit vertex_array(std::shared_ptr<object_stats> stats)
			: m_token(std::move(stats), &object_stats::vertex_arrays) {}

		void bind() const override {}
		void unbind() const override {}
		void add_vertex_buffer(const std::shared_ptr<buffer_interface>& vertexBuffer) override { m_vertex_buffers.push_back(vertexBuffer); }
		void set_index_buffer(const std::shared_ptr<buffer_interface>& indexBuffer) override { m_index_buffer = indexBuffer; }

		const std::vector<std::shared_ptr<buffer_interface>>& vertex_buffers() const { return m_vertex_buffers; }
		const std::shared_ptr<buffer_interface>& index_buffer() const { return m_index_buffer; }

	private:
		detail::lifetime_token m_token;
		std::vector<std::shared_ptr<buffer_interface>> m_vertex_buffers;
		std::shared_ptr<buffer_interface> m_index_buffer;
	};
}
