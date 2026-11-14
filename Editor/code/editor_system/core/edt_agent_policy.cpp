#include "edt_agent_policy.h"
#include "edt_agent_confirmations.h"
#include <snk/common/engine_assert.h>
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>
#include <snk/project/prj_json_text.h>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/basic_parser.hpp>
#include <boost/json/basic_parser_impl.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/parse_options.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/string.hpp>
#include <boost/json/value.hpp>
#include <algorithm>
#include <cstdint>
#include <format>
#include <fstream>
#include <initializer_list>
#include <set>
#include <thread>
#include <iterator>
#include <system_error>
#include <utility>
#include <vector>

namespace json = boost::json;

namespace edt
{
	namespace {

		constexpr cmd::agent_category settable[] = {
			cmd::agent_category::read, cmd::agent_category::scene_edit, cmd::agent_category::code_edit, cmd::agent_category::code_files,
			cmd::agent_category::cmake_presets, cmd::agent_category::build, cmd::agent_category::live, cmd::agent_category::editor_control,
			cmd::agent_category::play,
			cmd::agent_category::vcs_read, cmd::agent_category::vcs_commit, cmd::agent_category::vcs_branch,
			cmd::agent_category::vcs_sync, cmd::agent_category::vcs_push, cmd::agent_category::vcs_forge,
			cmd::agent_category::project_migrate,
		};

		cmd::policy_mode mode_of_risk(cmd::danger risk)
		{
			switch (risk) {
			case cmd::danger::none: return cmd::policy_mode::allow;
			case cmd::danger::confirm: return cmd::policy_mode::ask;
			case cmd::danger::deny_by_default: return cmd::policy_mode::deny;
			}
			return cmd::policy_mode::deny;
		}

		// What the registry's ids look like. Anything else is no command, and an id with control characters could only be a
		// way to name something the policy machinery uses itself.
		bool valid_command_id(std::string_view id)
		{
			if (id.empty()) return false;
			for (const char ch : id) {
				const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' || ch == '-';
				if (!ok) return false;
			}
			return true;
		}

		std::string text_of(const json::string& text)
		{
			return std::string(text.data(), text.size());
		}

		// Boost.JSON keeps the last of two equal keys without a word: `{"code_edit":"deny","code_edit":"allow"}` would read as
		// allow. This handler only watches the keys of every object and stops at the first one seen twice.
		class duplicate_key_finder
		{
		public:
			[[maybe_unused]] static constexpr std::size_t max_object_size = static_cast<std::size_t>(-1);
			[[maybe_unused]] static constexpr std::size_t max_array_size = static_cast<std::size_t>(-1);
			[[maybe_unused]] static constexpr std::size_t max_key_size = static_cast<std::size_t>(-1);
			[[maybe_unused]] static constexpr std::size_t max_string_size = static_cast<std::size_t>(-1);

			std::string duplicate;

			bool on_document_begin(json::error_code&) { return true; }
			bool on_document_end(json::error_code&) { return true; }
			bool on_array_begin(json::error_code&) { return true; }
			bool on_array_end(std::size_t, json::error_code&) { return true; }
			bool on_object_begin(json::error_code&) { m_keys.emplace_back(); return true; }
			bool on_object_end(std::size_t, json::error_code&) { m_keys.pop_back(); return true; }
			bool on_key_part(std::string_view part, std::size_t, json::error_code&) { m_key.append(part); return true; }
			bool on_key(std::string_view part, std::size_t, json::error_code& ec)
			{
				m_key.append(part);
				const bool fresh = m_keys.back().insert(m_key).second;
				if (!fresh) {
					duplicate = m_key;
					ec = json::error::object_too_large;
				}
				m_key.clear();
				return fresh;
			}
			bool on_string_part(std::string_view, std::size_t, json::error_code&) { return true; }
			bool on_string(std::string_view, std::size_t, json::error_code&) { return true; }
			bool on_number_part(std::string_view, json::error_code&) { return true; }
			bool on_int64(std::int64_t, std::string_view, json::error_code&) { return true; }
			bool on_uint64(std::uint64_t, std::string_view, json::error_code&) { return true; }
			bool on_double(double, std::string_view, json::error_code&) { return true; }
			bool on_bool(bool, json::error_code&) { return true; }
			bool on_null(json::error_code&) { return true; }
			bool on_comment_part(std::string_view, json::error_code&) { return true; }
			bool on_comment(std::string_view, json::error_code&) { return true; }

		private:
			std::vector<std::set<std::string>> m_keys;
			std::string m_key;
		};

		std::string find_duplicate_key(std::string_view text)
		{
			json::basic_parser<duplicate_key_finder> parser{ json::parse_options{} };
			json::error_code ec;
			parser.write_some(false, text.data(), text.size(), ec);
			return parser.handler().duplicate;
		}

		// A code point a person must not be shown from a file's own words: controls, C1 controls, and the invisible formatting
		// that hides or reorders text. The same set as the client names of the MCP endpoint (kept apart: mcp_server may not
		// depend on the editor).
		bool hidden_code_point(std::uint32_t cp)
		{
			return cp < 0x20 || cp == 0x7f || (cp >= 0x80 && cp <= 0x9f) || cp == 0xad || cp == 0x61c || cp == 0x180e
				|| (cp >= 0x200b && cp <= 0x200f) || (cp >= 0x2028 && cp <= 0x202e) || (cp >= 0x2060 && cp <= 0x206f) || cp == 0xfeff;
		}

		// The next code point of well-formed UTF-8 at `at` and its length; 0 for anything else.
		std::size_t decode_code_point(std::string_view text, std::size_t at, std::uint32_t& cp)
		{
			const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
			const unsigned char lead = byte(at);
			std::size_t length = 0;
			std::uint32_t value = 0;
			std::uint32_t minimum = 0;
			if (lead < 0x80) { cp = lead; return 1; }
			else if ((lead & 0xe0) == 0xc0) { length = 2; value = lead & 0x1f; minimum = 0x80; }
			else if ((lead & 0xf0) == 0xe0) { length = 3; value = lead & 0x0f; minimum = 0x800; }
			else if ((lead & 0xf8) == 0xf0) { length = 4; value = lead & 0x07; minimum = 0x10000; }
			else return 0;
			if (at + length > text.size()) return 0;
			for (std::size_t i = 1; i < length; ++i) {
				if ((byte(at + i) & 0xc0) != 0x80) return 0;
				value = (value << 6) | (byte(at + i) & 0x3f);
			}
			if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return 0;
			cp = value;
			return length;
		}

