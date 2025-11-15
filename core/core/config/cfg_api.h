#pragma once
#include <filesystem>

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
