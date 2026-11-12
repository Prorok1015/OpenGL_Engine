#include <snk/project_migration/pmg_apply.h>
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>
#include <snk/desc/adapters/desc_adapter_t.h>
#include <snk/desc/desc_schema.h>
#include <snk/project/prj_json_text.h>
#include <snk/project/prj_manifest.h>
#include <snk/project/prj_version.h>
#include <snk/resource/res_system.h>
#include <boost/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <format>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;
namespace json = boost::json;

namespace
{
	using namespace prj::migration;

	constexpr std::uint64_t k_max_file_bytes = std::uint64_t{ 64 } << 20;
	constexpr std::string_view k_migrations_dir = ".snk/migrations";

	std::string hash_of(std::string_view bytes)
	{
		return std::format("{:016x}", desc::schema_id(bytes));
	}

	std::string text_of(const std::vector<std::byte>& bytes)
	{
		return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
	}

	std::optional<std::string> default_write(const fs::path& root, const fs::path& relative, std::string_view text)
	{
		return prj::write_project_file(root, relative, text);
	}

	std::string default_id()
	{
		const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
		return std::format("{:%Y%m%dT%H%M%SZ}-{}", now, prj::to_string(prj::current_engine_version()));
	}

	std::string utc_now()
	{
		const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
		return std::format("{:%Y-%m-%dT%H:%M:%SZ}", now);
	}

	// The project's text of `relative`, read without following a link; throws with the file's name.
	std::string read_or_throw(const fs::path& root, const std::string& relative)
	{
		prj::project_file_text read = prj::read_project_file(root, fs_paths::path_from_utf8(relative), k_max_file_bytes);
		if (!read.error.empty()) throw std::runtime_error(std::format("{}: {}", relative, read.error));
		if (!read.exists) throw std::runtime_error(std::format("{}: the file is gone", relative));
		return std::move(read.text);
	}

	json::value step_json(const step_use& step)
	{
		return json::object{ { "type", step.type }, { "from", step.from_version }, { "summary", step.summary }, { "objects", step.objects } };
	}

	json::value report_json(const migration_report& report)
	{
		json::array files;
		for (const applied_file& file : report.files) {
			json::array steps;
			for (const step_use& step : file.steps) steps.push_back(step_json(step));
			json::array warnings;
			for (const desc::migration_warning& warning : file.warnings) {
				warnings.push_back(json::object{ { "type", warning.type }, { "from", warning.from_version }, { "message", warning.message } });
			}
			json::array notes;
			for (const std::string& note : file.notes) notes.push_back(json::string(note));
			json::object entry{ { "path", file.path }, { "hash_before", file.hash_before }, { "hash_after", file.hash_after },
				{ "written_as", file.written_as } };
			entry["steps"] = std::move(steps);
			entry["warnings"] = std::move(warnings);
			entry["notes"] = std::move(notes);
			files.push_back(std::move(entry));
		}
		json::array skipped;
		for (const skipped_file& file : report.skipped) {
			json::array types;
			for (const std::string& type : file.unknown_types) types.push_back(json::string(type));
			json::object entry{ { "path", file.path }, { "class", std::string(to_string(file.kind)) }, { "reason", file.reason } };
			entry["unknown_types"] = std::move(types);
			skipped.push_back(std::move(entry));
		}
		json::array created;
		for (const std::string& path : report.created_files) created.push_back(json::string(path));
		json::object out{ { "id", report.id }, { "created", report.created }, { "engine_before", report.engine_before },
			{ "engine_after", report.engine_after }, { "status", std::string(to_string(report.status)) }, { "backup", report.backup } };
		out["files"] = std::move(files);
		out["skipped"] = std::move(skipped);
		out["created_files"] = std::move(created);
		if (!report.error.empty()) out["error"] = report.error;
		return out;
	}

