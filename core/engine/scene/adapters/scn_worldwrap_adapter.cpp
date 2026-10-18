#include "scn_worldwrap_adapter.h"
#include <snk/desc/desc_system.h>
#include <stdexcept>
#include <string>

scn::worldwrap_adapter::worldwrap_adapter(::res::resource_system& res_system, ::desc::desc_system& desc_system)
	: m_res_system(res_system)
	, m_desc_system(desc_system)
{
}

std::shared_ptr<::res::resource_entry> scn::worldwrap_adapter::deserialize(const::res::tag& tag, const std::vector<std::byte>& data) const
{
    std::string_view json_data(reinterpret_cast<const char*>(data.data()), data.size());
    json::parse_options options;
    options.max_depth = 128;
    auto json_body = json::parse(json_data, {}, options).as_object();

    try {
        m_desc_system.migrate_tree(json_body, tag.view());
    } catch (const std::exception& e) {
        throw std::runtime_error("world desc '" + std::string{ tag.view() } + "': " + e.what());
    }

    if (!json_body.contains("__type")) {
        throw std::runtime_error("world desc '" + std::string{ tag.view() } + "' does not contain __type field");
    }

    std::string type_id = json::value_to<std::string>(json_body.at("__type"));

    auto instance = m_desc_system.create_instance(scn::world_desc::__type, tag);

    if (type_id == scn::world_desc::__type) {
        if (json_body.contains("__parent")) {
            auto parent_tag = json::value_to<::res::tag>(json_body.at("__parent"));
            auto parent = m_res_system.require_sync<::desc::desc_base>(parent_tag);
            const auto parent_desc = parent.get_sync();
            if (!parent_desc) {
                throw std::runtime_error("world desc '" + std::string{ tag.view() } + "': failed to load '__parent' '" + std::string{ parent_tag.view() } + "'");
            }
            parent_desc->copy_to(*instance);
        }
    }

    instance->deserialize(m_desc_system, json_body);

    return instance;
}

std::vector<std::byte> scn::worldwrap_adapter::serialize(const::res::tag& tag, const std::shared_ptr<::res::resource_entry>& resource) const
{
    const auto& desc_res = ds::polymorphic_pointer_cast<::desc::desc_base>(resource);
    json::object body;
    desc_res->serialize(body);
    m_desc_system.stamp_tree(body);
    return m_desc_system.serialize_to_bytes(json::value{ std::move(body) });
}