		// The "vcs" section of a layer; the error text, empty when it reads.
		std::string parse_vcs_section(const json::value& value, policy_layer_kind kind, vcs_policy_layer& out)
		{
			if (!value.is_object()) return "\"vcs\" must be an object";
			const auto read_list = [](const json::value& list, const std::string& name, std::optional<std::vector<std::string>>& into)
				-> std::string {
				if (!list.is_array()) return "\"vcs\": \"" + name + "\" must be a list of strings";
				std::vector<std::string> items;
				for (const json::value& item : list.as_array()) {
					if (!item.is_string() || item.as_string().empty()) return "\"vcs\": \"" + name + "\" lists non-empty strings";
					items.push_back(text_of(item.as_string()));
					// The reason only, never the entry: the error is shown to a person and a hostile entry is the point.
					if (const std::string problem = vcs_entry_problem(items.back()); !problem.empty()) {
						return "\"vcs\": \"" + name + "\": " + problem;
					}
				}
				into = std::move(items);
				return {};
			};
			for (const auto& member : value.as_object()) {
				const std::string name(member.key());
				const json::value& entry = member.value();
				std::optional<bool>* flag = nullptr;
				if (name == "push_branches") {
					if (std::string error = read_list(entry, name, out.push_branches); !error.empty()) return error;
					continue;
				}
				if (name == "remotes") {
					if (std::string error = read_list(entry, name, out.remotes); !error.empty()) return error;
					continue;
				}
				if (name == "protected_branches") {
					if (std::string error = read_list(entry, name, out.protected_branches); !error.empty()) return error;
					continue;
				}
				if (name == "allow_hooks") flag = &out.allow_hooks;
				else if (name == "allow_filters") flag = &out.allow_filters;
				else if (name == "allow_repo_credential_helper") flag = &out.allow_repo_credential_helper;
				else if (name == "allow_any_repo_config") flag = &out.allow_any_repo_config;
				else return "unknown key '" + name + "' in \"vcs\"";
				if (!entry.is_bool()) return "\"vcs\": \"" + name + "\" must be true or false";
				// The personal layer only tightens: it may switch a flag off, never on.
				if (kind == policy_layer_kind::personal && entry.as_bool()) {
					return "\"vcs\": only the project policy can allow " + name.substr(std::string_view("allow_").size());
				}
				*flag = entry.as_bool();
			}
			return {};
		}

		json::array json_list(const std::vector<std::string>& items)
		{
			json::array out;
			out.reserve(items.size());
			for (const auto& item : items) out.emplace_back(json::string(item));
			return out;
		}

		// The project's list (else the default), keeping only what the personal list names too when it has one.
		std::vector<std::string> narrowed(const std::optional<std::vector<std::string>>& project,
			const std::optional<std::vector<std::string>>& personal, std::vector<std::string> fallback)
		{
			std::vector<std::string> out = project ? *project : std::move(fallback);
			if (personal) {
				std::erase_if(out, [&personal](const std::string& item) {
					return std::find(personal->begin(), personal->end(), item) == personal->end();
				});
			}
			return out;
		}

		// Whether `path` is `folder` or lies under it (both canonical).
		bool is_within(const std::filesystem::path& path, const std::filesystem::path& folder)
		{
			return std::mismatch(folder.begin(), folder.end(), path.begin(), path.end()).first == folder.end();
		}

		// The --agent-policy file is a person's answer: one inside the project, or inside the git worktree that holds it,
		// could be written by the agent it answers for. Empty: the place is fine.
		std::string preanswers_location_error(const std::filesystem::path& file, const std::filesystem::path& project_root)
		{
			if (project_root.empty()) return "there is no project folder to check its place against";
			std::error_code ec;
			const std::filesystem::path real = std::filesystem::canonical(file, ec);
			if (ec) return "the file cannot be resolved: " + ec.message();
			const std::filesystem::path root = std::filesystem::weakly_canonical(project_root, ec);
			if (ec) return "the project folder cannot be resolved: " + ec.message();
			if (is_within(real, root)) return "the --agent-policy file must be outside the project";
			for (std::filesystem::path folder = root; !folder.empty(); folder = folder.parent_path()) {
				std::error_code probe;
				if (std::filesystem::exists(std::filesystem::symlink_status(folder / ".git", probe))) {
					if (is_within(real, folder)) {
						return "the --agent-policy file must be outside the project and the git worktree holding it ("
							+ fs_paths::utf8_of(folder) + ")";
					}
					break;
				}
				if (folder == folder.parent_path()) break;
			}
			return {};
		}

		// "": nothing there or it is what `wanted` says, as the entry itself (a link, a junction or any other reparse point is
		// none of them, whatever it points to); else why not.
		std::string plain_entry_problem(const std::filesystem::path& entry, std::filesystem::file_type wanted)
		{
			std::error_code ec;
			const std::filesystem::file_status status = std::filesystem::symlink_status(entry, ec);
			if (status.type() == std::filesystem::file_type::not_found || ec == std::errc::no_such_file_or_directory
				|| ec == std::errc::not_a_directory) {
				return {};
			}
			if (ec) return "cannot be examined: " + ec.message();
			if (status.type() != wanted) return "is a link or not a plain file or folder";
			return {};
		}

		// The project's policy decides what an agent may do and a commit can plant a link in the work tree: neither the file nor
		// its folder (.snk) may be one, and the folder has to be what its place under the project folder says. Conservative
		// check by path (lstat of each, then canonical), not a no-follow open: the resolver's primitives of #484 are internal
		// to core/resource. "" means fine (also: nothing there yet).
		std::string project_policy_link_error(const std::filesystem::path& file)
		{
			const std::filesystem::path folder = file.parent_path();
			if (!folder.empty()) {
				if (std::string why = plain_entry_problem(folder, std::filesystem::file_type::directory); !why.empty()) {
					return "'" + fs_paths::utf8_of(folder) + "' " + why;
				}
			}
			if (std::string why = plain_entry_problem(file, std::filesystem::file_type::regular); !why.empty()) {
				return "'" + fs_paths::utf8_of(file) + "' " + why;
			}
			std::error_code ec;
			if (folder.empty() || !std::filesystem::exists(folder, ec)) return {};
			const std::filesystem::path root = folder.parent_path().empty() ? std::filesystem::path(".") : folder.parent_path();
			const std::filesystem::path real_root = std::filesystem::canonical(root, ec);
			if (ec) return "the project folder cannot be resolved: " + ec.message();
			const std::filesystem::path real_folder = std::filesystem::canonical(folder, ec);
			if (ec) return "'" + fs_paths::utf8_of(folder) + "' cannot be resolved: " + ec.message();
			if (real_folder != real_root / folder.filename()) {
				return "'" + fs_paths::utf8_of(folder) + "' resolves outside the project folder";
			}
			return {};
		}

		std::optional<std::string> read_text(const std::filesystem::path& file)
		{
			std::ifstream in(file, std::ios::binary);
			if (!in) return std::nullopt;
			return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		}

		// <project>/.snk/agent_policy.json, read without following a link at .snk or at the file (#526): nullopt for a
		// link there, which fails the layer closed like an unreadable file.
		std::optional<std::string> read_project_layer(const std::filesystem::path& file)
		{
			const prj::project_file_location at = prj::project_file_location_of(file);
			prj::project_file_text read = prj::read_project_file(at.root, at.relative);
			if (!read.exists || !read.error.empty()) return std::nullopt;
			return std::move(read.text);
		}

	}

	std::string vcs_entry_problem(std::string_view entry)
	{
		if (entry.size() > k_max_vcs_entry_bytes) return "an entry is longer than " + std::to_string(k_max_vcs_entry_bytes) + " bytes";
		for (std::size_t at = 0; at < entry.size();) {
			std::uint32_t cp = 0;
			const std::size_t length = decode_code_point(entry, at, cp);
			if (length == 0) return "an entry is not valid UTF-8";
			if (hidden_code_point(cp)) return "an entry holds a control or invisible character";
			if (cp == ',') return "an entry holds a comma (the window writes the lists comma-separated)";
			at += length;
		}
		return {};
	}

