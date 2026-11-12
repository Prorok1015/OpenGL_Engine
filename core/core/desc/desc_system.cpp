#include <snk/desc/desc_system.h>
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
