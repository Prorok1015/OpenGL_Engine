#include <snk/desc/adapters/desc_adapter_t.h>
#include <snk/desc/desc_system.h>
#include <snk/desc/desc_base.hpp>
#include <snk/desc/desc_guid.h>
#include <algorithm>
#include <format>
#include <optional>
#include <stdexcept>
#include <vector>

core::desc::desc_adapter_t::desc_adapter_t(::res::resource_system&, ::desc::desc_system& desc_system)
	: m_desc_system(desc_system)
{
}

namespace {
// The "__parent" of a desc file, if it names one.
std::optional<::res::tag> read_parent(const ::res::tag& tag, const json::object& json_body)
{
	const auto* field = json_body.if_contains("__parent");
	if (!field) {
		return std::nullopt;
	}

	::res::tag parent_tag;
	try {
		parent_tag = json::value_to<::res::tag>(*field);
	} catch (const std::exception& e) {
		throw std::runtime_error(std::format("desc '{}': invalid '__parent': {}", tag.view(), e.what()));
	}
	if (parent_tag == tag) {
		throw std::runtime_error(std::format("desc '{}': '__parent' cycle through '{}'", tag.view(), parent_tag.view()));
	}
	return parent_tag;
}

json::object parse_body(const std::vector<std::byte>& data)
{
	std::string_view json_data(reinterpret_cast<const char*>(data.data()), data.size());
	json::parse_options options;
	options.max_depth = 128;
	return json::parse(json_data, {}, options).as_object();
}
}

std::vector<::res::res_dep> core::desc::desc_adapter_t::collect_dependencies(const ::res::tag& tag, const std::vector<std::byte>& data) const
{
	// Most files have no parent: skip parsing them twice.
	constexpr std::string_view marker = "\"__parent\"";
	const std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());
	if (text.find(marker) == std::string_view::npos) {
		return {};
	}

	if (const auto parent_tag = read_parent(tag, parse_body(data))) {
		return { ::res::make_dep<::desc::desc_base>(*parent_tag) };
	}
	return {};
}

std::shared_ptr<::res::resource_entry> core::desc::desc_adapter_t::deserialize(const::res::tag& tag, const std::vector<std::byte>& data) const
{
	return deserialize(tag, data, ::res::res_deps{});
}

std::shared_ptr<::res::resource_entry> core::desc::desc_adapter_t::deserialize(const::res::tag& tag, const std::vector<std::byte>& data, const ::res::res_deps& deps) const
{
	auto json_body = parse_body(data);

	auto fail = [&tag](const std::string& reason) {
		throw std::runtime_error(std::format("desc '{}': {}", tag.view(), reason));
	};

	try {
		m_desc_system.migrate_tree(json_body, tag.view());
	} catch (const std::exception& e) {
		fail(e.what());
	}

	if (!json_body.contains("__type") || !json_body.at("__type").is_string()) {
		fail("missing or non-string '__type' field");
	}

    std::string type_id = json::value_to<std::string>(json_body.at("__type"));
    auto instance = m_desc_system.create_instance(type_id, tag);
	if (!instance) {
		fail(std::format("unknown '__type' '{}'", type_id));
	}

	if (const auto parent_tag = read_parent(tag, json_body)) {
		const auto parent = deps.get<::desc::desc_base>(*parent_tag);
		if (!parent) {
			fail(std::format("failed to load '__parent' '{}'", parent_tag->view()));
		}
		parent->copy_to(*instance);
	}

    instance->deserialize(m_desc_system, json_body);

    // Read regardless of protocol: the editor loads level files through a memory:// copy, and that copy must keep the guid.
    // Only serialize() decides where __guid is written.
    if (auto guid_it = json_body.find("__guid"); guid_it != json_body.end() && guid_it->value().is_string()) {
        instance->set_guid(json::value_to<std::string>(guid_it->value()));
    }

    return instance;
}

std::vector<std::byte> core::desc::desc_adapter_t::serialize(const::res::tag& tag, const std::shared_ptr<::res::resource_entry>& resource) const
{
    auto desc_res = ds::polymorphic_pointer_cast<::desc::desc_base>(resource);
	json::object body;
	desc_res->serialize(body);
	// deserialize() requires __type. The instance knows what it was created as; a serialize() inherited from a base
	// class may name its own type (a texture_2d_desc would come back as texture_desc).
	if (!desc_res->get_type().empty() || !body.contains("__type")) {
		body["__type"] = std::string(desc_res->get_type());
	}
	// __guid identifies a file, so only res:// files get one; it is generated once and then kept on the instance.
	if (tag.protocol() == ::res::tag::default_protocol()) {
		if (desc_res->get_guid().empty()) {
			desc_res->set_guid(::desc::generate_guid());
		}
		body["__guid"] = desc_res->get_guid();
	}
	m_desc_system.stamp_tree(body);
	return m_desc_system.serialize_to_bytes(json::value{ std::move(body) });
}