#include "edt_code_commands.h"
#include "edt_code_tree.h"
#include <snk/editor/edt_editor_events.h>
#include <snk/common/engine_assert.h>
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>
#include <snk/project/prj_code.h>
#include <snk/resource/res_path_sanitize.h>
#include <snk/resource/res_system.h>
#include <boost/json.hpp>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>

namespace {
	namespace json = boost::json;
	namespace fs = std::filesystem;

	constexpr std::size_t k_max_path_bytes = 1024;
	constexpr std::size_t k_max_listed = 5000;
	// `old` of a replace: the search stays linear (Boyer-Moore) and small next to the file.
	constexpr std::size_t k_max_replace_old = 64 * 1024;
	constexpr const char* k_no_project = "no_project";

	json::value parse_schema(std::string_view text)
	{
		boost::system::error_code ec;
		json::value parsed = json::parse(text, ec);
		ASSERT_MSG(!ec, "a code.* schema is not JSON: {}", ec.message());
		return parsed;
	}

	std::string_view view_of(const json::string& text)
	{
		return { text.data(), text.size() };
	}

	std::string_view string_arg(const cmd::call& call, std::string_view key)
	{
		return view_of(call.args.as_object().at(key).as_string());
	}

	std::string text_of(const std::vector<std::byte>& bytes)
	{
		return { reinterpret_cast<const char*>(bytes.data()), bytes.size() };
	}

	std::vector<std::byte> bytes_of(std::string_view text)
	{
		const auto* first = reinterpret_cast<const std::byte*>(text.data());
		return { first, first + text.size() };
	}

	std::string check_file(std::string_view relative)
	{
		if (relative.size() > k_max_path_bytes) return std::format("the path is longer than {} bytes", k_max_path_bytes);
		if (relative.empty()) return "the path is empty";
		return edt::check_code_file_path(relative);
	}

	// A folder of code/ to list: the same name rules as a file, and every segment a real folder, never a link.
	std::string check_folder(const fs::path& root, std::string_view relative)
	{
		if (relative.empty()) return {};
		if (relative.size() > k_max_path_bytes) return std::format("the path is longer than {} bytes", k_max_path_bytes);
		if (relative.front() == '/' || relative.find('\\') != std::string_view::npos) {
			return "the path must be relative to code/ and '/' separated";
		}
		if (std::string problem = res::portable_path_problem(relative); !problem.empty()) return problem;
		fs::path at = root;
		std::size_t begin = 0;
		while (begin <= relative.size()) {
			const std::size_t end = relative.find('/', begin);
			const std::string_view segment = relative.substr(begin, end == std::string_view::npos ? end : end - begin);
			if (prj::is_code_ignored_dir(segment)) {
				return std::format("'{}' is not a place for code (build output or a hidden folder)", segment);
			}
			at /= fs_paths::path_from_utf8(segment);
			std::error_code ec;
			if (!fs::is_directory(fs::symlink_status(at, ec)) || ec) return std::format("no folder '{}' in code/", relative);
			if (end == std::string_view::npos) break;
			begin = end + 1;
		}
		return {};
	}

	// What a path names on disk, links and short (8.3) or Unicode-folded names resolved: still inside code/, and no
	// folder on the way that is never code. Checked again after the name rules, which see only what the agent wrote.
	// A path that does not exist (yet) is the operation's to report.
	std::string on_disk_problem(const fs::path& root, std::string_view relative, bool ends_in_file)
	{
		std::error_code ec;
		const fs::path base = fs::canonical(root, ec);
		if (ec) return "code/ cannot be resolved";
		const fs::path real = fs::canonical(relative.empty() ? root : root / fs_paths::path_from_utf8(relative), ec);
		if (ec) return {};
		const fs::path inside = real.lexically_relative(base);
		if (inside.empty() || (!inside.empty() && *inside.begin() == "..")) return "the path leads out of code/";
		if (inside == ".") return {};
		std::size_t parts = static_cast<std::size_t>(std::distance(inside.begin(), inside.end()));
		for (const fs::path& part : inside) {
			if (ends_in_file && --parts == 0) break;
			const std::string name = fs_paths::utf8_of(part);
			if (prj::is_code_ignored_dir(name)) {
				// The name on disk, which may not be UTF-8: it goes into a JSON message only when it is.
				return edt::is_valid_utf8(name) ? std::format("'{}' is not a place for code (build output or a hidden folder)", name)
					: std::string{ "the path leads into a folder that is not a place for code" };
			}
		}
		return {};
	}

