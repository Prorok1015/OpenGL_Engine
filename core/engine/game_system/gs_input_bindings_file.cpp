#include <snk/game/gs_input_bindings_file.h>
#include <snk/input/inp_input_stack.h>
#include <snk/common/logger/engine_log.h>
#include <snk/common/fs_paths.h>
#include <snk/project/prj_json_text.h>
#include <boost/json.hpp>
#include <format>
#include <algorithm>
#include <system_error>
#include <variant>

namespace json = boost::json;

namespace
{
	constexpr std::string_view k_type = "user_input_bindings";
	// The file is the player's, but a profile can be shared or damaged: what it may hold is bounded.
	constexpr std::uint64_t k_max_file_bytes = 256u * 1024u;
	constexpr std::size_t k_max_actions = 256;
	constexpr std::size_t k_max_bindings_per_action = 16;

	std::string string_field(const json::object& object, std::string_view key, bool& not_text)
	{
		const auto* value = object.if_contains(key);
		if (!value) return {};
		if (!value->is_string()) {
			not_text = true;
			return {};
		}
		return std::string{ value->as_string() };
	}

	std::string_view display(std::string_view source)
	{
		return source.empty() ? std::string_view{ "user input bindings" } : source;
	}

	void parse_binding(const json::value& item, std::vector<inp::action_binding>& out, std::string_view where)
	{
		const json::object* object = item.if_object();
		if (!object) {
			egLOG_WARN("input", "{}: not an object, skipped", where);
			return;
		}
		bool not_text = false;
		const std::string key = string_field(*object, "key", not_text);
		const std::string mouse = string_field(*object, "mouse", not_text);
		if (not_text || key.empty() == mouse.empty()) {
			egLOG_WARN("input", "{}: a binding names exactly one of \"key\" and \"mouse\" as a string, skipped", where);
			return;
		}
		if (!key.empty()) {
			if (const auto parsed = inp::key_from_name(key)) out.emplace_back(*parsed);
			else egLOG_WARN("input", "{}: unknown key '{}', skipped", where, key);
		} else if (const auto parsed = inp::mouse_button_from_name(mouse)) {
			out.emplace_back(*parsed);
		} else {
			egLOG_WARN("input", "{}: unknown mouse button '{}', skipped", where, mouse);
		}
	}
}

inp::user_bindings gs::parse_user_bindings(std::string_view text, std::string_view source)
{
	inp::user_bindings result;
	const std::string_view file = display(source);
	boost::system::error_code error;
	const json::value root = json::parse(text, error);
	const json::object* document = error ? nullptr : root.if_object();
	const json::value* type = document ? document->if_contains("__type") : nullptr;
	if (!document || !type || !type->is_string() || type->as_string() != k_type) {
		egLOG_WARN("input", "{}: not a {} document, the bindings of the project stay", file, k_type);
		return result;
	}
	const json::value* actions = document->if_contains("actions");
	if (!actions || !actions->is_array()) return result;

	if (actions->as_array().size() > k_max_actions) {
		egLOG_WARN("input", "{}: /actions: {} actions, only the first {} are read", file, actions->as_array().size(), k_max_actions);
	}
	for (std::size_t i = 0; i < std::min(actions->as_array().size(), k_max_actions); ++i) {
		const json::object* entry = actions->as_array()[i].if_object();
		const json::value* name = entry ? entry->if_contains("name") : nullptr;
		const json::value* bindings = entry ? entry->if_contains("bindings") : nullptr;
		if (!name || !name->is_string() || name->as_string().empty() || !bindings || !bindings->is_array()) {
			egLOG_WARN("input", "{}: /actions/{}: needs a name and a bindings array, skipped", file, i);
			continue;
		}
		std::vector<inp::action_binding> parsed;
		for (std::size_t j = 0; j < std::min(bindings->as_array().size(), k_max_bindings_per_action); ++j) {
			parse_binding(bindings->as_array()[j], parsed, std::format("{}: /actions/{}/bindings/{}", file, i, j));
		}
		result.set(std::string{ name->as_string() }, std::move(parsed));
	}
	return result;
}

std::string gs::write_user_bindings(const inp::user_bindings& bindings)
{
	json::array actions;
	for (const auto& [name, list] : bindings.overrides()) {
		json::array out;
		for (const inp::action_binding& binding : list) {
			json::object b;
			if (const auto* key = std::get_if<inp::KEYBOARD_BUTTONS>(&binding)) b["key"] = json::string(inp::key_code_name(*key));
			else b["mouse"] = json::string(inp::mouse_button_name(std::get<inp::MOUSE_BUTTONS>(binding)));
			out.push_back(std::move(b));
		}
		json::object a;
		a["name"] = json::string(name);
		a["bindings"] = std::move(out);
		actions.push_back(std::move(a));
	}
	json::object root;
	root["__type"] = json::string(k_type);
	root["__version"] = 1;
	root["actions"] = std::move(actions);
	return json::serialize(root);
}

inp::user_bindings gs::load_user_bindings(const std::filesystem::path& file)
{
	const std::string name = fs_paths::utf8_of(file);
	const prj::project_file_location where = prj::project_file_location_of(file);
	const prj::project_file_text found = prj::read_project_file(where.root, where.relative, k_max_file_bytes);
	if (!found.error.empty()) {
		egLOG_WARN("input", "cannot read {}: {}", name, found.error);
		return {};
	}
	if (!found.exists) return {};
	return parse_user_bindings(found.text, name);
}

bool gs::save_user_bindings(const std::filesystem::path& file, const inp::user_bindings& bindings)
{
	std::error_code ec;
	if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
	const prj::project_file_location where = prj::project_file_location_of(file);
	if (const auto error = prj::write_project_file(where.root, where.relative, write_user_bindings(bindings))) {
		egLOG_WARN("input", "cannot write {}: {}", fs_paths::utf8_of(file), *error);
		return false;
	}
	return true;
}
