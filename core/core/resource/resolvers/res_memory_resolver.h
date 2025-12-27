#pragma once
#include "res_tag.h"
#include <future>
#include <vector>
#include <unordered_map>
#include <cstddef>
#include <filesystem>

namespace res
{
	//TODO: split to memory_storage and memory_resolver
	class memory_resolver
	{
	public:
		std::future<std::vector<std::byte>> operator()(const tag& tag)
		{
			std::promise<std::vector<std::byte>> promise;


			promise.set_value(memory_[tag]);
			return promise.get_future();
		}

		void add_memory_resource(const tag& tag, std::vector<std::byte> data)
		{
			memory_[tag] = std::move(data);
		}

		void remove_memory(const tag& tag)
		{
			memory_.erase(tag);
		}

		bool is_exist(const tag& tag) const
		{
			return memory_.find(tag) != memory_.end();
		}

		std::optional<res::tag> path_mapper(const std::filesystem::path& path) const
		{
			return std::nullopt;
		}

	private:
		std::unordered_map<tag, std::vector<std::byte>> memory_; 
	};
}