#pragma once
#include <future>
#include <vector>
#include <string>
#include <filesystem>
#include <cstddef>
#include "res_tag.h"

namespace res
{
	class resource_resolver
	{
	public:
		std::future<std::vector<std::byte>> operator()(const tag& tag) const;
		resource_resolver(std::vector<std::string> entry_points_ = {})
			: entry_points(std::move(entry_points_)) {
		}

	private:
		std::filesystem::path resolve_tag(const tag& tag) const;

	private:
		std::vector<std::string> entry_points;
	};
}