	std::string display_safe(std::string_view text)
	{
		std::string out;
		out.reserve(text.size());
		for (std::size_t at = 0; at < text.size();) {
			std::uint32_t cp = 0;
			const std::size_t length = decode_code_point(text, at, cp);
			if (length == 0) {
				++at;
				continue;
			}
			if (cp == '\n' || cp == '\r' || cp == '\t') out += ' ';
			else if (!hidden_code_point(cp)) out.append(text.substr(at, length));
			at += length;
		}
		return out;
	}

	cmd::policy_mode default_mode(const cmd::command_desc& command)
	{
		return default_mode(command, command.category);
	}

	cmd::policy_mode default_mode(const cmd::command_desc& command, cmd::agent_category category)
	{
		return cmd::strictest(default_mode(category), mode_of_risk(command.risk));
	}

	std::span<const cmd::agent_category> settable_categories()
	{
		return settable;
	}

	std::optional<cmd::agent_category> parse_settable_category(std::string_view name)
	{
		for (const cmd::agent_category category : settable) {
			if (name == cmd::to_string(category)) return category;
		}
		return std::nullopt;
	}

	std::optional<cmd::policy_mode> parse_policy_mode(std::string_view name)
	{
		for (const cmd::policy_mode mode : { cmd::policy_mode::allow, cmd::policy_mode::ask, cmd::policy_mode::deny }) {
			if (name == cmd::to_string(mode)) return mode;
		}
		return std::nullopt;
	}

	const char* to_string(policy_source source)
	{
		switch (source) {
		case policy_source::fallback: return "fallback";
		case policy_source::default_table: return "default";
		case policy_source::project: return "project";
		case policy_source::personal: return "personal";
		case policy_source::approval_pending: return "approval_pending";
		case policy_source::preanswers: return "preanswers";
		}
		return "fallback";
	}

	std::optional<cmd::policy_mode> policy_layer::resolve(const cmd::command_desc& command) const
	{
		return resolve(command, command.category);
	}

	std::optional<cmd::policy_mode> policy_layer::resolve(const cmd::command_desc& command, cmd::agent_category category) const
	{
		// A command's own entry is the only thing that lifts its risk floor; an entry for its category is held to it.
		// It speaks for the command's own category only: "code.write": "allow" must not allow its CMake writes.
		// The one place a layer's word is read: an `allow` that must stay a question is held to ask here.
		const auto held = [&](cmd::policy_mode mode) {
			return needs_consent_each_call(command, category) ? cmd::strictest(mode, cmd::policy_mode::ask) : mode;
		};
		if (category == command.category) {
			if (const auto own = commands.find(command.id); own != commands.end()) return held(own->second);
		}
		if (const auto entry = categories.find(category); entry != categories.end()) {
			return held(cmd::strictest(entry->second, mode_of_risk(command.risk)));
		}
		return std::nullopt;
	}

	policy_parse_result parse_policy_layer(std::string_view text, policy_layer_kind kind)
	{
		policy_parse_result out;
		const auto fail = [&out](std::string why) {
			out.layer = {};
			out.error = std::move(why);
			return out;
		};

		if (const std::string duplicate = find_duplicate_key(text); !duplicate.empty()) {
			return fail("the key '" + duplicate + "' is written twice");
		}
		boost::system::error_code ec;
		const json::value root = json::parse(text, ec);
		if (ec || !root.is_object()) return fail("not a JSON object");

		const auto put_command = [&out](const std::string& id, cmd::policy_mode mode) {
			const auto found = out.layer.commands.find(id);
			// Twice in a file (a list and the map): the stricter, never the looser.
			out.layer.commands[id] = found == out.layer.commands.end() ? mode : cmd::strictest(found->second, mode);
		};

		for (const auto& member : root.as_object()) {
			const std::string name(member.key());
			const json::value& value = member.value();
			if (name == "version") {
				if (!value.is_int64() || value.as_int64() != 1) return fail("unsupported version (this editor reads version 1)");
			}
			else if (name == "categories") {
				if (!value.is_object()) return fail("\"categories\" must be an object");
				for (const auto& entry : value.as_object()) {
					const std::string category_name(entry.key());
					const json::value& mode_value = entry.value();
					const auto category = parse_settable_category(category_name);
					if (!category) return fail("unknown category '" + category_name + "'");
					if (!mode_value.is_string()) return fail("the mode of '" + category_name + "' must be a string");
					const auto mode = parse_policy_mode(mode_value.as_string());
					if (!mode) return fail("unknown mode '" + text_of(mode_value.as_string()) + "' for '" + category_name + "'");
					out.layer.categories[*category] = *mode;
				}
			}
			else if (name == "commands") {
				if (!value.is_object()) return fail("\"commands\" must be an object");
				for (const auto& entry : value.as_object()) {
					const std::string command_id(entry.key());
					const json::value& mode_value = entry.value();
					if (!valid_command_id(command_id)) return fail("a command id with characters outside [A-Za-z0-9_.-]");
					if (!mode_value.is_string()) return fail("the mode of '" + command_id + "' must be a string");
					const auto mode = parse_policy_mode(mode_value.as_string());
					if (!mode) return fail("unknown mode '" + text_of(mode_value.as_string()) + "' for '" + command_id + "'");
					put_command(command_id, *mode);
				}
			}
			else if (name == "vcs") {
				if (kind == policy_layer_kind::preanswers) {
					return fail("\"vcs\" is not allowed in the --agent-policy file: the git rules come from the project and personal policies");
				}
				vcs_policy_layer vcs;
				if (std::string error = parse_vcs_section(value, kind, vcs); !error.empty()) return fail(std::move(error));
				out.layer.vcs = std::move(vcs);
			}
			else if (const auto listed = parse_policy_mode(name)) {
				if (!value.is_array()) return fail("\"" + name + "\" must be a list of command ids");
				for (const json::value& id : value.as_array()) {
					if (!id.is_string()) return fail("\"" + name + "\" lists command ids as strings");
					if (!valid_command_id(id.as_string())) return fail("a command id with characters outside [A-Za-z0-9_.-]");
					put_command(text_of(id.as_string()), *listed);
				}
			}
			else {
				return fail("unknown key '" + name + "'");
			}
		}
		return out;
	}

	std::string serialize_policy_layer(const policy_layer& layer)
	{
		json::object categories;
		for (const auto& [category, mode] : layer.categories) categories[cmd::to_string(category)] = cmd::to_string(mode);
		json::object commands;
		for (const auto& [id, mode] : layer.commands) commands[id] = cmd::to_string(mode);
		json::object root;
		root["version"] = 1;
		root["categories"] = std::move(categories);
		root["commands"] = std::move(commands);
		if (layer.vcs) {
			json::object vcs;
			if (layer.vcs->push_branches) vcs["push_branches"] = json_list(*layer.vcs->push_branches);
			if (layer.vcs->remotes) vcs["remotes"] = json_list(*layer.vcs->remotes);
			if (layer.vcs->protected_branches) vcs["protected_branches"] = json_list(*layer.vcs->protected_branches);
			if (layer.vcs->allow_hooks) vcs["allow_hooks"] = *layer.vcs->allow_hooks;
			if (layer.vcs->allow_filters) vcs["allow_filters"] = *layer.vcs->allow_filters;
			if (layer.vcs->allow_repo_credential_helper) vcs["allow_repo_credential_helper"] = *layer.vcs->allow_repo_credential_helper;
			if (layer.vcs->allow_any_repo_config) vcs["allow_any_repo_config"] = *layer.vcs->allow_any_repo_config;
			root["vcs"] = std::move(vcs);
		}
		return prj::to_indented_json(root);
	}

