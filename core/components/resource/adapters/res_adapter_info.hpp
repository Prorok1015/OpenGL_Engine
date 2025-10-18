#pragma once
#include "ds_type_id.hpp"
#include <string>
#include <vector>
#include <initializer_list>

namespace res
{
	struct resource_info
	{
		ds::type_id resource_type;
		std::string extension;
		auto operator<=>(const resource_info&) const = default;
		template<class T>
		static resource_info make(const std::string_view ext) {
			return resource_info{ ds::type_id::make<T>(), std::string{ext} };
		}
	};

	struct adapter_info
	{
		ds::type_id resource_type;
		std::vector<std::string> extensions;

		bool operator==(const adapter_info&) const = default;
		bool operator==(const resource_info& other) const {
			return resource_type == other.resource_type && (extensions.empty() ||
				std::find(extensions.begin(), extensions.end(), other.extension) != extensions.end());
		}

		template<class T>
		static adapter_info make(std::initializer_list<std::string> exts = {}) {
			return adapter_info{ ds::type_id::make<T>(), std::vector<std::string>(exts)};
		}
	};
}

namespace std {
	template <>
	struct hash<res::adapter_info>
	{
		std::size_t operator()(const res::adapter_info& info) const noexcept
		{
			return std::hash<ds::type_id>{}(info.resource_type);
		}
	};
	template <>
	struct hash<res::resource_info>
	{
		std::size_t operator()(const res::resource_info& info) const noexcept
		{
			return std::hash<ds::type_id>{}(info.resource_type);
		}
	};
}