	std::string report_markdown(const migration_report& report)
	{
		std::string md = std::format("# Migration {}\n\nStatus: **{}**. Engine {} -> {}. Backup: `{}`.\n", report.id, to_string(report.status),
			report.engine_before, report.engine_after, report.backup);
		if (!report.error.empty()) md += std::format("\nRolled back: {}\n", report.error);
		bool any_warning = false;
		for (const applied_file& file : report.files) any_warning = any_warning || !file.warnings.empty() || !file.notes.empty();
		if (any_warning) {
			md += "\n## Warnings\n\n";
			for (const applied_file& file : report.files) {
				for (const desc::migration_warning& warning : file.warnings) {
					md += std::format("- `{}`: {} {} -> {}: {}\n", file.path, warning.type, warning.from_version, warning.from_version + 1, warning.message);
				}
				for (const std::string& note : file.notes) md += std::format("- `{}`: {}\n", file.path, note);
			}
		}
		md += "\n## Files\n\n";
		for (const applied_file& file : report.files) {
			md += std::format("- `{}`\n", file.path);
			for (const step_use& step : file.steps) {
				md += std::format("  - {} {} -> {}: {}\n", step.type, step.from_version, step.from_version + 1, step.summary);
			}
		}
		if (!report.skipped.empty()) {
			md += "\n## Not migrated\n\n";
			for (const skipped_file& file : report.skipped) md += std::format("- `{}`: {} {}\n", file.path, to_string(file.kind), file.reason);
		}
		return md;
	}

	// res:// of a file of the project: its path below the content root that holds it.
	std::optional<res::tag> tag_of(const std::string& path, const std::vector<fs::path>& content_roots)
	{
		for (const fs::path& root : content_roots) {
			std::string prefix = fs_paths::generic_utf8_of(root.lexically_normal());
			while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
			if (prefix == ".") return res::tag::make(path);
			if (path.size() > prefix.size() && path.starts_with(prefix) && path[prefix.size()] == '/') {
				return res::tag::make(std::string_view(path).substr(prefix.size() + 1));
			}
		}
		return std::nullopt;
	}

	struct watch_pause
	{
		const apply_context& context;
		bool paused = false;

		void pause()
		{
			if (context.pause_watch) context.pause_watch();
			paused = true;
		}

		~watch_pause()
		{
			if (paused && context.resume_watch) context.resume_watch();
		}
	};

	class applier
	{
	public:
		applier(const apply_context& context, migration_report& report)
			: m_context(context)
			, m_report(report)
			, m_write(context.write ? context.write : project_writer{ default_write })
			, m_adapter(context.resources, context.descs)
		{
		}

		void journal()
		{
			const std::string dir = std::string(k_migrations_dir) + "/" + m_report.id;
			if (auto error = m_write(m_context.root, fs_paths::path_from_utf8(dir + "/report.json"), prj::to_indented_json(report_json(m_report)))) {
				throw std::runtime_error(std::format("the journal cannot be written: {}", *error));
			}
		}

		void write_report_md()
		{
			const std::string dir = std::string(k_migrations_dir) + "/" + m_report.id;
			if (auto error = m_write(m_context.root, fs_paths::path_from_utf8(dir + "/report.md"), report_markdown(m_report))) {
				egLOG_WARN("project/migrate", "report.md of {} cannot be written: {}", m_report.id, *error);
			}
		}

		void write(const std::string& relative, std::string_view text)
		{
			if (auto error = m_write(m_context.root, fs_paths::path_from_utf8(relative), text)) throw std::runtime_error(*error);
		}

