#include "desc_adapter_t.h"
#include "desc_system.h"
#include "desc_base.hpp"

core::desc::desc_adapter_t::desc_adapter_t(::res::resource_system& res_system, ::desc::desc_system& desc_system)
	: m_res_system(res_system)
	, m_desc_system(desc_system)
{
}

std::shared_ptr<::res::resource_entry> core::desc::desc_adapter_t::deserialize(const::res::tag& tag, const std::vector<std::byte>& data) const
{
    std::string_view json_data(reinterpret_cast<const char*>(data.data()), data.size());
    auto json_body = json::parse(json_data).as_object();

	ASSERT_MSG(json_body.contains("__type"), "Deserialized desc resource does not contain __type field!");

    std::string type_id = json::value_to<std::string>(json_body.at("__type"));
    auto instance = m_desc_system.create_instance(type_id, tag);

    if (json_body.contains("__parent")) {
        auto parent_tag = json::value_to<::res::tag>(json_body.at("__parent"));
        auto parent = m_res_system.require_sync<::desc::desc_base>(parent_tag);
        parent.get_sync()->copy_to(*instance);
    }

    instance->deserialize(m_desc_system, json_body);

    return instance;
}

std::vector<std::byte> core::desc::desc_adapter_t::serialize(const::res::tag& tag, const std::shared_ptr<::res::resource_entry>& resource) const
{
    auto desc_res = ds::polymorphic_cast<::desc::desc_base>(resource);
	json::object body;
	desc_res->serialize(body);
    std::string data = json::serialize(body);
    std::vector<std::byte> byte_data;
    byte_data.resize(data.size());
    std::memcpy(byte_data.data(), data.data(), data.size());
    return byte_data;
}