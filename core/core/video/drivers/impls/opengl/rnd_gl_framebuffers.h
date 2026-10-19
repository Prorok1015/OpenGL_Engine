#pragma once
#include <snk/driver/rnd_framebuffer_cache.h>
#include <glad/glad.h>

namespace rnd::driver::gl
{
	// The driver's framebuffers by attachment set. Shared with every texture the driver creates: a texture
	// that is destroyed evicts the framebuffers built from it, so the cache never holds a dead attachment
	// or matches a recycled texture name. Textures may outlive the driver; the driver empties it on exit.
	class framebuffer_registry
	{
	public:
		framebuffer_registry() = default;
		framebuffer_registry(const framebuffer_registry&) = delete;
		framebuffer_registry& operator=(const framebuffer_registry&) = delete;
		~framebuffer_registry();

		framebuffer_cache<GLuint> cache;

		void evict_texture(std::uint64_t object);
		void release_all();
	};
}
