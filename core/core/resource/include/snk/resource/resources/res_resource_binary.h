#pragma once
#include <snk/resource/resources/res_resource_base.h>
#include <snk/common/snk_api.h>
#include <cstddef>
#include <vector>

namespace res
{
	// The bytes of a file as they are, for formats the engine does not decode itself (e.g. a TTF handed to ImGui).
	class SNK_API binary_resource : public resource_entry
	{
	public:
		binary_resource(const tag& tag, std::vector<std::byte> bytes);

		const std::vector<std::byte>& bytes() const { return m_bytes; }

	protected:
		std::vector<std::byte> m_bytes;
	};
}
