#include "edt_desc_file_commands.h"
#include "edt_forge_types.h"
#include "edt_code_commands.h"
#include "edt_file_menu.h"
#include "edt_forge_types.h"
#include "edt_reference.h"
#include "edt_tag_text.h"
#include <snk/resource/res_reference.h>
#include <snk/common/engine_assert.h>
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>
#include <snk/desc/desc_schema_ops.h>
#include <snk/desc/desc_system.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/resource/res_asset_meta.h>
#include <snk/resource/res_path_sanitize.h>
#include <snk/resource/res_system.h>
#include <boost/json.hpp>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iterator>
#include <limits>
#include <chrono>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
	namespace json = boost::json;
	namespace fs = std::filesystem;

	constexpr std::chrono::seconds k_read_timeout{ 5 };
	constexpr std::size_t k_max_reported = 20;

	json::value parse_schema(std::string_view text)
	{
		boost::system::error_code ec;
		json::value parsed = json::parse(text, ec);
		ASSERT_MSG(!ec, "a desc.* schema is not JSON: {}", ec.message());
		return parsed;
	}

	std::string_view view_of(const json::string& text)
	{
		return { text.data(), text.size() };
	}

	std::string text_of(const std::vector<std::byte>& bytes)
	{
		return { reinterpret_cast<const char*>(bytes.data()), bytes.size() };
	}

	std::string who(const cmd::origin& from)
	{
		switch (from.kind) {
		case cmd::origin_kind::agent: return "agent " + from.session;
		case cmd::origin_kind::test: return "test";
		case cmd::origin_kind::game: return "game";
		case cmd::origin_kind::ui: return "ui";
		}
		return "ui";
	}

	json::object diagnostic_json(const res::diagnostic& item)
	{
		return json::object{ { "severity", item.severity == res::diagnostic_severity::error ? "error" : "warning" },
			{ "kind", item.kind }, { "resource", item.resource }, { "json_path", item.json_path }, { "reason", item.reason } };
	}

	std::string one_line(const std::vector<res::diagnostic>& problems)
	{
		const res::diagnostic& first = problems.front();
		std::string text = std::format("{}{}: {}", first.resource, first.json_path, first.reason);
		if (problems.size() > 1) text += std::format(" (and {} more)", problems.size() - 1);
		return text;
	}

	cmd::result invalid_content(const std::vector<res::diagnostic>& problems)
	{
		cmd::result out = cmd::result::failure(cmd::codes::invalid_args,
			"the desc does not fit its schemas, nothing was written: " + one_line(problems));
		json::array errors;
		for (const res::diagnostic& item : problems) errors.push_back(diagnostic_json(item));
		out.value = json::object{ { "errors", std::move(errors) } };
		return out;
	}

	cmd::result stale(std::string_view path, const std::string& version)
	{
		cmd::result out = cmd::result::failure(cmd::codes::stale,
			std::format("'{}' changed since that version was read: read it again", path));
		out.value = json::object{ { "version", version } };
		return out;
	}

	struct located_file
	{
		fs::path file;
		std::size_t layer = 0;
		bool found = false;
		std::string error;
	};

	// What the open level says of a file: not its own, its own (the level file or one of its worlds), or it cannot say,
	// since one of its `worlds` entries is a res:// tag the path rules reject and that entry is not this file (#966).
	enum class level_claim { none, this_file, unreadable_entry };

	struct level_answer
	{
		level_claim claim = level_claim::none;
		// unreadable_entry only: the level file, the entry as the file spells it and the rule it breaks.
		std::string level;
		std::string entry;
		std::string rule;
	};

	class desc_files
	{
	public:
		explicit desc_files(edt::desc_file_deps deps) : m_deps(std::move(deps)) {}

		std::vector<fs::path> layers() const
		{
			std::vector<fs::path> out = m_deps.layers ? m_deps.layers() : std::vector<fs::path>{};
			if (out.empty() && m_deps.top_layer) out.push_back(m_deps.top_layer());
			return out;
		}

		// The file res://<relative> names in the highest layer that has one; a link or a folder anywhere is an error.
		located_file locate(const std::string& relative) const
		{
			located_file out;
			const std::vector<fs::path> roots = layers();
			for (std::size_t i = roots.size(); i-- > 0;) {
				switch (edt::inspect_level_file(roots[i], relative)) {
				case edt::level_file_state::unsafe:
					out.error = std::format("'{}' is not a plain file inside the project", relative);
					return out;
				case edt::level_file_state::file:
					out.file = roots[i] / fs_paths::path_from_utf8(relative);
					out.layer = i;
					out.found = true;
					return out;
				case edt::level_file_state::missing: break;
				}
			}
			return out;
		}

		bool is_top_layer(const located_file& file) const
		{
			return file.found && file.layer + 1 == layers().size();
		}

		// Whether `target` is the file of the open level or one of the world files that level refers to: the same file on
		// disk (links, case, short names resolved), not the same text. An entry the path rules reject is compared as the file
		// the resolver opens for it; when that is not `target` the answer is unreadable_entry (#966).
		level_answer belongs_to_open_level(const located_file& target) const
		{
			if (!target.found) return {};
			const auto same_path = [&target](const fs::path& other) {
				std::error_code ec;
				return fs::equivalent(other, target.file, ec) && !ec;
			};
			const auto same_file = [&same_path](const located_file& other) { return other.found && same_path(other.file); };
			return claim_of_open_level(
				[&](const std::string& open) { return same_file(locate(open)); },
				[&](const std::string& named) { return same_file(locate(named)); },
				[&](const std::vector<fs::path>& roots, std::string_view entry) {
					// The highest layer that has the file is the one the resolver opens.
					for (std::size_t i = roots.size(); i-- > 0;) {
						const std::optional<fs::path> candidate = resolver_path(roots[i], entry);
						std::error_code ec;
						if (candidate && fs::exists(*candidate, ec)) return same_path(*candidate);
					}
					return false;
				});
		}

		// The files an entry of a level's `worlds` names, as a path or as {guid, path}: where the guid is now and what `path`
		// spells, since the index may have moved the guid while the stale path still names a world (#920). None when inline,
		// and none for an empty tag (a guid no file holds, without a path): the resolver loads nothing by it. Nor for a tag under
		// a protocol that is neither res:// nor engine:// (memory://, RES://): it names no folder a project file can be in (#966).
		std::vector<res::tag> world_files_of(const json::value& world) const
		{
			std::vector<res::tag> files;
			const std::optional<res::reference> reference = res::reference_from_json(world);
			if (!reference) return files;
			const auto has_folder = [](const res::tag& file) {
				return file.protocol().empty() || file.protocol() == res::tag::default_protocol() || file.protocol() == res::tag::engine;
			};
			const res::tag resolved = m_deps.res.resolve_reference(*reference);
			if (resolved.is_valid() && has_folder(resolved)) files.push_back(resolved);
			if (reference->path.is_valid() && !(reference->path == resolved) && has_folder(reference->path)) files.push_back(reference->path);
			return files;
		}

		// The same question for a path no file has: it is the open level's own name or one of the names in its `worlds`, so a
		// file written there would be the level's. Names are compared the way the file system does (without case on Windows
		// and macOS). A level file that cannot be read or understood makes every name its own, as above.
		level_answer names_open_level(const std::string& relative) const
		{
			const std::string wanted = name_key(relative);
			const std::vector<fs::path> own = layers();
			const std::optional<fs::path> wanted_file = own.empty() ? std::nullopt : resolver_path(own.back(), relative);
			return claim_of_open_level(
				[&](const std::string& open) { return name_key(open) == wanted; },
				[&](const std::string& named) { return name_key(named) == wanted; },
				[&](const std::vector<fs::path>& roots, std::string_view entry) {
					// A new file lands in the top folder: the entry names it when both spell one path there.
					if (roots.empty() || !wanted_file) return false;
					const std::optional<fs::path> candidate = resolver_path(roots.back(), entry);
					return candidate && name_key(fs_paths::generic_utf8_of(*candidate)) == name_key(fs_paths::generic_utf8_of(*wanted_file));
				});
		}

		// The file a vfs mount over `root` opens for `relative`, as vfs_resolver::safe_path builds it (the folder made
		// canonical, so `//` and `/./` are gone); none for what it refuses (`..`, a control character, a drive, an absolute path).
		static std::optional<fs::path> resolver_path(const fs::path& root, std::string_view relative)
		{
			if (root.empty() || relative.empty() || res::has_control_chars(relative)) return std::nullopt;
			const fs::path rel = fs_paths::path_from_utf8(relative);
			if (rel.has_root_name() || rel.has_root_directory()) return std::nullopt;
			for (const fs::path& part : rel) {
				const std::string name = fs_paths::generic_utf8_of(part);
				if (name == "..") return std::nullopt;
				if (name.size() >= 2 && std::isalpha(static_cast<unsigned char>(name[0])) && name[1] == ':') return std::nullopt;
			}
			const fs::path joined = root / rel;
			if (!joined.has_filename()) return std::nullopt;
			std::error_code ec;
			fs::path candidate = fs::weakly_canonical(joined.parent_path(), ec);
			if (ec) return std::nullopt;
			candidate /= joined.filename();
			return candidate;
		}

		static std::string name_key(std::string name)
		{
#if defined(_WIN32) || defined(__APPLE__)
			std::ranges::transform(name, name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
			return name;
		}

		static res::tag tag_of(const std::string& relative)
		{
			return res::tag("res://" + relative);
		}

		struct read_result
		{
			std::vector<std::byte> bytes;
			std::string error;
			bool ok() const { return error.empty(); }
		};

		// The bytes on disk now, through the resolver, never the cache; at most k_desc_file_limit.
		read_result read_fresh(const std::string& relative, const located_file& file) const
		{
			std::error_code ec;
			const auto size = fs::file_size(file.file, ec);
			if (ec) return { {}, std::format("'{}' could not be read", relative) };
			if (size > edt::k_desc_file_limit) {
				return { {}, std::format("'{}' is {} bytes: at most {} go through desc.* in one call", relative, size, edt::k_desc_file_limit) };
			}
			const auto raw = m_deps.res.fetch_data(tag_of(relative));
			if (!raw) return { {}, "the project resources are not mounted" };
			if (!raw->wait_for(k_read_timeout)) return { {}, std::format("'{}' could not be read in {} s", relative, k_read_timeout.count()) };
			if (!raw->is_ready()) return { {}, std::format("'{}' could not be read", relative) };
			if (raw->data.size() > edt::k_desc_file_limit) {
				return { {}, std::format("'{}' is {} bytes: at most {} go through desc.* in one call", relative, raw->data.size(), edt::k_desc_file_limit) };
			}
			return { raw->data, {} };
		}

		void changed(const cmd::call& call, std::string_view command, std::string target, std::size_t bytes) const
		{
			egLOG_INFO("agent", "{} {} ({} bytes) by {}", command, target, bytes, who(call.from));
			if (m_deps.events && call.from.kind == cmd::origin_kind::agent) {
				m_deps.events->agent_action.emit(edt::agent_action_info{ std::string(command), std::move(target), bytes, call.from.session });
			}
		}

		const edt::desc_file_deps& deps() const { return m_deps; }

		std::optional<std::string> write_blocked() const
		{
			return m_deps.write_blocker ? m_deps.write_blocker() : std::nullopt;
		}

	private:
		// One pass over the open level's files and their `worlds`. `is_level(name)`: the level file itself; `is_named(path)`:
		// an entry the path rules accept; `is_resolved(roots, relative)`: an entry compared as the file a mount over `roots` opens.
		template<class IsLevel, class IsNamed, class IsResolved>
		level_answer claim_of_open_level(const IsLevel& is_level, const IsNamed& is_named, const IsResolved& is_resolved) const
		{
			level_answer out;
			if (!m_deps.open_level_files) return out;
			const level_answer mine{ level_claim::this_file, {}, {}, {} };
			for (const std::string& open : m_deps.open_level_files()) {
				if (is_level(open)) return mine;
				const located_file level = locate(open);
				if (!level.found) continue;
				// A level file that cannot be read or understood cannot be told from its worlds: everything is its own.
				const read_result text = read_fresh(open, level);
				if (!text.ok()) return mine;
				boost::system::error_code parse_error;
				const json::value parsed = json::parse(text_of(text.bytes), parse_error);
				if (parse_error || !parsed.is_object()) return mine;
				const json::value* worlds = parsed.as_object().if_contains("worlds");
				if (!worlds || !worlds->is_array()) continue;
				for (const json::value& world : worlds->as_array()) {
					for (const res::tag& world_file : world_files_of(world)) {
						// The project's root may be the engine's base folder: engine:// then names project files.
						if (world_file.protocol() == res::tag::engine) {
							if (is_resolved(m_deps.res.mount_roots(res::tag::engine), world_file.relative())) return mine;
							continue;
						}
						const edt::agent_level_path named = edt::check_agent_folder(world_file.view());
						if (named.ok()) {
							if (is_named(named.path)) return mine;
							continue;
						}
						// The resolver still opens a res:// tag the path rules reject (`//`, `/./`, an overlong path) (#966).
						if (world_file.protocol() == res::tag::default_protocol() && is_resolved(layers(), world_file.relative())) return mine;
						if (out.claim == level_claim::none) out = level_answer{ level_claim::unreadable_entry, open, world_file.string(), named.error };
					}
				}
			}
			return out;
		}

		edt::desc_file_deps m_deps;
	};

	// Why an agent writes no desc file while the open level has an entry the guard cannot read; `answer.entry` comes from a file.
	std::string unreadable_entry_text(const level_answer& answer)
	{
		return std::format("the open level '{}' has a worlds entry '{}' that the path rules reject ({}): "
			"desc files are not written until it is fixed or the level is closed", answer.level, edt::forge::escaped_text(answer.entry, 200), answer.rule);
	}

	struct request_path
	{
		std::string relative;
		std::string error;
		bool ok() const { return error.empty(); }
	};

	request_path path_of(const cmd::call& call)
	{
		const std::string_view text = view_of(call.args.as_object().at("path").as_string());
		if (res::is_asset_meta_name(text)) {
			return { {}, "a .resource file holds the ID of its file and is the editor's own: desc.* neither reads nor writes it" };
		}
		const edt::agent_level_path named = edt::check_agent_level_path(text);
		if (!named.ok()) {
			std::string why = named.error;
			constexpr std::string_view level_file = "a level file (*.desc)";
			if (const std::size_t at = why.find(level_file); at != std::string::npos) why.replace(at, level_file.size(), "a desc file (*.desc)");
			return { {}, std::move(why) };
		}
		return { named.path, {} };
	}

	// What an agent sends as the file: an object, whose header the editor writes (__version is stamped, a wrong one is refused).
	struct prepared_file
	{
		std::vector<std::byte> bytes;
		std::vector<res::diagnostic> problems;
		json::array warnings;
		std::string error;
	};

	// Where `content` carries a __guid, at any depth: the ID of a file is the editor's to write (EPIC-57).
	void find_guid_keys(const json::value& value, const std::string& pointer, const std::string& resource, std::vector<res::diagnostic>& out)
	{
		if (value.is_array()) {
			std::size_t index = 0;
			for (const json::value& element : value.as_array()) find_guid_keys(element, std::format("{}/{}", pointer, index++), resource, out);
			return;
		}
		if (!value.is_object()) return;
		for (const auto& member : value.as_object()) {
			if (out.size() >= k_max_reported) return;
			const std::string_view key(member.key().data(), member.key().size());
			if (key == "__guid") {
				out.push_back({ res::diagnostic_severity::error, std::string(res::diagnostic_kind::schema), resource, pointer + "/__guid",
					"`__guid` is written by the editor: leave it out (desc.write keeps the one the file has)" });
			} else {
				find_guid_keys(member.value(), pointer + "/" + res::json_pointer_token(key), resource, out);
			}
		}
	}

	// `kept_guid`: the __guid of the file being replaced, written back so a desc.write does not drop the file's ID. `replaced`:
	// that file's bytes, whose reference guids are kept the same way (desc_system::write_references).
	prepared_file prepare(const desc::desc_system& descs, const res::resource_system& resources, const std::string& relative,
		const json::value& content, const std::string& kept_guid = {}, std::span<const std::byte> replaced = {})
	{
		prepared_file out;
		const std::string resource = "res://" + relative;
		find_guid_keys(content, "", resource, out.problems);
		for (res::diagnostic& item : edt::validate_desc_tree(descs, content, resource, &resources)) {
			if (item.severity == res::diagnostic_severity::error) out.problems.push_back(std::move(item));
			else out.warnings.push_back(diagnostic_json(item));
		}
		if (!out.problems.empty()) return out;
		json::value stamped = content;
		if (!kept_guid.empty() && stamped.is_object()) stamped.as_object()["__guid"] = kept_guid;
		descs.stamp_tree(stamped);
		descs.write_references(stamped, replaced);
		const std::vector<std::byte> bytes = descs.serialize_to_bytes(stamped);
		if (bytes.size() > edt::k_desc_file_limit) {
			out.error = std::format("the file would be {} bytes: at most {} go through desc.* in one call", bytes.size(), edt::k_desc_file_limit);
			return out;
		}
		out.bytes = bytes;
		return out;
	}

	std::string type_of_text(std::string_view text)
	{
		boost::system::error_code ec;
		const json::value parsed = json::parse(text, ec);
		if (ec || !parsed.is_object()) return {};
		const json::value* type = parsed.as_object().if_contains("__type");
		return type && type->is_string() ? std::string(view_of(type->as_string())) : std::string{};
	}

	class desc_commands
	{
	public:
		explicit desc_commands(edt::desc_file_deps deps) : m_files(std::move(deps)) {}

		cmd::result read(const cmd::call& call) const
		{
			const request_path path = path_of(call);
			if (!path.ok()) return cmd::result::failure(cmd::codes::invalid_args, path.error);
			const located_file found = m_files.locate(path.relative);
			if (!found.error.empty()) return cmd::result::failure(cmd::codes::invalid_args, found.error);
			if (!found.found) return cmd::result::failure(cmd::codes::not_found, std::format("no file 'res://{}'", path.relative));
			const auto file = m_files.read_fresh(path.relative, found);
			if (!file.ok()) return cmd::result::failure(cmd::codes::failed, file.error);
			const std::string text = text_of(file.bytes);
			if (!edt::is_valid_utf8(text)) return cmd::result::failure(cmd::codes::failed, std::format("'{}' is not a text file (not UTF-8)", path.relative));
			return cmd::result::success(json::object{ { "text", text }, { "type", type_of_text(text) },
				{ "version", edt::code_version_of(file.bytes) }, { "bytes", file.bytes.size() } });
		}

		cmd::result create(const cmd::call& call) const
		{
			const request_path path = path_of(call);
			if (!path.ok()) return cmd::result::failure(cmd::codes::invalid_args, path.error);
			if (std::optional<std::string> why = m_files.write_blocked()) return cmd::result::failure(cmd::codes::busy, std::move(*why));
			const located_file found = m_files.locate(path.relative);
			if (!found.error.empty()) return cmd::result::failure(cmd::codes::invalid_args, found.error);
			if (found.found) {
				return cmd::result::failure(cmd::codes::needs_input,
					std::format("'res://{}' exists: desc.create never replaces a file; read it and use desc.write (a person confirms that)", path.relative));
			}
			const level_answer level = m_files.names_open_level(path.relative);
			if (level.claim == level_claim::this_file) {
				return cmd::result::failure(cmd::codes::busy,
					std::format("'res://{}' is a name of the open level (its file or one of its worlds): change the level with the scene commands and save it with file.save", path.relative));
			}
			if (level.claim == level_claim::unreadable_entry) return cmd::result::failure(cmd::codes::busy, unreadable_entry_text(level));
			const prepared_file ready = prepare(m_files.deps().descs, m_files.deps().res, path.relative, call.args.as_object().at("desc"));
			if (!ready.problems.empty()) return invalid_content(ready.problems);
			if (!ready.error.empty()) return cmd::result::failure(cmd::codes::invalid_args, ready.error);
			// Exclusive: refused when the file appears while it is written (create_sync).
			const res::file_operation made = m_files.deps().res.create_sync(desc_files::tag_of(path.relative), ready.bytes);
			if (!made.ok) return cmd::result::failure(cmd::codes::failed, std::format("'res://{}' was not created: {}", path.relative, made.error));
			m_files.changed(call, "desc.create", path.relative, ready.bytes.size());
			return cmd::result::success(json::object{ { "version", edt::code_version_of(ready.bytes) }, { "bytes", ready.bytes.size() }, { "warnings", ready.warnings } });
		}

		cmd::result write(const cmd::call& call) const
		{
			const request_path path = path_of(call);
			if (!path.ok()) return cmd::result::failure(cmd::codes::invalid_args, path.error);
			if (std::optional<std::string> why = m_files.write_blocked()) return cmd::result::failure(cmd::codes::busy, std::move(*why));
			const located_file found = m_files.locate(path.relative);
			if (!found.error.empty()) return cmd::result::failure(cmd::codes::invalid_args, found.error);
			if (!found.found) {
				return cmd::result::failure(cmd::codes::not_found, std::format("no file 'res://{}': desc.create makes a new one", path.relative));
			}
			const level_answer level = m_files.belongs_to_open_level(found);
			if (level.claim == level_claim::this_file) {
				return cmd::result::failure(cmd::codes::busy,
					std::format("'res://{}' belongs to the open level (its file or one of its worlds): change the level with the scene commands and save it with file.save", path.relative));
			}
			if (level.claim == level_claim::unreadable_entry) return cmd::result::failure(cmd::codes::busy, unreadable_entry_text(level));
			if (!m_files.is_top_layer(found)) {
				return cmd::result::failure(cmd::codes::invalid_args,
					std::format("'res://{}' belongs to a lower layer (the engine or a package), not to the project's own res/", path.relative));
			}
			// Checked here, after the person's answer, and written in the same step on the main thread: a file that changed
			// between the question and the yes is `stale`.
			const auto current = m_files.read_fresh(path.relative, found);
			if (!current.ok()) return cmd::result::failure(cmd::codes::failed, current.error);
			const std::string version = edt::code_version_of(current.bytes);
			if (view_of(call.args.as_object().at("version").as_string()) != version) return stale(path.relative, version);
			const prepared_file ready = prepare(m_files.deps().descs, m_files.deps().res, path.relative, call.args.as_object().at("desc"), edt::desc_root_guid(current.bytes), current.bytes);
			if (!ready.problems.empty()) return invalid_content(ready.problems);
			if (!ready.error.empty()) return cmd::result::failure(cmd::codes::invalid_args, ready.error);
			const res::tag tag = desc_files::tag_of(path.relative);
			if (!m_files.deps().res.store_sync(tag, ready.bytes)) {
				return cmd::result::failure(cmd::codes::failed, std::format("'res://{}' could not be written", path.relative));
			}
			m_files.deps().res.signal_changed(tag);
			m_files.changed(call, "desc.write", path.relative, ready.bytes.size());
			return cmd::result::success(json::object{ { "version", edt::code_version_of(ready.bytes) }, { "bytes", ready.bytes.size() }, { "warnings", ready.warnings } });
		}

		std::optional<std::string> describe_write(const cmd::call& call) const
		{
			// A reason may quote the file or the agent's desc: it stays one line of the question (#943).
			const auto refused = [](const std::string& why) {
				return std::optional<std::string>("This request will be refused: " + edt::forge::escaped_text(why, 1024));
			};
			const request_path path = path_of(call);
			if (!path.ok()) return refused(path.error);
			if (std::optional<std::string> why = m_files.write_blocked()) return refused(*why);
			const located_file found = m_files.locate(path.relative);
			if (!found.error.empty()) return refused(found.error);
			if (!found.found) return refused(std::format("no file 'res://{}'", path.relative));
			const level_answer level = m_files.belongs_to_open_level(found);
			if (level.claim == level_claim::this_file) return refused(std::format("'res://{}' belongs to the open level", path.relative));
			if (level.claim == level_claim::unreadable_entry) return refused(unreadable_entry_text(level));
			if (!m_files.is_top_layer(found)) return refused(std::format("'res://{}' belongs to a lower layer, not to the project's own res/", path.relative));
			const auto current = m_files.read_fresh(path.relative, found);
			if (!current.ok()) return refused(current.error);
			if (view_of(call.args.as_object().at("version").as_string()) != edt::code_version_of(current.bytes)) {
				return refused("the file changed since that version was read");
			}
			const prepared_file ready = prepare(m_files.deps().descs, m_files.deps().res, path.relative, call.args.as_object().at("desc"), edt::desc_root_guid(current.bytes), current.bytes);
			if (!ready.problems.empty()) return refused(one_line(ready.problems));
			if (!ready.error.empty()) return refused(ready.error);
			const std::string type = type_of_text(text_of(current.bytes));
			return std::format("Replace the desc file\n  res://{}\n  type {}, {} bytes now, {} bytes after\nThe old content is not kept. "
				"If the open level uses this file, the change is applied to it at once (hot reload) and cannot be undone with undo.",
				edt::forge::escaped_text(path.relative, 1024), type.empty() ? "?" : edt::forge::escaped_text(type, 128), current.bytes.size(),
				ready.bytes.size());
		}

	private:
		desc_files m_files;
	};
}

