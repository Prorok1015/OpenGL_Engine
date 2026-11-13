#include <snk/project_migration/pmg_apply.h>
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>
#include <snk/desc/adapters/desc_adapter_t.h>
#include <snk/desc/desc_schema.h>
#include <snk/platform/plt_beneath.h>
#include <snk/project/prj_json_text.h>
#include <snk/project/prj_manifest.h>
#include <snk/project/prj_version.h>
#include <snk/resource/res_asset_meta.h>
#include <snk/resource/res_system.h>
#include <boost/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <format>
#include <random>
#include <span>
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
	constexpr std::string_view k_manifest_path = prj::k_manifest_file_name;

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
		json::array project_steps;
		for (const std::string& id : report.project_steps) project_steps.push_back(json::string(id));
		json::array project_notes;
		for (const std::string& note : report.project_notes) project_notes.push_back(json::string(note));
		json::object out{ { "id", report.id }, { "created", report.created }, { "engine_before", report.engine_before },
			{ "engine_after", report.engine_after }, { "status", std::string(to_string(report.status)) }, { "backup", report.backup } };
		out["files"] = std::move(files);
		out["skipped"] = std::move(skipped);
		out["created_files"] = std::move(created);
		out["project_steps"] = std::move(project_steps);
		out["project_notes"] = std::move(project_notes);
		if (!report.error.empty()) out["error"] = report.error;
		return out;
	}

	std::string report_markdown(const migration_report& report)
	{
		std::string md = std::format("# Migration {}\n\nStatus: **{}**. Engine {} -> {}. Backup: `{}`.\n", report.id, to_string(report.status),
			report.engine_before, report.engine_after, report.backup);
		if (!report.error.empty()) {
		const std::string_view label = report.status == migration_status::rollback_failed ? "Rollback failed" : "Rolled back";
		md += std::format("\n{}: {}\n", label, report.error);
	}
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
		if (!report.project_steps.empty() || !report.project_notes.empty()) {
			md += "\n## Project\n\n";
			for (const std::string& id : report.project_steps) md += std::format("- step `{}`\n", id);
			for (const std::string& note : report.project_notes) md += std::format("- {}\n", note);
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

	// A journal is a file in the project anyone can edit: every path it names must stay a chain of plain names below the root.
	bool is_plain_relative(std::string_view path)
	{
		if (path.empty() || path.find('\\') != std::string_view::npos || path.find(':') != std::string_view::npos) return false;
		const fs::path relative = fs_paths::path_from_utf8(path);
		if (relative.has_root_path()) return false;
		for (const fs::path& part : relative) {
			if (part.empty() || part == "." || part == "..") return false;
		}
		return true;
	}

	std::optional<std::string> created_file_error(const std::string& path, const std::vector<fs::path>& content_roots)
	{
		if (!is_plain_relative(path)) return std::format("'{}' is not a relative path inside the project", path);
		if (!tag_of(path, content_roots)) return std::format("'{}' is not inside a content root", path);
		return std::nullopt;
	}

	// Removes a file the migration created without following a link below the root (platform::beneath).
	std::optional<std::string> remove_created(const fs::path& root, const std::string& path)
	{
		std::error_code ec;
		const fs::path canonical_root = fs::canonical(root, ec);
		if (ec) return std::format("the project root cannot be opened: {}", ec.message());
		platform::beneath::pinned_root pinned{ canonical_root };
		const fs::path relative = fs_paths::path_from_utf8(path);
		if (std::string error = platform::beneath::remove_beneath(pinned, relative.parent_path(), relative.filename(), true); !error.empty()) {
			return std::format("{} not deleted: {}", path, error);
		}
		return std::nullopt;
	}

	enum class create_result { created, taken, failed };

	// Creates <root>/<relative> only if nothing is at its name, without following a link below the root.
	create_result create_new_file(const fs::path& root, const std::string& relative, std::string_view text, std::string& error)
	{
		std::error_code ec;
		const fs::path canonical_root = fs::canonical(root, ec);
		if (ec) {
			error = std::format("the project root cannot be opened: {}", ec.message());
			return create_result::failed;
		}
		const fs::path path = fs_paths::path_from_utf8(relative);
		std::random_device random;
		fs::path temp_leaf{ "." };
		temp_leaf += path.filename();
		temp_leaf += std::format(".{:08x}.tmp", random());
		platform::beneath::pinned_root pinned{ canonical_root };
		platform::beneath::write_request request;
		request.folder = path.parent_path();
		request.leaf = path.filename();
		request.tmp_leaf = temp_leaf;
		request.data = std::as_bytes(std::span<const char>(text.data(), text.size()));
		request.replace = false;
		request.tmp_path = canonical_root / request.folder / temp_leaf;
		error = platform::beneath::write_beneath(pinned, request);
		if (error.empty()) return create_result::created;
		return prj::read_project_file(root, path).exists ? create_result::taken : create_result::failed;
	}

	enum class file_state { original, migrated, changed };

	file_state state_of(const fs::path& root, const std::string& path, const std::string& hash_before, const std::string& hash_after)
	{
		const prj::project_file_text now = prj::read_project_file(root, fs_paths::path_from_utf8(path), k_max_file_bytes);
		if (!now.exists || !now.error.empty()) return file_state::changed;
		const std::string hash = hash_of(now.text);
		if (hash == hash_before) return file_state::original;
		if (!hash_after.empty() && hash == hash_after) return file_state::migrated;
		return file_state::changed;
	}

	// Puts `original` back where the migration wrote `hash_after`, and checks that it is back. `ours`: the failed
	// migration itself puts back what it has just written, with the watcher paused, whatever is on the disk (a failed
	// check may be the reason); a later rollback refuses a file someone else changed.
	std::optional<std::string> restore_file(const fs::path& root, const project_writer& write, const std::string& path,
		const std::string& hash_before, const std::string& hash_after, const std::string& original, bool ours)
	{
		switch (state_of(root, path, hash_before, hash_after)) {
		case file_state::original:
			return std::nullopt;
		case file_state::changed:
			if (!ours) return std::format("{} was changed after the migration wrote it", path);
			break;
		case file_state::migrated:
			break;
		}
		if (hash_of(original) != hash_before) return std::format("{}: the backup is not the original", path);
		if (auto error = write(root, fs_paths::path_from_utf8(path), original)) return std::format("{} not restored: {}", path, *error);
		if (state_of(root, path, hash_before, hash_after) != file_state::original) {
			return std::format("{} not restored: the file on the disk is not the original", path);
		}
		return std::nullopt;
	}

	class watch_pause
	{
	public:
		watch_pause(const std::function<void()>& pause, const std::function<void()>& resume) : m_pause(pause), m_resume(resume) {}
		watch_pause(const watch_pause&) = delete;
		watch_pause& operator=(const watch_pause&) = delete;

		void pause()
		{
			if (m_pause) m_pause();
			m_paused = true;
		}

		~watch_pause()
		{
			if (!m_paused || !m_resume) return;
			try {
				m_resume();
			} catch (const std::exception& e) {
				egLOG_ERROR("project/migrate", "the file watcher cannot be resumed: {}", e.what());
			} catch (...) {
				egLOG_ERROR("project/migrate", "the file watcher cannot be resumed");
			}
		}

	private:
		const std::function<void()>& m_pause;
		const std::function<void()>& m_resume;
		bool m_paused = false;
	};

	json::parse_options parse_options()
	{
		json::parse_options options;
		options.max_depth = 128;
		return options;
	}

	bool is_engine_key(std::string_view key)
	{
		return key == "__type" || key == "__version" || key == "__guid" || key == "__id";
	}

	// Why `written` loses what derives from a parent in `migrated`: an object with __parent whose saved form names
	// another parent (or none), or has a field its file does not have (the parent's, baked in). Empty when nothing is lost.
	std::string lost_parent(const json::value& migrated, const json::value* written, const std::string& at)
	{
		if (migrated.is_object()) {
			const json::object& object = migrated.as_object();
			const json::object* saved = written && written->is_object() ? &written->as_object() : nullptr;
			const std::string where = at.empty() ? std::string{ "/" } : at;
			if (const json::value* parent = object.if_contains("__parent")) {
				// prefab_desc saves a component that only names its parent as the parent's tag alone.
				const bool only_parent = std::ranges::all_of(object, [](const auto& member) { return member.key() == "__parent"; });
				if (only_parent && written && *written == *parent) return {};
				const json::value* kept = saved ? saved->if_contains("__parent") : nullptr;
				if (!kept || *kept != *parent) return std::format("{} does not keep its __parent", where);
				for (const auto& member : *saved) {
					if (!object.contains(member.key()) && !is_engine_key(member.key())) {
						return std::format("{} gets '{}' of its parent baked in", where, std::string_view(member.key()));
					}
				}
			}
			for (const auto& member : object) {
				const json::value* next = saved ? saved->if_contains(member.key()) : nullptr;
				std::string why = lost_parent(member.value(), next, std::format("{}/{}", at, std::string_view(member.key())));
				if (!why.empty()) return why;
			}
		} else if (migrated.is_array()) {
			const json::array& array = migrated.as_array();
			const json::array* saved = written && written->is_array() ? &written->as_array() : nullptr;
			for (std::size_t i = 0; i < array.size(); ++i) {
				const json::value* next = saved && i < saved->size() ? &(*saved)[i] : nullptr;
				std::string why = lost_parent(array[i], next, std::format("{}/{}", at, i));
				if (!why.empty()) return why;
			}
		}
		return {};
	}

	class applier
	{
	public:
		applier(const apply_context& context, migration_report& report, const std::vector<fs::path>& content_roots)
			: m_context(context)
			, m_report(report)
			, m_content_roots(content_roots)
			, m_write(context.write ? context.write : project_writer{ default_write })
			, m_adapter(context.resources, context.descs)
		{
		}

		const project_writer& writer() const { return m_write; }

		std::string journal_path() const
		{
			return std::format("{}/{}/report.json", k_migrations_dir, m_report.id);
		}

		std::string journal_text() const
		{
			return prj::to_indented_json(report_json(m_report));
		}

		void journal()
		{
			if (auto error = m_write(m_context.root, fs_paths::path_from_utf8(journal_path()), journal_text())) {
				throw std::runtime_error(std::format("the journal cannot be written: {}", *error));
			}
		}

		void write_report_md()
		{
			const std::string path = std::format("{}/{}/report.md", k_migrations_dir, m_report.id);
			if (auto error = m_write(m_context.root, fs_paths::path_from_utf8(path), report_markdown(m_report))) {
				egLOG_WARN("project/migrate", "report.md of {} cannot be written: {}", m_report.id, *error);
			}
		}

		void write(const std::string& relative, std::string_view text)
		{
			if (auto error = m_write(m_context.root, fs_paths::path_from_utf8(relative), text)) throw std::runtime_error(*error);
		}

		// What desc_resave writes: the file loaded by the engine (migrated on load) and saved by its adapter. Loaded
		// from `original`, the text on the disk, not from the cache, which may hold an older or edited state. The
		// migrated JSON instead when the engine cannot load it or its saved form would lose a __parent.
		std::string form_of(applied_file& file, const std::string& original)
		{
			const bool sidecar = res::is_asset_meta_name(file.path);
			const std::string_view text = sidecar && original.starts_with("\xEF\xBB\xBF") ? std::string_view(original).substr(3) : std::string_view(original);
			json::value migrated = json::parse(text, {}, parse_options());
			m_context.descs.migrate_tree(migrated.as_object(), file.path);
			m_context.descs.stamp_tree(migrated);
			// A sidecar is no desc the engine loads: its steps are all there is to it (EPIC-58 3.4).
			if (sidecar) {
				file.written_as = "json";
				return text_of(m_context.descs.serialize_to_bytes(migrated));
			}

			std::string why_json;
			if (const std::optional<res::tag> tag = tag_of(file.path, m_content_roots); !tag) {
				why_json = "not under a content root";
			} else {
				try {
					std::string saved = text_of(m_adapter.serialize(*tag, load_from(*tag, original)));
					const json::value written = json::parse(saved, {}, parse_options());
					why_json = lost_parent(migrated, &written, {});
					if (why_json.empty()) {
						file.written_as = "engine";
						return saved;
					}
				} catch (const std::exception& e) {
					why_json = std::format("the engine cannot load it: {}", e.what());
				}
			}

			file.written_as = "json";
			file.notes.push_back(std::format("written as migrated JSON: {}", why_json));
			egLOG_WARN("project/migrate", "{}: written as migrated JSON: {}", file.path, why_json);
			return text_of(m_context.descs.serialize_to_bytes(migrated));
		}

		// The file as it is on the disk now, not as any cache holds it. A file the engine wrote must load again, its
		// parent included; the migrated JSON of a file the engine could not load is only classified.
		void check(const applied_file& file, std::string_view written)
		{
			const std::string on_disk = read_or_throw(m_context.root, file.path);
			if (on_disk != written) throw std::runtime_error(std::format("{}: the file on the disk is not what was written", file.path));
			const scanned_file again = res::is_asset_meta_name(file.path) ? classify_sidecar(file.path, on_disk, m_context.descs)
				: classify_desc(file.path, on_disk, m_context.descs);
			if (again.kind != file_class::current) {
				throw std::runtime_error(std::format("{}: still {} after the migration {}", file.path, to_string(again.kind), again.reason));
			}
			if (file.written_as != "engine") return;
			const std::optional<res::tag> tag = tag_of(file.path, m_content_roots);
			if (!tag) return;
			try {
				load_from(*tag, on_disk);
			} catch (const std::exception& e) {
				throw std::runtime_error(std::format("{}: does not load after the migration: {}", file.path, e.what()));
			}
		}

		// project.snkproj after the project steps, stamped: the engine that migrated it, which also ends the version
		// dialog, and every project step that ever ran on it (EPIC-58 2, 4.1).
		std::string manifest_form(applied_file& file, json::object& manifest) const
		{
			const std::string engine = prj::to_string(prj::current_engine_version());
			manifest["engine_version"] = engine;
			manifest["migrated_with"] = engine;
			json::array applied;
			if (const json::value* before = manifest.if_contains("applied_steps"); before && before->is_array()) {
				for (const json::value& id : before->as_array()) {
					if (id.is_string()) applied.push_back(id);
				}
			}
			for (const std::string& id : m_report.project_steps) {
				if (std::ranges::find(applied, json::value(id)) == applied.end()) applied.push_back(json::string(id));
			}
			manifest["applied_steps"] = std::move(applied);
			file.written_as = "manifest";
			return prj::to_indented_json(manifest);
		}

		void check_manifest(const applied_file& file, std::string_view written) const
		{
			const std::string on_disk = read_or_throw(m_context.root, file.path);
			if (on_disk != written) throw std::runtime_error(std::format("{}: the file on the disk is not what was written", file.path));
			const prj::manifest_result parsed = prj::parse_manifest(on_disk);
			if (!parsed.data) {
				throw std::runtime_error(std::format("{}: does not read after the migration: {}", file.path,
					parsed.error ? prj::to_string(*parsed.error) : std::string{ "unknown error" }));
			}
			if (prj::compare_engine_versions(parsed.data->engine, prj::current_engine_version()) != prj::version_match::exact) {
				throw std::runtime_error(std::format("{}: engine_version is {} after the migration", file.path, prj::to_string(parsed.data->engine)));
			}
		}

		void run_project_steps(const migration_plan& plan, std::string_view phase, json::object& manifest)
		{
			for (const planned_project_step& planned : plan.project_steps) {
				if (planned.phase != phase) continue;
				const project_step* step = m_context.project_steps->find(planned.id);
				project_step_context step_context{ m_context.root, plan.scan, manifest, m_report.project_notes };
				step->apply(step_context);
				m_report.project_steps.push_back(planned.id);
			}
		}

	private:
		std::shared_ptr<res::resource_entry> load_from(const res::tag& tag, std::string_view text) const
		{
			const auto* first = reinterpret_cast<const std::byte*>(text.data());
			const std::vector<std::byte> bytes(first, first + text.size());
			res::res_deps deps;
			for (res::res_dep& dep : m_adapter.collect_dependencies(tag, bytes)) {
				auto parent = m_context.resources.require_sync<desc::desc_base>(dep.target).get_sync();
				if (!parent) throw std::runtime_error(std::format("its parent '{}' does not load", dep.target.view()));
				deps.add(std::move(dep), parent);
			}
			auto loaded = m_adapter.deserialize(tag, bytes, deps);
			if (!loaded) throw std::runtime_error("the engine returned nothing");
			return loaded;
		}

		const apply_context& m_context;
		migration_report& m_report;
		const std::vector<fs::path>& m_content_roots;
		project_writer m_write;
		core::desc::desc_adapter_t m_adapter;
	};

	// Puts back every file the failed migration may have written and deletes what it created; the errors, empty when
	// the project is as it was.
	std::string roll_back(const fs::path& root, const project_writer& write, const migration_report& report,
		const std::vector<std::string>& originals, const std::vector<fs::path>& content_roots)
	{
		std::string errors;
		const auto add = [&errors](const std::string& error) { errors += (errors.empty() ? "" : "; ") + error; };
		for (std::size_t i = 0; i < report.files.size(); ++i) {
			const applied_file& file = report.files[i];
			if (file.hash_after.empty()) continue;
			if (auto error = restore_file(root, write, file.path, file.hash_before, file.hash_after, originals[i], true)) add(*error);
		}
		for (const std::string& created : report.created_files) {
			if (auto error = created_file_error(created, content_roots)) {
				add(*error);
			} else if (auto removed = remove_created(root, created)) {
				add(*removed);
			}
		}
		return errors;
	}
}

std::string_view prj::migration::to_string(migration_status status)
{
	switch (status) {
	case migration_status::in_progress: return "in_progress";
	case migration_status::ok: return "ok";
	case migration_status::partial: return "partial";
	case migration_status::rolled_back: return "rolled_back";
	case migration_status::rollback_failed: return "rollback_failed";
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
	for (const planned_project_step& step : plan.project_steps) {
		if (!context.project_steps || !context.project_steps->find(step.id)) {
			outcome.refused = std::format("project step '{}' is not registered", step.id);
			return outcome;
		}
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
	const std::vector<fs::path>& content_roots = loaded_manifest.data->content_roots;

	std::vector<std::string> originals;
	try {
		for (const file_plan& planned : plan.files) {
			if (!is_plain_relative(planned.path)) throw std::runtime_error(std::format("'{}' is not a relative path inside the project", planned.path));
			applied_file file;
			file.path = planned.path;
			file.steps = planned.steps;
			file.warnings = planned.warnings;
			originals.push_back(read_or_throw(context.root, planned.path));
			file.hash_before = hash_of(originals.back());
			report.files.push_back(std::move(file));
		}
		// Last: it is written after the post steps, and is in the backup and the journal as any other file.
		applied_file manifest_file;
		manifest_file.path = std::string(k_manifest_path);
		originals.push_back(read_or_throw(context.root, manifest_file.path));
		manifest_file.hash_before = hash_of(originals.back());
		report.files.push_back(std::move(manifest_file));
	} catch (const std::exception& e) {
		outcome.refused = e.what();
		report.files.clear();
		return outcome;
	}

	applier apply(context, report, content_roots);
	report.status = migration_status::in_progress;
	const auto make_id = context.make_id ? context.make_id : std::function<std::string()>{ default_id };
	const std::string base_id = make_id();
	if (!is_plain_relative(base_id) || base_id.find('/') != std::string::npos) {
		outcome.refused = std::format("'{}' cannot name a migration", base_id);
		return outcome;
	}
	for (int suffix = 1;; ++suffix) {
		report.id = suffix == 1 ? base_id : std::format("{}-{}", base_id, suffix);
		report.backup = std::format("{}/{}/backup", k_migrations_dir, report.id);
		std::string error;
		const create_result created = create_new_file(context.root, apply.journal_path(), apply.journal_text(), error);
		if (created == create_result::created) break;
		if (created == create_result::failed || suffix == 1000) {
			outcome.refused = std::format("the journal cannot be written: {}", error);
			report.id.clear();
			report.backup.clear();
			return outcome;
		}
	}

	{
		watch_pause watch{ context.pause_watch, context.resume_watch };
		try {
			for (std::size_t i = 0; i < report.files.size(); ++i) {
				apply.write(report.backup + "/" + report.files[i].path, originals[i]);
			}
			watch.pause();
			const std::size_t manifest_index = report.files.size() - 1;
			json::value manifest_json = json::parse(originals[manifest_index], {}, parse_options());
			if (!manifest_json.is_object()) throw std::runtime_error("project.snkproj: the root is not a JSON object");

			apply.run_project_steps(plan, to_string(project_phase::pre), manifest_json.as_object());
			for (std::size_t i = 0; i < manifest_index; ++i) {
				applied_file& file = report.files[i];
				const std::string text = apply.form_of(file, originals[i]);
				file.hash_after = hash_of(text);
				apply.journal();
				apply.write(file.path, text);
				apply.check(file, text);
			}
			apply.run_project_steps(plan, to_string(project_phase::post), manifest_json.as_object());

			applied_file& manifest_file = report.files[manifest_index];
			const std::string text = apply.manifest_form(manifest_file, manifest_json.as_object());
			manifest_file.hash_after = hash_of(text);
			apply.journal();
			apply.write(manifest_file.path, text);
			apply.check_manifest(manifest_file, text);
			report.status = report.skipped.empty() ? migration_status::ok : migration_status::partial;
		} catch (const std::exception& e) {
			report.error = e.what();
			egLOG_ERROR("project/migrate", "migration {} failed, rolling back: {}", report.id, report.error);
			const std::string restore_errors = roll_back(context.root, apply.writer(), report, originals, content_roots);
			if (restore_errors.empty()) {
				report.status = migration_status::rolled_back;
			} else {
				report.status = migration_status::rollback_failed;
				report.error += "; the rollback failed: " + restore_errors;
				egLOG_ERROR("project/migrate", "migration {}: the rollback failed: {}", report.id, restore_errors);
			}
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
		const json::value journal = json::parse(text.text, error, {}, parse_options());
		if (error || !journal.is_object()) continue;
		const json::value* status = journal.as_object().if_contains("status");
		if (!status || !status->is_string()) continue;
		if (status->as_string() == to_string(migration_status::in_progress) || status->as_string() == to_string(migration_status::rollback_failed)) {
			ids.push_back(id);
		}
	}
	std::ranges::sort(ids);
	return ids;
}

std::optional<std::string> prj::migration::rollback_migration(const fs::path& root, std::string_view id, const rollback_context& context)
{
	if (!is_plain_relative(id) || id.find('/') != std::string_view::npos) return std::format("'{}' does not name a migration", id);
	const project_writer writer = context.write ? context.write : project_writer{ default_write };
	const std::string journal_path = std::format("{}/{}/report.json", k_migrations_dir, id);
	const prj::project_file_text text = prj::read_project_file(root, fs_paths::path_from_utf8(journal_path));
	if (!text.exists || !text.error.empty()) return std::format("migration {} has no journal{}", id, text.error.empty() ? "" : ": " + text.error);
	boost::system::error_code error;
	json::value journal = json::parse(text.text, error, {}, parse_options());
	if (error || !journal.is_object()) return std::format("the journal of migration {} cannot be read", id);
	json::object& object = journal.as_object();
	const auto string_of = [](const json::object& from, std::string_view key) {
		const json::value* value = from.if_contains(key);
		return value && value->is_string() ? std::string(value->as_string()) : std::string{};
	};
	const std::string backup = string_of(object, "backup");

	struct entry { std::string path; std::string hash_before; std::string hash_after; file_state state; };
	std::vector<entry> files;
	std::vector<std::string> refused;
	if (const json::value* list = object.if_contains("files"); list && list->is_array()) {
		for (const json::value& item : list->as_array()) {
			if (!item.is_object()) continue;
			const json::object& file = item.as_object();
			entry next{ string_of(file, "path"), string_of(file, "hash_before"), string_of(file, "hash_after"), file_state::changed };
			if (!is_plain_relative(next.path) || next.hash_before.empty()) {
				refused.push_back(std::format("'{}' (not a relative path inside the project, or no hash)", next.path));
				continue;
			}
			next.state = state_of(root, next.path, next.hash_before, next.hash_after);
			if (next.state == file_state::changed) refused.push_back(next.path);
			files.push_back(std::move(next));
		}
	}
	const bool restores = std::ranges::any_of(files, [](const entry& file) { return file.state == file_state::migrated; });
	if (restores && !is_plain_relative(backup)) refused.push_back(std::format("the backup '{}' (not a relative path inside the project)", backup));

	std::vector<std::string> created;
	if (const json::value* list = object.if_contains("created_files"); list && list->is_array()) {
		for (const json::value& item : list->as_array()) {
			if (item.is_string()) created.emplace_back(item.as_string());
		}
	}
	if (!created.empty()) {
		const prj::manifest_result manifest = prj::load_manifest(root);
		for (const std::string& path : created) {
			if (!manifest.data) {
				refused.push_back(std::format("'{}' (project.snkproj cannot be read, so its content roots are unknown)", path));
			} else if (auto why = created_file_error(path, manifest.data->content_roots)) {
				refused.push_back(*why);
			}
		}
	}
	if (!refused.empty()) {
		std::string list;
		for (const std::string& path : refused) list += (list.empty() ? "" : ", ") + path;
		return std::format("migration {} cannot be rolled back, nothing was written: {}", id, list);
	}

	// in_progress before the first write: a rollback that stops half way leaves the project for unfinished_migrations.
	const auto journal_as = [&](migration_status status) {
		object["status"] = std::string(to_string(status));
		return writer(root, fs_paths::path_from_utf8(journal_path), prj::to_indented_json(journal));
	};
	if (auto failed = journal_as(migration_status::in_progress)) return std::format("the journal cannot be written: {}", *failed);

	const auto roll_back_files = [&]() -> std::optional<std::string> {
		for (const entry& file : files) {
			if (file.state != file_state::migrated) continue;
			const prj::project_file_text saved = prj::read_project_file(root, fs_paths::path_from_utf8(backup + "/" + file.path), k_max_file_bytes);
			if (!saved.exists || !saved.error.empty()) {
				return std::format("{}: the backup cannot be read{}", file.path, saved.error.empty() ? "" : ": " + saved.error);
			}
			if (auto failed = restore_file(root, writer, file.path, file.hash_before, file.hash_after, saved.text, false)) return failed;
		}
		for (const std::string& path : created) {
			if (auto failed = remove_created(root, path)) return failed;
		}
		return std::nullopt;
	};

	std::optional<std::string> failed;
	{
		watch_pause watch{ context.pause_watch, context.resume_watch };
		watch.pause();
		failed = roll_back_files();
	}
	if (failed) {
		object["error"] = *failed;
		if (auto journal_failed = journal_as(migration_status::rollback_failed)) {
			egLOG_ERROR("project/migrate", "the journal of {} cannot be written: {}", id, *journal_failed);
		}
		return failed;
	}
	if (auto journal_failed = journal_as(migration_status::rolled_back)) return std::format("the journal cannot be written: {}", *journal_failed);
	return std::nullopt;
}
