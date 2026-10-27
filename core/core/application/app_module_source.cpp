#include <snk/application/app_module_source.h>
#include <bit>
#include <format>

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

	void* open_library(const std::filesystem::path& path)
	{
#if defined(_WIN32)
		return ::LoadLibraryW(path.c_str());
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

	void close_library(void* handle)
	{
#if defined(_WIN32)
		::FreeLibrary(static_cast<HMODULE>(handle));
#else
		::dlclose(handle);
#endif
	}

}

app::modules::native_library_source::native_library_source(std::filesystem::path path)
	: m_path(std::move(path))
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
	m_handle = open_library(m_path);
	if (!m_handle) {
		return std::format("cannot open '{}': {}", m_path.string(), last_error());
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