	struct file_read
	{
		std::vector<std::byte> bytes;
		std::string error;
		bool ok() const { return error.empty(); }
	};

	// The bytes on disk now, through the resolver (its safe path), never the resource cache. A file over the limit is
	// refused before it is read into memory.
	file_read read_fresh(res::resource_system& res, const fs::path& root, const res::tag& tag)
	{
		if (!res.exists(tag)) return { {}, std::format("no file '{}' in code/", tag.relative()) };
		if (std::string problem = on_disk_problem(root, tag.relative(), true); !problem.empty()) return { {}, std::move(problem) };
		const fs::path file = root / fs_paths::path_from_utf8(tag.relative());
		std::error_code ec;
		// A plain file only, with a message of its own: the resolver refuses a link, a folder or a FIFO too (#484).
		if (!fs::is_regular_file(fs::symlink_status(file, ec)) || ec) return { {}, std::format("'{}' is not a regular file", tag.relative()) };
		const auto size = fs::file_size(file, ec);
		if (ec) return { {}, std::format("'{}' could not be read", tag.relative()) };
		if (size > edt::k_code_file_limit) {
			return { {}, std::format("'{}' is {} bytes: at most {} go through code.* in one call", tag.relative(), size, edt::k_code_file_limit) };
		}
		const auto raw = res.fetch_data(tag);
		if (!raw) return { {}, "code:// is not mounted" };
		// A file swapped after the checks above is still refused by the resolver, which reads without following a link and
		// without blocking on a FIFO (#484).
		raw->wait();
		if (!raw->is_ready()) return { {}, std::format("'{}' could not be read", tag.relative()) };
		return { raw->data, {} };
	}

	cmd::result stale(std::string_view path, const std::string& version)
	{
		cmd::result out = cmd::result::failure(cmd::codes::stale,
			std::format("'{}' changed since that version was read: read it again", path));
		out.value = json::object{ { "version", version } };
		return out;
	}

	cmd::result not_text(std::string_view path)
	{
		return cmd::result::failure(cmd::codes::failed, std::format("'{}' is not a text file (not UTF-8)", path));
	}

