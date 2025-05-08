#pragma once
#include "common.h"
#include "res_system.h"
#include "res_tag.h"
#include "desc_base.hpp"
#include "desc_resource.hpp"

namespace std {
	template<> struct hash<std::pair<ds::Type, std::string>> {
		std::size_t operator()(const std::pair<ds::Type, std::string>& k) const {
			return std::hash<ds::Type>{}(k.first) ^ std::hash<std::string>{}(k.second);
		}
	};
}

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
		void register_desc(const desc_name& name, desc_namespace nms = {})
		{
			if (name_map.find(name) != name_map.end()) {
				ASSERT_FAIL("desc with name already registered");
				return;
			}

			auto type = std::pair{ ds::Type::make<T>(), nms };
			if (desc_map.find(type) != desc_map.end()) {
				ASSERT_FAIL("desc with type already registered");
				return;
			}

			auto desc = std::make_shared<T>();
			desc_map[type] = desc;
			name_map[name] = type;
			descs_for_load.push_back(name);
		}

		template<typename T>
		void unregister_desc(desc_namespace nms = {})
		{
			auto type = std::pair{ ds::Type::make<T>(), nms };
			auto it = desc_map.find(type);
			if (it != desc_map.end()) {
				desc_map.erase(it);
			}

			for (auto it = name_map.begin(); it != name_map.end(); ++it) {
				if (it->second == type) {
					name_map.erase(it);
					break;
				}
			}
		}

		void finish_descs()
		{
			for (auto& tag : descs_for_load)
			{
				if (name_map.find(tag) == name_map.end()) {
					continue;
				}

				auto type = name_map[tag];
				auto it = desc_map.find(type);
				if (it != desc_map.end()) {
					auto desc = res_system.require_resource2<desc::desc_resource>(tag);
					it->second->deserialize(*this, desc->body);
					it->second->set_is_loaded();
					it->second->set_tag(tag);
				}
			}

			descs_for_load.clear();
		}

		template<typename T>
		std::shared_ptr<T> get_desc(desc_namespace nms = {})
		{
			auto it = desc_map.find(std::pair{ ds::Type::make<T>(), nms });
			if (it == desc_map.end()) {
				return nullptr;
			}
			return std::static_pointer_cast<T>(it->second);
		}

		template<typename T>
		std::shared_ptr<T> get_desc(const desc_name& name)
		{
			auto it = name_map.find(name);
			if (it == name_map.end()) {
				return nullptr;
			}
			auto& type = it->second;
			auto it2 = desc_map.find(type);
			if (it2 == desc_map.end()) {
				return nullptr;
			}
			return std::static_pointer_cast<T>(it2->second);
		}

	private:
		res::resource_system& res_system;
		std::vector<desc_name> descs_for_load;
		std::unordered_map<desc_name, std::pair<ds::Type, desc_namespace>> name_map;
		std::unordered_map<std::pair<ds::Type, desc_namespace>, std::shared_ptr<desc_base>> desc_map;
	};

}