		// What desc_resave writes: the file loaded by the engine (migrated on load) and saved by its adapter. The migrated
		// JSON instead when the engine cannot load it or its type does not write __parent back (it would be flattened).
		std::string form_of(applied_file& file, const std::string& original, const std::vector<fs::path>& content_roots)
		{
			const json::value source = json::parse(original, {}, parse_options());
			const json::object& source_root = source.as_object();
			const std::string type = source_root.contains("__type") && source_root.at("__type").is_string()
				? std::string(source_root.at("__type").as_string()) : std::string{};

			const std::optional<res::tag> tag = tag_of(file.path, content_roots);
			std::string why_json;
			if (!tag) {
				why_json = "not under a content root";
			} else {
				std::shared_ptr<desc::desc_base> loaded;
				try {
					loaded = m_context.resources.require_sync<desc::desc_base>(*tag).get_sync();
				} catch (const std::exception& e) {
					why_json = std::format("the engine cannot load it: {}", e.what());
				}
				if (!loaded && why_json.empty()) why_json = "the engine cannot load it";
				if (loaded) {
					std::string saved = text_of(m_adapter.serialize(*tag, loaded));
					const json::value* parent = source_root.if_contains("__parent");
					if (!parent) {
						file.written_as = "engine";
						return saved;
					}
					const json::value written = json::parse(saved, {}, parse_options());
					const json::value* kept = written.is_object() ? written.as_object().if_contains("__parent") : nullptr;
					if (kept && *kept == *parent) {
						file.written_as = "engine";
						return saved;
					}
					why_json = std::format("'{}' does not keep its __parent when saved", type);
				}
			}

			json::value migrated = source;
			m_context.descs.migrate_tree(migrated.as_object(), file.path);
			m_context.descs.stamp_tree(migrated);
			file.written_as = "json";
			file.notes.push_back(std::format("written as migrated JSON: {}", why_json));
			egLOG_WARN("project/migrate", "{}: written as migrated JSON: {}", file.path, why_json);
			return text_of(m_context.descs.serialize_to_bytes(migrated));
		}

		// The file as it is on the disk now, not as any cache holds it.
		void check(const applied_file& file, std::string_view written)
		{
			const std::string on_disk = read_or_throw(m_context.root, file.path);
			if (on_disk != written) throw std::runtime_error(std::format("{}: the file on the disk is not what was written", file.path));
			const scanned_file again = classify_desc(file.path, on_disk, m_context.descs);
			if (again.kind != file_class::current) {
				throw std::runtime_error(std::format("{}: still {} after the migration {}", file.path, to_string(again.kind), again.reason));
			}
			const json::value root = json::parse(on_disk, {}, parse_options());
			const json::object& object = root.as_object();
			if (const json::value* type = object.if_contains("__type"); type && type->is_string()) {
				const std::string name(type->as_string());
				if (m_context.descs.is_registered(name)) {
					auto instance = m_context.descs.create_instance(name, res::tag::make(file.path));
					if (instance) instance->deserialize(m_context.descs, object);
				}
			}
		}

	private:
		static json::parse_options parse_options()
		{
			json::parse_options options;
			options.max_depth = 128;
			return options;
		}

		const apply_context& m_context;
		migration_report& m_report;
		project_writer m_write;
		core::desc::desc_adapter_t m_adapter;
	};

	bool is_regular(const fs::path& path)
	{
		std::error_code ec;
		return fs::is_regular_file(fs::symlink_status(path, ec));
	}
}

std::string_view prj::migration::to_string(migration_status status)
{
	switch (status) {
	case migration_status::in_progress: return "in_progress";
	case migration_status::ok: return "ok";
	case migration_status::partial: return "partial";
	case migration_status::rolled_back: return "rolled_back";
	}
	return "in_progress";
}

