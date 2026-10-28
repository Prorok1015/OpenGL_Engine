#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <type_traits>
#include <optional>
#include <unordered_map>
#include <vector>
#include <snk/common/snk_api.h>
#include <snk/config/cfg_api.h>

// Base only (include_base/, not installed in the SDK): defining a variable registers it in the process-wide
// configuration, and its value can be written. A game module reads through cfg::config_view (MODULE_LAYER.md).

namespace cfg {

	SNK_API void initialize_configs(std::filesystem::path init_file);
	SNK_API bool initialize_configs(int argc, char* args[]);
	// The absolute path of the file initialize_configs loaded (--config); empty when none was. The editor passes it on
	// when it restarts itself into another project (EPIC-42 #158).
	SNK_API std::filesystem::path loaded_config_path();

	// Loads a config file with its `extends` chain (a later file replaces what an earlier one set, lists included),
	// without the once-per-process guard of initialize_configs. False when a file is missing or is not valid yaml.
	SNK_API bool load_config_file(const std::filesystem::path& file);

	// Loads yaml text the way a config file is loaded (without `extends`, and without the once-per-process guard of
	// initialize_configs). False when the text is not valid yaml. Paths are relative to `location`.
	SNK_API bool load_config_text(std::string_view yaml_text, const std::filesystem::path& location);

	// What `--set name value` does: a scalar variable parses `value`; a list variable takes `value` as a flow yaml
	// sequence ("[a, b]", "[]") or, without a leading '[', as a list of that one element. Always replaces. Yaml rules
	// apply inside the brackets: an element holding ',' is quoted ("[\"a,b\", c]"; "[a\\, b]" is the two elements `a\` and `b`),
	// and so is one that starts with '*'. Null (`~`), nested lists and maps are refused, the list is left as it was.
	SNK_API void set_variable(std::string_view name, std::string_view value);

	// A list as the JSON array text config_view::value() returns.
	SNK_API std::string format_list(const std::vector<std::string>& values);

	// Reports a value of the wrong kind (a scalar for a list variable or the other way round); the value is ignored.
	SNK_API void warn_wrong_kind(std::string_view name, std::string_view problem);

	struct variable_base
	{
		virtual ~variable_base() = default;
		virtual void update(const std::filesystem::path& location, std::string_view val) = 0;
		// The current value as text, what config_view reads.
		virtual std::string text() const = 0;
		// List variables only (variable<std::vector<std::string>>): the whole list is replaced.
		virtual bool is_list() const { return false; }
		virtual bool update_list(const std::vector<std::string>& /*values*/) { return false; }
		virtual std::optional<std::vector<std::string>> list() const { return std::nullopt; }
	};

	// Variables of the same name (a test standing in for a module's variable) are all updated; removing one leaves the
	// others registered.
	SNK_API void registrate_variable(std::string_view name, variable_base* var);
	SNK_API void unregistrate_variable(std::string_view name, variable_base* var);

	template<class T>
	struct variable : public variable_base
	{
		variable(std::string_view name, T default_value)
		: name_(name), value_(default_value) {
			registrate_variable(name, this);
		}

		virtual ~variable() override {
			unregistrate_variable(name_, this);
		}

		virtual void update(const std::filesystem::path& location, std::string_view val) override {
			if constexpr (std::is_same_v<T, int>) {
				value_ = std::stoi(std::string(val));
			} else if constexpr (std::is_same_v<T, float>) {
				value_ = std::stof(std::string(val));
			} else if constexpr (std::is_same_v<T, bool>) {
				value_ = (val == "true" || val == "1");
			} else if constexpr (std::is_same_v<T, std::string>) {
				value_ = std::string(val);
			} else if constexpr (std::is_same_v<T, std::filesystem::path>) {
				// An empty value is an empty path, not the directory of the config file.
				value_ = val.empty() ? std::filesystem::path{} : location / val;
			} else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
				warn_wrong_kind(name_, "the variable is a list: write a yaml sequence (--set takes \"[a, b]\")");
			} else {
				value_ = T{ val };
			}
		}
		