namespace {
	// The keys with "__" a desc file may carry; any other would hide an object from the check.
	bool is_allowed_header_key(std::string_view key)
	{
		return key == "__type" || key == "__version" || key == "__guid" || key == "__id" || key == "__parent";
	}

	bool is_other_protocol(std::string_view protocol) { return edt::is_other_tag_protocol(protocol); }

	// The custom fields that hold names, not tags (#604: action names and key names). Only the strict tag rule is off for
	// them; the walk over every string still refuses one that names another protocol. A list, not a flag of the schema:
	// desc::field_schema is built by module code too, and a new member there would change the module API's layout.
	bool holds_plain_text(std::string_view type, std::string_view field_key)
	{
		return type == "input_actions_desc" && (field_key == "actions" || field_key == "/actions");
	}

	// A tag a file refers to: literally "res://" and then a path inside the project. Nothing else is rewritten or guessed.
	std::string strict_tag_problem(std::string_view text)
	{
		if (!text.starts_with("res://")) return "a tag must be written as res://<path> (a path inside the project)";
		return edt::check_agent_folder(text).error;
	}

	// Empty when `text` as a plain string value cannot reach another protocol: res::tag gives it none, it does not begin like
	// a mounted one ("user/x", edt_tag_text.h) and holds no "://"; or it is a good res:// tag.
	std::string loose_tag_problem(std::string_view text)
	{
		const std::string normalized = edt::tag_text_normalized(text);
		const std::string_view protocol = edt::tag_protocol(normalized);
		const std::string_view prefix = edt::protocol_like_prefix(normalized);
		if (protocol == "res" || (protocol.empty() && prefix == "res")) return strict_tag_problem(normalized);
		if (!protocol.empty() || is_other_protocol(prefix) || normalized.find("://") != std::string::npos) {
			const std::string_view named = protocol.empty() ? prefix : protocol;
			return std::format("'{}' is not a place a project file may refer to: only res://<path> tags are written", std::string(named).substr(0, 40));
		}
		return {};
	}
}

