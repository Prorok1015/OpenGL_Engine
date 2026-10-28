#include <snk/application/app_module_source.h>
#include <snk/common/fs_paths.h>
#include <bit>
#include <format>
#include <system_error>

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
		return std::format("error {}", ::GetLastError());
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
	m_entry.api_version = find_symbol<decltype(m_entry.api_version)>(m_handle, "snk_module_api_version");
	m_entry.build_info = find_symbol<decltype(m_entry.build_info)>(m_handle, "snk_module_build_info");
	m_entry.create = find_symbol<decltype(m_entry.create)>(m_handle, "snk_create_module");
	m_entry.destroy = find_symbol<decltype(m_entry.destroy)>(m_handle, "snk_destroy_module");
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
