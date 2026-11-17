#include <snk/common/shader_serial.h>

#include <atomic>

std::uint64_t rnd::driver::next_shader_serial() noexcept
{
	static std::atomic<std::uint64_t> counter{ 0 };
	return ++counter;
}

std::uint32_t rnd::driver::next_texture_driver_id() noexcept
{
	static std::atomic<std::uint32_t> counter{ 0 };
	return counter.fetch_add(1, std::memory_order_relaxed) % 255u + 1u;
}