std::vector<res::diagnostic> edt::validate_desc_tree(const desc::desc_system& descs, const json::value& root, std::string_view resource,
	const res::resource_system* resources, reference_rule rule)
{
	const bool agent = rule == reference_rule::agent;
	std::vector<res::diagnostic> out;
	std::size_t errors = 0;
	std::size_t warnings = 0;
	std::unordered_set<std::string> unchecked;
	// Where a reference was checked as a schema's field: the walk over every value does not report it again.
	std::unordered_set<std::string> referenced;
	// Errors and warnings are counted apart: a pile of warnings cannot push an error out of the list.
	const auto add = [&](std::string kind, std::string pointer, std::string reason, res::diagnostic_severity severity = res::diagnostic_severity::error) {
		std::size_t& count = severity == res::diagnostic_severity::error ? errors : warnings;
		if (count >= k_max_reported) return;
		++count;
		out.push_back({ severity, std::move(kind), std::string(resource), std::move(pointer), std::move(reason) });
	};
	const auto schema_error = [&](std::string pointer, std::string reason) {
		add(std::string(res::diagnostic_kind::schema), std::move(pointer), std::move(reason));
	};
	// A {guid, path} reference (EPIC-57 section 4.4): its form, its path a tag a file may hold, then what the asset index
	// says of its guid (reference_rule).
	const auto check_reference = [&](const json::value& value, const std::string& at) {
		if (!referenced.insert(at).second) return;
		const json::object& object = value.as_object();
		if (std::string problem = agent ? edt::agent_reference_form_problem(object) : edt::reference_object_problem(object); !problem.empty()) {
			schema_error(at, std::move(problem));
			return;
		}
		const std::string path = edt::reference_path_text(object);
		if (!path.empty()) {
			if (std::string problem = strict_tag_problem(path); !problem.empty()) {
				schema_error(at + "/path", std::move(problem));
				return;
			}
		}
		if (agent) {
			if (std::string problem = edt::agent_reference_guid_problem(object, resources); !problem.empty()) {
				schema_error(at, std::move(problem));
				return;
			}
		}
		const std::optional<edt::reference_target> target = edt::target_of(value, resources);
		if (!target || !target->unknown_id) return;
		const std::string guid = res::to_text(target->reference.guid);
		if (path.empty()) {
			schema_error(at + "/guid", std::format("no file has the ID {} and the reference has no path", guid));
		} else {
			add(std::string(res::diagnostic_kind::reference), at + "/guid", std::format("no file has the ID {}: '{}' is loaded by its path", guid, path),
				res::diagnostic_severity::warning);
		}
	};

	const auto walk = [&](const auto& self, const json::value& value, const std::string& pointer) -> void {
		if (value.is_string()) {
			if (std::string problem = loose_tag_problem(view_of(value.as_string())); !problem.empty()) schema_error(pointer, std::move(problem));
			return;
		}
		if (value.is_array()) {
			std::size_t index = 0;
			for (const json::value& element : value.as_array()) self(self, element, std::format("{}/{}", pointer, index++));
			return;
		}
		if (!value.is_object()) return;
		const json::object& object = value.as_object();
		// No schema reads a prefab's component, an override record or a world's content: there the shape tells a reference.
		if (agent && !pointer.empty() && edt::is_guid_reference(object)) {
			check_reference(value, pointer);
			return;
		}
		for (const auto& member : object) {
			const std::string_view key(member.key().data(), member.key().size());
			if (key.starts_with("__") && !is_allowed_header_key(key)) {
				schema_error(pointer + "/" + res::json_pointer_token(key), "a key with \"__\" other than __type, __version, __guid, __id and __parent is not allowed");
			}
		}
		if (const json::value* parent = object.if_contains("__parent")) {
			// The object form is checked by its path as written, like the string.
			std::optional<std::string> written;
			if (parent->is_string()) {
				written = std::string(view_of(parent->as_string()));
			} else if (!parent->is_object()) {
				schema_error(pointer + "/__parent", "`__parent` must be a string tag or a {\"guid\", \"path\"} reference");
			} else if (std::string problem = agent ? edt::agent_reference_form_problem(parent->as_object()) : edt::reference_object_problem(parent->as_object());
				!problem.empty()) {
				schema_error(pointer + "/__parent", "`__parent`: " + problem);
			} else {
				written = edt::reference_path_text(parent->as_object());
			}
			if (written) {
				if (std::string problem = strict_tag_problem(*written); !problem.empty()) {
					schema_error(pointer + "/__parent", "`__parent`: " + problem);
				} else if (const edt::agent_level_path named = edt::check_agent_level_path(*written); !named.ok()) {
					schema_error(pointer + "/__parent", "`__parent` must name a desc file of the project: " + named.error);
				} else if (agent && parent->is_object()) {
					if (std::string elsewhere = edt::agent_reference_guid_problem(parent->as_object(), resources); !elsewhere.empty()) {
						schema_error(pointer + "/__parent", "`__parent`: " + elsewhere);
					}
				}
			}
		}
		if (const json::value* type_value = object.if_contains("__type")) {
			if (!type_value->is_string()) {
				add(std::string(res::diagnostic_kind::unknown_type), pointer + "/__type", "`__type` must be a string");
			} else {
				const std::string type(view_of(type_value->as_string()));
				const desc::type_schema* schema = descs.find_schema(type);
				if (!schema) {
					if (!descs.is_registered(type)) {
						add(std::string(res::diagnostic_kind::unknown_type), pointer + "/__type", std::format("no desc type '{}' is registered", type));
					} else {
						// Hand-written descs (level, world, prefab...) have no schema yet: only their shape is checked.
						if (unchecked.insert(type).second) {
							add(std::string(res::diagnostic_kind::schema), pointer + "/__type", std::format("'{}' has no schema: its fields are not checked", type), res::diagnostic_severity::warning);
						}
						if (type == "level_desc") {
							const json::value* worlds = object.if_contains("worlds");
							if (worlds && !worlds->is_array()) schema_error(pointer + "/worlds", "`worlds` must be an array");
							// The walk refuses an odd res:// entry; one without a protocol is a project path to the open-level guard (#966).
							if (worlds && worlds->is_array()) {
								std::size_t index = 0;
								for (const json::value& world : worlds->as_array()) {
									const std::string at = std::format("{}/worlds/{}", pointer, index++);
									const bool by_reference = world.is_object() && !world.as_object().contains("__type");
									const json::value* spelled = by_reference ? world.as_object().if_contains("path") : &world;
									if (!spelled || !spelled->is_string()) continue;
									const std::string_view text = view_of(spelled->as_string());
									if (text.empty() || !loose_tag_problem(text).empty()) continue;
									const std::string problem = edt::check_agent_folder(edt::tag_text_normalized(text)).error;
									if (!problem.empty()) schema_error(by_reference ? at + "/path" : at, "a `worlds` entry must be a path inside the project: " + problem);
								}
							}
							const json::value* level_name = object.if_contains("name");
							if (level_name && !level_name->is_string()) schema_error(pointer + "/name", "`name` must be a string");
						}
					}
				} else {
					bool fits = true;
					if (const json::value* version = object.if_contains("__version")) {
						std::int64_t number = 0;
						if (!desc::json_to_int64(*version, number) || number != descs.current_version(type)) {
							schema_error(pointer + "/__version", std::format("`__version` is written by the editor (now {}): leave it out", descs.current_version(type)));
							fits = false;
						}
					}
					if (fits) {
						json::object patch;
						for (const auto& member : object) {
							if (!member.key().starts_with("__")) patch.emplace(member.key(), member.value());
						}
						const desc::override_failure bad = desc::check_override_object(*schema, patch, nullptr, &descs.schemas());
						if (bad.status != desc::override_status::valid) {
							const std::string expected = desc::override_expectation(*schema, bad, &descs.schemas());
							const std::string owner = desc::override_owner(*schema, bad, &descs.schemas());
							schema_error(pointer + bad.pointer,
								bad.status == desc::override_status::stale ? std::format("'{}' has no such field", owner)
									: std::format("the value does not fit the field of '{}'{}{}", owner, expected.empty() ? "" : ": expected ", expected));
						}
						// The fields the schema says hold a tag: a path inside the project, whatever it looks like.
						for (const desc::field_schema& field : schema->fields) {
							// A custom field (a list of tags by slot, say) is read as tags too: the strict side.
							const bool tag_field = field.kind == desc::field_kind::res_tag || field.kind == desc::field_kind::desc_ref
								|| field.kind == desc::field_kind::custom
								|| ((field.kind == desc::field_kind::array || field.kind == desc::field_kind::map)
									&& (field.element == desc::field_kind::res_tag || field.element == desc::field_kind::desc_ref));
							if (!tag_field || (field.kind == desc::field_kind::custom && holds_plain_text(type, field.key))) continue;
							const std::string_view key = std::string_view(field.key).starts_with('/') ? std::string_view(field.key).substr(1) : std::string_view(field.key);
							const json::value* held = patch.if_contains(key);
							if (!held) continue;
							// Inline descs (desc_ref objects) are walked as descs; the values of any other object are all tags.
							const bool inline_descs_allowed = field.kind == desc::field_kind::desc_ref || field.element == desc::field_kind::desc_ref;
							// A custom field's codec reads its own objects (the samplers of a material): only a res_tag or a
							// desc_ref holds a {guid, path} reference.
							const bool references = field.kind != desc::field_kind::custom;
							const auto check_tag = [&](const auto& self_tag, const json::value& tag, const std::string& at) -> void {
								if (tag.is_string()) {
									if (tag.as_string().empty()) return;
									if (const std::string problem = strict_tag_problem(view_of(tag.as_string())); !problem.empty()) schema_error(at, problem);
								} else if (tag.is_array()) {
									std::size_t index = 0;
									for (const json::value& element : tag.as_array()) self_tag(self_tag, element, std::format("{}/{}", at, index++));
								} else if (references && edt::is_reference_object(tag)) {
									check_reference(tag, at);
								} else if (tag.is_object() && !inline_descs_allowed) {
									for (const auto& member : tag.as_object()) {
										self_tag(self_tag, member.value(), at + "/" + res::json_pointer_token(std::string_view(member.key().data(), member.key().size())));
									}
								}
							};
							const std::string at = pointer + "/" + res::json_pointer_token(key);
							if (field.kind == desc::field_kind::map) {
								// The values of a map are the tags (or inline descs), one by one, never the map itself as one inline desc.
								if (const json::object* entries = held->if_object()) {
									for (const auto& entry : *entries) {
										check_tag(check_tag, entry.value(), at + "/" + res::json_pointer_token(std::string_view(entry.key().data(), entry.key().size())));
									}
								}
								continue;
							}
							check_tag(check_tag, *held, at);
						}
					}
				}
			}
		} else if (pointer.empty()) {
			add(std::string(res::diagnostic_kind::unknown_type), "/__type", "the file must be an object with a `__type`");
		}
		for (const auto& member : object) {
			if (member.key().starts_with("__")) continue;
			self(self, member.value(), pointer + "/" + res::json_pointer_token(std::string_view(member.key().data(), member.key().size())));
		}
	};

	if (!root.is_object()) {
		add(std::string(res::diagnostic_kind::unknown_type), "", "the file must be a JSON object with a `__type`");
		return out;
	}
	walk(walk, root, std::string{});
	return out;
}