prj::migration::apply_outcome prj::migration::apply_migration(const migration_plan& plan, const apply_context& context)
{
	apply_outcome outcome;
	migration_report& report = outcome.report;

	const manifest_state& manifest = plan.scan.manifest;
	if (manifest.kind == file_class::newer || manifest.kind == file_class::invalid) {
		outcome.refused = std::format("project.snkproj: {} {}", to_string(manifest.kind), manifest.reason);
		return outcome;
	}
	for (const scanned_file& file : plan.scan.files) {
		if (file.kind == file_class::newer || file.kind == file_class::invalid) {
			outcome.refused = std::format("{}: {} {}", file.path, to_string(file.kind), file.reason);
			return outcome;
		}
		if (file.kind == file_class::unknown_type) report.skipped.push_back(skipped_file{ file.path, file.kind, file.reason, file.unknown_types });
	}
	report.created = utc_now();
	report.engine_before = manifest.engine_version;
	report.engine_after = prj::to_string(prj::current_engine_version());
	if (plan.empty()) {
		report.status = report.skipped.empty() ? migration_status::ok : migration_status::partial;
		return outcome;
	}

	const prj::manifest_result loaded_manifest = prj::load_manifest(context.root);
	if (!loaded_manifest.data) {
		outcome.refused = "project.snkproj cannot be read";
		return outcome;
	}

	std::vector<std::string> originals;
	try {
		for (const file_plan& planned : plan.files) {
			applied_file file;
			file.path = planned.path;
			file.steps = planned.steps;
			file.warnings = planned.warnings;
			originals.push_back(read_or_throw(context.root, planned.path));
			file.hash_before = hash_of(originals.back());
			report.files.push_back(std::move(file));
		}
	} catch (const std::exception& e) {
		outcome.refused = e.what();
		report.files.clear();
		return outcome;
	}

	const auto make_id = context.make_id ? context.make_id : std::function<std::string()>{ default_id };
	const std::string base_id = make_id();
	report.id = base_id;
	for (int suffix = 2; prj::read_project_file(context.root, fs_paths::path_from_utf8(std::format("{}/{}/report.json", k_migrations_dir, report.id))).exists; ++suffix) {
		report.id = std::format("{}-{}", base_id, suffix);
	}
	report.backup = std::format("{}/{}/backup", k_migrations_dir, report.id);

	applier apply(context, report);
	report.status = migration_status::in_progress;
	try {
		apply.journal();
	} catch (const std::exception& e) {
		outcome.refused = e.what();
		return outcome;
	}

	std::size_t written = 0;
	{
		watch_pause watch{ context };
		try {
			for (std::size_t i = 0; i < report.files.size(); ++i) {
				apply.write(report.backup + "/" + report.files[i].path, originals[i]);
			}
			watch.pause();
			for (std::size_t i = 0; i < report.files.size(); ++i) {
				applied_file& file = report.files[i];
				const std::string text = apply.form_of(file, originals[i], loaded_manifest.data->content_roots);
				apply.write(file.path, text);
				++written;
				file.hash_after = hash_of(text);
				apply.journal();
				apply.check(file, text);
			}
			report.status = report.skipped.empty() ? migration_status::ok : migration_status::partial;
		} catch (const std::exception& e) {
			report.error = e.what();
			egLOG_ERROR("project/migrate", "migration {} failed, rolling back: {}", report.id, report.error);
			std::string restore_errors;
			for (std::size_t i = 0; i < written; ++i) {
				try {
					apply.write(report.files[i].path, originals[i]);
				} catch (const std::exception& restore) {
					restore_errors += std::format("; {} not restored: {}", report.files[i].path, restore.what());
				}
			}
			for (const std::string& created : report.created_files) {
				const fs::path path = context.root / fs_paths::path_from_utf8(created);
				std::error_code ec;
				if (is_regular(path)) fs::remove(path, ec);
			}
			report.error += restore_errors;
			report.status = migration_status::rolled_back;
		}
	}

	try {
		apply.journal();
	} catch (const std::exception& e) {
		egLOG_ERROR("project/migrate", "{}", e.what());
	}
	apply.write_report_md();
	return outcome;
}