	cmd::result too_big(std::string_view path, std::size_t size)
	{
		return cmd::result::failure(cmd::codes::failed,
			std::format("'{}' would be {} bytes: at most {} go through code.* in one call", path, size, edt::k_code_file_limit));
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

	const char* category_name(prj::code_category category)
	{
		switch (category) {
		case prj::code_category::source: return "source";
		case prj::code_category::header: return "header";
		case prj::code_category::cmake: return "cmake";
		case prj::code_category::other: return "other";
		}
		return "other";
	}

	class code_commands
	{
	public:
		explicit code_commands(edt::code_commands_deps deps) : m_deps(deps) {}

		// The code/ folder, or nothing without a project that has one.
		std::optional<fs::path> root() const
		{
			const auto roots = m_deps.res.mount_roots(prj::k_code_protocol);
			if (roots.empty()) return std::nullopt;
			return roots.back();
		}

		void changed(const cmd::call& call, std::string_view command, std::string target, std::size_t bytes) const
		{
			egLOG_INFO("agent", "{} {} ({} bytes) by {}", command, target, bytes, who(call.from));
			if (m_deps.events && call.from.kind == cmd::origin_kind::agent) {
				m_deps.events->agent_action.emit(edt::agent_action_info{ std::string(command), std::move(target), bytes, call.from.session });
			}
		}

		cmd::result list(const cmd::call& call) const
		{
			const auto code_root = root();
			if (!code_root) return cmd::result::failure(k_no_project, "the project has no code/ folder");
			const json::object& args = call.args.as_object();
			const json::value* dir_arg = args.if_contains("dir");
			const std::string dir = dir_arg ? std::string(view_of(dir_arg->as_string())) : std::string{};
			const bool recursive = args.contains("recursive") && args.at("recursive").as_bool();
			if (std::string problem = check_folder(*code_root, dir); !problem.empty()) {
				return cmd::result::failure(cmd::codes::failed, std::move(problem));
			}

			const edt::code_tree_model tree{ *code_root };
			json::array entries;
			bool truncated = false;
			std::vector<std::string> folders{ dir };
			while (!folders.empty() && !truncated) {
				const std::string folder = std::move(folders.back());
				folders.pop_back();
				// Before and after: a folder swapped for a link meanwhile is not listed (the window between is a
				// second program's race; listing through a handle that never follows links would close it).
				if (std::string problem = on_disk_problem(*code_root, folder, false); !problem.empty()) {
					return cmd::result::failure(cmd::codes::failed, std::move(problem));
				}
				const std::vector<edt::code_entry> listed = tree.list(folder);
				if (std::string problem = on_disk_problem(*code_root, folder, false); !problem.empty()) {
					return cmd::result::failure(cmd::codes::failed, std::move(problem));
				}
				for (const edt::code_entry& entry : listed) {
					// A name that is not UTF-8 cannot go into JSON, nor be read or written through code.*.
					if (!edt::is_valid_utf8(entry.name)) continue;
					if (entries.size() >= k_max_listed) {
						truncated = true;
						break;
					}
					json::object item{ { "path", entry.relative }, { "kind", entry.is_directory ? "dir" : "file" } };
					if (!entry.is_directory) item["category"] = category_name(entry.category);
					entries.push_back(std::move(item));
					if (entry.is_directory && recursive) folders.push_back(entry.relative);
				}
			}
			return cmd::result::success(json::object{ { "entries", std::move(entries) }, { "truncated", truncated } });
		}

		cmd::result read(const cmd::call& call) const
		{
			const auto code_root = root();
			if (!code_root) return cmd::result::failure(k_no_project, "the project has no code/ folder");
			const std::string_view path = string_arg(call, "path");
			if (std::string problem = check_file(path); !problem.empty()) return cmd::result::failure(cmd::codes::failed, std::move(problem));
			const file_read file = read_fresh(m_deps.res, *code_root, edt::code_actions::tag_of(path));
			if (!file.ok()) return cmd::result::failure(cmd::codes::failed, file.error);
			if (file.bytes.size() > edt::k_code_file_limit) return too_big(path, file.bytes.size());
			const std::string text = text_of(file.bytes);
			if (!edt::is_valid_utf8(text)) return not_text(path);

			// A line ends after its '\n' (CRLF stays in the line); a last line without one still counts.
			std::vector<std::size_t> starts;
			if (!text.empty()) starts.push_back(0);
			for (std::size_t i = 0; i + 1 < text.size(); ++i) {
				if (text[i] == '\n') starts.push_back(i + 1);
			}
			const std::size_t total = starts.size();
			const json::object& args = call.args.as_object();
			const std::size_t from = args.contains("from_line") ? static_cast<std::size_t>(args.at("from_line").to_number<std::uint64_t>()) : 1;
			const std::size_t wanted = args.contains("line_count") ? static_cast<std::size_t>(args.at("line_count").to_number<std::uint64_t>()) : total;
			std::string part;
			std::size_t count = 0;
			if (from <= total) {
				count = std::min(wanted, total - (from - 1));
				const std::size_t begin = starts[from - 1];
				const std::size_t last = from - 1 + count;
				const std::size_t end = last < total ? starts[last] : text.size();
				part = text.substr(begin, end - begin);
			}
			return cmd::result::success(json::object{ { "text", part }, { "version", edt::code_version_of(file.bytes) },
				{ "total_lines", total }, { "from_line", from }, { "line_count", count } });
		}

		cmd::result write(const cmd::call& call) const
		{
			const auto code_root = root();
			if (!code_root) return cmd::result::failure(k_no_project, "the project has no code/ folder");
			const json::object& args = call.args.as_object();
			const std::string_view path = string_arg(call, "path");
			const json::value* text_arg = args.if_contains("text");
			const json::value* replace_arg = args.if_contains("replace");
			if ((text_arg != nullptr) == (replace_arg != nullptr)) {
				return cmd::result::failure(cmd::codes::invalid_args, "give either `text` (the whole file) or `replace` {old, new}");
			}
			if (std::string problem = check_file(path); !problem.empty()) return cmd::result::failure(cmd::codes::failed, std::move(problem));
			const res::tag tag = edt::code_actions::tag_of(path);
			const file_read file = read_fresh(m_deps.res, *code_root, tag);
			if (!file.ok()) return cmd::result::failure(cmd::codes::failed, file.error);
			// Compared and written in one main-thread call: an agent cannot slip in between. Another program writing the
			// file right then still can (the store has no compare-and-swap): accepted (#451).
			const std::string current_version = edt::code_version_of(file.bytes);
			if (string_arg(call, "version") != current_version) return stale(path, current_version);
			const std::string current = text_of(file.bytes);
			if (!edt::is_valid_utf8(current)) return not_text(path);

			std::string next;
			if (text_arg) {
				next = std::string(view_of(text_arg->as_string()));
			} else {
				const json::object& replace = replace_arg->as_object();
				const std::string_view old_text = view_of(replace.at("old").as_string());
				const std::string_view new_text = view_of(replace.at("new").as_string());
				if (old_text.size() > k_max_replace_old) {
					return cmd::result::failure(cmd::codes::failed,
						std::format("`old` is {} bytes: at most {}; write the whole `text` instead", old_text.size(), k_max_replace_old));
				}
				// The first match and whether there is a second, no more: linear in the file whatever `old` is.
				const std::boyer_moore_searcher searcher(old_text.begin(), old_text.end());
				const auto first = std::search(current.begin(), current.end(), searcher);
				if (first == current.end()) {
					return cmd::result::failure(cmd::codes::failed, std::format("`old` is not found in '{}'", path));
				}
				if (std::search(first + 1, current.end(), searcher) != current.end()) {
					return cmd::result::failure(cmd::codes::failed,
						std::format("`old` is found more than once in '{}': give more of the text around it", path));
				}
				const auto found = static_cast<std::size_t>(first - current.begin());
				next = current.substr(0, found);
				next += new_text;
				next += current.substr(found + old_text.size());
			}
			if (next.size() > edt::k_code_file_limit) return too_big(path, next.size());
			if (!edt::is_valid_utf8(next)) return not_text(path);
			const std::vector<std::byte> bytes = bytes_of(next);
			if (!m_deps.res.store_sync(tag, bytes)) {
				return cmd::result::failure(cmd::codes::failed, std::format("'{}' could not be written", path));
			}
			changed(call, "code.write", std::string(path), bytes.size());
			return cmd::result::success(json::object{ { "version", edt::code_version_of(bytes) }, { "bytes", bytes.size() } });
		}

		cmd::result create(const cmd::call& call) const
		{
			if (!root()) return cmd::result::failure(k_no_project, "the project has no code/ folder");
			const json::object& args = call.args.as_object();
			const std::string_view path = string_arg(call, "path");
			if (std::string problem = check_file(path); !problem.empty()) return cmd::result::failure(cmd::codes::failed, std::move(problem));
			const json::value* text_arg = args.if_contains("text");
			const std::string_view text = text_arg ? view_of(text_arg->as_string()) : std::string_view{};
			if (text.size() > edt::k_code_file_limit) return too_big(path, text.size());
			if (!edt::is_valid_utf8(text)) return not_text(path);
			const std::vector<std::byte> bytes = bytes_of(text);
			// Never over an existing file, also one that appears meanwhile (create_sync).
			const res::file_operation made = m_deps.res.create_sync(edt::code_actions::tag_of(path), bytes);
			if (!made.ok) return cmd::result::failure(cmd::codes::failed, std::format("'{}' was not created: {}", path, made.error));
			changed(call, "code.create", std::string(path), bytes.size());
			return cmd::result::success(json::object{ { "version", edt::code_version_of(bytes) }, { "bytes", bytes.size() } });
		}

		cmd::result remove(const cmd::call& call) const
		{
			const auto code_root = root();
			if (!code_root) return cmd::result::failure(k_no_project, "the project has no code/ folder");
			const std::string_view path = string_arg(call, "path");
			if (std::string problem = check_file(path); !problem.empty()) return cmd::result::failure(cmd::codes::failed, std::move(problem));
			const res::tag tag = edt::code_actions::tag_of(path);
			const file_read file = read_fresh(m_deps.res, *code_root, tag);
			if (!file.ok()) return cmd::result::failure(cmd::codes::failed, file.error);
			if (const std::string current = edt::code_version_of(file.bytes); string_arg(call, "version") != current) {
				return stale(path, current);
			}
			const res::file_operation gone = m_deps.res.remove(tag);
			if (!gone.ok) return cmd::result::failure(cmd::codes::failed, std::format("'{}' was not deleted: {}", path, gone.error));
			changed(call, "code.delete", std::string(path), file.bytes.size());
			return cmd::result::success(json::object{});
		}

		cmd::result rename(const cmd::call& call) const
		{
			if (!root()) return cmd::result::failure(k_no_project, "the project has no code/ folder");
			const std::string_view from = string_arg(call, "from");
			const std::string_view to = string_arg(call, "to");
			for (const std::string_view path : { from, to }) {
				if (std::string problem = check_file(path); !problem.empty()) {
					return cmd::result::failure(cmd::codes::failed, std::format("'{}': {}", path, problem));
				}
			}
			// The target's folder must be there already: a rename makes none (code.create does).
			const std::size_t slash = to.rfind('/');
			if (slash != std::string_view::npos) {
				const std::string_view folder = to.substr(0, slash);
				std::error_code ec;
				const auto status = fs::symlink_status(*root() / fs_paths::path_from_utf8(folder), ec);
				if (ec || !fs::is_directory(status)) {
					return cmd::result::failure(cmd::codes::failed,
						std::format("there is no folder '{}' in code/ to move into: code.create makes the folders of a new file", folder));
				}
			}
			// Never over an existing file.
			const res::file_operation moved = m_deps.res.rename(edt::code_actions::tag_of(from), edt::code_actions::tag_of(to));
			if (!moved.ok) return cmd::result::failure(cmd::codes::failed, std::format("'{}' was not renamed: {}", from, moved.error));
			changed(call, "code.rename", std::format("{} -> {}", from, to), 0);
			return cmd::result::success(json::object{});
		}

	private:
		edt::code_commands_deps m_deps;
	};

	constexpr std::string_view k_version_result = R"json({"type":"object","properties":{
		"version":{"type":"string","description":"what code.write and code.delete take back"},
		"bytes":{"type":"integer"}},"required":["version","bytes"]})json";
	constexpr std::string_view k_empty_result = R"json({"type":"object","properties":{}})json";
}

