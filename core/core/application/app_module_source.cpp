#include <snk/application/app_module_source.h>
#include <snk/common/fs_paths.h>
#include <bit>
#include <cstddef>
#include <format>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

	// `path` is absolute and canonical (checked_path).
	void* open_library(const std::filesystem::path& path)
	{
#if defined(_WIN32)
		return ::LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
#else
		return ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
	}

	std::string last_error()
	{
#if defined(_WIN32)
		const DWORD code = ::GetLastError();
		// 127: the module imports a function this engine does not export (built against another engine version).
		if (code == ERROR_PROC_NOT_FOUND) {
			return "error 127: the module imports a function the engine does not export; rebuild it against this engine";
		}
		return std::format("error {}", code);
#else
		const char* text = ::dlerror();
		return text ? text : "unknown error";
#endif
	}

	template<class F>
	F find_symbol(void* handle, const char* name)
	{
#if defined(_WIN32)
		return std::bit_cast<F>(::GetProcAddress(static_cast<HMODULE>(handle), name));
#else
		return std::bit_cast<F>(::dlsym(handle, name));
#endif
	}

	// The canonical path of the library, or why it may not be loaded.
	std::string checked_path(const std::filesystem::path& path, const std::filesystem::path& module_dir, std::filesystem::path& out)
	{
		namespace fs = std::filesystem;
		if (!path.is_absolute()) {
			return std::format("'{}' is not an absolute path", path.string());
		}
		if (module_dir.empty() || !module_dir.is_absolute()) {
			return std::format("the module directory '{}' is not an absolute path", module_dir.string());
		}
		std::error_code ec;
		const fs::path dir = fs::canonical(module_dir, ec);
		if (ec) {
			return std::format("the module directory '{}' cannot be resolved: {}", module_dir.string(), ec.message());
		}
		const fs::path real = fs::canonical(path, ec);
		if (ec) {
			return std::format("'{}' cannot be resolved: {}", path.string(), ec.message());
		}
		if (!fs_paths::is_inside(dir, real) || real == dir) {
			return std::format("'{}' is outside the module directory '{}'", real.string(), dir.string());
		}
		if (!fs::is_regular_file(real, ec)) {
			return std::format("'{}' is not a file", real.string());
		}
		out = real;
		return {};
	}

	void close_library(void* handle)
	{
#if defined(_WIN32)
		::FreeLibrary(static_cast<HMODULE>(handle));
#else
		::dlclose(handle);
#endif
	}

	void resolve_entry(void* handle, app::modules::module_entry& entry)
	{
		entry.api_version = find_symbol<decltype(entry.api_version)>(handle, "snk_module_api_version");
		entry.build_info = find_symbol<decltype(entry.build_info)>(handle, "snk_module_build_info");
		entry.create = find_symbol<decltype(entry.create)>(handle, "snk_create_module");
		entry.destroy = find_symbol<decltype(entry.destroy)>(handle, "snk_destroy_module");
		entry.editor_api_version = find_symbol<decltype(entry.editor_api_version)>(handle, "snk_editor_api_version");
		entry.layout_key = find_symbol<decltype(entry.layout_key)>(handle, "snk_module_layout_key");
		entry.editor_layout_key = find_symbol<decltype(entry.editor_layout_key)>(handle, "snk_editor_layout_key");
	}

	// Whether the system still maps the library loaded by `path` (a closed one it keeps: GNU_UNIQUE symbols, another
	// reference).
	bool still_loaded(const std::filesystem::path& path)
	{
#if defined(_WIN32)
		return ::GetModuleHandleW(path.c_str()) != nullptr;
#else
		void* again = ::dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD);
		if (again) {
			::dlclose(again);
		}
		return again != nullptr;
#endif
	}

