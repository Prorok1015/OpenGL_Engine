#include <snk/desc/desc_system.h>
#include <snk/desc/desc_schema_ops.h>
#include <snk/resource/res_reference.h>
#include <algorithm>
#include <utility>

namespace
{
	// What register_migration with a summary stores in the registry. Its type stays in the engine: only the engine asks
	// a step for it (std::function::target), so the summary needs no table of its own and desc_system keeps its size
	// (scn::scene_layer holds one by value and modules read the members after it, #751).
	struct described_step
	{
		std::string type;
		int from_version = 0;
		std::string summary;
		std::function<void(json::object&, desc::migration_context&)> fn;

		// The path of the inline migrate_tree (and of modules built before the summaries): warnings go to the log.
		void operator()(json::object& obj) const
		{
			desc::migration_context context(type, from_version, {}, nullptr);
			fn(obj, context);
		}
	};
}

desc::migration_context::migration_context(std::string_view type, int from_version, std::string_view file,
	std::vector<migration_warning>* sink)
	: m_type(type)
	, m_from_version(from_version)
	, m_file(file)
	, m_sink(sink)
{
}

void desc::migration_context::warn(std::string message)
{
	if (m_file.empty()) {
		egLOG_WARN("desc/migrate", "'{}' {} -> {}: {}", m_type, m_from_version, m_from_version + 1, message);
	} else {
		egLOG_WARN("desc/migrate", "{}: '{}' {} -> {}: {}", m_file, m_type, m_from_version, m_from_version + 1, message);
	}
	if (m_sink) {
		m_sink->push_back(migration_warning{ std::string{ m_type }, m_from_version, std::string{ m_file }, std::move(message) });
	}
}

void desc::desc_system::register_migration(std::string_view type, int from_version, std::string_view summary,
	std::function<void(json::object&, migration_context&)> fn)
{
	register_migration(type, from_version, migration_step_fn{ described_step{ std::string{ type }, from_version, std::string{ summary }, std::move(fn) } });
}

std::string desc::desc_system::migration_summary(std::string_view type, int from_version) const
{
	const migration_steps* steps = find_migrations(type);
	if (!steps) return {};
	const auto step = steps->find(from_version);
	if (step == steps->end()) return {};
	const auto* described = step->second.target<described_step>();
	return described ? described->summary : std::string{};
}

void desc::desc_system::migrate_tree(json::object& root, std::string_view file, std::vector<migration_warning>& warnings) const
{
	migrate_object(root, file, [file, &warnings](const migration_step_fn& step, json::object& obj, const std::string& type, int from) {
		if (const auto* described = step.target<described_step>()) {
			migration_context context(type, from, file, &warnings);
			described->fn(obj, context);
		} else {
			step(obj);
		}
	});
}

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

res::tag desc::desc_system::resolve_reference(const res::reference& ref) const
{
	return m_res_system.resolve_reference(ref);
}

std::optional<res::tag> desc::reference_tag(const json::value& in, const read_context& ctx)
{
	const auto read = res::reference_from_json(in);
	if (!read) {
		return std::nullopt;
	}
	return ctx.system ? ctx.system->resolve_reference(*read) : read->path;
}