		std::string text() const override {
			if constexpr (std::is_same_v<T, bool>) {
				return value_ ? "true" : "false";
			} else if constexpr (std::is_arithmetic_v<T>) {
				return std::to_string(value_);
			} else if constexpr (std::is_same_v<T, std::filesystem::path>) {
				return value_.string();
			} else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
				return format_list(value_);
			} else if constexpr (std::is_convertible_v<const T&, std::string>) {
				return std::string(value_);
			} else {
				return {};
			}
		}

		bool is_list() const override { return std::is_same_v<T, std::vector<std::string>>; }

		bool update_list(const std::vector<std::string>& values) override {
			if constexpr (std::is_same_v<T, std::vector<std::string>>) {
				value_ = values;
				return true;
			} else {
				(void)values;
				return false;
			}
		}

		std::optional<std::vector<std::string>> list() const override {
			if constexpr (std::is_same_v<T, std::vector<std::string>>) {
				return value_;
			} else {
				return std::nullopt;
			}
		}

		T& operator*() {
			return value_;
		}

		T* operator->() {
			return &value_;
		}

		operator T() const {
			return value_;
		}

	private:
		const std::string name_;
		T value_;// TODO: atomic?
	};

	// Forward declarations for variable_map
	struct variable_map;
	SNK_API void registrate_prefix(std::string_view prefix, variable_map* var);
	SNK_API void unregistrate_prefix(std::string_view prefix, variable_map* var);

	// A map variable that captures all sub-keys under a given prefix.
	// E.g. prefix "logger/file" captures "logger/file/edt" → entries["edt"] = value.
	struct variable_map : public variable_base
	{
		variable_map(std::string_view prefix)
			: prefix_(prefix)
		{
			registrate_prefix(prefix, this);
		}

		virtual ~variable_map() override
		{
			unregistrate_prefix(prefix_, this);
		}

		// Called by the config parser with the full key path.
		// Extracts the suffix after the prefix separator.
		virtual void update(const std::filesystem::path& /*location*/, std::string_view /*val*/) override {}
		std::string text() const override { return {}; }

		void update_entry(std::string_view suffix, std::string_view val)
		{
			entries_[std::string(suffix)] = std::string(val);
		}

		const std::unordered_map<std::string, std::string>& entries() const { return entries_; }

	private:
		const std::string prefix_;
		std::unordered_map<std::string, std::string> entries_;
	};

} // namespace cfg


#define CFG_VAR_DEF_INT(var, path, default_val) cfg::variable<int> var{path, default_val}
#define CFG_VAR_DEF_FLOAT(var, path, default_val) cfg::variable<float> var{path, default_val}
#define CFG_VAR_DEF_BOOL(var, path, default_val) cfg::variable<bool> var{path, default_val}
#define CFG_VAR_DEF_STRING(var, path, default_val) cfg::variable<std::string> var{path, default_val}
#define CFG_VAR_DEF_PATH(var, name, default_val) cfg::variable<std::filesystem::path> var{name, default_val}
// The default is the list of the remaining arguments: CFG_VAR_DEF_STRING_LIST(v, "a/b", "x", "y") or without elements.
#define CFG_VAR_DEF_STRING_LIST(var, name, ...) cfg::variable<std::vector<std::string>> var{name, std::vector<std::string>{__VA_ARGS__}}
#define CFG_VAR_EXT_INT(var) extern cfg::variable<int> var
#define CFG_VAR_EXT_FLOAT(var) extern cfg::variable<float> var
#define CFG_VAR_EXT_BOOL(var) extern cfg::variable<bool> var
#define CFG_VAR_EXT_STRING(var) extern cfg::variable<std::string> var
#define CFG_VAR_EXT_PATH(var) extern cfg::variable<std::filesystem::path> var
#define CFG_VAR_EXT_STRING_LIST(var) extern cfg::variable<std::vector<std::string>> var