	effective_policy effective_mode(const cmd::command_desc& command, const policy_layer& project, const policy_layer& personal,
		const std::string& project_error, const std::string& personal_error, bool live_mode_on)
	{
		return effective_mode_of_call(command, command.category, project, personal, project_error, personal_error, live_mode_on);
	}

	effective_policy effective_mode_of_call(const cmd::command_desc& command, cmd::agent_category category, const policy_layer& project,
		const policy_layer& personal, const std::string& project_error, const std::string& personal_error, bool live_mode_on)
	{
		using cmd::agent_category;
		if (command.category == agent_category::unset || category == agent_category::unset) {
			return { cmd::policy_mode::deny, policy_source::fallback, "the command is not classified for agents" };
		}
		if (command.category == agent_category::not_for_agents || category == agent_category::not_for_agents) {
			return { cmd::policy_mode::deny, policy_source::fallback, "the command is not for agents" };
		}
		// Before any layer is read: neither file can open the live commands while the mode is off.
		if ((command.category == agent_category::live || category == agent_category::live) && !live_mode_on) {
			return { cmd::policy_mode::deny, policy_source::fallback, "the live mode is off" };
		}
		if (!project_error.empty()) {
			return { cmd::policy_mode::deny, policy_source::project, "the project's agent_policy.json cannot be used: " + project_error };
		}
		if (!personal_error.empty()) {
			return { cmd::policy_mode::deny, policy_source::personal, "your agent policy cannot be used: " + personal_error };
		}

		const std::optional<cmd::policy_mode> from_project = project.resolve(command, category);
		const cmd::policy_mode base = from_project.value_or(default_mode(command, category));
		const policy_source base_source = from_project ? policy_source::project : policy_source::default_table;

		const std::optional<cmd::policy_mode> from_personal = personal.resolve(command, category);
		if (from_personal && *from_personal > base) {
			return { *from_personal, policy_source::personal, "your own settings make it " + std::string(cmd::to_string(*from_personal)) };
		}
		std::string why = std::string(base_source == policy_source::project ? "the project's policy" : "the default policy") + " says "
			+ cmd::to_string(base);
		return { base, base_source, std::move(why) };
	}

	effective_policy apply_preanswers(const cmd::command_desc& command, cmd::agent_category category, effective_policy decided,
		const policy_layer& preanswers, const std::string& preanswers_error)
	{
		if (!preanswers_error.empty()) {
			return { cmd::policy_mode::deny, policy_source::preanswers, "the --agent-policy file cannot be used: " + preanswers_error };
		}
		if (decided.mode == cmd::policy_mode::deny) return decided;

		// The raw entries, not policy_layer::resolve: the risk floor resolve adds to a category entry would turn "allow" into
		// a tightening.
		std::optional<cmd::policy_mode> answer;
		bool own_entry = false;
		if (category == command.category) {
			if (const auto own = preanswers.commands.find(command.id); own != preanswers.commands.end()) {
				answer = own->second;
				own_entry = true;
			}
		}
		if (!answer) {
			if (const auto entry = preanswers.categories.find(category); entry != preanswers.categories.end()) answer = entry->second;
		}
		if (!answer) return decided;

		if (*answer > decided.mode) {
			return { *answer, policy_source::preanswers, "the --agent-policy file makes it " + std::string(cmd::to_string(*answer)) };
		}
		// As in the layers, only the command's own entry lifts the floor of its risk.
		const bool lifts = own_entry || command.risk == cmd::danger::none;
		if (decided.mode == cmd::policy_mode::ask && *answer == cmd::policy_mode::allow && lifts) {
			return { cmd::policy_mode::allow, policy_source::preanswers, "the --agent-policy file answers the question with yes in advance" };
		}
		return decided;
	}

	namespace {

		// One category held to a risk floor, from the category entries alone: a command's own entry (any id a file can write)
		// must not be able to answer for the category.
		cmd::policy_mode category_mode(const policy_layer& layer, cmd::agent_category category, cmd::danger risk)
		{
			cmd::policy_mode floor = mode_of_risk(risk);
			if (category == cmd::agent_category::project_migrate) floor = cmd::strictest(floor, cmd::policy_mode::ask);
			if (const auto entry = layer.categories.find(category); entry != layer.categories.end()) return cmd::strictest(entry->second, floor);
			return cmd::strictest(default_mode(category), floor);
		}

		cmd::policy_mode mode_in(const policy_layer& layer, const cmd::command_desc& command, cmd::agent_category category)
		{
			return layer.resolve(command, category).value_or(default_mode(command, category));
		}

		// The commands whose id a layer names and the registry does not know: judged by the entries alone.
		void add_entry_loosenings(const policy_layer& approved, const policy_layer& current, const std::set<std::string>& known,
			std::vector<policy_loosening>& out)
		{
			for (const auto& [id, mode] : current.commands) {
				if (known.contains(id)) continue;
				const auto before = approved.commands.find(id);
				if (before == approved.commands.end()) {
					if (mode != cmd::policy_mode::deny) out.push_back({ "entry '" + id + "'", cmd::policy_mode::deny, mode });
				}
				else if (mode < before->second) {
					out.push_back({ "entry '" + id + "'", before->second, mode });
				}
			}
			for (const auto& [id, mode] : approved.commands) {
				if (known.contains(id) || current.commands.contains(id) || mode == cmd::policy_mode::allow) continue;
				out.push_back({ "entry '" + id + "' (dropped)", mode, cmd::policy_mode::allow });
			}
		}

		std::uint64_t fnv1a_64(std::string_view text)
		{
			std::uint64_t hash = 14695981039346656037ull;
			for (const char c : text) {
				hash ^= static_cast<unsigned char>(c);
				hash *= 1099511628211ull;
			}
			return hash;
		}

	}

	namespace {

		bool listed(const std::vector<std::string>& list, const std::string& entry)
		{
			return std::find(list.begin(), list.end(), entry) != list.end();
		}

		// What both rules let through: the git rules of a layer held back by the approved layer while a change waits.
		vcs_policy stricter_vcs(const vcs_policy& a, const vcs_policy& b)
		{
			vcs_policy out;
			out.push_branches.clear();
			out.remotes.clear();
			for (const std::string& entry : a.push_branches) {
				if (listed(b.push_branches, entry)) out.push_branches.push_back(entry);
			}
			for (const std::string& entry : a.remotes) {
				if (listed(b.remotes, entry)) out.remotes.push_back(entry);
			}
			out.protected_branches = a.protected_branches;
			for (const std::string& entry : b.protected_branches) {
				if (!listed(out.protected_branches, entry)) out.protected_branches.push_back(entry);
			}
			out.permissions.allow_hooks = a.permissions.allow_hooks && b.permissions.allow_hooks;
			out.permissions.allow_filters = a.permissions.allow_filters && b.permissions.allow_filters;
			out.permissions.allow_repo_credential_helper = a.permissions.allow_repo_credential_helper && b.permissions.allow_repo_credential_helper;
			out.permissions.allow_any_repo_config = a.permissions.allow_any_repo_config && b.permissions.allow_any_repo_config;
			return out;
		}