#if defined(_WIN32)
	// `path` without its \\?\ prefix: the form GetModuleFileNameW gives for a path the loader took either way.
	std::wstring without_prefix(const std::wstring& path)
	{
		if (path.starts_with(L"\\\\?\\UNC\\")) {
			return L"\\\\" + path.substr(8);
		}
		if (path.starts_with(L"\\\\?\\")) {
			return path.substr(4);
		}
		return path;
	}

	// The final path of a held file as the loader takes it best: without the \\?\ prefix when it is short enough.
	std::filesystem::path loader_form(const std::filesystem::path& path)
	{
		const std::wstring plain = without_prefix(path.native());
		return plain.size() < static_cast<std::size_t>(MAX_PATH) ? std::filesystem::path{ plain } : path;
	}

	// The module `handle` lies at `path` (compared without case, as Windows names files).
	bool loaded_from(void* handle, const std::filesystem::path& path)
	{
		std::wstring name(32768, L'\0');
		const DWORD length =
			::GetModuleFileNameW(static_cast<HMODULE>(handle), name.data(), static_cast<DWORD>(name.size()));
		if (length == 0 || length >= name.size()) {
			return false;
		}
		name.resize(length);
		const std::wstring loaded = without_prefix(name);
		const std::wstring expected = without_prefix(path.native());
		return ::CompareStringOrdinal(loaded.c_str(), static_cast<int>(loaded.size()), expected.c_str(),
			static_cast<int>(expected.size()), TRUE) == CSTR_EQUAL;
	}
#endif

}

app::modules::native_library_source::native_library_source(std::filesystem::path path, std::filesystem::path module_dir)
	: m_path(std::move(path))
	, m_module_dir(std::move(module_dir))
{
}

app::modules::native_library_source::~native_library_source()
{
	close();
}

std::string app::modules::native_library_source::open()
{
	if (m_handle) {
		return {};
	}
	std::filesystem::path real;
	if (std::string problem = checked_path(m_path, m_module_dir, real); !problem.empty()) {
		return problem;
	}
	m_handle = open_library(real);
	if (!m_handle) {
		return std::format("cannot open '{}': {}", real.string(), last_error());
	}
	resolve_entry(m_handle, m_entry);
	if (std::string problem = check_library_module(m_entry); !problem.empty()) {
		close();
		return std::format("refusing '{}': {}", real.string(), problem);
	}
	return {};
}

void app::modules::native_library_source::close()
{
	if (m_handle) {
		close_library(m_handle);
		m_handle = nullptr;
	}
	m_entry = {};
}

std::string app::modules::native_library_source::describe() const
{
	return m_path.string();
}

app::modules::held_library_source::held_library_source(std::filesystem::path load_path, std::filesystem::path shown,
	std::shared_ptr<void> keep)
	: m_load_path(std::move(load_path))
	, m_shown(std::move(shown))
	, m_keep(std::move(keep))
{
}

app::modules::held_library_source::~held_library_source()
{
	close();
}

std::string app::modules::held_library_source::open()
{
	if (m_handle) {
		return {};
	}
	if (!m_load_path.is_absolute()) {
		return std::format("'{}' is not an absolute path", m_load_path.string());
	}
#if defined(_WIN32)
	const std::filesystem::path path = loader_form(m_load_path);
#else
	// Never made canonical: that would resolve /proc/self/fd/<n> away to the folder's path, which can be replaced.
	const std::filesystem::path& path = m_load_path;
#endif
	m_handle = open_library(path);
	if (!m_handle) {
		return std::format("cannot open '{}': {}", describe(), last_error());
	}
#if defined(_WIN32)
	if (!loaded_from(m_handle, m_load_path)) {
		close();
		return std::format("refusing '{}': the system loaded another file under its name", describe());
	}
#endif
	resolve_entry(m_handle, m_entry);
	if (std::string problem = check_library_module(m_entry); !problem.empty()) {
		close();
		return std::format("refusing '{}': {}", describe(), problem);
	}
	return {};
}

void app::modules::held_library_source::close()
{
	if (m_handle) {
		close_library(m_handle);
		m_handle = nullptr;
#if defined(_WIN32)
		const std::filesystem::path path = loader_form(m_load_path);
#else
		const std::filesystem::path& path = m_load_path;
#endif
		if (m_keep && still_loaded(path)) {
			// Never let go: see the class comment. Deliberately leaked with the image the system keeps.
			static_cast<void>(new std::shared_ptr<void>(m_keep));
		}
	}
	m_entry = {};
}

std::string app::modules::held_library_source::describe() const
{
	return m_shown.empty() ? m_load_path.string() : m_shown.string();
}
