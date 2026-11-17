#include "rnd_null_objects.h"
#include <snk/common/engine_assert.h>
#include <cstring>

namespace {
	bool starts_with_version(const std::string& body)
	{
		const auto first = body.find_first_not_of(" \t\r\n");
		return first != std::string::npos && body.compare(first, 8, "#version") == 0;
	}

	// Writes `size` bytes of `data` at `offset`; a null `data` only checks the range.
	bool write_range(std::vector<std::byte>& bytes, const void* data, std::size_t size, std::size_t offset)
	{
		if (!ENGINE_VERIFY(offset <= bytes.size() && size <= bytes.size() - offset,
			"null driver: write of {} bytes at offset {} exceeds the buffer size {}", size, offset, bytes.size())) {
			return false;
		}
		if (data && size > 0) {
			std::memcpy(bytes.data() + offset, data, size);
		}
		return true;
	}
}

std::string rnd::driver::null::validate_shader_headers(const std::vector<shader_header>& headers)
{
	if (headers.empty()) {
		return "no shader stages";
	}
	for (const auto& header : headers) {
		if (header.body.find_first_not_of(" \t\r\n") == std::string::npos) {
			return "stage '" + header.title + "' is empty";
		}
		if (!starts_with_version(header.body)) {
			return "stage '" + header.title + "' does not start with #version";
		}
	}
	return {};
}

// Mirrors the GL buffer: the first call (or a null `data`) allocates, later calls overwrite in place.
void rnd::driver::null::buffer::set_data(const void* data, size_t size, BUFFER_BINDING binding)
{
	if (data && m_allocated) {
		write_range(m_bytes, data, size, 0);
		return;
	}
	m_allocated = true;
	m_binding = binding;
	m_bytes.assign(size, std::byte{});
	if (data && size > 0) {
		std::memcpy(m_bytes.data(), data, size);
	}
}
