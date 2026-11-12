#include "edt_data_document.h"
#include "edt_code_commands.h"
#include "edt_desc_file_commands.h"
#include "edt_schema_inspector.h"
#include <snk/desc/desc_json_ops.h>
#include <snk/desc/desc_system.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/resource/res_system.h>
#include <boost/json/parse.hpp>
#include <boost/json/value_to.hpp>
#include <algorithm>
#include <chrono>
#include <format>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace
{
	namespace json = boost::json;

	constexpr std::chrono::seconds k_read_timeout{ 5 };
	constexpr std::size_t k_max_chain = 16;

	std::string_view stem_of(const res::tag& tag)
	{
		std::string_view name = tag.pure_name();
		if (const auto slash = name.find_last_of('/'); slash != std::string_view::npos) {
			name.remove_prefix(slash + 1);
		}
		if (const auto dot = name.find('.'); dot != std::string_view::npos) {
			name.remove_suffix(name.size() - dot);
		}
		return name;
	}

	std::string where(const res::tag& file, std::string_view pointer = {})
	{
		return std::format("'{}'{}{}", file.view(), pointer.empty() ? "" : " ", pointer);
	}

	std::optional<res::tag> parent_of(const json::object& body, const res::tag& file, std::string& error)
	{
		const json::value* field = body.if_contains("__parent");
		if (!field) {
			return std::nullopt;
		}
		res::tag parent;
		try {
			parent = json::value_to<res::tag>(*field);
		} catch (const std::exception& e) {
			error = std::format("{} /__parent: {}", where(file), e.what());
			return std::nullopt;
		}
		if (std::string why = res::reference_problem(parent, file.protocol()); !why.empty()) {
			error = std::format("{} /__parent: {}", where(file), why);
			return std::nullopt;
		}
		return parent;
	}
}

edt::data_document::data_document(editor_services& services)
	: m_services(services)
{
}

const desc::type_schema* edt::data_document::schema() const
{
	const desc::desc_system* descs = m_services.descs();
	return descs ? descs->find_schema(m_type) : nullptr;
}

edt::open_result edt::data_document::read_file(const res::tag& file, json::object& out, std::string& version) const
{
	res::resource_system* resources = m_services.resources();
	desc::desc_system* descs = m_services.descs();
	if (!resources || !descs) {
		return open_result::failure("the project resources are not available");
	}
	const auto raw = resources->fetch_data(file);
	if (!raw) {
		return open_result::failure(std::format("{} could not be read: its resources are not mounted", where(file)));
	}
	if (!raw->wait_for(k_read_timeout) || !raw->is_ready()) {
		return open_result::failure(std::format("{} could not be read", where(file)));
	}
	if (raw->data.size() > k_desc_file_limit) {
		return open_result::failure(std::format("{} is {} bytes: a data document holds at most {}", where(file), raw->data.size(), k_desc_file_limit));
	}
	const std::string_view text(reinterpret_cast<const char*>(raw->data.data()), raw->data.size());
	boost::system::error_code parse_error;
	json::parse_options options;
	options.max_depth = 128;
	json::value parsed = json::parse(text, parse_error, {}, options);
	if (parse_error || !parsed.is_object()) {
		return open_result::failure(std::format("{} is not a JSON object{}{}", where(file), parse_error ? ": " : "", parse_error ? parse_error.message() : std::string{}));
	}
	try {
		descs->migrate_tree(parsed.as_object(), file.view());
	} catch (const std::exception& e) {
		return open_result::failure(std::format("{}: {}", where(file), e.what()));
	}
	out = std::move(parsed.as_object());
	version = code_version_of(raw->data);
	return open_result::ok();
}

