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
#include <cstdint>
#include <format>
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
		case policy_source::approval_pending: return "approval_pending";
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
					if (!valid_command_id(command_id)) return fail("a command id with characters outside [A-Za-z0-9_.-]");
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

	namespace {

		// One category held to a risk floor, from the category entries alone: a command's own entry (any id a file can write)
		// must not be able to answer for the category.
		cmd::policy_mode category_mode(const policy_layer& layer, cmd::agent_category category, cmd::danger risk)
		{
			const cmd::policy_mode floor = mode_of_risk(risk);
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

	// A field added to policy_layer that can loosen has to be compared in find_loosenings, written by serialize_policy_layer
	// and read by parse_policy_layer: this stops the build until it is.
	static_assert(sizeof(policy_layer) == 2 * sizeof(std::map<std::string, cmd::policy_mode>),
		"policy_layer changed: update find_loosenings, serialize_policy_layer, parse_policy_layer and their tests");

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

	void agent_policy::set_command_source(std::function<std::vector<const cmd::command_desc*>()> commands)
	{
		m_commands = std::move(commands);
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
		policy_layer silently_recordable(const policy_layer& approved, const policy_layer& current)
		{
			policy_layer out = current;
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

	void agent_policy::reconcile_project()
	{
		m_loosenings.clear();
		m_pending = false;
		const std::vector<const cmd::command_desc*> known = commands();
		m_reconciled_commands = known.size();
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

	std::optional<std::string> agent_policy::approve_project_layer(const policy_layer& reviewed)
	{
		ASSERT_MSG(std::this_thread::get_id() == m_main_thread, "agent_policy::approve_project_layer: main thread only");
		if (!m_approval_allowed) return "no person is here to approve";
		const std::string shown = serialize_policy_layer(reviewed);
		refresh();
		if (!m_project.error.empty()) return "the project's agent_policy.json cannot be used: " + m_project.error;
		if (serialize_policy_layer(m_project.layer) != shown) return "the project's agent_policy.json changed since it was shown; review it again";
		if (const std::optional<std::string> error = keep_approved(m_project.layer)) return error;
		reconcile_project();
		if (m_on_changed) m_on_changed();
		return std::nullopt;
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
				policy_parse_result parsed = parse_policy_layer(*text);
				what.layer = std::move(parsed.layer);
				what.error = std::move(parsed.error);
			}
		}
		if (!what.error.empty()) {
			egLOG_WARN("editor/agent_policy", "{}: {}", fs_paths::utf8_of(what.file), what.error);
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
		// A command registered after the file was judged (a module loaded later) may carry a risk floor an entry lifts.
		if (m_commands && m_project.loaded && m_commands().size() != m_reconciled_commands) reconcile_project();
		const bool live_on = m_live_on && m_live_on();
		effective_policy now = effective_mode_of_call(command, category, m_project.layer, m_personal.layer, m_project.error, m_personal.error, live_on);
		if (!m_pending) return now;
		const effective_policy held = effective_mode_of_call(command, category, m_approved, m_personal.layer, {}, m_personal.error, live_on);
		if (held.mode <= now.mode) return now;
		return { held.mode, policy_source::approval_pending,
			"the project's agent_policy.json changed and waits for a person's approval; until then the approved version applies: " + held.reason };
	}

	std::optional<std::string> agent_policy::save(source& what, const policy_layer& layer, const prj::text_writer& write, bool approve)
	{
		ASSERT_MSG(std::this_thread::get_id() == m_main_thread, "agent_policy::save: main thread only");
		const std::string text = serialize_policy_layer(layer);
		std::error_code ec;
		if (!what.file.has_parent_path()) return "no place to write the policy";
		std::filesystem::create_directories(what.file.parent_path(), ec);
		const std::optional<std::string> error = write ? write(what.file, text) : prj::write_text_atomically(what.file, text);
		if (error) return error;
		// A person's own edit is the approval of it.
		if (approve) {
			if (const std::optional<std::string> kept = keep_approved(layer)) {
				reload(what);
				return "saved, but the approval could not be kept: " + *kept;
			}
		}
		reload(what);
		return what.error.empty() ? std::nullopt : std::optional<std::string>{ what.error };
	}

	std::optional<std::string> agent_policy::save_project(const policy_layer& layer, const prj::text_writer& write)
	{
		return save(m_project, layer, write, true);
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
