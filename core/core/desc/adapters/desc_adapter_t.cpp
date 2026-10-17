#include "desc_adapter_t.h"
#include "desc_system.h"
#include "desc_base.hpp"
#include <algorithm>
#include <format>
#include <stdexcept>
#include <vector>

core::desc::desc_adapter_t::desc_adapter_t(::res::resource_system& res_system, ::desc::desc_system& desc_system)
	: m_res_system(res_system)
	, m_desc_system(desc_system)
{
}

std::shared_ptr<::res::resource_entry> core::desc::desc_adapter_t::deserialize(const::res::tag& tag, const std::vector<std::byte>& data) const
{
    std::string_view json_data(reinterpret_cast<const char*>(data.data()), data.size());
	json::parse_options options;
	options.max_depth = 128;
    auto json_body = json::parse(json_data, {}, options).as_object();

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

    if (json_body.contains("__parent")) {
		// require_sync runs the parent's adapter on this thread, so a cycle shows up as a repeated tag on this stack.
		// Waiting on a pending ancestor instead would deadlock.
		thread_local std::vector<::res::tag> loading_stack;
		constexpr size_t max_parent_depth = 32;

		::res::tag parent_tag;
		try {
			parent_tag = json::value_to<::res::tag>(json_body.at("__parent"));
		} catch (const std::exception& e) {
			fail(std::format("invalid '__parent': {}", e.what()));
		}

		if (parent_tag == tag || std::ranges::find(loading_stack, parent_tag) != loading_stack.end()) {
			fail(std::format("'__parent' cycle through '{}'", parent_tag.view()));
		}
		if (loading_stack.size() >= max_parent_depth) {
			fail("'__parent' chain is too deep");
		}

		struct stack_guard {
			std::vector<::res::tag>& stack;
			stack_guard(std::vector<::res::tag>& s, const ::res::tag& t) : stack(s) { stack.push_back(t); }
			~stack_guard() { stack.pop_back(); }
		} guard(loading_stack, tag);

        auto parent_handle = m_res_system.require_sync<::desc::desc_base>(parent_tag);
		auto parent = parent_handle.get_sync();
		if (!parent) {
			fail(std::format("failed to load '__parent' '{}'", parent_tag.view()));
		}
        parent->copy_to(*instance);
    }

    instance->deserialize(m_desc_system, json_body);

    return instance;
}

std::vector<std::byte> core::desc::desc_adapter_t::serialize(const::res::tag& tag, const std::shared_ptr<::res::resource_entry>& resource) const
{
    const auto& desc_res = ds::polymorphic_pointer_cast<const ::desc::desc_base>(resource);
	json::object body;
	desc_res->serialize(body);
	// Most descs do not write their own __type, but deserialize() requires it.
	if (!body.contains("__type")) {
		body["__type"] = std::string(desc_res->get_type());
	}
	m_desc_system.stamp_header(body);
    std::string data = json::serialize(body);
    std::vector<std::byte> byte_data;
    byte_data.resize(data.size());
    std::memcpy(byte_data.data(), data.data(), data.size());
    return byte_data;
}