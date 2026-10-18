#include <snk/desc/adapters/desc_adapter_t.h>
#include <snk/desc/desc_system.h>
#include <snk/desc/desc_base.hpp>
#include <snk/desc/desc_guid.h>
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