std::vector<edt::registration> edt::register_desc_file_commands(cmd::command_registry& commands, desc_file_deps deps)
{
	auto impl = std::make_shared<desc_commands>(std::move(deps));
	std::vector<registration> out;
	const auto add = [&](std::string id, std::string summary, std::string_view args, std::string_view result, bool mutates,
		bool destructive, cmd::agent_category category, cmd::danger risk, cmd::result (desc_commands::*body)(const cmd::call&) const) {
		cmd::command_desc desc;
		desc.id = std::move(id);
		desc.summary = std::move(summary);
		desc.args_schema = parse_schema(args);
		desc.result_schema = parse_schema(result);
		desc.mutates = mutates;
		desc.destructive = destructive;
		desc.category = category;
		desc.risk = risk;
		desc.invoke = [impl, body](const cmd::call& call) { return ((*impl).*body)(call); };
		if (risk != cmd::danger::none) desc.describe = [impl](const cmd::call& call) { return impl->describe_write(call); };
		registration added = commands.add(std::move(desc));
		if (added.active()) out.push_back(std::move(added));
	};

	constexpr std::string_view version_result = R"json({"type":"object","properties":{
		"version":{"type":"string"},"bytes":{"type":"integer"},
		"warnings":{"type":"array","description":"what could not be checked, e.g. a type without a schema: its fields are written as given","items":{"type":"object"}}},
		"required":["version","bytes","warnings"]})json";
	add("desc.read", "Read a desc file (*.desc) of the project's res/ folder; `version` is what desc.write takes back",
		R"json({"type":"object","properties":{
			"path":{"type":"string","minLength":1,"description":"res://materials/a.mat.desc or materials/a.mat.desc: inside the project, *.desc only"}},
			"required":["path"],"additionalProperties":false})json",
		R"json({"type":"object","properties":{
			"text":{"type":"string"},"type":{"type":"string","description":"the file's __type, empty when it has none"},
			"version":{"type":"string"},"bytes":{"type":"integer"}},"required":["text","type","version","bytes"]})json",
		false, false, cmd::agent_category::read, cmd::danger::none, &desc_commands::read);
	add("desc.create", "Create a desc file (*.desc) in the project's res/ folder from a JSON object with a `__type`; never replaces a file. "
			"Every object with a `__type` must fit its schema (unknown fields are refused), else nothing is written and the errors name the JSON pointer. "
			"A reference is a res:// string or a {\"guid\", \"path\"} object (desc.read may show one); a string is enough",
		R"json({"type":"object","properties":{
			"path":{"type":"string","minLength":1,"description":"res://levels/a.desc or levels/a.desc: inside the project, *.desc only"},
			"desc":{"type":"object","description":"the file's content; the editor writes __version and the header order"}},
			"required":["path","desc"],"additionalProperties":false})json",
		version_result, true, false, cmd::agent_category::scene_edit, cmd::danger::none, &desc_commands::create);
	add("desc.write", "Replace an existing desc file of the project's res/ folder (a person confirms each time). `version` must be the one desc.read gave, "
			"else `stale`; the content is checked against the schemas like desc.create. The file of the open level is refused (`busy`)",
		R"json({"type":"object","properties":{
			"path":{"type":"string","minLength":1},"version":{"type":"string","minLength":1},
			"desc":{"type":"object","description":"the whole new content"}},
			"required":["path","version","desc"],"additionalProperties":false})json",
		version_result, true, true, cmd::agent_category::scene_edit, cmd::danger::confirm, &desc_commands::write);
	return out;
}

