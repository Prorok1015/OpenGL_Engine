#include "cfg_api.h"
#include <unordered_map>
#include <variant>
#include <yaml-cpp/yaml.h>
#include <boost/program_options.hpp>
#include <iostream>

namespace po = boost::program_options;
using config_types = std::variant<int, float, bool, std::string>;

bool is_initialized = false;

constexpr auto EXTENDS_KEY = "extends";
constexpr auto SEPARATOR = '/';

namespace cfg {
	struct tag
	{
		tag(std::string_view path)
			: id(std::hash<std::string_view>{}(path)) {
		}

		auto operator<=>(const tag&) const = default;

		std::size_t id;
	};

	struct config_manager
	{
		static config_manager& instance()
		{
			static config_manager instance;
			return instance;
		}

		std::unordered_map<std::string, cfg::variable_base*> registered_variables;
		std::unordered_map<std::string, cfg::variable_map*>  registered_prefixes;
	};

} // namespace cfg

namespace std {
	template <>
	struct hash<cfg::tag>
	{
		std::size_t operator()(const cfg::tag& k) const
		{
			return k.id;
		}
	};
} // namespace std

namespace {
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
			auto var_it = mgr.registered_variables.find(current);
			if (var_it != mgr.registered_variables.end()) {
				var_it->second->update(config_location, val_str);
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
						pfx_it->second->update_entry(suffix, val_str);
						break;
					}
					path = prefix;
				}
			}
		} else if (value.IsMap()) {
			load_map(config_location, current, value);
		}
	}
}

void load_config_file(std::filesystem::path file_path)
{
	YAML::Node config = YAML::LoadFile(std::filesystem::absolute(file_path).string());
	if (config[EXTENDS_KEY]) {
		// TODO: add assert if its not a sequence
		for (auto it = config[EXTENDS_KEY].begin(); it != config[EXTENDS_KEY].end(); ++it) {
			auto file = file_path.parent_path() / it->as<std::string>();
			load_config_file(file);
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

	load_config_file(init_file);
}

bool cfg::initialize_configs(int argc, char* argv[])
{
	po::options_description desc("Allowed options");
	desc.add_options()
		("help,c", "call list of variables")
		("config,c", po::value<std::string>(), "set a path to a config entry")
		("set,s", po::value<std::vector<std::string>>()->multitoken()->composing(), "set a config var");
	
	po::variables_map vm;
	po::store(po::parse_command_line(argc, argv, desc), vm);
	po::notify(vm);

	if (vm.count("help")) {
		std::cout << desc << "\n";
		std::cout << "Config variables:\n";
		for (const auto& [name, var] : cfg::config_manager::instance().registered_variables) {
			std::cout << "--set " << name << "\n";
		}
		return false;
	}

	if (vm.count("config")) {
		std::string config_path = vm["config"].as<std::string>();
		cfg::initialize_configs(config_path);
	}

	if (vm.count("set")) {
		auto& tokens = vm["set"].as<std::vector<std::string>>();
		for (int i = 1; i < tokens.size(); i += 2) {
			std::string name = tokens[i - 1];
			std::string val = tokens[i];
			auto var_it = cfg::config_manager::instance().registered_variables.find(name);
			if (var_it != cfg::config_manager::instance().registered_variables.end()) {
				var_it->second->update(std::filesystem::current_path(), val);
			}
		}
	}

	return true;
}

void cfg::registrate_variable(std::string_view name, variable_base* var)
{
	cfg::config_manager::instance().registered_variables[std::string{ name }] = var;
}

void cfg::unregistrate_variable(std::string_view name, variable_base* var)
{
	cfg::config_manager::instance().registered_variables.erase(std::string{ name });
}

void cfg::registrate_prefix(std::string_view prefix, variable_map* var)
{
	cfg::config_manager::instance().registered_prefixes[std::string{ prefix }] = var;
}

void cfg::unregistrate_prefix(std::string_view prefix, variable_map* var)
{
	cfg::config_manager::instance().registered_prefixes.erase(std::string{ prefix });
}
