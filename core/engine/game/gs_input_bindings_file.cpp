#include <snk/game/gs_input_bindings_file.h>
#include <snk/input/inp_input_stack.h>
#include <snk/common/logger/engine_log.h>
#include <snk/common/fs_paths.h>
#include <snk/project/prj_json_text.h>
#include <snk/desc/desc_system.h>
#include <snk/platform/plt_beneath.h>
#include <boost/json.hpp>
#include <chrono>
#include <cstdint>
#include <format>
#include <algorithm>
#include <system_error>
#include <type_traits>
#include <variant>

namespace json = boost::json;

namespace
{
	constexpr std::string_view k_type = "user_input_bindings";
	// The format write_user_bindings writes. A change of it raises this with a migration step registered for k_type.
	constexpr std::int64_t k_version = 1;
	// The file is the player's, but a profile can be shared or damaged: what it may hold is bounded.
	constexpr std::uint64_t k_max_file_bytes = 256u * 1024u;
	constexpr std::size_t k_max_actions = 256;
	constexpr std::size_t k_max_bindings_per_action = 16;

	// <name>.broken-<UTC time> next to it, -2, -3... when one of that second is there; the way save_user_bindings
	// writes it (prj::write_project_file): its folder opened without following a link, and a file already at the new
	// name is never replaced. False, with a warning, when the file could not be moved: it is then kept as it is.
	bool move_aside(const std::filesystem::path& file)
	{
		const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
		const prj::project_file_location where = prj::project_file_location_of(file);
		const std::filesystem::path stem = where.relative.filename().native()
			+ fs_paths::path_from_utf8(std::format(".broken-{:%Y%m%dT%H%M%SZ}", now)).native();
		platform::beneath::pinned_root root{ where.root };
		platform::beneath::rename_request request;
		request.from_folder = where.relative.parent_path();
		request.from_leaf = where.relative.filename();
		request.to_folder = request.from_folder;
		std::string error;
		for (int attempt = 1; attempt <= 100; ++attempt) {
			request.to_leaf = stem;
			if (attempt > 1) request.to_leaf += fs_paths::path_from_utf8(std::format("-{}", attempt));
			std::error_code ec;
			if (std::filesystem::exists(std::filesystem::symlink_status(file.parent_path() / request.to_leaf, ec))) continue;
			error = platform::beneath::rename_beneath(root, request);
			if (!error.empty()) break;
			egLOG_WARN("input", "{} could not be read: moved aside to {}, the next save writes a new file", fs_paths::utf8_of(file),
				fs_paths::utf8_of(file.parent_path() / request.to_leaf));
			return true;
		}
		egLOG_WARN("input", "{} could not be read and could not be moved aside{}{}; it is not written over", fs_paths::utf8_of(file),
			error.empty() ? "" : ": ", error);
		return false;
	}

	std::string_view display(std::string_view source)
	{
		return source.empty() ? std::string_view{ "user input bindings" } : source;
	}

	// The optional "threshold" of a gamepad binding: a number in 0..1, else 0 (the device's) with a warning.
	float threshold_of(const json::object& object, std::string_view where)
	{
		const auto* value = object.if_contains("threshold");
		if (!value) return 0.0f;
		const double number = value->is_number() ? value->to_number<double>() : -1.0;
		if (!(number >= 0.0 && number <= 1.0)) {
			egLOG_WARN("input", "{}: /threshold: not a number in 0..1, the device's stays", where);
			return 0.0f;
		}
		return static_cast<float>(number);
	}

	void warn_if_threshold(const json::object& object, std::string_view where)
	{
		if (object.contains("threshold")) {
			egLOG_WARN("input", "{}: /threshold: ignored for a key or a mouse button", where);
		}
	}

	// The float as its shortest text read back as a double (0.35f writes 0.35, not 0.3499999940395355). The default parse of
	// Boost.JSON is fast, not exact: 0.35 may come out one step off, so the exact one is asked for.
	json::value threshold_json(float value)
	{
		json::error_code ec;
		json::parse_options options;
		options.numbers = json::number_precision::precise;
		json::value number = json::parse(std::format("{}", value), ec, {}, options);
		return ec ? json::value(static_cast<double>(value)) : number;
	}

	void parse_binding(const json::value& item, std::vector<inp::action_binding>& out, std::string_view where)
	{
		const json::object* object = item.if_object();
		if (!object) {
			egLOG_WARN("input", "{}: not an object, skipped", where);
			return;
		}
		constexpr std::string_view kinds[] = { "key", "mouse", "gamepad", "axis" };
		std::string_view kind;
		std::string name;
		for (const std::string_view candidate : kinds) {
			const auto* value = object->if_contains(candidate);
			if (!value) continue;
			if (!value->is_string() || !kind.empty()) {
				egLOG_WARN("input", "{}: a binding names exactly one of \"key\", \"mouse\", \"gamepad\" and \"axis\" as a string, skipped", where);
				return;
			}
			kind = candidate;
			name = std::string{ value->as_string() };
		}
		if (kind.empty() || name.empty()) {
			egLOG_WARN("input", "{}: a binding names exactly one of \"key\", \"mouse\", \"gamepad\" and \"axis\" as a string, skipped", where);
			return;
		}
		if (kind == "key") {
			if (const auto parsed = inp::key_from_name(name)) {
				out.emplace_back(*parsed);
				warn_if_threshold(*object, where);
			} else {
				egLOG_WARN("input", "{}: unknown key '{}', skipped", where, name);
			}
		} else if (kind == "mouse") {
			if (const auto parsed = inp::mouse_button_from_name(name)) {
				out.emplace_back(*parsed);
				warn_if_threshold(*object, where);
			} else {
				egLOG_WARN("input", "{}: unknown mouse button '{}', skipped", where, name);
			}
		} else if (kind == "gamepad") {
			if (const auto parsed = inp::gamepad_button_from_name(name)) out.emplace_back(*parsed, threshold_of(*object, where));
			else egLOG_WARN("input", "{}: unknown gamepad button '{}', skipped", where, name);
		} else if (const auto parsed = inp::gamepad_axis_from_name(name)) {
			out.emplace_back(*parsed, threshold_of(*object, where));
		} else {
			egLOG_WARN("input", "{}: unknown gamepad axis '{}', skipped", where, name);
		}
	}
}

