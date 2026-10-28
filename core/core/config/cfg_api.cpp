#include <snk/config/cfg_variables.h>
#include <snk/common/logger/engine_log.h>
#include <algorithm>
#include <unordered_map>
#include <optional>
#include <string>
#include <variant>
#include <vector>
#include <yaml-cpp/yaml.h>
#include <boost/program_options.hpp>
#include <cstdio>
#include <sstream>

namespace po = boost::program_options;

bool is_initialized = false;
std::filesystem::path loaded_config_file;

constexpr auto EXTENDS_KEY = "extends";
constexpr auto SEPARATOR = '/';

namespace cfg {
	struct config_manager
	{
		static config_manager& instance()
		{
			static config_manager instance;
			return instance;
		}

		// Several variables may share a name: every one is updated, and each one leaves on its own.
		std::unordered_map<std::string, std::vector<cfg::variable_base*>> registered_variables;
		std::unordered_map<std::string, std::vector<cfg::variable_map*>>  registered_prefixes;

		void update_variable(std::string_view name, const std::filesystem::path& location, std::string_view value)
		{
			if (auto it = registered_variables.find(std::string{ name }); it != registered_variables.end()) {
				for (auto* var : it->second) {
					var->update(location, value);
				}
			}
		}

		// Every variable of the name takes the list; one that is not a list reports it and keeps its value.
		void update_list(std::string_view name, const std::vector<std::string>& values)
		{
			if (auto it = registered_variables.find(std::string{ name }); it != registered_variables.end()) {
				for (auto* var : it->second) {
					if (!var->update_list(values)) {
						cfg::warn_wrong_kind(name, "a list was given but the variable is not a list");
					}
				}
			}
		}
	};

	template<class T>
	void remove_registration(std::unordered_map<std::string, std::vector<T*>>& map, std::string_view name, T* var)
	{
		const auto it = map.find(std::string{ name });
		if (it == map.end()) {
			return;
		}
		std::erase(it->second, var);
		if (it->second.empty()) {
			map.erase(it);
		}
	}

} // namespace cfg

namespace {

// Empty when `sequence` is a flat list of scalars (copied to `values`); otherwise why it is not one.
std::string read_flat_list(const YAML::Node& sequence, std::vector<std::string>& values)
{
	for (const auto& element : sequence) {
		if (element.IsNull()) {
			return "null elements are not allowed in a list";
		}
		if (!element.IsScalar()) {
			return "nested lists and maps are not allowed in a list";
		}
		values.push_back(element.as<std::string>());
	}
	return {};
}

void load_map(const std::filesystem::path& config_location, const std::string& parent, YAML::Node map_node)
{
	for (auto it = map_node.begin(); it != map_node.end(); ++it) {
		std::string current = it->first.as<std::string>();
		if (!parent.empty()) {
			current = parent + SEPARATOR + current;
		}

		YAML::Node value = it->second;
		if (value.IsScalar()) {
			std::string val_str = value.as<std::string>();
			auto& mgr = cfg::config_manager::instance();
			if (mgr.registered_variables.contains(current)) {
				mgr.update_variable(current, config_location, val_str);
			} else {
				// Check prefix variables by walking up the path.
				// "a/b/c/d" → try "a/b/c" (suffix "d"), then "a/b" (suffix "c/d"), then "a" (suffix "b/c/d").
				// The suffix preserves the '/' separators so categories like "editor/asset/import" work.
				std::string path = current;
				while (true) {
					auto sep = path.rfind(SEPARATOR);
					if (sep == std::string::npos) break;
					std::string prefix = path.substr(0, sep);
					std::string suffix = current.substr(sep + 1);
					auto pfx_it = mgr.registered_prefixes.find(prefix);
					if (pfx_it != mgr.registered_prefixes.end()) {
						for (auto* map : pfx_it->second) {
							map->update_entry(suffix, val_str);
						}
						break;
					}
					path = prefix;
				}
			}
		} else if (value.IsSequence()) {
			std::vector<std::string> values;
			if (const std::string problem = read_flat_list(value, values); !problem.empty()) {
				cfg::warn_wrong_kind(current, problem);
			} else if (cfg::config_manager::instance().registered_variables.contains(current)) {
				cfg::config_manager::instance().update_list(current, values);
			}
		} else if (value.IsMap()) {
			load_map(config_location, current, value);
		}
	}
}

void load_config_file_with_extends(std::filesystem::path file_path)
{
	YAML::Node config = YAML::LoadFile(std::filesystem::absolute(file_path).string());
	if (config[EXTENDS_KEY]) {
		// TODO: add assert if its not a sequence
		for (auto it = config[EXTENDS_KEY].begin(); it != config[EXTENDS_KEY].end(); ++it) {
			auto file = file_path.parent_path() / it->as<std::string>();
			load_config_file_with_extends(file);
		}
	}
	
	config.remove(EXTENDS_KEY);
	load_map(file_path.parent_path(), "", config);
}
}

