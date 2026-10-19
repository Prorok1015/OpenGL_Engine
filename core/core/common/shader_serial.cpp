#include <snk/common/shader_serial.h>

#include <atomic>

std::uint64_t rnd::driver::next_shader_serial() noexcept
{
	static std::atomic<std::uint64_t> counter{ 0 };
	return ++counter;
}