edt::open_result edt::data_document::open(const res::tag& file)
{
	m_open = false;
	desc::desc_system* descs = m_services.descs();
	if (!descs) {
		return open_result::failure("the project resources are not available");
	}

	json::object own;
	std::string version;
	if (open_result read = read_file(file, own, version); !read.opened) {
		return read;
	}
	const json::value* type_field = own.if_contains("__type");
	if (!type_field || !type_field->is_string()) {
		return open_result::failure(std::format("{} /__type: missing or not a string", where(file)));
	}
	const std::string type{ type_field->as_string() };
	const desc::type_schema* type_schema = descs->find_schema(type);
	if (!type_schema) {
		return open_result::failure(std::format("{} /__type: '{}' has no schema, so it cannot be edited as data", where(file), type));
	}
	if (!type_schema->is_document) {
		return open_result::failure(std::format("{} /__type: '{}' is not a data document type", where(file), type));
	}

	std::vector<json::object> chain;
	std::unordered_set<std::string> seen{ std::string{ file.view() } };
	res::tag current_tag = file;
	json::object current = own;
	while (true) {
		std::string error;
		const std::optional<res::tag> parent = parent_of(current, current_tag, error);
		if (!error.empty()) {
			return open_result::failure(std::move(error));
		}
		if (!parent) {
			break;
		}
		if (!seen.insert(std::string{ parent->view() }).second) {
			return open_result::failure(std::format("{} /__parent: '__parent' cycle through '{}'", where(current_tag), parent->view()));
		}
		if (chain.size() >= k_max_chain) {
			return open_result::failure(std::format("{} /__parent: the chain is longer than {} files", where(file), k_max_chain));
		}
		json::object loaded;
		std::string ignored;
		if (open_result read = read_file(*parent, loaded, ignored); !read.opened) {
			return open_result::failure(std::format("{} /__parent: {}", where(current_tag), read.error));
		}
		const json::value* parent_type = loaded.if_contains("__type");
		if (!parent_type || !parent_type->is_string() || std::string_view{ parent_type->as_string() } != type) {
			return open_result::failure(std::format("{} /__parent: '{}' is of another type than '{}'", where(current_tag), parent->view(), type));
		}
		current_tag = *parent;
		current = loaded;
		chain.push_back(std::move(loaded));
	}

	json::object inherited;
	for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
		json::object layer;
		for (const auto& member : *it) {
			if (!member.key().starts_with("__")) {
				layer.insert_or_assign(member.key(), member.value());
			}
		}
		desc::json_deep_merge(inherited, layer);
	}

	m_source = file;
	m_title = std::string{ stem_of(file) };
	m_type = type;
	m_own = std::move(own);
	m_saved_own = m_own;
	m_inherited = std::move(inherited);
	m_version = std::move(version);
	++m_generation;
	m_open = true;
	return open_result::ok();
}

json::value edt::data_document::shown() const
{
	const desc::type_schema* type_schema = schema();
	const desc::desc_system* descs = m_services.descs();
	if (!type_schema || !descs) {
		return json::object{};
	}
	return schema_ui::compose(*type_schema, descs->schemas(), { &m_inherited, &m_own });
}

json::value edt::data_document::base() const
{
	const desc::type_schema* type_schema = schema();
	const desc::desc_system* descs = m_services.descs();
	if (!type_schema || !descs) {
		return json::object{};
	}
	return schema_ui::compose(*type_schema, descs->schemas(), { &m_inherited }, false);
}

bool edt::data_document::edit(const std::function<bool(json::value&)>& draw)
{
	const desc::type_schema* type_schema = schema();
	const desc::desc_system* descs = m_services.descs();
	if (!m_open || !type_schema || !descs) {
		return false;
	}
	if (!schema_ui::edit_component(*type_schema, descs->schemas(), &m_inherited, m_own, draw)) {
		return false;
	}
	++m_generation;
	return true;
}

void edt::data_document::restore_own(json::object own)
{
	if (own == m_own) {
		return;
	}
	m_own = std::move(own);
	++m_generation;
}

edt::save_result edt::data_document::save()
{
	if (!m_open) {
		return save_result::failure("the document is not open");
	}
	if (!is_dirty()) {
		return save_result::ok();
	}
	desc::desc_system* descs = m_services.descs();
	res::resource_system* reader = m_services.resources();
	res::resource_system* writer = m_services.writable_resources();
	if (!descs || !reader || !writer) {
		return save_result::failure("the project resources are not available");
	}

	json::object on_disk;
	std::string disk_version;
	if (open_result read = read_file(m_source, on_disk, disk_version); !read.opened) {
		return save_result::failure(std::format("{} cannot be checked before it is written: {}", where(m_source), read.error));
	}
	if (disk_version != m_version) {
		return save_result::failure(std::format("{} changed on disk since it was opened: nothing was written", where(m_source)));
	}

	json::value content(m_own);
	for (const res::diagnostic& item : validate_desc_tree(*descs, content, m_source.view())) {
		if (item.severity == res::diagnostic_severity::error) {
			return save_result::failure(std::format("{}{}: {}", where(m_source), item.json_path, item.reason));
		}
	}
	descs->stamp_tree(content);
	const std::vector<std::byte> bytes = descs->serialize_to_bytes(content);
	if (bytes.size() > k_desc_file_limit) {
		return save_result::failure(std::format("{} would be {} bytes: a data document holds at most {}", where(m_source), bytes.size(), k_desc_file_limit));
	}
	if (!writer->store_sync(m_source, bytes)) {
		return save_result::failure(std::format("{} could not be written", where(m_source)));
	}
	writer->signal_changed(m_source);

	m_own = std::move(content.as_object());
	m_saved_own = m_own;
	m_version = code_version_of(bytes);
	return save_result::ok();
}