edt::desc_file_read edt::read_desc_file(res::resource_system& resources, const desc::desc_system& descriptions, const desc_file_layout& layout, const res::tag& tag)
{
	desc_file_read out;
	if (tag.protocol() != res::tag::default_protocol()) {
		out.error = std::format("'{}': only res:// files of the project are data documents", tag.view());
		return out;
	}
	const agent_level_path named = check_agent_level_path(tag.view());
	if (!named.ok()) {
		out.error = std::format("'{}': {}", tag.view(), named.error);
		return out;
	}
	desc_file_deps deps{ resources, descriptions };
	deps.top_layer = layout.top_layer;
	deps.layers = layout.layers;
	deps.open_level_files = layout.open_level_files;
	const desc_files files{ std::move(deps) };
	const located_file found = files.locate(named.path);
	if (!found.error.empty()) {
		out.error = found.error;
		return out;
	}
	out.located = true;
	if (!found.found) {
		out.error = std::format("no file '{}'", tag.view());
		out.missing = true;
		// A new file lands in the top layer; it is the open level's only when it has the level's own name.
		out.top_layer = true;
		out.in_open_level = files.names_open_level(named.path).claim == level_claim::this_file;
		return out;
	}
	out.top_layer = files.is_top_layer(found);
	// A person's document: an entry the path rules reject was compared by file, so unreadable_entry is not this file (#966).
	out.in_open_level = files.belongs_to_open_level(found).claim == level_claim::this_file;
	desc_files::read_result read = files.read_fresh(named.path, found);
	if (!read.ok()) {
		out.located = false;
		out.error = std::move(read.error);
		return out;
	}
	out.bytes = std::move(read.bytes);
	out.version = edt::code_version_of(out.bytes);
	return out;
}

std::string edt::desc_root_guid(std::span<const std::byte> bytes)
{
	boost::system::error_code ec;
	const json::value parsed = json::parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), ec);
	if (ec || !parsed.is_object()) return {};
	const json::value* guid = parsed.as_object().if_contains("__guid");
	return guid && guid->is_string() ? std::string(guid->as_string().data(), guid->as_string().size()) : std::string{};
}

int edt::desc_root_version(std::span<const std::byte> bytes)
{
	boost::system::error_code ec;
	const json::value parsed = json::parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), ec);
	if (ec || !parsed.is_object()) return 0;
	const json::value* version = parsed.as_object().if_contains("__version");
	if (version == nullptr) return 1;
	if (version->is_int64()) {
		const std::int64_t value = version->as_int64();
		return value >= 1 && value <= std::numeric_limits<int>::max() ? static_cast<int>(value) : 0;
	}
	if (version->is_uint64()) {
		const std::uint64_t value = version->as_uint64();
		return value >= 1 && value <= static_cast<std::uint64_t>(std::numeric_limits<int>::max()) ? static_cast<int>(value) : 0;
	}
	return 0;
}
