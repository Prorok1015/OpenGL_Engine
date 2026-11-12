#include <snk/desc/desc_system.h>
#include <algorithm>

desc::desc_system::desc_system(res::resource_system& res_system_)
	: m_res_system(res_system_)
{
}

desc::desc_system::desc_system(res::resource_system& res_system_, const desc_system* parent)
	: m_res_system(res_system_)
	, m_parent(parent)
	, m_schemas(parent ? &parent->m_schemas : nullptr)
{
}

std::string desc::desc_system::document_problem(const type_schema& schema, std::span<const std::string_view> codec_fields) const
{
	return desc::document_problem(schema, m_schemas, codec_fields);
}

bool desc::desc_system::mark_document(std::string_view type_name, std::span<const std::string_view> codec_fields)
{
	if (owned_by_parent(type_name)) {
		egLOG_ERROR("desc/error", "Desc type '{}' not marked as a document: the type belongs to the parent desc system", type_name);
		return false;
	}
	const type_schema* schema = m_schemas.find(type_name);
	if (!schema) {
		egLOG_ERROR("desc/error", "Desc type '{}' not marked as a document: it has no schema (it is not on the generic desc schema)", type_name);
		return false;
	}
	if (schema->is_document) {
		return true;
	}
	if (const std::string problem = document_problem(*schema, codec_fields); !problem.empty()) {
		egLOG_ERROR("desc/error", "Desc type '{}' not marked as a document: {}", type_name, problem);
		return false;
	}
	return m_schemas.set_document(type_name, true);
}

std::vector<std::string> desc::desc_system::document_types() const
{
	std::vector<std::string> names;
	for (const std::string& name : m_schemas.type_names()) {
		if (const type_schema* schema = m_schemas.find(name); schema && schema->is_document) {
			names.push_back(name);
		}
	}
	std::ranges::sort(names);
	return names;
}