		// The "vcs" section judged by what each layer alone lets through: a branch or remote the approved rules did not name,
		// a protected branch they did and the new ones do not, a flag turned on. Entries are compared as written (a glob
		// that merely looks narrower counts as wider): a miss would be a push the person never agreed to.
		void add_vcs_loosenings(const policy_layer& approved, const policy_layer& current, std::vector<policy_loosening>& out)
		{
			const vcs_policy before = effective_vcs(approved, {});
			const vcs_policy now = effective_vcs(current, {});
			const auto widened = [&out](const char* what, const std::vector<std::string>& was, const std::vector<std::string>& is) {
				for (const std::string& entry : is) {
					if (!listed(was, entry)) out.push_back({ std::string("vcs ") + what + " '" + entry + "'", cmd::policy_mode::deny, cmd::policy_mode::allow });
				}
			};
			widened("push_branches", before.push_branches, now.push_branches);
			widened("remotes", before.remotes, now.remotes);
			for (const std::string& entry : before.protected_branches) {
				if (!listed(now.protected_branches, entry)) {
					out.push_back({ "vcs protected_branches '" + entry + "' (dropped)", cmd::policy_mode::deny, cmd::policy_mode::allow });
				}
			}
			const auto flag = [&out](const char* what, bool was, bool is) {
				if (is && !was) out.push_back({ std::string("vcs ") + what, cmd::policy_mode::deny, cmd::policy_mode::allow });
			};
			flag("allow_hooks", before.permissions.allow_hooks, now.permissions.allow_hooks);
			flag("allow_filters", before.permissions.allow_filters, now.permissions.allow_filters);
			flag("allow_repo_credential_helper", before.permissions.allow_repo_credential_helper, now.permissions.allow_repo_credential_helper);
			flag("allow_any_repo_config", before.permissions.allow_any_repo_config, now.permissions.allow_any_repo_config);
		}

	}

	// A field added to policy_layer that can loosen has to be compared in find_loosenings, written by serialize_policy_layer
	// and read by parse_policy_layer: this stops the build until it is.
	static_assert(sizeof(policy_layer) == 2 * sizeof(std::map<std::string, cmd::policy_mode>) + sizeof(std::optional<vcs_policy_layer>),
		"policy_layer changed: update find_loosenings, serialize_policy_layer, parse_policy_layer and their tests");
	static_assert(sizeof(vcs_policy_layer) == 3 * sizeof(std::optional<std::vector<std::string>>) + 4 * sizeof(std::optional<bool>),
		"vcs_policy_layer changed: update add_vcs_loosenings, effective_vcs, serialize, parse and their tests");
	static_assert(sizeof(vcs_write_permissions) == 4 * sizeof(bool),
		"vcs_write_permissions changed: update stricter_vcs, add_vcs_loosenings, effective_vcs and their tests");

	std::vector<policy_loosening> find_loosenings(const policy_layer& approved, const policy_layer& current,
		const std::vector<const cmd::command_desc*>& commands)
	{
		std::vector<policy_loosening> out;
		for (const cmd::agent_category category : settable) {
			for (const cmd::danger risk : { cmd::danger::none, cmd::danger::confirm, cmd::danger::deny_by_default }) {
				const cmd::policy_mode before = category_mode(approved, category, risk);
				const cmd::policy_mode now = category_mode(current, category, risk);
				if (now < before) {
					std::string what = "category " + std::string(cmd::to_string(category));
					if (risk != cmd::danger::none) what += risk == cmd::danger::confirm ? " (commands that confirm)" : " (commands denied by default)";
					out.push_back({ std::move(what), before, now });
				}
			}
		}
		std::set<std::string> known;
		for (const cmd::command_desc* command : commands) {
			if (command == nullptr) continue;
			known.insert(command->id);
			if (command->category == cmd::agent_category::unset || command->category == cmd::agent_category::not_for_agents) continue;
			const cmd::policy_mode before = mode_in(approved, *command, command->category);
			const cmd::policy_mode now = mode_in(current, *command, command->category);
			if (now < before) out.push_back({ "command " + command->id, before, now });
		}
		add_entry_loosenings(approved, current, known, out);
		add_vcs_loosenings(approved, current, out);
		return out;
	}

	approved_snapshot_location approved_snapshot_for(const std::filesystem::path& personal_settings_dir, const std::filesystem::path& project_root)
	{
		std::error_code ec;
		std::filesystem::path canonical = std::filesystem::weakly_canonical(project_root, ec);
		if (ec) canonical = project_root.lexically_normal();
		std::string key = fs_paths::utf8_of(canonical);
		for (char& ch : key) {
			if (ch == '\\') ch = '/';
#if defined(_WIN32) || defined(__APPLE__)
			else if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
#endif
		}
		while (key.size() > 1 && key.back() == '/') key.pop_back();
		return { personal_settings_dir / "agent_policy_approved" / (std::format("{:016x}", fnv1a_64(key)) + ".json"), key };
	}

	namespace {

		std::string serialize_snapshot(const std::string& key, const policy_layer& layer)
		{
			json::object root;
			root["project"] = key;
			root["layer"] = json::parse(serialize_policy_layer(layer));
			return prj::to_indented_json(root);
		}

		policy_parse_result parse_snapshot(std::string_view text, const std::string& key)
		{
			boost::system::error_code ec;
			const json::value root = json::parse(text, ec);
			if (ec || !root.is_object()) return { {}, "not a JSON object" };
			const json::object& object = root.as_object();
			const json::value* project = object.if_contains("project");
			const json::value* layer = object.if_contains("layer");
			if (project == nullptr || !project->is_string() || layer == nullptr || !layer->is_object()) return { {}, "not an approved copy" };
			if (!key.empty() && text_of(project->as_string()) != key) return { {}, "approved for another project" };
			return parse_policy_layer(json::serialize(*layer));
		}

	}

	agent_policy::agent_policy(std::filesystem::path project_file, std::filesystem::path personal_file, approved_snapshot_location approved)
	{
		m_project.file = std::move(project_file);
		m_personal.file = std::move(personal_file);
		m_personal.kind = policy_layer_kind::personal;
		m_preanswers.kind = policy_layer_kind::preanswers;
		m_approved_file = std::move(approved.file);
		m_approved_key = std::move(approved.key);
		load_approved();
	}

