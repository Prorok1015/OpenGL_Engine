#pragma once
#include "ds/ds_type_id.hpp"
#include <string>
#include <vector>
#include <initializer_list>

namespace res
{
	struct adapter_info
	{
		ds::type_id resource_type;
		std::vector<std::string> extensions;
		uint32_t magic_number = 0;

		bool operator==(const adapter_info&) const = default;

		template<class T>
		static adapter_info make(std::initializer_list<std::string> exts = {}, uint32_t magic = 0) {
			return adapter_info{ ds::type_id::make<T>(), std::vector<std::string>(exts), magic};
		}
	};
}