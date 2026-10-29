#include "edt_agent_policy.h"
#include <snk/common/engine_assert.h>
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/basic_parser.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/parse_options.hpp>
#include <fstream>
#include <set>
#include <thread>
#include <iterator>
#include <system_error>
#include <utility>

namespace json = boost::json;

namespace edt
{
	namespace {

		constexpr cmd::agent_category settable[] = {
			cmd::agent_category::read, cmd::agent_category::scene_edit, cmd::agent_category::code_edit, cmd::agent_category::code_files,
			cmd::agent_category::cmake_presets, cmd::agent_category::build, cmd::agent_category::live,
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
			static constexpr std::size_t max_object_size = static_cast<std::size_t>(-1);
			static constexpr std::size_t max_array_size = static_cast<std::size_t>(-1);
			static constexpr std::size_t max_key_size = static_cast<std::size_t>(-1);
			static constexpr std::size_t max_string_size = static_cast<std::size_t>(-1);

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
			parser.write(text.data(), text.size(), ec);
			return parser.handler().duplicate;
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
		return cmd::strictest(default_mode(command.category), mode_of_risk(command.risk));
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
		if (const auto own = commands.find(command.id); own != commands.end()) return own->second;
		if (const auto category = categories.find(command.category); category != categories.end()) return category->second;
		return std::nullopt;
	}

	policy_parse_result parse_policy_layer(std::string_view text)
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
		return prj::to_indented_json(root);
	}

	effective_policy effective_mode(const cmd::command_desc& command, const policy_layer& project, const policy_layer& personal,
		const std::string& project_error, const std::string& personal_error, bool live_mode_on)
	{
		using cmd::agent_category;
		if (command.category == agent_category::unset) {
			return { cmd::policy_mode::deny, policy_source::fallback, "the command is not classified for agents" };
		}
		if (command.category == agent_category::not_for_agents) {
			return { cmd::policy_mode::deny, policy_source::fallback, "the command is not for agents" };
		}
		// Before any layer is read: neither file can open the live commands while the mode is off.
		if (command.category == agent_category::live && !live_mode_on) {
			return { cmd::policy_mode::deny, policy_source::fallback, "the live mode is off" };
		}
		if (!project_error.empty()) {
			return { cmd::policy_mode::deny, policy_source::project, "the project's agent_policy.json cannot be used: " + project_error };
		}
		if (!personal_error.empty()) {
			return { cmd::policy_mode::deny, policy_source::personal, "your agent policy cannot be used: " + personal_error };
		}

		// A command's own entry is the only thing that lifts its risk floor; an entry for its category is held to it.
		const cmd::policy_mode floor = mode_of_risk(command.risk);
		const auto resolve = [&command, floor](const policy_layer& layer) -> std::optional<cmd::policy_mode> {
			if (const auto own = layer.commands.find(command.id); own != layer.commands.end()) return own->second;
			if (const auto category = layer.categories.find(command.category); category != layer.categories.end()) {
				return cmd::strictest(category->second, floor);
			}
			return std::nullopt;
		};

		const std::optional<cmd::policy_mode> from_project = resolve(project);
		const cmd::policy_mode base = from_project.value_or(default_mode(command));
		const policy_source base_source = from_project ? policy_source::project : policy_source::default_table;

		const std::optional<cmd::policy_mode> from_personal = resolve(personal);
		if (from_personal && *from_personal > base) {
			return { *from_personal, policy_source::personal, "your own settings make it " + std::string(cmd::to_string(*from_personal)) };
		}
		std::string why = std::string(base_source == policy_source::project ? "the project's policy" : "the default policy") + " says "
			+ cmd::to_string(base);
		return { base, base_source, std::move(why) };
	}

	agent_policy::agent_policy(std::filesystem::path project_file, std::filesystem::path personal_file)
	{
		m_project.file = std::move(project_file);
		m_personal.file = std::move(personal_file);
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
		what.loaded = true;
		what.layer = {};
		what.error.clear();
		const file_probe now = probe_file(what.file);
		what.existed = now.exists;
		what.modified = now.modified;
		what.size = now.size;
		what.stat_error = now.error;
		if (!now.error.empty()) {
			what.error = now.error;
		}
		else if (now.exists) {
			const std::optional<std::string> text = read_text(what.file);
			if (!text) {
				what.error = "the file cannot be read";
			}
			else {
				policy_parse_result parsed = parse_policy_layer(*text);
				what.layer = std::move(parsed.layer);
				what.error = std::move(parsed.error);
			}
		}
		if (!what.error.empty()) {
			egLOG_WARN("editor/agent_policy", "{}: {}", fs_paths::utf8_of(what.file), what.error);
		}
		warn_unknown_commands(what);
	}

	void agent_policy::warn_unknown_commands(const source& what) const
	{
		if (!m_known) return;
		for (const auto& [id, mode] : what.layer.commands) {
			if (!m_known(id)) {
				egLOG_WARN("editor/agent_policy", "{}: no command '{}' (a typo, or a module that is not loaded)", fs_paths::utf8_of(what.file), id);
			}
		}
	}

	void agent_policy::set_known_commands(std::function<bool(std::string_view)> exists)
	{
		m_known = std::move(exists);
		for (const source* what : { &m_project, &m_personal }) {
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
		ASSERT_MSG(std::this_thread::get_id() == m_main_thread, "agent_policy::effective: main thread only");
		refresh();
		const bool live_on = m_live_on && m_live_on();
		return effective_mode(command, m_project.layer, m_personal.layer, m_project.error, m_personal.error, live_on);
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

	cmd::policy_decision agent_policy_gate::decide(const cmd::command_desc& command, const cmd::call&)
	{
		const effective_policy decision = m_policy.effective(command);
		switch (decision.mode) {
		case cmd::policy_mode::allow: return cmd::policy_decision::allow();
		case cmd::policy_mode::ask: return cmd::policy_decision::ask(command.summary);
		case cmd::policy_mode::deny: return cmd::policy_decision::deny(decision.reason);
		}
		return cmd::policy_decision::deny("the policy cannot be read");
	}
}
