#include <snk/desc/desc_system.h>
#include <algorithm>
#include <format>

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

namespace
{
	constexpr int max_document_depth = 8;

	std::string field_problem(const desc::field_schema& field, const desc::schema_registry& schemas, std::span<const std::string_view> codec_fields, int depth, const std::string& path)
	{
		const std::string where = path + field.name;
		auto structure_problem = [&](const std::string& name) -> std::string {
			if (depth >= max_document_depth) {
				return std::format("field '{}' is nested deeper than {} levels", where, max_document_depth);
			}
			const desc::type_schema* nested = schemas.find(name);
			if (!nested) {
				return std::format("field '{}' is of structure '{}', which has no schema", where, name);
			}
			for (const auto& inner : nested->fields) {
				if (std::string problem = field_problem(inner, schemas, {}, depth + 1, where + "/"); !problem.empty()) {
					return problem;
				}
			}
			return {};
		};

		switch (field.kind) {
		case desc::field_kind::custom: {
			const bool has_codec = std::ranges::find(codec_fields, std::string_view{ field.name }) != codec_fields.end();
			if (has_codec) {
				return {};
			}
			return std::format("field '{}' is a custom value without a json_codec, so the schema does not describe it", where);
		}
		case desc::field_kind::structure:
			return structure_problem(field.ref_type);
		case desc::field_kind::array:
			if (field.element == desc::field_kind::custom) {
				return std::format("field '{}' is a list of custom values, so the schema does not describe its elements", where);
			}
			if (field.element == desc::field_kind::structure) {
				return structure_problem(field.ref_type);
			}
			return {};
		default:
			return {};
		}
	}
}

std::string desc::desc_system::document_problem(const type_schema& schema, std::span<const std::string_view> codec_fields) const
{
	if (schema.kind != schema_kind::desc || schema.is_component) {
		return std::format("a {} cannot be a document, only a plain desc can", schema.is_component ? "component" : std::string{ to_string(schema.kind) });
	}
	for (const auto& field : schema.fields) {
		if (std::string problem = field_problem(field, m_schemas, codec_fields, 0, {}); !problem.empty()) {
			return problem;
		}
	}
	return {};
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
