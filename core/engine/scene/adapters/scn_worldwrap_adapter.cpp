#include <snk/scene/adapters/scn_worldwrap_adapter.h>
#include <snk/desc/desc_system.h>
#include <stdexcept>
#include <string>

scn::worldwrap_adapter::worldwrap_adapter(::res::resource_system&, ::desc::desc_system& desc_system)
	: m_desc_system(desc_system)
{
}

std::vector<::res::res_dep> scn::worldwrap_adapter::collect_dependencies(const::res::tag& tag, const std::vector<std::byte>& data) const
{
    // Most files have no parent: skip parsing them twice.
    constexpr std::string_view marker = "\"__parent\"";
    const std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());
    if (text.find(marker) == std::string_view::npos) {
        return {};
    }

    json::parse_options options;
    options.max_depth = 128;
    auto json_body = json::parse(text, {}, options).as_object();
    try {
        m_desc_system.migrate_tree(json_body, tag.view());
    } catch (const std::exception& e) {
        throw std::runtime_error("world desc '" + std::string{ tag.view() } + "': " + e.what());
    }

    const auto* type = json_body.if_contains("__type");
    const auto* parent = json_body.if_contains("__parent");
    if (!type || !type->is_string() || !parent || json::value_to<std::string>(*type) != scn::world_desc::__type) {
        return {};
    }
    return { ::res::make_dep<::desc::desc_base>(json::value_to<::res::tag>(*parent)) };
}

std::shared_ptr<::res::resource_entry> scn::worldwrap_adapter::deserialize(const::res::tag& tag, const std::vector<std::byte>& data) const
{
    return deserialize(tag, data, ::res::res_deps{});
}

std::shared_ptr<::res::resource_entry> scn::worldwrap_adapter::deserialize(const::res::tag& tag, const std::vector<std::byte>& data, const ::res::res_deps& deps) const
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
            const auto parent_desc = deps.get<::desc::desc_base>(parent_tag);
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
