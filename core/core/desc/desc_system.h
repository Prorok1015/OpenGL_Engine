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
		void register_desc(const std::string_view type_name)
		{
			ASSERT_MSG(!factory_map.contains(std::string{ type_name }), "That desc type is already registered!");
			factory_map[std::string{ type_name }] = []() -> std::shared_ptr<desc::desc_base> { return std::make_shared<T>(); };
		}

		// register_component_desc marks the type as user-addable in the editor.
		// Use this for component descs that users can add to prefab nodes.
		// Use register_desc for nested/container descs not shown in "Add Component".
		template<typename T>
		void register_component_desc(const std::string_view type_name)
		{
			register_desc<T>(type_name);
			m_component_type_names.push_back(std::string{ type_name });
		}

		void unregister_desc(const std::string_view type_name)
		{
			factory_map.erase(std::string{ type_name });
			std::erase(m_component_type_names, std::string{ type_name });
		}

		const std::vector<std::string>& get_component_type_names() const
		{
			return m_component_type_names;
		}

		std::shared_ptr<desc::desc_base> create_instance(const std::string_view type_id, const res::tag& tag) const
		{
            auto it = factory_map.find(std::string{ type_id });
			if (it == factory_map.end()) {
				egLOG("desc/error", "Failed to create desc instance of type '{0}': type not registered", type_id);
				return nullptr;
			}
			auto instance = it->second();
			instance->set_tag(tag);
            instance->set_type(type_id);
			return instance;
		}

		template<typename T>
		auto get_field_desc(const desc::desc_base& owner, const json::value& data) const
		{
			if (data.is_string()) {
				return m_res_system.require<T>(json::value_to<res::tag>(data));
			}

            ASSERT_MSG(data.is_object() && data.as_object().contains("__type"), "Invalid desc field data: expected either string tag or object with '__type' field");

			res::tag mem_tag = make_mem_tag_deterministic(owner, json::value_to<std::string>(data.at("__type")), data);

			// Only store if not already in cache — avoids re-creating the same resource on every assembly.
			if (!m_res_system.exists(mem_tag)) {
				m_res_system.store(mem_tag, serialize_to_bytes(data));
			}
			return m_res_system.require<T>(mem_tag);
		}

        void write_pretty_json(std::ostream& os, json::value const& jv, std::string* indent = nullptr) const
        {
            std::string indent_;
            if (!indent)
                indent = &indent_;
            switch (jv.kind())
            {
            case json::kind::object:
            {
                os << "{\n";
                indent->append(4, ' ');
                auto const& obj = jv.get_object();
                if (!obj.empty())
                {
                    auto it = obj.begin();
                    for (;;)
                    {
                        os << *indent << json::serialize(it->key()) << " : ";
                        write_pretty_json(os, it->value(), indent);
                        if (++it == obj.end())
                            break;
                        os << ",\n";
                    }
                }
                os << "\n";
                indent->resize(indent->size() - 4);
                os << *indent << "}";
                break;
            }

            case json::kind::array:
            {
                os << "[\n";
                indent->append(4, ' ');
                auto const& arr = jv.get_array();
                if (!arr.empty())
                {
                    auto it = arr.begin();
                    for (;;)
                    {
                        os << *indent;
                        write_pretty_json(os, *it, indent);
                        if (++it == arr.end())
                            break;
                        os << ",\n";
                    }
                }
                os << "\n";
                indent->resize(indent->size() - 4);
                os << *indent << "]";
                break;
            }

            case json::kind::string:
            {
                os << json::serialize(jv.get_string());
                break;
            }

            case json::kind::uint64:
            case json::kind::int64:
            case json::kind::double_:
                os << jv;
                break;

            case json::kind::bool_:
                if (jv.get_bool())
                    os << "true";
                else
                    os << "false";
                break;

            case json::kind::null:
                os << "null";
                break;
            }

            if (indent->empty())
                os << "\n";
        }


		std::vector<std::byte> serialize_to_bytes(const json::value& data) const
		{
			std::string str;
			bool cfg_is_enable_pritty_json = true;
            if (cfg_is_enable_pritty_json) {
                std::stringstream ss;
                write_pretty_json(ss, data);
				str = ss.str();
            } else {
                str = json::serialize(data);
            }

			std::vector<std::byte> bytes(str.size());
			std::memcpy(bytes.data(), str.data(), str.size());
			return bytes;
		}

		res::tag make_mem_tag_deterministic(const desc::desc_base& owner, const std::string& field, const json::value& data) const
		{
			std::string_view owner_name = owner.get_tag().pure_name();
			std::string serialized = json::serialize(data);
			size_t content_hash = std::hash<std::string>{}(serialized);

			std::string path = std::format("memory://overrides/{}/{}_{:x}.desc",
				owner_name, field, content_hash);

			return res::tag{ path };
		}

	private:
		res::resource_system& m_res_system;
		std::unordered_map<std::string, std::function<std::shared_ptr<desc::desc_base>()>> factory_map;
		std::vector<std::string> m_component_type_names;
	};
}
