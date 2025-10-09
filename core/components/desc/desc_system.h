#pragma once
#include "common.h"
#include "res_system.h"
#include "res_tag.h"
#include "desc_base.hpp"
#include "desc_resource.hpp"
#include <vector>

namespace std {
	template<> struct hash<std::pair<ds::type_id, std::string>> {
		std::size_t operator()(const std::pair<ds::type_id, std::string>& k) const {
			return std::hash<ds::type_id>{}(k.first) ^ std::hash<std::string>{}(k.second);
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

			auto type = std::pair{ ds::type_id::make<T>(), nms };
			if (desc_map.find(type) != desc_map.end()) {
				ASSERT_FAIL("desc with type already registered");
				return;
			}

			auto desc = std::make_shared<T>();
			desc->set_tag(name);
			desc_map[type] = desc;
			name_map[name] = type;
			descs_for_load.push_back(name);
		}

		template<typename T>
		void unregister_desc(desc_namespace nms = {})
		{
			auto type = std::pair{ ds::type_id::make<T>(), nms };
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

		void deserialize_desc(std::shared_ptr<desc::desc_base> desc, const desc::desc_resource& resource)
		{
			if (resource.body.contains("__parent")) {
				res::tag parent = json::value_to<res::tag>(resource.body.at("__parent"));
				auto parent_desc = get_desc<desc::desc_base>(parent);
				if (!parent_desc->is_loaded()) {
					auto presource = res_system.require_resource2<desc::desc_resource>(parent);
					if (presource) {
						deserialize_desc(parent_desc, *presource);
					}

					ASSERT_MSG(presource, "parent desc file not found");
				}

				parent_desc->copy_to(*desc); 
			}

			if (resource.body.contains("__type")) {
				auto type_str = json::value_to<res::tag>(resource.body.at("__type"));
				auto presource = res_system.require_resource2<desc::desc_resource>(type_str);
				if (presource) {
					deserialize_desc(desc, *presource);
				}
				return;
			}

			desc->deserialize(*this, resource.body);
			desc->set_is_loaded();
		}

		void process_pending_descs()
		{
			auto tmp_loading_qeueue = loading_qeueue;
			loading_qeueue.clear();
			for (auto& [desc_type, desc_resource] : tmp_loading_qeueue)
			{
				auto& [desc, type] = desc_type;
				auto it = desc_map.find(type);
				if (it == desc_map.end()) {
					desc_map[type] = desc;
					name_map[desc->get_tag()] = type;
					deserialize_desc(desc, desc_resource);
				}
			}

			auto process_list = descs_for_load;
			descs_for_load.clear();

			for (auto& tag : process_list)
			{
				if (name_map.find(tag) == name_map.end()) {
					continue;
				}

				auto type = name_map[tag];
				auto it = desc_map.find(type);
				if (it != desc_map.end() && !it->second->is_loaded()) {
					auto desc = res_system.require_resource2<desc::desc_resource>(tag);
					deserialize_desc(it->second, *desc);
				}
			}
		}

		template<typename T>
		std::shared_ptr<T> get_desc(desc_namespace nms = {}) const
		{
			auto it = desc_map.find(std::pair{ ds::type_id::make<T>(), nms });
			if (it == desc_map.end()) {
				return nullptr;
			}
			return std::static_pointer_cast<T>(it->second);
		}

		template<typename T>
		std::shared_ptr<T> get_desc(const desc_name& name) const
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

		template<class T>
		std::shared_ptr<T> try_create_runtime_desc(const desc_resource& resource, desc_namespace nms = {})
		{
			auto name = resource.get_tag();
			if (auto desc = get_desc<T>(name)) {
				return desc;
			}

			auto pred = [&name](auto t) {
				return t.first.first->get_tag() == name;
			};

			if (auto it = std::find_if(loading_qeueue.begin(), loading_qeueue.end(), pred); it != loading_qeueue.end()) 
			{
				return std::static_pointer_cast<T>(it->first.first);
			}

			auto desc = std::make_shared<T>();
			desc->set_tag(resource.get_tag());
			auto type = std::pair{ ds::type_id::make<T>(), resource.get_tag().string()};
			loading_qeueue.push_back({ {desc, type}, resource });
			return desc;
		}

		template<typename T>
		std::shared_ptr<T> get_or_override_desc(const desc::desc_base& owner, const json::value& data) const
		{
			if (data.is_string()) {
				auto name = json::value_to<desc_name>(data);
				return get_desc<T>(name);

			} else if (data.is_object()) {
				auto& obj = data.get_object();
				res::tag cur_tag = owner.get_tag();

				auto parent_tag = json::value_to<res::tag>(obj.at("__parent"));

				std::string path = "memory://override/" + std::string{ cur_tag.pure_name() } + std::to_string(name_map.size() + loading_qeueue.size()) + "/" + 
					std::string{ parent_tag.pure_name() } +".desc";
				auto name = res::tag{ path };

				auto desc = std::make_shared<T>();
				desc->set_tag(name);
				auto type = std::pair{ ds::type_id::make<T>(), std::string{name.path()} };
				loading_qeueue.push_back({ {desc, type}, desc::desc_resource{ name, obj} });
				return desc;
			}

			egLOG("desc/get_or_override", "desc is not string or object! owner is {}", owner.get_tag().get_full());

			return nullptr;
		}

	private:
		res::resource_system& res_system;
		std::vector<desc_name> descs_for_load;// TODO: change
		mutable std::vector<std::pair<
			std::pair<std::shared_ptr<desc::desc_base>, std::pair<ds::type_id, desc_namespace>
			>, 
			desc::desc_resource>> loading_qeueue;
		std::unordered_map<desc_name, std::pair<ds::type_id, desc_namespace>> name_map;
		std::unordered_map<std::pair<ds::type_id, desc_namespace>, std::shared_ptr<desc_base>> desc_map;
	};

}
