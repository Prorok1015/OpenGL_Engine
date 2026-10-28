#pragma once
#include <snk/platform/plt_process.h>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace edt
{
	// platform::run_process, or a fake in tests.
	using process_runner = std::function<platform::process_result(const std::filesystem::path& program,
		const std::vector<std::string>& args, const platform::process_options& options,
		const std::function<void(std::string_view line)>& on_line, std::stop_token stop)>;

	// What builds a game module: CMake and the environment (PATH with the compiler and ninja) to run it in.
	struct toolchain
	{
		std::filesystem::path cmake;
		// "NAME=value" each: the whole environment of cmake.
		std::vector<std::string> environment;
		// For the log, e.g. "Visual Studio at C:\..., CMake C:\...\cmake.exe".
		std::string description;
	};

	// cfg build/cmake, build/vs_path, build/vs_min_version.
	struct toolchain_settings
	{
		// Empty: the one of Visual Studio (Windows), else "cmake" on PATH.
		std::filesystem::path cmake;
		// Empty: found with vswhere. Windows only.
		std::filesystem::path vs_path;
		std::string vs_min_version = "17.14";
	};

	// How environment variables are spelled: Windows ignores the case of names and separates PATH with ';'.
	enum class env_flavor { windows, posix };

#ifdef _WIN32
	inline constexpr env_flavor k_native_env = env_flavor::windows;
#else
	inline constexpr env_flavor k_native_env = env_flavor::posix;
#endif

	// The installation path vswhere printed (-property installationPath): the first line that is an absolute
	// Windows path. Logo and blank lines are skipped.
	std::optional<std::filesystem::path> parse_vswhere_output(const std::vector<std::string>& lines);
	// The "NAME=value" lines of cmd's `set`; banners, blank lines and cmd's hidden "=C:=C:\dir" entries are dropped.
	std::vector<std::string> parse_set_output(const std::vector<std::string>& lines);
	// What `cmd /u` writes for `set`: UTF-16LE bytes as read (raw_output), decoded and parsed like parse_set_output.
	std::vector<std::string> parse_set_output_utf16le(std::string_view bytes);
	// A leading BOM is dropped, a last odd byte (output cut short) too; an unpaired surrogate becomes U+FFFD.
	std::string utf16le_to_utf8(std::string_view bytes);
	// The text for the person when `path` cannot be handed to cmd.exe: %VAR% (and !VAR!) expand even inside quotes,
	// and & | < > ^ would split or redirect the command if the quoting ever slipped. Nullopt when it is safe.
	std::optional<std::string> cmd_unsafe_path_error(const std::filesystem::path& path);

	std::optional<std::string> variable_of(const std::vector<std::string>& environment, std::string_view name,
		env_flavor flavor = k_native_env);
	// Replaces the variable (keeping the spelling of its name on Windows) or appends it.
	void set_variable(std::vector<std::string>& environment, std::string_view name, std::string_view value,
		env_flavor flavor = k_native_env);
	// Puts `dir` in front of PATH (adds PATH if there is none).
	void merge_path(std::vector<std::string>& environment, const std::filesystem::path& dir,
		env_flavor flavor = k_native_env);
	// `name` (with its extension on Windows) in a PATH directory of `environment`.
	std::optional<std::filesystem::path> find_on_path(const std::vector<std::string>& environment,
		std::string_view name, env_flavor flavor = k_native_env);

	// Finds CMake, ninja and (on Windows) MSVC: vswhere for Visual Studio, then vcvars64.bat for its environment.
	// Runs processes and blocks: call it from a worker thread. The error is text for the person.
	std::variant<toolchain, std::string> find_toolchain(const toolchain_settings& settings, const process_runner& run,
		std::stop_token stop = {});

	// find_toolchain once per editor session: a found toolchain is kept (for the same settings), a failure is not,
	// so installing the missing piece needs no restart. Thread-safe.
	class toolchain_cache
	{
	public:
		std::variant<toolchain, std::string> get(const toolchain_settings& settings, const process_runner& run,
			std::stop_token stop);

	private:
		std::mutex m_mutex;
		std::optional<toolchain_settings> m_settings;
		std::optional<toolchain> m_found;
	};
}