	void agent_policy::load_approved()
	{
		if (m_approved_file.empty()) return;
		std::error_code ec;
		if (!std::filesystem::exists(m_approved_file, ec) && !ec) return;
		const std::optional<std::string> text = read_text(m_approved_file);
		policy_parse_result parsed = text ? parse_snapshot(*text, m_approved_key) : policy_parse_result{ {}, "the file cannot be read" };
		if (parsed.error.empty()) {
			m_approved = std::move(parsed.layer);
			return;
		}
		m_snapshot_lost = true;
		std::filesystem::path kept = m_approved_file;
		kept += ".corrupt";
		std::filesystem::rename(m_approved_file, kept, ec);
		egLOG_WARN("editor/agent_policy", "{}: {}; kept as {} and not adopted: the project policy needs a person's approval again",
			fs_paths::utf8_of(m_approved_file), parsed.error, fs_paths::utf8_of(kept));
	}

	std::vector<const cmd::command_desc*> agent_policy::commands() const
	{
		return m_commands ? m_commands() : std::vector<const cmd::command_desc*>{};
	}

	void agent_policy::set_command_source(std::function<std::vector<const cmd::command_desc*>()> commands,
		std::function<std::uint64_t()> generation)
	{
		m_commands = std::move(commands);
		m_generation = std::move(generation);
		if (m_project.loaded) reconcile_project();
	}

	std::optional<std::string> agent_policy::keep_approved(const policy_layer& layer)
	{
		if (!m_approved_file.empty()) {
			std::error_code ec;
			std::filesystem::create_directories(m_approved_file.parent_path(), ec);
			if (const std::optional<std::string> error = prj::write_text_atomically(m_approved_file, serialize_snapshot(m_approved_key, layer))) return error;
		}
		m_approved = layer;
		m_snapshot_lost = false;
		return std::nullopt;
	}

	namespace {

		// What of a file that loosens nothing is recorded without a person: its entries for a command other than `deny` are
		// not. Such an entry equals the command's default today, but the command may be registered again with a risk floor,
		// and a recorded entry would lift it unseen; so it is recorded only by an explicit approval.
		// A value that is only what the default says is not recorded either: the person approved nothing there, and the
		// default may be tightened later (a recorded copy of the old default would then hold the new one back). A value
		// that went back to the default drops the approved entry (the baseline is tightened to the default).
		void keep_default_values_out(const policy_layer& approved, policy_layer& out)
		{
			for (auto it = out.categories.begin(); it != out.categories.end();) {
				const auto before = approved.categories.find(it->first);
				const bool already = before != approved.categories.end() && before->second == it->second;
				if (!already && it->second == default_mode(it->first)) {
					it = out.categories.erase(it);
				}
				else {
					++it;
				}
			}
			if (!out.vcs) return;
			const vcs_policy defaults;
			const vcs_policy_layer none{};
			const vcs_policy_layer& was = approved.vcs ? *approved.vcs : none;
			vcs_policy_layer& now = *out.vcs;
			const auto only_default_list = [](std::optional<std::vector<std::string>>& value, const std::optional<std::vector<std::string>>& before,
				const std::vector<std::string>& default_list, bool entries_may_repeat_defaults) {
				if (!value || value == before) return;
				const bool all_default = entries_may_repeat_defaults
					? std::all_of(value->begin(), value->end(), [&default_list](const std::string& entry) { return listed(default_list, entry); })
					: *value == default_list;
				if (all_default) value.reset();
			};
			only_default_list(now.push_branches, was.push_branches, defaults.push_branches, false);
			only_default_list(now.remotes, was.remotes, defaults.remotes, false);
			only_default_list(now.protected_branches, was.protected_branches, defaults.protected_branches, true);
			const auto only_default_flag = [](std::optional<bool>& value, const std::optional<bool>& before) {
				if (value && value != before && !*value) value.reset();
			};
			only_default_flag(now.allow_hooks, was.allow_hooks);
			only_default_flag(now.allow_filters, was.allow_filters);
			only_default_flag(now.allow_repo_credential_helper, was.allow_repo_credential_helper);
			only_default_flag(now.allow_any_repo_config, was.allow_any_repo_config);
			if (now == vcs_policy_layer{}) out.vcs.reset();
		}

		policy_layer silently_recordable(const policy_layer& approved, const policy_layer& current)
		{
			policy_layer out = current;
			keep_default_values_out(approved, out);
			for (auto it = out.commands.begin(); it != out.commands.end();) {
				const auto before = approved.commands.find(it->first);
				if (it->second == cmd::policy_mode::deny) {
					++it;
				}
				else if (before != approved.commands.end() && before->second == it->second) {
					++it;
				}
				else if (before != approved.commands.end()) {
					it->second = before->second;
					++it;
				}
				else {
					it = out.commands.erase(it);
				}
			}
			return out;
		}

	}

	void agent_policy::rejudge_if_commands_changed()
	{
		if (!m_commands || !m_project.loaded) return;
		const std::uint64_t stamp = m_generation ? m_generation() : static_cast<std::uint64_t>(m_commands().size());
		if (stamp != m_reconciled_commands) reconcile_project();
	}

	void agent_policy::reconcile_project()
	{
		m_loosenings.clear();
		m_pending = false;
		const std::vector<const cmd::command_desc*> known = commands();
		m_reconciled_commands = m_generation ? m_generation() : static_cast<std::uint64_t>(known.size());
		if (!m_project.error.empty()) return;
		m_loosenings = find_loosenings(m_approved, m_project.layer, known);
		if (m_snapshot_lost && !m_project.layer.empty()) {
			m_loosenings.push_back({ "the approved copy of this policy is missing or unreadable", cmd::policy_mode::deny, cmd::policy_mode::allow });
		}
		m_pending = !m_loosenings.empty();
		if (m_pending) {
			egLOG_WARN("editor/agent_policy", "{}: loosens {} thing(s) an agent may do; the approved policy applies until a person approves it",
				fs_paths::utf8_of(m_project.file), m_loosenings.size());
			return;
		}
		if (m_snapshot_lost) return;
		const policy_layer recordable = silently_recordable(m_approved, m_project.layer);
		if (serialize_policy_layer(recordable) == serialize_policy_layer(m_approved)) return;
		if (const std::optional<std::string> error = keep_approved(recordable)) {
			egLOG_WARN("editor/agent_policy", "the approved copy of the project policy cannot be kept: {}", *error);
		}
	}

	std::optional<std::string> agent_policy::approve_project_layer(const policy_layer& reviewed, const std::vector<policy_loosening>* displayed)
	{
		ASSERT_MSG(std::this_thread::get_id() == m_main_thread, "agent_policy::approve_project_layer: main thread only");
		if (!m_approval_allowed) return "no person is here to approve";
		const std::string shown = serialize_policy_layer(reviewed);
		refresh();
		if (!m_project.error.empty()) return "the project's agent_policy.json cannot be used: " + m_project.error;
		if (serialize_policy_layer(m_project.layer) != shown) return "the project's agent_policy.json changed since it was shown; review it again";
		// Only what the person saw is approved: a command entry that loosens nothing today (and so was not listed) could lift
		// the risk floor of a command registered later, so it stays unapproved like on a silent take-over.
		policy_layer record = silently_recordable(m_approved, m_project.layer);
		for (const policy_loosening& change : displayed != nullptr ? *displayed : m_loosenings) {
			for (const auto& [id, mode] : m_project.layer.commands) {
				if (change.what == "entry '" + id + "'" || change.what == "command " + id) record.commands[id] = mode;
			}
		}
		if (const std::optional<std::string> error = keep_approved(record)) return error;
		reconcile_project();
		if (m_on_changed) m_on_changed();
		return std::nullopt;
	}

