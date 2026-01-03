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
					auto presource = m_res_system.require<desc::desc_resource>(parent).get_sync();
					if (presource) {
						deserialize_desc(parent_desc, *presource);
					}

					ASSERT_MSG(presource, "parent desc file not found");
				}

				parent_desc->copy_to(*desc); 
			}

			if (resource.body.contains("__type")) {
				auto type_str = json::value_to<res::tag>(resource.body.at("__type"));
				auto presource = m_res_system.require<desc::desc_resource>(type_str).get_sync();
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
					auto desc = m_res_system.require<desc::desc_resource>(tag).get_sync();
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

			egLOG("desc/get_or_override", "desc is not string or object! owner is {}", owner.get_tag().view());

			return nullptr;
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

		std::vector<desc_name> descs_for_load;// TODO: change
		mutable std::vector<std::pair<
			std::pair<std::shared_ptr<desc::desc_base>, std::pair<ds::type_id, desc_namespace>
			>, 
			desc::desc_resource>> loading_qeueue;
		std::unordered_map<desc_name, std::pair<ds::type_id, desc_namespace>> name_map;
		std::unordered_map<std::pair<ds::type_id, desc_namespace>, std::shared_ptr<desc_base>> desc_map;
	};

}
