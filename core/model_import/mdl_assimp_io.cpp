#include "mdl_assimp_io.h"
#include <snk/common/logger/engine_log.h>
#include <assimp/MemoryIOWrapper.h>
#include <cstdint>

bool mdl::detail::source_io::Exists(const char* file) const
{
	return m_files.exists(file);
}

Assimp::IOStream* mdl::detail::source_io::Open(const char* file, const char* mode)
{
	static_cast<void>(mode);
	const opened_file opened = m_files.open(file);
	if (!opened.bytes) {
		if (!opened.reason.empty()) {
			egLOG_WARN("import", "model '{}' refers to '{}', which is not read: {}", logged(m_display_name), logged(file), opened.reason);
		}
		return nullptr;
	}
	return new Assimp::MemoryIOStream(reinterpret_cast<const std::uint8_t*>(opened.bytes->data()), opened.bytes->size());
}

void mdl::detail::source_io::Close(Assimp::IOStream* file)
{
	delete file;
}