void cfg::initialize_configs(std::filesystem::path init_file)
{
	if (is_initialized) {
		return;
	}
	is_initialized = true;

	loaded_config_file = std::filesystem::absolute(init_file).lexically_normal();
	load_config_file_with_extends(init_file);
}

std::filesystem::path cfg::loaded_config_path()
{
	return loaded_config_file;
}

bool cfg::initialize_configs(int argc, char* argv[])
{
	po::options_description desc("Allowed options");
	desc.add_options()
		("help,c", "call list of variables")
		("config,c", po::value<std::string>(), "set a path to a config entry")
		("set,s", po::value<std::vector<std::string>>()->multitoken()->composing(), "set a config var")
		("project", po::value<std::string>(), "open a game project: its project.snkproj or the directory holding it")
		("allow-engine-version-mismatch", "open a project made for an older engine version without asking")
		("non-interactive,n", "do not wait for answers, take the safe one (this is not --yes). Today it covers the "
			"engine version check: a project made for another engine version is refused with exit code 3 instead of "
			"asking; an older one still opens with --allow-engine-version-mismatch");
	
	po::variables_map vm;
	po::store(po::parse_command_line(argc, argv, desc), vm);
	po::notify(vm);

	if (vm.count("help")) {
		std::ostringstream help_text;
		help_text << desc << "\nConfig variables:\n";
		for (const auto& [name, var] : cfg::config_manager::instance().registered_variables) {
			help_text << "--set " << name << "\n";
		}
		std::fputs(help_text.str().c_str(), stdout);
		return false;
	}

	if (vm.count("config")) {
		std::string config_path = vm["config"].as<std::string>();
		cfg::initialize_configs(config_path);
	}

	// Before --set, so that `--set project/...` still wins like every other --set.
	const auto set_registered = [](std::string_view name, std::string_view value) {
		cfg::config_manager::instance().update_variable(name, std::filesystem::current_path(), value);
	};
	if (vm.count("project")) {
		set_registered("project/file", vm["project"].as<std::string>());
	}
	if (vm.count("allow-engine-version-mismatch")) {
		set_registered("project/allow_engine_version_mismatch", "true");
	}
	if (vm.count("non-interactive")) {
		set_registered("app/non_interactive", "true");
	}

	if (vm.count("set")) {
		auto& tokens = vm["set"].as<std::vector<std::string>>();
		for (std::size_t i = 1; i < tokens.size(); i += 2) {
			cfg::set_variable(tokens[i - 1], tokens[i]);
		}
	}

	return true;
}

bool cfg::load_config_file(const std::filesystem::path& file)
{
	try {
		load_config_file_with_extends(file);
		return true;
	} catch (const YAML::Exception& e) {
		egLOG_ERROR("config", "cannot load '{}': {}", file.string(), e.what());
		return false;
	}
}