	vcs_policy effective_vcs(const policy_layer& project, const policy_layer& personal, const std::string& project_error,
		const std::string& personal_error)
	{
		vcs_policy out;
		if (!project_error.empty() || !personal_error.empty()) {
			out.push_branches.clear();
			out.remotes.clear();
			return out;
		}
		const vcs_policy_layer none{};
		const vcs_policy_layer& own = project.vcs ? *project.vcs : none;
		const vcs_policy_layer& mine = personal.vcs ? *personal.vcs : none;
		out.push_branches = narrowed(own.push_branches, mine.push_branches, std::move(out.push_branches));
		out.remotes = narrowed(own.remotes, mine.remotes, std::move(out.remotes));
		for (const auto* extra : { &own.protected_branches, &mine.protected_branches }) {
			if (!*extra) continue;
			for (const auto& branch : **extra) {
				if (std::find(out.protected_branches.begin(), out.protected_branches.end(), branch) == out.protected_branches.end()) {
					out.protected_branches.push_back(branch);
				}
			}
		}
		const auto flag = [](const std::optional<bool>& from_project, const std::optional<bool>& from_personal) {
			return from_project.value_or(false) && from_personal.value_or(true);
		};
		out.permissions.allow_hooks = flag(own.allow_hooks, mine.allow_hooks);
		out.permissions.allow_filters = flag(own.allow_filters, mine.allow_filters);
		out.permissions.allow_repo_credential_helper = flag(own.allow_repo_credential_helper, mine.allow_repo_credential_helper);
		out.permissions.allow_any_repo_config = flag(own.allow_any_repo_config, mine.allow_any_repo_config);
		return out;
	}

	void agent_policy::set_preanswers_file(std::filesystem::path file, std::filesystem::path project_root)
	{
		ASSERT_MSG(std::this_thread::get_id() == m_main_thread, "agent_policy::set_preanswers_file: main thread only");
		m_preanswers = source{};
		m_preanswers.kind = policy_layer_kind::preanswers;
		m_preanswers.file = std::move(file);
		m_preanswers_project_root = std::move(project_root);
		m_warned_preanswers_change = false;
		if (has_preanswers()) reload(m_preanswers);
	}

	void agent_policy::set_headless(bool headless)
	{
		if (m_headless == headless) return;
		m_headless = headless;
		// What an agent may see follows: the --agent-policy file applies or stops applying.
		if (has_preanswers() && m_on_changed) m_on_changed();
	}

	namespace {

		// What can be told about a policy file without reading it. A file that cannot be examined (not "not found": a refused
		// access, a folder where the file should be) is an error of the layer, never "no file".
		struct file_probe
		{
			bool exists = false;
			std::filesystem::file_time_type modified{};
			std::uintmax_t size = 0;
			std::string error;
			// The file or .snk is a link (#526): logged as an ERROR, not a WARN.
			bool refused_link = false;
		};

		constexpr std::string_view k_not_regular = "it is not a regular file";

		file_probe probe_file(const std::filesystem::path& file, bool refuse_links)
		{
			file_probe out;
			if (file.empty()) return out;
			// Before status(), which follows a link: a dangling one would read as "no file" and open the defaults.
			if (refuse_links) {
				if (std::string why = project_policy_link_error(file); !why.empty()) {
					out.error = std::move(why);
					out.refused_link = true;
					return out;
				}
			}
			std::error_code ec;
			const std::filesystem::file_status status = std::filesystem::status(file, ec);
			// A missing file is the normal case of a project with no policy of its own. Some standard libraries report it as
			// an error code besides file_type::not_found, so that comes first.
			if (status.type() == std::filesystem::file_type::not_found || ec == std::errc::no_such_file_or_directory
				|| ec == std::errc::not_a_directory) {
				return out;
			}
			if (ec) {
				out.error = "the file cannot be examined: " + ec.message();
				return out;
			}
			if (!std::filesystem::exists(status)) return out;
			if (!std::filesystem::is_regular_file(status)) {
				out.error = k_not_regular;
				return out;
			}
			out.exists = true;
			out.modified = std::filesystem::last_write_time(file, ec);
			if (ec) {
				out.error = "the file cannot be examined: " + ec.message();
				return out;
			}
			out.size = std::filesystem::file_size(file, ec);
			if (ec) out.error = "the file cannot be examined: " + ec.message();
			return out;
		}

	}

	bool agent_policy::changed_on_disk(const source& what)
	{
		if (!what.loaded) return true;
		const file_probe now = probe_file(what.file, what.kind == policy_layer_kind::project);
		return now.exists != what.existed || now.modified != what.modified || now.size != what.size || now.error != what.stat_error;
	}

	void agent_policy::reload(source& what)
	{
		const bool was_loaded = what.loaded;
		what.loaded = true;
		what.layer = {};
		what.error.clear();
		const bool project_layer = what.kind == policy_layer_kind::project;
		const file_probe now = probe_file(what.file, project_layer);
		// A link at the project file or at .snk (#526): the layer fails closed and the person is told loudly.
		bool refused_link = project_layer && (now.refused_link || now.error == k_not_regular);
		what.existed = now.exists;
		what.modified = now.modified;
		what.size = now.size;
		what.stat_error = now.error;
		what.warned_unknown = false;
		if (!now.error.empty()) {
			what.error = now.error;
		}
		else if (!now.exists && what.kind == policy_layer_kind::preanswers) {
			// Not "no answers": the person named a file, and a run that cannot read it must not go on as if they had not.
			what.error = "the file does not exist";
		}
		else if (std::string misplaced = what.kind == policy_layer_kind::preanswers
				? preanswers_location_error(what.file, m_preanswers_project_root) : std::string{};
			!misplaced.empty()) {
			what.error = std::move(misplaced);
		}
		else if (now.exists) {
			const std::optional<std::string> text = project_layer ? read_project_layer(what.file) : read_text(what.file);
			if (!text) {
				what.error = project_layer ? "the file cannot be read, or it or .snk is a link" : "the file cannot be read";
				refused_link = project_layer;
			}
			else {
				policy_parse_result parsed = parse_policy_layer(*text, what.kind);
				what.layer = std::move(parsed.layer);
				what.error = std::move(parsed.error);
			}
		}
		if (!what.error.empty()) {
			if (what.kind == policy_layer_kind::preanswers) {
				egLOG_ERROR("editor/agent_policy", "--agent-policy {}: {}; every agent request of a headless run is denied",
					fs_paths::utf8_of(what.file), what.error);
			}
			else if (refused_link) {
				egLOG_ERROR("editor/agent_policy", "{}: {}; every agent request is denied", fs_paths::utf8_of(what.file), what.error);
			}
			else {
				egLOG_WARN("editor/agent_policy", "{}: {}", fs_paths::utf8_of(what.file), what.error);
			}
		}
		warn_unknown_commands(what);
		if (&what == &m_project) reconcile_project();
		if (was_loaded && m_on_changed) m_on_changed();
	}

