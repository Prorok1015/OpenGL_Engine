#pragma once
#include "common.h"
#include "res_resource_base.h"

namespace res
{
	class text_resource : public resource_entry
	{
	public:
		text_resource(const tag& tag, const std::vector<std::byte>& string);

		const char* c_str() const { return content.c_str(); }
		const std::string& str() const { return content; }

	protected:
		std::string content;
	};
}
