#include "edt_desc_file_commands.h"
#include "edt_code_commands.h"
#include "edt_file_menu.h"
#include <snk/common/engine_assert.h>
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>
#include <snk/desc/desc_schema_ops.h>
#include <snk/desc/desc_system.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/resource/res_system.h>
#include <boost/json.hpp>
#include <algorithm>
#include <cctype>
#include <iterator>
#include <chrono>
#include <format>
#include <memory>
#include <optional>
#include <system_error>
#include <unordered_set>
#include <utility>

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
		// disk (links, case, short names resolved), not the same text.
		bool belongs_to_open_level(const located_file& target) const
		{
			if (!m_deps.open_level_files || !target.found) return false;
			const auto same_file = [&target](const located_file& other) {
				std::error_code ec;
				return other.found && fs::equivalent(other.file, target.file, ec) && !ec;
			};
			for (const std::string& open : m_deps.open_level_files()) {
				const located_file level = locate(open);
				if (!level.found) continue;
				if (same_file(level)) return true;
				// A level file that cannot be read or understood cannot be told from its worlds: everything is busy.
				const read_result text = read_fresh(open, level);
				if (!text.ok()) return true;
				boost::system::error_code parse_error;
				const json::value parsed = json::parse(text_of(text.bytes), parse_error);
				if (parse_error || !parsed.is_object()) return true;
				const json::value* worlds = parsed.as_object().if_contains("worlds");
				if (!worlds || !worlds->is_array()) continue;
				for (const json::value& world : worlds->as_array()) {
					if (!world.is_string()) continue;
					const edt::agent_level_path named = edt::check_agent_folder(view_of(world.as_string()));
					if (named.ok() && same_file(locate(named.path))) return true;
				}
			}
			return false;
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

	private:
		edt::desc_file_deps m_deps;
	};

	struct request_path
	{
		std::string relative;
		std::string error;
		bool ok() const { return error.empty(); }
	};

	request_path path_of(const cmd::call& call)
	{
		const edt::agent_level_path named = edt::check_agent_level_path(view_of(call.args.as_object().at("path").as_string()));
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

	prepared_file prepare(const desc::desc_system& descs, const std::string& relative, const json::value& content)
	{
		prepared_file out;
		for (res::diagnostic& item : edt::validate_desc_tree(descs, content, "res://" + relative)) {
			if (item.severity == res::diagnostic_severity::error) out.problems.push_back(std::move(item));
			else out.warnings.push_back(diagnostic_json(item));
		}
		if (!out.problems.empty()) return out;
		json::value stamped = content;
		descs.stamp_tree(stamped);
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
			const located_file found = m_files.locate(path.relative);
			if (!found.error.empty()) return cmd::result::failure(cmd::codes::invalid_args, found.error);
			if (found.found) {
				return cmd::result::failure(cmd::codes::needs_input,
					std::format("'res://{}' exists: desc.create never replaces a file; read it and use desc.write (a person confirms that)", path.relative));
			}
			const prepared_file ready = prepare(m_files.deps().descs, path.relative, call.args.as_object().at("desc"));
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
			const located_file found = m_files.locate(path.relative);
			if (!found.error.empty()) return cmd::result::failure(cmd::codes::invalid_args, found.error);
			if (!found.found) {
				return cmd::result::failure(cmd::codes::not_found, std::format("no file 'res://{}': desc.create makes a new one", path.relative));
			}
			if (m_files.belongs_to_open_level(found)) {
				return cmd::result::failure(cmd::codes::busy,
					std::format("'res://{}' belongs to the open level (its file or one of its worlds): change the level with the scene commands and save it with file.save", path.relative));
			}
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
			const prepared_file ready = prepare(m_files.deps().descs, path.relative, call.args.as_object().at("desc"));
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
			const auto refused = [](const std::string& why) { return std::optional<std::string>("This request will be refused: " + why); };
			const request_path path = path_of(call);
			if (!path.ok()) return refused(path.error);
			const located_file found = m_files.locate(path.relative);
			if (!found.error.empty()) return refused(found.error);
			if (!found.found) return refused(std::format("no file 'res://{}'", path.relative));
			if (m_files.belongs_to_open_level(found)) return refused(std::format("'res://{}' belongs to the open level", path.relative));
			if (!m_files.is_top_layer(found)) return refused(std::format("'res://{}' belongs to a lower layer, not to the project's own res/", path.relative));
			const auto current = m_files.read_fresh(path.relative, found);
			if (!current.ok()) return refused(current.error);
			if (view_of(call.args.as_object().at("version").as_string()) != edt::code_version_of(current.bytes)) {
				return refused("the file changed since that version was read");
			}
			const prepared_file ready = prepare(m_files.deps().descs, path.relative, call.args.as_object().at("desc"));
			if (!ready.problems.empty()) return refused(one_line(ready.problems));
			if (!ready.error.empty()) return refused(ready.error);
			const std::string type = type_of_text(text_of(current.bytes));
			return std::format("Replace the desc file\n  res://{}\n  type {}, {} bytes now, {} bytes after\nThe old content is not kept. "
				"If the open level uses this file, the change is applied to it at once (hot reload) and cannot be undone with undo.",
				path.relative, type.empty() ? "?" : type, current.bytes.size(), ready.bytes.size());
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

	// res::tag takes the text up to the first ':' or '/' as the protocol, so "user/x.png" and "user:/x.png" are
	// user://x.png: the protocol is read the way the tag reads it, not by looking for "://".
	std::string_view leading_protocol(const std::string& text)
	{
		const std::size_t end = text.find_first_of(":/\\");
		return end == std::string::npos ? std::string_view{} : std::string_view(text).substr(0, end);
	}

	// A place a tag can name that is not the project's res/: every protocol the editor mounts.
	bool is_other_protocol(std::string_view protocol)
	{
		constexpr std::string_view mounted[] = { "memory", "engine", "cache", "code", "user", "edt" };
		return std::find(std::begin(mounted), std::end(mounted), protocol) != std::end(mounted);
	}

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

	// Empty when `text` as a plain string value cannot reach another protocol: it names none, or it is a good res:// tag.
	std::string loose_tag_problem(std::string_view text)
	{
		const std::string normalized = [&] {
			std::string copy(text);
			std::replace(copy.begin(), copy.end(), '\\', '/');
			return copy;
		}();
		const std::string_view protocol = leading_protocol(normalized);
		if (protocol.empty()) return {};
		if (protocol == "res") return strict_tag_problem(normalized);
		const bool scheme = normalized.size() > protocol.size() + 2 && normalized.compare(protocol.size(), 3, "://") == 0;
		if (is_other_protocol(protocol) || scheme) {
			return std::format("'{}' is not a place a project file may refer to: only res://<path> tags are written", std::string(protocol).substr(0, 40));
		}
		return {};
	}
}

std::vector<res::diagnostic> edt::validate_desc_tree(const desc::desc_system& descs, const json::value& root, std::string_view resource)
{
	std::vector<res::diagnostic> out;
	std::size_t errors = 0;
	std::size_t warnings = 0;
	std::unordered_set<std::string> unchecked;
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
		for (const auto& member : object) {
			const std::string_view key(member.key().data(), member.key().size());
			if (key.starts_with("__") && !is_allowed_header_key(key)) {
				schema_error(pointer + "/" + res::json_pointer_token(key), "a key with \"__\" other than __type, __version, __guid, __id and __parent is not allowed");
			}
		}
		if (const json::value* parent = object.if_contains("__parent")) {
			if (!parent->is_string()) {
				schema_error(pointer + "/__parent", "`__parent` must be a string tag");
			} else if (std::string problem = strict_tag_problem(view_of(parent->as_string())); !problem.empty()) {
				schema_error(pointer + "/__parent", "`__parent`: " + problem);
			} else if (const edt::agent_level_path named = edt::check_agent_level_path(view_of(parent->as_string())); !named.ok()) {
				schema_error(pointer + "/__parent", "`__parent` must name a desc file of the project: " + named.error);
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
							schema_error(pointer + bad.pointer,
								bad.status == desc::override_status::stale ? std::format("'{}' has no such field", type)
									: std::format("the value does not fit the field of '{}'", type));
						}
						// The fields the schema says hold a tag: a path inside the project, whatever it looks like.
						for (const desc::field_schema& field : schema->fields) {
							// A custom field (a list of tags by slot, say) is read as tags too: the strict side.
							const bool tag_field = field.kind == desc::field_kind::res_tag || field.kind == desc::field_kind::desc_ref
								|| field.kind == desc::field_kind::custom
								|| (field.kind == desc::field_kind::array && (field.element == desc::field_kind::res_tag || field.element == desc::field_kind::desc_ref));
							if (!tag_field || (field.kind == desc::field_kind::custom && holds_plain_text(type, field.key))) continue;
							const std::string_view key = std::string_view(field.key).starts_with('/') ? std::string_view(field.key).substr(1) : std::string_view(field.key);
							const json::value* held = patch.if_contains(key);
							if (!held) continue;
							// Inline descs (desc_ref objects) are walked as descs; the values of any other object are all tags.
							const bool inline_descs_allowed = field.kind == desc::field_kind::desc_ref || field.element == desc::field_kind::desc_ref;
							const auto check_tag = [&](const auto& self_tag, const json::value& tag, const std::string& at) -> void {
								if (tag.is_string()) {
									if (tag.as_string().empty()) return;
									if (const std::string problem = strict_tag_problem(view_of(tag.as_string())); !problem.empty()) schema_error(at, problem);
								} else if (tag.is_array()) {
									std::size_t index = 0;
									for (const json::value& element : tag.as_array()) self_tag(self_tag, element, std::format("{}/{}", at, index++));
								} else if (tag.is_object() && !inline_descs_allowed) {
									for (const auto& member : tag.as_object()) {
										self_tag(self_tag, member.value(), at + "/" + res::json_pointer_token(std::string_view(member.key().data(), member.key().size())));
									}
								}
							};
							check_tag(check_tag, *held, pointer + "/" + res::json_pointer_token(key));
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
			"Every object with a `__type` must fit its schema (unknown fields are refused), else nothing is written and the errors name the JSON pointer",
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