inp::user_bindings gs::parse_user_bindings(std::string_view text, std::string_view source)
{
	user_file_state state = user_file_state::usable;
	return parse_user_bindings(text, source, nullptr, state);
}

inp::user_bindings gs::parse_user_bindings(std::string_view text, std::string_view source, const desc::desc_system* descs,
	user_file_state& state)
{
	inp::user_bindings result;
	state = user_file_state::usable;
	const std::string_view file = display(source);
	boost::system::error_code error;
	json::value root = json::parse(text, error);
	json::object* document = error ? nullptr : root.if_object();
	const json::value* type = document ? document->if_contains("__type") : nullptr;
	if (!document || !type || !type->is_string() || type->as_string() != k_type) {
		egLOG_WARN("input", "{}: not a {} document, the bindings of the project stay", file, k_type);
		state = user_file_state::unreadable;
		return result;
	}
	const std::int64_t supported = descs ? descs->current_version(k_type) : k_version;
	if (const json::value* version = document->if_contains("__version"); version && version->is_int64() && version->as_int64() > supported) {
		egLOG_WARN("input", "{}: '{}' is version {}, newer than the version {} this engine reads; the bindings of the project stay "
			"and the file is not written over", file, k_type, version->as_int64(), supported);
		state = user_file_state::newer;
		return result;
	}
	std::string problem;
	if (descs) {
		problem = descs->upgrade_in_memory(*document, file);
	} else if (const json::value* version = document->if_contains("__version"); version && !(version->is_int64() && version->as_int64() == k_version)) {
		problem = std::format("{}: '{}' is version {}, not the version {} this engine reads", file, k_type, json::serialize(*version), k_version);
	}
	if (!problem.empty()) {
		egLOG_WARN("input", "{}; the bindings of the project stay", problem);
		state = user_file_state::unreadable;
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
			if (const auto key = binding.key()) b["key"] = json::string(inp::key_code_name(*key));
			else if (const auto button = binding.mouse_button()) b["mouse"] = json::string(inp::mouse_button_name(*button));
			else if (const auto pad = binding.gamepad_button()) b["gamepad"] = json::string(inp::gamepad_button_name(*pad));
			else if (const auto axis = binding.gamepad_axis()) b["axis"] = json::string(inp::gamepad_axis_name(*axis));
			if (binding.threshold() > 0.0f) b["threshold"] = threshold_json(binding.threshold());
			out.push_back(std::move(b));
		}
		json::object a;
		a["name"] = json::string(name);
		a["bindings"] = std::move(out);
		actions.push_back(std::move(a));
	}
	json::object root;
	root["__type"] = json::string(k_type);
	root["__version"] = k_version;
	root["actions"] = std::move(actions);
	return json::serialize(root);
}

inp::user_bindings gs::load_user_bindings(const std::filesystem::path& file)
{
	const prj::project_file_location where = prj::project_file_location_of(file);
	const prj::project_file_text found = prj::read_project_file(where.root, where.relative, k_max_file_bytes);
	if (!found.error.empty()) {
		egLOG_WARN("input", "cannot read {}: {}", fs_paths::utf8_of(file), found.error);
		return {};
	}
	return found.exists ? parse_user_bindings(found.text, fs_paths::utf8_of(file)) : inp::user_bindings{};
}

inp::user_bindings gs::load_user_bindings(const std::filesystem::path& file, const desc::desc_system* descs, bool& keep_file)
{
	keep_file = false;
	const std::string name = fs_paths::utf8_of(file);
	const prj::project_file_location where = prj::project_file_location_of(file);
	const prj::project_file_text found = prj::read_project_file(where.root, where.relative, k_max_file_bytes);
	if (!found.error.empty()) {
		// Too big for any bindings document, a newer one too; kept, it would stop the bindings from ever being saved.
		std::error_code ec;
		const bool regular = std::filesystem::is_regular_file(std::filesystem::symlink_status(file, ec));
		if (regular && !ec && std::filesystem::file_size(file, ec) > k_max_file_bytes && !ec) {
			egLOG_WARN("input", "{} is over {} bytes, no bindings document", name, k_max_file_bytes);
			keep_file = !move_aside(file);
			return {};
		}
		egLOG_WARN("input", "cannot read {}: {}; it is not written over", name, found.error);
		keep_file = true;
		return {};
	}
	if (!found.exists) return {};
	user_file_state state = user_file_state::usable;
	inp::user_bindings bindings = parse_user_bindings(found.text, name, descs, state);
	if (state == user_file_state::newer) {
		keep_file = true;
	} else if (state == user_file_state::unreadable) {
		keep_file = !move_aside(file);
	}
	return bindings;
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

std::string gs::user_bindings_version_problem(const desc::desc_system& descs)
{
	const int steps = descs.current_version(k_type);
	if (steps == k_version) return {};
	return std::format("'{}' is written at version {}, but its migration steps lead to version {}: raise the version with its step",
		k_type, k_version, steps);
}