	void agent_policy::warn_unknown_commands(source& what) const
	{
		if (!m_known || what.warned_unknown) return;
		what.warned_unknown = true;
		for (const auto& [id, mode] : what.layer.commands) {
			if (!m_known(id)) {
				egLOG_WARN("editor/agent_policy", "{}: no command '{}' (a typo, or a module that is not loaded)", fs_paths::utf8_of(what.file), id);
			}
		}
	}

	void agent_policy::set_known_commands(std::function<bool(std::string_view)> exists)
	{
		m_known = std::move(exists);
		for (source* what : { &m_project, &m_personal, &m_preanswers }) {
			if (what->loaded) warn_unknown_commands(*what);
		}
	}

	void agent_policy::refresh()
	{
		for (source* what : { &m_project, &m_personal }) {
			if (changed_on_disk(*what)) reload(*what);
		}
		rejudge_if_commands_changed();
		// Read once: answers a running editor took in must not change under it (nor be swapped in by what it runs).
		if (has_preanswers() && !m_warned_preanswers_change && changed_on_disk(m_preanswers)) {
			m_warned_preanswers_change = true;
			egLOG_WARN("editor/agent_policy", "--agent-policy {} changed while running: ignored until restart",
				fs_paths::utf8_of(m_preanswers.file));
		}
	}

	effective_policy agent_policy::effective(const cmd::command_desc& command)
	{
		return effective(command, command.category);
	}

	effective_policy agent_policy::effective(const cmd::command_desc& command, cmd::agent_category category)
	{
		ASSERT_MSG(std::this_thread::get_id() == m_main_thread, "agent_policy::effective: main thread only");
		refresh();
		return decide(command, category);
	}

	effective_policy agent_policy::effective_as_loaded(const cmd::command_desc& command)
	{
		ASSERT_MSG(std::this_thread::get_id() == m_main_thread, "agent_policy::effective_as_loaded: main thread only");
		return decide(command, command.category);
	}

	effective_policy agent_policy::decide(const cmd::command_desc& command, cmd::agent_category category)
	{
		const bool live_on = m_live_on && m_live_on();
		effective_policy decided = effective_mode_of_call(command, category, m_project.layer, m_personal.layer, m_project.error,
			m_personal.error, live_on);
		// Also when nothing is pending: an entry the file has and the approved copy does not (see silently_recordable) must not
		// lift what a command registered since carries, e.g. a risk floor after a module reload that left the registry's size.
		const effective_policy held = effective_mode_of_call(command, category, m_approved, m_personal.layer, {}, m_personal.error, live_on);
		if (held.mode > decided.mode) {
			decided = { held.mode, policy_source::approval_pending,
				"the project's agent_policy.json has something that waits for a person's approval; until then the approved version applies: " + held.reason };
		}
		// After the approval: the --agent-policy file answers only an `ask` the approved result gives, never above it.
		if (!has_preanswers() || !m_headless) return decided;
		return apply_preanswers(command, category, std::move(decided), m_preanswers.layer, m_preanswers.error);
	}

	vcs_policy agent_policy::effective_vcs()
	{
		ASSERT_MSG(std::this_thread::get_id() == m_main_thread, "agent_policy::effective_vcs: main thread only");
		refresh();
		rejudge_if_commands_changed();
		const vcs_policy now = edt::effective_vcs(m_project.layer, m_personal.layer, m_project.error, m_personal.error);
		if (!m_pending) return now;
		return stricter_vcs(now, edt::effective_vcs(m_approved, m_personal.layer, {}, m_personal.error));
	}

	std::optional<std::string> agent_policy::save(source& what, const policy_layer& layer, const prj::text_writer& write, const policy_layer* record)
	{
		ASSERT_MSG(std::this_thread::get_id() == m_main_thread, "agent_policy::save: main thread only");
		const std::string text = serialize_policy_layer(layer);
		if (!what.file.has_parent_path()) return "no place to write the policy";
		std::optional<std::string> error;
		if (!write && what.kind == policy_layer_kind::project) {
			// <project>/.snk/agent_policy.json without following a link at .snk or at the file, nor racing one (#523).
			std::error_code ec;
			std::filesystem::path file = std::filesystem::absolute(what.file, ec);
			if (ec) file = what.file;
			const std::filesystem::path folder = file.parent_path();
			error = prj::write_project_file(folder.parent_path(), folder.filename() / file.filename(), text);
			if (error) {
				return "not written: " + *error + " (neither .snk nor agent_policy.json may be a link or a junction)";
			}
		} else {
			std::error_code ec;
			std::filesystem::create_directories(what.file.parent_path(), ec);
			error = write ? write(what.file, text) : prj::write_text_atomically(what.file, text);
		}
		if (error) return error;
		// A person's own edit is the approval of what they changed.
		if (record != nullptr) {
			if (const std::optional<std::string> kept = keep_approved(*record)) {
				reload(what);
				return "saved, but the approval could not be kept: " + *kept;
			}
		}
		reload(what);
		return what.error.empty() ? std::nullopt : std::optional<std::string>{ what.error };
	}

	std::optional<std::string> agent_policy::save_project(const policy_layer& layer, const policy_layer& edited_from,
		const prj::text_writer& write)
	{
		if (!m_approval_allowed) return "no person is here to save the project policy";
		// Entries of commands other than `deny` are recorded only when the person changed them in this edit: one the file
		// brought along could lift the risk floor of a command registered later, and nobody looked at it.
		policy_layer record = silently_recordable(m_approved, layer);
		for (const auto& [id, mode] : layer.commands) {
			const auto before = edited_from.commands.find(id);
			if (before == edited_from.commands.end() || before->second != mode) record.commands[id] = mode;
		}
		return save(m_project, layer, write, &record);
	}

	std::optional<std::string> agent_policy::save_personal(const policy_layer& layer, const prj::text_writer& write)
	{
		return save(m_personal, layer, write);
	}

	cmd::policy_decision agent_policy_gate::decide(const cmd::command_desc& command, const cmd::call& request)
	{
		// The command's own category and its call's (code.write to a CMake file: cmake_presets); the stricter decides, so
		// a call's category never loosens what the command's allows.
		const cmd::agent_category of_call = cmd::category_of_call(command, request);
		const effective_policy by_command = m_policy.effective(command, command.category);
		const effective_policy by_call = of_call == command.category ? by_command : m_policy.effective(command, of_call);
		const effective_policy& decision = by_call.mode >= by_command.mode ? by_call : by_command;
		switch (decision.mode) {
		case cmd::policy_mode::allow: return cmd::policy_decision::allow();
		case cmd::policy_mode::ask: {
			// The grant has to be for the category that asks; when both ask, the call's (the narrower one) covers it.
			const cmd::agent_category asking = by_call.mode == cmd::policy_mode::ask ? of_call : command.category;
			// A grant given for a harmless call of the category must not carry a call that sends or destroys something:
			// a command with a risk is asked every time.
			if (command.risk == cmd::danger::none && m_grants != nullptr && !request.from.session.empty()
				&& m_grants->has(request.from.session, asking)) {
				return cmd::policy_decision::allow();
			}
			return cmd::policy_decision::ask(command.summary, asking);
		}
		case cmd::policy_mode::deny: return cmd::policy_decision::deny(decision.reason);
		}
		return cmd::policy_decision::deny("the policy cannot be read");
	}
}