bool cfg::load_config_text(std::string_view yaml_text, const std::filesystem::path& location)
{
	try {
		load_map(location, "", YAML::Load(std::string{ yaml_text }));
		return true;
	} catch (const YAML::Exception& e) {
		egLOG_ERROR("config", "invalid yaml: {}", e.what());
		return false;
	}
}

namespace {

// nullopt (after a warning) when `value` starts a flow sequence that is not a flat list of scalars.
std::optional<std::vector<std::string>> parse_list_argument(std::string_view name, std::string_view value)
{
	const auto first = value.find_first_not_of(" \t");
	if (first == std::string_view::npos || value[first] != '[') {
		return std::vector<std::string>{ std::string{ value } };
	}
	try {
		const YAML::Node node = YAML::Load(std::string{ value });
		if (node.IsSequence()) {
			std::vector<std::string> values;
			if (const std::string problem = read_flat_list(node, values); problem.empty()) {
				return values;
			} else {
				cfg::warn_wrong_kind(name, "--set: " + problem);
				return std::nullopt;
			}
		}
	} catch (const YAML::Exception&) {
	}
	cfg::warn_wrong_kind(name, "--set expects a flat list such as \"[a, b]\" (quote elements that start with '*' or hold ',')");
	return std::nullopt;
}

}

void cfg::set_variable(std::string_view name, std::string_view value)
{
	const auto& vars = cfg::config_manager::instance().registered_variables;
	const auto it = vars.find(std::string{ name });
	if (it == vars.end()) {
		return;
	}
	bool list_parsed = false;
	std::optional<std::vector<std::string>> list;
	for (auto* var : it->second) {
		if (!var->is_list()) {
			var->update(std::filesystem::current_path(), value);
			continue;
		}
		if (!list_parsed) {
			list = parse_list_argument(name, value);
			list_parsed = true;
		}
		if (list) {
			var->update_list(*list);
		}
	}
}

std::string cfg::format_list(const std::vector<std::string>& values)
{
	std::string out = "[";
	for (std::size_t i = 0; i < values.size(); ++i) {
		if (i != 0) {
			out += ',';
		}
		out += '"';
		for (const char ch : values[i]) {
			switch (ch) {
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (static_cast<unsigned char>(ch) < 0x20) {
					char escaped[8];
					std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(ch)));
					out += escaped;
				} else {
					out += ch;
				}
			}
		}
		out += '"';
	}
	out += ']';
	return out;
}

void cfg::warn_wrong_kind(std::string_view name, std::string_view problem)
{
	egLOG_WARN("config", "'{}': {}; the value is ignored", name, problem);
}

void cfg::registrate_variable(std::string_view name, variable_base* var)
{
	cfg::config_manager::instance().registered_variables[std::string{ name }].push_back(var);
}

void cfg::unregistrate_variable(std::string_view name, variable_base* var)
{
	cfg::remove_registration(cfg::config_manager::instance().registered_variables, name, var);
}

void cfg::registrate_prefix(std::string_view prefix, variable_map* var)
{
	cfg::config_manager::instance().registered_prefixes[std::string{ prefix }].push_back(var);
}

void cfg::unregistrate_prefix(std::string_view prefix, variable_map* var)
{
	cfg::remove_registration(cfg::config_manager::instance().registered_prefixes, prefix, var);
}

std::optional<std::vector<std::string>> cfg::config_view::list(std::string_view name) const
{
	const auto& vars = cfg::config_manager::instance().registered_variables;
	const auto it = vars.find(std::string{ name });
	if (it == vars.end()) {
		return std::nullopt;
	}
	for (const auto* var : it->second) {
		if (auto values = var->list()) {
			return values;
		}
	}
	return std::nullopt;
}

std::optional<std::string> cfg::config_view::value(std::string_view name) const
{
	const auto& vars = cfg::config_manager::instance().registered_variables;
	const auto it = vars.find(std::string{ name });
	if (it == vars.end() || it->second.empty()) {
		return std::nullopt;
	}
	return it->second.front()->text();
}