std::string edt::code_version_of(std::span<const std::byte> bytes)
{
	std::uint64_t hash = 14695981039346656037ull;
	for (const std::byte b : bytes) {
		hash ^= static_cast<std::uint64_t>(b);
		hash *= 1099511628211ull;
	}
	return std::format("{:016x}-{}", hash, bytes.size());
}

cmd::agent_category edt::code_call_category(std::string_view relative, cmd::agent_category otherwise)
{
	if (!check_file(relative).empty()) return otherwise;
	return prj::code_category_of(relative) == prj::code_category::cmake ? cmd::agent_category::cmake_presets : otherwise;
}

bool edt::is_valid_utf8(std::string_view text)
{
	std::size_t i = 0;
	while (i < text.size()) {
		const auto lead = static_cast<unsigned char>(text[i]);
		std::size_t length = 0;
		std::uint32_t code = 0;
		if (lead < 0x80) {
			++i;
			continue;
		}
		if (lead >= 0xC2 && lead <= 0xDF) {
			length = 2;
			code = lead & 0x1Fu;
		} else if (lead >= 0xE0 && lead <= 0xEF) {
			length = 3;
			code = lead & 0x0Fu;
		} else if (lead >= 0xF0 && lead <= 0xF4) {
			length = 4;
			code = lead & 0x07u;
		} else {
			return false;
		}
		if (text.size() - i < length) return false;
		for (std::size_t k = 1; k < length; ++k) {
			const auto next = static_cast<unsigned char>(text[i + k]);
			if ((next & 0xC0) != 0x80) return false;
			code = (code << 6) | (next & 0x3Fu);
		}
		if ((length == 3 && code < 0x800) || (length == 4 && (code < 0x10000 || code > 0x10FFFF))
			|| (code >= 0xD800 && code <= 0xDFFF)) {
			return false;
		}
		i += length;
	}
	return true;
}

