#include "edt_agent_policy.h"
#include "edt_agent_confirmations.h"
#include <snk/common/engine_assert.h>
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/basic_parser.hpp>
#include <boost/json/basic_parser_impl.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/parse_options.hpp>
#include <boost/json/string.hpp>
#include <boost/json/value.hpp>
#include <algorithm>
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
			cmd::agent_category::vcs_read, cmd::agent_category::vcs_commit, cmd::agent_category::vcs_branch,
			cmd::agent_category::vcs_sync, cmd::agent_category::vcs_push,
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

		std::optional<std::string> read_text(const std::filesystem::path& file)
		{
			std::ifstream in(file, std::ios::binary);
			if (!in) return std::nullopt;
			return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		}

	}

	cmd::policy_mode default_mode(const cmd::command_desc& command)
	{
		return default_mode(command, command.category);
	}

	cmd::policy_mode default_mode(const cmd::command_desc& command, cmd::agent_category category)
	{
		return cmd::strictest(default_mode(category), mode_of_risk(command.risk));
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
		if (category == command.category) {
			if (const auto own = commands.find(command.id); own != commands.end()) return own->second;
		}
		if (const auto entry = categories.find(category); entry != categories.end()) {
			return cmd::strictest(entry->second, mode_of_risk(command.risk));
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
					if (!mode_value.is_string()) return fail("the mode of '" + command_id + "' must be a string");
					const auto mode = parse_policy_mode(mode_value.as_string());
					if (!mode) return fail("unknown mode '" + text_of(mode_value.as_string()) + "' for '" + command_id + "'");
					put_command(command_id, *mode);
				}
			}
			else if (name == "vcs") {
				vcs_policy_layer vcs;
				if (std::string error = parse_vcs_section(value, kind, vcs); !error.empty()) return fail(std::move(error));
				out.layer.vcs = std::move(vcs);
			}
			else if (const auto listed = parse_policy_mode(name)) {
				if (!value.is_array()) return fail("\"" + name + "\" must be a list of command ids");
				for (const json::value& id : value.as_array()) {
					if (!id.is_string()) return fail("\"" + name + "\" lists command ids as strings");
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

	agent_policy::agent_policy(std::filesystem::path project_file, std::filesystem::path personal_file)
	{
		m_project.file = std::move(project_file);
		m_personal.file = std::move(personal_file);
		m_personal.kind = policy_layer_kind::personal;
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
		};

		file_probe probe_file(const std::filesystem::path& file)
		{
			file_probe out;
			if (file.empty()) return out;
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
				out.error = "it is not a regular file";
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
		const file_probe now = probe_file(what.file);
		return now.exists != what.existed || now.modified != what.modified || now.size != what.size || now.error != what.stat_error;
	}

	void agent_policy::reload(source& what)
	{
		const bool was_loaded = what.loaded;
		what.loaded = true;
		what.layer = {};
		what.error.clear();
		const file_probe now = probe_file(what.file);
		what.existed = now.exists;
		what.modified = now.modified;
		what.size = now.size;
		what.stat_error = now.error;
		what.warned_unknown = false;
		if (!now.error.empty()) {
			what.error = now.error;
		}
		else if (now.exists) {
			const std::optional<std::string> text = read_text(what.file);
			if (!text) {
				what.error = "the file cannot be read";
			}
			else {
				policy_parse_result parsed = parse_policy_layer(*text, what.kind);
				what.layer = std::move(parsed.layer);
				what.error = std::move(parsed.error);
			}
		}
		if (!what.error.empty()) {
			egLOG_WARN("editor/agent_policy", "{}: {}", fs_paths::utf8_of(what.file), what.error);
		}
		warn_unknown_commands(what);
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
		for (source* what : { &m_project, &m_personal }) {
			if (what->loaded) warn_unknown_commands(*what);
		}
	}

	void agent_policy::refresh()
	{
		for (source* what : { &m_project, &m_personal }) {
			if (changed_on_disk(*what)) reload(*what);
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
		const bool live_on = m_live_on && m_live_on();
		return effective_mode_of_call(command, category, m_project.layer, m_personal.layer, m_project.error, m_personal.error, live_on);
	}

	vcs_policy agent_policy::effective_vcs()
	{
		ASSERT_MSG(std::this_thread::get_id() == m_main_thread, "agent_policy::effective_vcs: main thread only");
		refresh();
		return edt::effective_vcs(m_project.layer, m_personal.layer, m_project.error, m_personal.error);
	}

	std::optional<std::string> agent_policy::save(source& what, const policy_layer& layer, const prj::text_writer& write)
	{
		ASSERT_MSG(std::this_thread::get_id() == m_main_thread, "agent_policy::save: main thread only");
		const std::string text = serialize_policy_layer(layer);
		std::error_code ec;
		if (!what.file.has_parent_path()) return "no place to write the policy";
		std::filesystem::create_directories(what.file.parent_path(), ec);
		const std::optional<std::string> error = write ? write(what.file, text) : prj::write_text_atomically(what.file, text);
		if (error) return error;
		reload(what);
		return what.error.empty() ? std::nullopt : std::optional<std::string>{ what.error };
	}

	std::optional<std::string> agent_policy::save_project(const policy_layer& layer, const prj::text_writer& write)
	{
		return save(m_project, layer, write);
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
			if (m_grants != nullptr && !request.from.session.empty() && m_grants->has(request.from.session, asking)) {
				return cmd::policy_decision::allow();
			}
			return cmd::policy_decision::ask(command.summary, asking);
		}
		case cmd::policy_mode::deny: return cmd::policy_decision::deny(decision.reason);
		}
		return cmd::policy_decision::deny("the policy cannot be read");
	}
}
