#pragma once
#include "common.h"
#include "res_system.h"
#include "res_tag.h"
#include "desc_base.hpp"
#include "resources/desc_resource.h"
#include "adapters/desc_adapter_t.h"
#include <vector>

namespace desc
{
	class desc_system
	{
	public:
		using desc_name = res::tag;
		using desc_namespace = std::string;

		desc_system(res::resource_system& res_system);
		~desc_system() = default;
		desc_system(const desc_system&) = delete;
		desc_system& operator=(const desc_system&) = delete;
		desc_system(desc_system&&) = delete;
		desc_system& operator=(desc_system&&) = delete;

		template<typename T>
		void register_desc2(const std::string_view type_name)
		{
			factory_map[std::string{ type_name }] = []() -> std::shared_ptr<desc::desc_base> { return std::make_shared<T>(); };
		}

		void unregister_desc2(const std::string_view type_name)
		{
			factory_map.erase(std::string{ type_name });
		}

		std::shared_ptr<desc::desc_base> create_instance(const std::string& type_id, const res::tag& tag) const
		{
			auto it = factory_map.find(type_id);
			if (it == factory_map.end()) {
				egLOG("desc/error", "Failed to create desc instance of type '{0}': type not registered", type_id);
				return nullptr;
			}
			auto instance = it->second();
			instance->set_tag(tag);
			return instance;
		}

		template<typename T>
		auto get_or_override_desc2(const desc::desc_base& owner, const json::value& data) const
		{
			if (data.is_string()) {
				return m_res_system.require_sync<T>(json::value_to<res::tag>(data));
			}

			res::tag mem_tag = make_mem_tag(owner);
			m_res_system.store(mem_tag, serialize_to_bytes(data));
			return m_res_system.require_sync<T>(mem_tag);
		}

		std::vector<std::byte> serialize_to_bytes(const json::value& data) const
		{
			std::string str = json::serialize(data);
			std::vector<std::byte> bytes(str.size());
			std::memcpy(bytes.data(), str.data(), str.size());
			return bytes;
		}

		res::tag make_mem_tag(const desc::desc_base& owner) const
		{
			static std::atomic<uint64_t> override_id{ 0 };

			std::string_view owner_name = owner.get_tag().pure_name();
			std::string path = std::format("memory://overrides/{}_{}.desc",
				owner_name,
				override_id.fetch_add(1, std::memory_order_relaxed));

			return res::tag{ path };
		}

	private:
		res::resource_system& m_res_system;
		std::unordered_map<std::string, std::function<std::shared_ptr<desc::desc_base>()>> factory_map;
	};
}
