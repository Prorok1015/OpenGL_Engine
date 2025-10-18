#pragma once
#include "common.h"
#include "res_resource_base.h"

namespace res
{
	class TextFile : public Resource
	{
	public:
		TextFile(const tag& tag);
		TextFile(const tag& tag, const std::vector<std::byte>& string);

		const char* c_str() const { return body_.c_str(); }

	protected:
		std::string body_;
	};
}
