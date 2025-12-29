#pragma once
#include "res_tag.h"
#include "res_resolver_interface.hpp"
#include <future>
#include <vector>
#include <unordered_map>
#include <cstddef>
#include <filesystem>

namespace res
{
	//TODO: split to memory_storage and memory_resolver
	class memory_resolver : public core::res::res_resolver_interface
	{
	public:
		std::future<std::vector<std::byte>> operator()(const tag& tag)
		{
			std::promise<std::vector<std::byte>> promise;


			promise.set_value(m_memory[tag]);
			return promise.get_future();
		}

		void add_memory_resource(const tag& tag, std::vector<std::byte> data)
		{
			m_memory[tag] = std::move(data);
		}

		void remove_memory(const tag& tag)
		{
			m_memory.erase(tag);
		}

		bool is_exist(const tag& tag) const
		{
			return m_memory.contains(tag);
		}

		std::optional<res::tag> path_mapper(const std::filesystem::path& path) const
		{
			return std::nullopt;
		}

		virtual core::res::res_resolver_interface::async_raw_data resolve(const res::tag& tag) const override;

	private:
		std::unordered_map<tag, std::vector<std::byte>> m_memory; 
	};
}