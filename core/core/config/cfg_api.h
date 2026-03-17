#pragma once
#include <filesystem>
#include <unordered_map>
#include <string>

namespace cfg {

	void initialize_configs(std::filesystem::path init_file);
	bool initialize_configs(int argc, char* args[]);

	struct variable_base
	{
		virtual ~variable_base() = default;
		virtual void update(const std::filesystem::path& location, std::string_view val) = 0;
	};

	void registrate_variable(std::string_view name, variable_base* var);
	void unregistrate_variable(std::string_view name, variable_base* var);

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
				value_ = location / val;
			} else {
				value_ = T{ val };
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
	void registrate_prefix(std::string_view prefix, variable_map* var);
	void unregistrate_prefix(std::string_view prefix, variable_map* var);

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
#define CFG_VAR_EXT_INT(var) extern cfg::variable<int> var
#define CFG_VAR_EXT_FLOAT(var) extern cfg::variable<float> var
#define CFG_VAR_EXT_BOOL(var) extern cfg::variable<bool> var
#define CFG_VAR_EXT_STRING(var) extern cfg::variable<std::string> var
#define CFG_VAR_EXT_PATH(var) extern cfg::variable<std::filesystem::path> var
