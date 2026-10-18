#pragma once
#include "common.h"
#include "res_system.h"
#include "res_tag.h"
#include "res_tag_json.h" // get_field_desc<T> (inline template below) does json::value_to<res::tag>
#include "desc_base.hpp"
#include "adapters/desc_adapter_t.h"
#include <functional>
#include <limits>
#include <map>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace desc
{
	class desc_system
	{
	public:
		using desc_name = res::tag;
		using desc_namespace = std::string;

		// std::hash is not stable across platforms/STLs; these names end up in saved data.
		static constexpr uint64_t fnv1a_64(std::string_view s) noexcept
		{
			uint64_t h = 14695981039346656037ull;
			for (unsigned char c : s) {
				h ^= c;
				h *= 1099511628211ull;
			}
			return h;
		}

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

		bool is_registered(const std::string_view type_name) const
		{
			return factory_map.contains(std::string{ type_name });
		}

		const std::vector<std::string>& get_component_type_names() const
		{
			return m_component_type_names;
		}

		// Migration step: upgrades an object of `type` from `from_version` to `from_version + 1`.
		// Objects without __version are version 1. A duplicate (type, from_version) keeps the first one.
		void register_migration(std::string_view type, int from_version, std::function<void(json::object&)> fn)
		{
			auto& steps = m_migrations[std::string{ type }];
			ASSERT_MSG(!steps.contains(from_version), "Migration for '{}' from version {} is already registered!", type, from_version);
			steps.emplace(from_version, std::move(fn));
		}

		int current_version(std::string_view type) const
		{
			auto it = m_migrations.find(std::string{ type });
			if (it == m_migrations.end() || it->second.empty()) {
				return 1;
			}
			return it->second.rbegin()->first + 1;
		}

		// Brings every typed object under `root` up to current_version. Throws std::runtime_error on
		// a malformed or newer-than-supported __version, or when a migration step is missing.
		void migrate_tree(json::object& root, std::string_view context) const
		{
			migrate_object(root, context);
		}

		// Writes the current __version into an object that has a string __type.
		void stamp_header(json::object& obj) const
		{
			auto it = obj.find("__type");
			if (it != obj.end() && it->value().is_string()) {
				obj["__version"] = current_version(std::string{ it->value().as_string() });
			}
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
			uint64_t content_hash = fnv1a_64(serialized);

			std::string path = std::format("memory://overrides/{}/{}_{:x}.desc",
				owner_name, field, content_hash);

			return res::tag{ path };
		}

	private:
		// Migrates the object itself first, then its members, so children a migration creates are visited too.
		void migrate_object(json::object& obj, std::string_view context) const
		{
			auto type_it = obj.find("__type");
			if (type_it != obj.end() && type_it->value().is_string()) {
				migrate_typed(obj, std::string{ type_it->value().as_string() }, context);
			}
			for (auto& member : obj) {
				migrate_value(member.value(), context);
			}
		}

		void migrate_value(json::value& value, std::string_view context) const
		{
			if (value.is_object()) {
				migrate_object(value.as_object(), context);
			}
			else if (value.is_array()) {
				auto& arr = value.as_array();
				// Numeric payloads (geometry) can be megabytes long and never contain typed descs.
				if (!arr.empty() && arr.front().is_number()) {
					return;
				}
				for (auto& element : arr) {
					migrate_value(element, context);
				}
			}
		}

		void migrate_typed(json::object& obj, const std::string& type, std::string_view context) const
		{
			constexpr std::int64_t max_version = std::numeric_limits<int>::max();

			std::int64_t parsed = 1;
			if (auto ver_it = obj.find("__version"); ver_it != obj.end()) {
				const json::value& raw = ver_it->value();
				if (raw.is_int64()) {
					parsed = raw.as_int64();
				}
				else if (raw.is_uint64()) {
					parsed = raw.as_uint64() > static_cast<std::uint64_t>(max_version) ? max_version + 1 : static_cast<std::int64_t>(raw.as_uint64());
				}
				else {
					throw std::runtime_error(std::format("{}: '{}' has a non-integer __version", context, type));
				}
				if (parsed < 1 || parsed > max_version) {
					throw std::runtime_error(std::format("{}: '{}' has an invalid __version", context, type));
				}
			}

			const int current = current_version(type);
			const int version = static_cast<int>(parsed);
			if (version > current) {
				throw std::runtime_error(std::format("{}: '{}' version {} is newer than supported {}", context, type, version, current));
			}
			if (version < current) {
				const auto& steps = m_migrations.at(type);
				for (int from = version; from < current; ++from) {
					auto step = steps.find(from);
					if (step == steps.end()) {
						throw std::runtime_error(std::format("{}: no migration for '{}' from version {}", context, type, from));
					}
					step->second(obj);
				}
			}
			obj["__version"] = current;
		}

		res::resource_system& m_res_system;
		std::unordered_map<std::string, std::function<std::shared_ptr<desc::desc_base>()>> factory_map;
		std::unordered_map<std::string, std::map<int, std::function<void(json::object&)>>> m_migrations;
		std::vector<std::string> m_component_type_names;
	};
}