std::vector<edt::registration> edt::register_code_commands(cmd::command_registry& commands, code_commands_deps deps)
{
	auto impl = std::make_shared<code_commands>(deps);
	std::vector<registration> out;
	const auto add = [&](std::string id, std::string summary, std::string_view args, std::string_view result, bool mutates,
		bool destructive, cmd::agent_category category, std::function<cmd::agent_category(const cmd::call&)> category_of,
		cmd::result (code_commands::*body)(const cmd::call&) const) {
		cmd::command_desc desc;
		desc.id = std::move(id);
		desc.summary = std::move(summary);
		desc.args_schema = parse_schema(args);
		desc.result_schema = parse_schema(result);
		desc.mutates = mutates;
		desc.destructive = destructive;
		desc.category = category;
		desc.category_of = std::move(category_of);
		desc.invoke = [impl, body](const cmd::call& call) { return ((*impl).*body)(call); };
		registration added = commands.add(std::move(desc));
		if (added.active()) out.push_back(std::move(added));
	};
	const auto by_path = [](cmd::agent_category otherwise) {
		return [otherwise](const cmd::call& call) { return code_call_category(string_arg(call, "path"), otherwise); };
	};

	add("code.list", "List the code files and folders of the project's code/ folder (build output and hidden folders left out)",
		R"json({"type":"object","properties":{
			"dir":{"type":"string","description":"a folder under code/, '/' separated; default: code/ itself"},
			"recursive":{"type":"boolean"}},"additionalProperties":false})json",
		R"json({"type":"object","properties":{
			"entries":{"type":"array","items":{"type":"object","properties":{
				"path":{"type":"string"},"kind":{"type":"string","enum":["file","dir"]},
				"category":{"type":"string","enum":["source","header","cmake","other"]}},"required":["path","kind"]}},
			"truncated":{"type":"boolean","description":"more than 5000 entries: list a folder at a time"}},"required":["entries","truncated"]})json",
		false, false, cmd::agent_category::read, {}, &code_commands::list);
	add("code.read", "Read a code file (UTF-8) whole or a range of its lines; `version` is what code.write and code.delete take back",
		R"json({"type":"object","properties":{
			"path":{"type":"string","minLength":1,"description":"under code/, '/' separated: src/game.cpp"},
			"from_line":{"type":"integer","minimum":1},"line_count":{"type":"integer","minimum":1}},
			"required":["path"],"additionalProperties":false})json",
		R"json({"type":"object","properties":{
			"text":{"type":"string","description":"the lines asked for, line endings as they are"},
			"version":{"type":"string"},"total_lines":{"type":"integer"},"from_line":{"type":"integer"},"line_count":{"type":"integer"}},
			"required":["text","version","total_lines","from_line","line_count"]})json",
		false, false, cmd::agent_category::read, {}, &code_commands::read);
	add("code.write", "Change an existing code file: the whole `text`, or `replace` {old, new} where `old` is found exactly once (byte for byte); `version` must be the one code.read gave, else `stale` and nothing is written",
		R"json({"type":"object","properties":{
			"path":{"type":"string","minLength":1},
			"version":{"type":"string","minLength":1},
			"text":{"type":"string"},
			"replace":{"type":"object","properties":{"old":{"type":"string","minLength":1},"new":{"type":"string"}},
				"required":["old","new"],"additionalProperties":false}},
			"required":["path","version"],"additionalProperties":false})json",
		k_version_result, true, false, cmd::agent_category::code_edit, by_path(cmd::agent_category::code_edit), &code_commands::write);
	add("code.create", "Create a code file (never over an existing one); folders it needs are made",
		R"json({"type":"object","properties":{"path":{"type":"string","minLength":1},"text":{"type":"string"}},
			"required":["path"],"additionalProperties":false})json",
		k_version_result, true, false, cmd::agent_category::code_files, by_path(cmd::agent_category::code_files), &code_commands::create);
	add("code.delete", "Delete a code file; `version` must be the one code.read gave, else `stale` and the file stays",
		R"json({"type":"object","properties":{"path":{"type":"string","minLength":1},"version":{"type":"string","minLength":1}},
			"required":["path","version"],"additionalProperties":false})json",
		k_empty_result, true, true, cmd::agent_category::code_files, by_path(cmd::agent_category::code_files), &code_commands::remove);
	add("code.rename", "Rename or move a code file into an existing folder of code/ (never over an existing file)",
		R"json({"type":"object","properties":{"from":{"type":"string","minLength":1},"to":{"type":"string","minLength":1}},
			"required":["from","to"],"additionalProperties":false})json",
		k_empty_result, true, true, cmd::agent_category::code_files,
		[](const cmd::call& call) {
			// The stricter side: a move into or out of a CMake name is a CMake edit.
			const cmd::agent_category from = code_call_category(string_arg(call, "from"), cmd::agent_category::code_files);
			const cmd::agent_category to = code_call_category(string_arg(call, "to"), cmd::agent_category::code_files);
			return from == cmd::agent_category::cmake_presets ? from : to;
		},
		&code_commands::rename);
	return out;
}
