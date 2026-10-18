#include "desc_guid.h"
#include <cstdint>
#include <random>

std::string desc::generate_guid()
{
	thread_local std::mt19937_64 engine{ std::random_device{}() };
	std::uniform_int_distribution<std::uint32_t> nibble(0, 15);

	constexpr const char* hex = "0123456789abcdef";
	std::string guid(36, '-');
	for (std::size_t i = 0; i < guid.size(); ++i) {
		if (i == 8 || i == 13 || i == 18 || i == 23) {
			continue;
		}
		guid[i] = hex[nibble(engine)];
	}
	guid[14] = '4';                       // version
	guid[19] = hex[8 + nibble(engine) % 4]; // variant: 8, 9, a or b
	return guid;
}
