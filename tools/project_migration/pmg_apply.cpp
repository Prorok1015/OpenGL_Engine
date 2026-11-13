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
#include <snk/resource/res_asset_walk.h>
#include <snk/resource/res_system.h>
#include <boost/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <map>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <set>
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
	// hash_after of a file a project step deleted: no hash_of() is ever this.
	constexpr std::string_view k_removed = "removed";

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
		if (!now.exists && now.error.empty() && hash_after == k_removed) return file_state::migrated;
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

	struct root_guid
	{
		json::value value;
		std::size_t position = 0;
	};

	// Puts the root __guid of a saved desc back to what the file had: the same value, or none. One the save dropped
	// goes back at the position it had in the file. Whether anything changed.
	bool restore_guid(json::object& saved, const std::optional<root_guid>& before)
	{
		json::value* now = saved.if_contains("__guid");
		if (!before) return now && saved.erase("__guid") > 0;
		if (now) {
			if (*now == before->value) return false;
			*now = before->value;
			return true;
		}
		json::object ordered;
		ordered.reserve(saved.size() + 1);
		std::size_t index = 0;
		for (auto& member : saved) {
			if (index++ == before->position) ordered.emplace("__guid", before->value);
			ordered.emplace(member.key(), std::move(member.value()));
		}
		if (!ordered.contains("__guid")) ordered.emplace("__guid", before->value);
		saved = std::move(ordered);
		return true;
	}

	bool is_desc_path(std::string_view path)
	{
		constexpr std::string_view suffix = ".desc";
		if (path.size() <= suffix.size()) return false;
		for (std::size_t i = 0; i < suffix.size(); ++i) {
			const char c = path[path.size() - suffix.size() + i];
			if ((c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c) != suffix[i]) return false;
		}
		return true;
	}

	// Writes the files a migration changes: the planned ones, project.snkproj and what the project steps touch through
	// project_step_files. report.files and `originals` stay parallel, project.snkproj last.
	class applier final : public project_step_files
	{
	public:
		applier(const apply_context& context, migration_report& report, const std::vector<fs::path>& content_roots, std::int64_t resource_meta_version,
			std::vector<std::string>& originals)
			: m_context(context)
			, m_report(report)
			, m_content_roots(content_roots)
			, m_resource_meta_version(resource_meta_version)
			, m_originals(originals)
			, m_write(context.write ? context.write : project_writer{ default_write })
			, m_adapter(context.resources, context.descs)
			, m_assets(context.root, content_roots)
		{
		}

		applier(const applier&) = delete;
		applier& operator=(const applier&) = delete;

		// The text a planned file is migrated from: a pre step's write, else the original. None when a step deleted it.
		std::optional<std::string> input_of(std::size_t index) const
		{
			const std::string key = key_of(m_report.files[index].path);
			if (m_removed.contains(key)) return std::nullopt;
			if (const auto written = m_current.find(key); written != m_current.end()) return written->second.text;
			return m_originals[index];
		}

		// After a type migration wrote a file, a post step reads that.
		void wrote(const std::string& path, std::string text) { m_current.insert_or_assign(key_of(path), written_file{ path, std::move(text) }); }

		std::string read(std::string_view path) override
		{
			const std::string relative = step_path(path);
			const std::string key = key_of(relative);
			if (m_removed.contains(key)) throw std::runtime_error(std::format("{}: {} was deleted by this migration", m_step, relative));
			if (const auto written = m_current.find(key); written != m_current.end()) return written->second.text;
			return read_or_throw(m_context.root, relative);
		}

		bool exists(std::string_view path) override
		{
			const std::string relative = step_path(path);
			const std::string key = key_of(relative);
			if (m_removed.contains(key)) return false;
			if (m_current.contains(key)) return true;
			const prj::project_file_text now = prj::read_project_file(m_context.root, fs_paths::path_from_utf8(relative), k_max_file_bytes);
			if (!now.error.empty()) throw std::runtime_error(std::format("{}: {}: {}", m_step, relative, now.error));
			return now.exists;
		}

		void write(std::string_view path, std::string_view text) override
		{
			const std::string relative = step_path(path);
			if (!exists(relative)) throw std::runtime_error(std::format("{}: {} is not there to be written (create makes a new file)", m_step, relative));
			overwrite(relative, text);
		}

		void create(std::string_view path, std::string_view text) override
		{
			const std::string relative = step_path(path);
			const std::string key = key_of(relative);
			if (exists(relative)) throw std::runtime_error(std::format("{}: {} cannot be created: something is at its name", m_step, relative));
			// A file this migration deleted: writing it again is a change of that file, which the rollback puts back from
			// the backup; listed as created, the rollback would delete what it has just restored.
			if (m_removed.contains(key)) {
				m_removed.erase(key);
				overwrite(relative, text);
				return;
			}
			std::string error;
			switch (create_new_file(m_context.root, relative, text, error)) {
			case create_result::created: break;
			case create_result::taken: throw std::runtime_error(std::format("{}: {} cannot be created: something is at its name", m_step, relative));
			case create_result::failed: throw std::runtime_error(std::format("{}: {} cannot be created: {}", m_step, relative, error));
			}
			// After the file is there: a failure before it leaves nothing for a rollback to delete.
			m_report.created_files.push_back(relative);
			journal();
			m_current.insert_or_assign(key, written_file{ relative, std::string(text) });
			m_step_touched.insert(key);
		}

		void remove(std::string_view path) override
		{
			const std::string relative = step_path(path);
			const std::string key = key_of(relative);
			if (!exists(relative)) throw std::runtime_error(std::format("{}: {} is not there to be deleted", m_step, relative));
			if (created_here(key)) throw std::runtime_error(std::format("{}: {} was created by this migration and is not deleted by it", m_step, relative));
			applied_file& file = m_report.files[entry_for(relative)];
			file.notes.push_back(std::format("deleted by the project step {}", m_step));
			file.hash_after = std::string(k_removed);
			journal();
			if (auto error = remove_created(m_context.root, file.path)) throw std::runtime_error(std::format("{}: {}", m_step, *error));
			m_current.erase(key);
			m_removed.insert(key);
			m_step_touched.erase(key);
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

		void put(const std::string& relative, std::string_view text)
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
			// A sidecar is no desc the engine loads: its steps are all there is to it, on its root only (EPIC-58 3.4).
			if (sidecar) {
				migrate_sidecar(m_context.descs, migrated.as_object(), file.path);
				file.written_as = "json";
				return text_of(m_context.descs.serialize_to_bytes(migrated));
			}
			// The engine's save makes a __guid for a res:// desc that has none or an empty one (desc_adapter_t::serialize);
			// a migration writes the root __guid exactly as the file had it, or none (#851): the ID of a file is its
			// .resource's (EPIC-57 4.2).
			std::optional<root_guid> guid_before;
			if (migrated.is_object()) {
				const json::object& root = migrated.as_object();
				for (auto member = root.begin(); member != root.end(); ++member) {
					if (member->key() != "__guid") continue;
					guid_before = root_guid{ member->value(), static_cast<std::size_t>(member - root.begin()) };
					break;
				}
			}
			m_context.descs.migrate_tree(migrated.as_object(), file.path);
			m_context.descs.stamp_tree(migrated);

			std::string why_json;
			if (const std::optional<res::tag> tag = tag_of(file.path, m_content_roots); !tag) {
				why_json = "not under a content root";
			} else {
				try {
					std::string saved = text_of(m_adapter.serialize(*tag, load_from(*tag, original)));
					json::value written = json::parse(saved, {}, parse_options());
					why_json = lost_parent(migrated, &written, {});
					if (why_json.empty()) {
						file.written_as = "engine";
						if (written.is_object() && restore_guid(written.as_object(), guid_before)) {
							return text_of(m_context.descs.serialize_to_bytes(written));
						}
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
			if (!res::is_asset_meta_name(file.path) && !is_desc_path(file.path)) return;
			// The class of a desc is its __type and __version: a __guid the header keeps or lost is never a difference.
			const scanned_file again = res::is_asset_meta_name(file.path)
				? classify_sidecar(file.path, on_disk, m_context.descs, m_resource_meta_version)
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

		// project.snkproj after the project steps, stamped with the engine that migrated it and every project step that
		// ever ran on it (EPIC-58 2, 4.1). Only a complete migration writes this engine's engine_version, which ends the
		// version dialog: files of a module that is not loaded are still to migrate.
		std::string manifest_form(applied_file& file, json::object& manifest, bool complete)
		{
			const std::string engine = prj::to_string(prj::current_engine_version());
			if (complete) manifest["engine_version"] = engine;
			manifest["migrated_with"] = engine;
			json::array applied;
			if (const json::value* before = manifest.if_contains("applied_steps"); before && before->is_array()) {
				for (const json::value& id : before->as_array()) {
					if (id.is_string()) {
						applied.push_back(id);
					} else {
						m_report.project_notes.push_back(std::format("project.snkproj: an entry of applied_steps that is not a string was dropped: {}",
							json::serialize(id)));
					}
				}
			} else if (before) {
				m_report.project_notes.push_back(std::format("project.snkproj: applied_steps was not a list and is replaced: {}", json::serialize(*before)));
			}
			for (const std::string& id : m_report.project_steps) {
				if (std::ranges::find(applied, json::value(id)) == applied.end()) applied.push_back(json::string(id));
			}
			manifest["applied_steps"] = std::move(applied);
			file.written_as = "manifest";
			return prj::to_indented_json(manifest);
		}

		void check_manifest(const applied_file& file, std::string_view written, bool complete) const
		{
			const std::string on_disk = read_or_throw(m_context.root, file.path);
			if (on_disk != written) throw std::runtime_error(std::format("{}: the file on the disk is not what was written", file.path));
			const prj::manifest_result parsed = prj::parse_manifest(on_disk);
			if (!parsed.data) {
				throw std::runtime_error(std::format("{}: does not read after the migration: {}", file.path,
					parsed.error ? prj::to_string(*parsed.error) : std::string{ "unknown error" }));
			}
			if (complete && prj::compare_engine_versions(parsed.data->engine, prj::current_engine_version()) != prj::version_match::exact) {
				throw std::runtime_error(std::format("{}: engine_version is {} after the migration", file.path, prj::to_string(parsed.data->engine)));
			}
		}

		// The steps of `phase` in the order of the plan, then a check of every file they left: a planned file a pre
		// step wrote is checked once it is migrated. The assets are read before the pre steps and again after them.
		void run_project_steps(const migration_plan& plan, project_phase phase, json::object& manifest, std::size_t planned_files)
		{
			if (phase == project_phase::pre) m_assets.rebuild();
			m_step_touched.clear();
			for (const planned_project_step& planned : plan.project_steps) {
				if (planned.phase != to_string(phase)) continue;
				const project_step* step = m_context.project_steps->find(planned.id);
				m_step = planned.id;
				project_step_context step_context{ m_context.root, plan.scan, manifest, m_report.project_notes, *this, m_assets, m_step };
				step->apply(step_context);
				m_report.project_steps.push_back(planned.id);
			}
			m_step.clear();
			for (const std::string& key : m_step_touched) {
				const written_file& written = m_current.at(key);
				const std::string& path = written.path;
				const std::string& text = written.text;
				const auto found = std::ranges::find(m_report.files, key, [](const applied_file& file) { return key_of(file.path); });
				const bool planned = found != m_report.files.end() && static_cast<std::size_t>(found - m_report.files.begin()) < planned_files;
				if (phase == project_phase::pre && planned) continue;
				applied_file probe;
				probe.path = path;
				check(found != m_report.files.end() ? *found : probe, text);
			}
			if (phase == project_phase::pre) m_assets.rebuild();
		}

	private:
		struct written_file
		{
			// As the migration spells it (the report's spelling when the file is in the report).
			std::string path;
			std::string text;
		};

		// Two spellings of one file share the key: on Windows names differ only in letter case, as res:// merges them.
		static std::string key_of(std::string_view path) { return res::asset_name_key(path, res::asset_names_case_insensitive); }

		bool created_here(const std::string& key) const
		{
			return std::ranges::any_of(m_report.created_files, [&key](const std::string& created) { return key_of(created) == key; });
		}

		// A step writes like a change of the asset tree: the path is rebuilt from its parts ("res//a.png" is
		// "res/a.png"), lies below a content root, and has no name the asset walk skips (".snk/", ".git/", a temporary
		// file); project.snkproj is the context's manifest; below a content root "." neither code/ nor build/, which
		// the scan leaves out too. Names are compared by key, so another letter case passes none of these.
		std::string step_path(std::string_view path) const
		{
			const std::string given(path);
			if (!is_plain_relative(given)) throw std::runtime_error(std::format("{}: '{}' is not a relative path inside the project", m_step, given));
			std::string relative;
			std::vector<std::string> parts;
			for (const fs::path& part : fs_paths::path_from_utf8(given)) {
				const std::string name = fs_paths::utf8_of(part);
				if (name.empty()) continue;
				relative += (relative.empty() ? "" : "/") + name;
				parts.push_back(name);
			}
			if (parts.empty()) throw std::runtime_error(std::format("{}: '{}' names no file", m_step, given));
			for (std::size_t i = 0; i < parts.size(); ++i) {
				if (res::is_skipped_asset_name(parts[i], i + 1 < parts.size())) {
					throw std::runtime_error(std::format("{}: '{}' is no asset (the asset walk leaves '{}' out)", m_step, relative, parts[i]));
				}
			}
			const std::string key = key_of(relative);
			if (key == key_of(k_manifest_path)) throw std::runtime_error(std::format("{}: project.snkproj is the context's manifest, not a file", m_step));
			bool inside = false;
			for (const fs::path& root : m_content_roots) {
				std::string prefix = fs_paths::generic_utf8_of(root.lexically_normal());
				while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
				if (prefix == "." || prefix.empty()) {
					if (parts.size() > 1 && res::is_project_own_name(parts.front())) {
						throw std::runtime_error(std::format("{}: '{}' is the project's code or build output, not content", m_step, relative));
					}
					inside = true;
				} else if (key.starts_with(key_of(prefix) + "/")) {
					inside = true;
				}
			}
			if (!inside) throw std::runtime_error(std::format("{}: '{}' is not inside a content root", m_step, relative));
			return relative;
		}

		// A change of an existing file (or of one this migration deleted): its entry, backup and journal first.
		void overwrite(const std::string& relative, std::string_view text)
		{
			const std::string key = key_of(relative);
			if (created_here(key)) {
				put(relative, text);
				m_current.insert_or_assign(key, written_file{ relative, std::string(text) });
				m_step_touched.insert(key);
				return;
			}
			applied_file& file = m_report.files[entry_for(relative)];
			if (file.written_as.empty()) file.written_as = "step:" + m_step;
			const std::string note = std::format("written by the project step {}", m_step);
			if (std::ranges::find(file.notes, note) == file.notes.end()) file.notes.push_back(note);
			file.hash_after = hash_of(text);
			journal();
			put(file.path, text);
			m_current.insert_or_assign(key, written_file{ file.path, std::string(text) });
			m_step_touched.insert(key);
		}

		// The report's entry of `path`, made (and the original backed up) the first time a step changes a file that is
		// not planned; it goes before project.snkproj, which stays last.
		std::size_t entry_for(const std::string& path)
		{
			const std::string key = key_of(path);
			const auto found = std::ranges::find(m_report.files, key, [](const applied_file& file) { return key_of(file.path); });
			if (found != m_report.files.end()) {
				return static_cast<std::size_t>(found - m_report.files.begin());
			}
			std::string original = read_or_throw(m_context.root, path);
			applied_file file;
			file.path = path;
			file.hash_before = hash_of(original);
			put(m_report.backup + "/" + path, original);
			const std::size_t index = m_report.files.size() - 1;
			m_report.files.insert(m_report.files.begin() + static_cast<std::ptrdiff_t>(index), std::move(file));
			m_originals.insert(m_originals.begin() + static_cast<std::ptrdiff_t>(index), std::move(original));
			journal();
			return index;
		}

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
		std::int64_t m_resource_meta_version;
		std::vector<std::string>& m_originals;
		project_writer m_write;
		core::desc::desc_adapter_t m_adapter;
		project_assets m_assets;
		// The step that runs, for the messages.
		std::string m_step;
		// By key_of: what this migration wrote so far; deleted files are only in m_removed.
		std::map<std::string, written_file> m_current;
		std::set<std::string> m_removed;
		// What the steps of the phase that runs wrote or created, for the check after it.
		std::set<std::string> m_step_touched;
	};

	// project.snkproj changed after the migration (a later migration, an edit by hand): only what this migration
	// stamped goes back - engine_version, migrated_with, its own steps out of applied_steps, and format_version if
	// engine.manifest_format raised it. Every other key stays as it is now. Throws with the reason.
	std::string unstamped_manifest(std::string_view now_text, std::string_view backup_text, const std::vector<std::string>& steps)
	{
		boost::system::error_code error;
		json::value now = json::parse(now_text, error, {}, parse_options());
		if (error || !now.is_object()) throw std::runtime_error("project.snkproj is not a JSON object now");
		const json::value before = json::parse(backup_text, error, {}, parse_options());
		if (error || !before.is_object()) throw std::runtime_error("the backup of project.snkproj is not a JSON object");
		json::object& manifest = now.as_object();
		const json::object& original = before.as_object();
		const auto restore = [&](std::string_view key) {
			if (const json::value* was = original.if_contains(key)) {
				manifest[key] = *was;
			} else {
				manifest.erase(key);
			}
		};
		restore("engine_version");
		restore("migrated_with");
		if (std::ranges::find(steps, "engine.manifest_format") != steps.end()) restore("format_version");

		const json::value* had = original.if_contains("applied_steps");
		const auto had_step = [had](const json::value& id) {
			return had && had->is_array() && std::ranges::find(had->as_array(), id) != had->as_array().end();
		};
		if (json::value* applied = manifest.if_contains("applied_steps"); applied && applied->is_array()) {
			json::array kept;
			for (const json::value& id : applied->as_array()) {
				const bool ours = id.is_string() && std::ranges::find(steps, std::string_view(id.as_string())) != steps.end();
				if (!ours || had_step(id)) kept.push_back(id);
			}
			if (kept.empty() && !had) {
				manifest.erase("applied_steps");
			} else {
				*applied = std::move(kept);
			}
		}
		return prj::to_indented_json(now);
	}

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
	if (!plan.refused.empty()) {
		outcome.refused = plan.refused;
		return outcome;
	}
	if (const int steps_to = context.descs.current_version(res::asset_meta_type); steps_to != plan.resource_meta_version) {
		outcome.refused = std::format("the steps of {} lead to version {}, but the engine reads version {}", res::asset_meta_type, steps_to,
			plan.resource_meta_version);
		return outcome;
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

	applier apply(context, report, content_roots, plan.resource_meta_version, originals);
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
				apply.put(report.backup + "/" + report.files[i].path, originals[i]);
			}
			watch.pause();
			json::value manifest_json = json::parse(originals.back(), {}, parse_options());
			if (!manifest_json.is_object()) throw std::runtime_error("project.snkproj: the root is not a JSON object");

			const std::size_t planned_files = plan.files.size();
			apply.run_project_steps(plan, project_phase::pre, manifest_json.as_object(), planned_files);
			for (std::size_t i = 0; i < planned_files; ++i) {
				const std::optional<std::string> input = apply.input_of(i);
				if (!input) continue;
				applied_file& file = report.files[i];
				std::string text = apply.form_of(file, *input);
				file.hash_after = hash_of(text);
				apply.journal();
				apply.put(file.path, text);
				apply.check(file, text);
				apply.wrote(file.path, std::move(text));
			}
			apply.run_project_steps(plan, project_phase::post, manifest_json.as_object(), planned_files);

			applied_file& manifest_file = report.files.back();
			const bool complete = report.skipped.empty();
			const std::string text = apply.manifest_form(manifest_file, manifest_json.as_object(), complete);
			manifest_file.hash_after = hash_of(text);
			apply.journal();
			apply.put(manifest_file.path, text);
			apply.check_manifest(manifest_file, text, complete);
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

	std::vector<std::string> project_steps;
	if (const json::value* list = object.if_contains("project_steps"); list && list->is_array()) {
		for (const json::value& item : list->as_array()) {
			if (item.is_string()) project_steps.emplace_back(item.as_string());
		}
	}

	// unstamp: project.snkproj changed after the migration; only its stamp goes back (unstamped_manifest).
	struct entry { std::string path; std::string hash_before; std::string hash_after; file_state state; bool unstamp = false; };
	std::vector<entry> files;
	std::vector<std::string> refused;
	if (const json::value* list = object.if_contains("files"); list && list->is_array()) {
		for (const json::value& item : list->as_array()) {
			if (!item.is_object()) continue;
			const json::object& file = item.as_object();
			entry next{ string_of(file, "path"), string_of(file, "hash_before"), string_of(file, "hash_after"), file_state::changed, false };
			if (!is_plain_relative(next.path) || next.hash_before.empty()) {
				refused.push_back(std::format("'{}' (not a relative path inside the project, or no hash)", next.path));
				continue;
			}
			next.state = state_of(root, next.path, next.hash_before, next.hash_after);
			if (next.state == file_state::changed) {
				if (next.path == k_manifest_path && !next.hash_after.empty()) {
					next.unstamp = true;
				} else {
					refused.push_back(next.path);
				}
			}
			files.push_back(std::move(next));
		}
	}
	const bool restores = std::ranges::any_of(files, [](const entry& file) { return file.state == file_state::migrated || file.unstamp; });
	if (restores && !is_plain_relative(backup)) refused.push_back(std::format("the backup '{}' (not a relative path inside the project)", backup));

	// The new project.snkproj is worked out before anything is written, so a manifest that cannot be read refuses the
	// whole rollback.
	std::optional<std::string> unstamped;
	if (const auto manifest = std::ranges::find_if(files, &entry::unstamp); manifest != files.end() && is_plain_relative(backup)) {
		const prj::project_file_text now = prj::read_project_file(root, fs_paths::path_from_utf8(manifest->path), k_max_file_bytes);
		const prj::project_file_text saved = prj::read_project_file(root, fs_paths::path_from_utf8(backup + "/" + manifest->path), k_max_file_bytes);
		try {
			if (!now.exists || !now.error.empty()) throw std::runtime_error("project.snkproj cannot be read");
			if (!saved.exists || !saved.error.empty()) throw std::runtime_error("the backup of project.snkproj cannot be read");
			if (hash_of(saved.text) != manifest->hash_before) throw std::runtime_error("the backup of project.snkproj is not the original");
			unstamped = unstamped_manifest(now.text, saved.text, project_steps);
		} catch (const std::exception& e) {
			refused.push_back(std::format("{} ({})", manifest->path, e.what()));
		}
	}

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
			if (file.unstamp) {
				if (auto failed = writer(root, fs_paths::path_from_utf8(file.path), *unstamped)) return std::format("{} not restored: {}", file.path, *failed);
				continue;
			}
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