std::vector<std::string> prj::migration::unfinished_migrations(const fs::path& root)
{
	std::vector<std::string> ids;
	std::error_code ec;
	const fs::path dir = root / fs_paths::path_from_utf8(k_migrations_dir);
	for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
		std::error_code status_error;
		if (!fs::is_directory(it->symlink_status(status_error))) continue;
		const std::string id = fs_paths::utf8_of(it->path().filename());
		const prj::project_file_text text = prj::read_project_file(root, fs_paths::path_from_utf8(std::format("{}/{}/report.json", k_migrations_dir, id)));
		if (!text.exists || !text.error.empty()) continue;
		boost::system::error_code error;
		const json::value journal = json::parse(text.text, error);
		if (error || !journal.is_object()) continue;
		const json::value* status = journal.as_object().if_contains("status");
		if (status && status->is_string() && status->as_string() == to_string(migration_status::in_progress)) ids.push_back(id);
	}
	std::ranges::sort(ids);
	return ids;
}

std::optional<std::string> prj::migration::rollback_migration(const fs::path& root, std::string_view id, const project_writer& write)
{
	const project_writer writer = write ? write : project_writer{ default_write };
	const std::string journal_path = std::format("{}/{}/report.json", k_migrations_dir, id);
	const prj::project_file_text text = prj::read_project_file(root, fs_paths::path_from_utf8(journal_path));
	if (!text.exists || !text.error.empty()) return std::format("migration {} has no journal{}", id, text.error.empty() ? "" : ": " + text.error);
	boost::system::error_code error;
	json::value journal = json::parse(text.text, error);
	if (error || !journal.is_object()) return std::format("the journal of migration {} cannot be read", id);
	json::object& object = journal.as_object();
	const std::string backup = object.contains("backup") && object.at("backup").is_string() ? std::string(object.at("backup").as_string()) : std::string{};

	struct entry { std::string path; std::string hash_after; };
	std::vector<entry> files;
	if (const json::value* list = object.if_contains("files"); list && list->is_array()) {
		for (const json::value& item : list->as_array()) {
			if (!item.is_object()) continue;
			const json::object& file = item.as_object();
			const json::value* path = file.if_contains("path");
			const json::value* hash = file.if_contains("hash_after");
			if (!path || !path->is_string()) continue;
			files.push_back({ std::string(path->as_string()), hash && hash->is_string() ? std::string(hash->as_string()) : std::string{} });
		}
	}

	std::vector<std::string> changed;
	for (const entry& file : files) {
		if (file.hash_after.empty()) continue;
		const prj::project_file_text now = prj::read_project_file(root, fs_paths::path_from_utf8(file.path), k_max_file_bytes);
		if (!now.exists || !now.error.empty() || hash_of(now.text) != file.hash_after) changed.push_back(file.path);
	}
	if (!changed.empty()) {
		std::string list;
		for (const std::string& path : changed) list += (list.empty() ? "" : ", ") + path;
		return std::format("changed after migration {}: {}", id, list);
	}

	for (const entry& file : files) {
		const prj::project_file_text saved = prj::read_project_file(root, fs_paths::path_from_utf8(backup + "/" + file.path), k_max_file_bytes);
		if (!saved.exists) continue; // never backed up, so never written
		if (!saved.error.empty()) return std::format("{}: the backup cannot be read: {}", file.path, saved.error);
		if (auto failed = writer(root, fs_paths::path_from_utf8(file.path), saved.text)) return std::format("{}: {}", file.path, *failed);
	}
	if (const json::value* created = object.if_contains("created_files"); created && created->is_array()) {
		for (const json::value& item : created->as_array()) {
			if (!item.is_string()) continue;
			const fs::path path = root / fs_paths::path_from_utf8(std::string_view(item.as_string()));
			std::error_code ec;
			if (is_regular(path)) fs::remove(path, ec);
		}
	}
	object["status"] = std::string(to_string(migration_status::rolled_back));
	if (auto failed = writer(root, fs_paths::path_from_utf8(journal_path), prj::to_indented_json(journal))) return *failed;
	return std::nullopt;
}
