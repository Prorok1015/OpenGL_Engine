#pragma once
#include "common.h"
#include "res_system.h"
#include "res_tag.h"
#include "desc_base.hpp"
#include "desc_resource.hpp"

namespace desc
{
	class desc_system
	{
	public:
		using desc_name = res::tag;

		desc_system(res::resource_system& res_system);
		~desc_system() = default;
		desc_system(const desc_system&) = delete;
		desc_system& operator=(const desc_system&) = delete;
		desc_system(desc_system&&) = delete;
		desc_system& operator=(desc_system&&) = delete;

		template<typename T>
		void register_desc(const desc_name& name)
		{
			auto desc = std::make_shared<T>();
			auto type = ds::Type::make<T>();
			desc_map[type] = desc;
			name_map[name] = type;
			descs_for_load.push_back(name);
		}

		template<typename T>
		void unregister_desc()
		{
			auto type = ds::Type::make<T>();
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
				auto desc = res_system.require_resource2<desc::desc_resource>(tag);
				auto it = desc_map.find(type);
				if (it != desc_map.end()) {
					it->second->deserialize(res_system, desc->body);
				}
			}

			descs_for_load.clear();
		}

		template<typename T>
		std::shared_ptr<T> get_desc()
		{
			auto it = desc_map.find(ds::Type::make<T>());
			if (it == desc_map.end()) {
				return nullptr;
			}
			return std::static_pointer_cast<T>(it->second);
		}

	private:
		res::resource_system& res_system;
		std::vector<desc_name> descs_for_load;
		std::unordered_map<desc_name, ds::Type> name_map;
		std::unordered_map<ds::Type, std::shared_ptr<desc_base>> desc_map;
	};

}
