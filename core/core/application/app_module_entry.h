#pragma once
#include "app_module_interface.h"
#include <cstdint>
#include <memory>
#include <string>
#include <version>

// C entry point of a loadable game module (EPIC-29 #62, docs/roadmap/design/EPIC-29_SERVICES_DLL.md §6).
//
// A module binary exports four C-named functions; behind them is a C++ module_interface, so host
// and module must share toolchain, standard library and CRT. The host checks that through
// snk_module_build_info before it creates anything, and the binary that created a module
// destroys it (snk_destroy_module), so allocation never crosses a heap boundary.

#define SNK_MODULE_API_VERSION 0x0001'0000u // major << 16 | minor

#if defined(_WIN32)
#define SNK_MODULE_EXPORT __declspec(dllexport)
#else
#define SNK_MODULE_EXPORT __attribute__((visibility("default")))
#endif

struct snk_module_build_info
{
	std::uint32_t api;      // SNK_MODULE_API_VERSION the module was built against
	std::uint32_t compiler; // (compiler id << 16) | major version, see app::modules::compiler_id
	std::uint32_t flags;    // app::modules::build_flag bits
};

namespace app::modules {

	namespace build_flag {
		inline constexpr std::uint32_t debug = 1u << 0;       // debug build (debug CRT on MSVC, no NDEBUG elsewhere)
		inline constexpr std::uint32_t static_crt = 1u << 1;  // MSVC /MT or /MTd
		inline constexpr std::uint32_t libcxx = 1u << 2;      // LLVM libc++ (otherwise libstdc++ / MSVC STL)
		inline constexpr std::uint32_t old_stl_abi = 1u << 3; // libstdc++ _GLIBCXX_USE_CXX11_ABI=0
	}

	namespace compiler_id {
		inline constexpr std::uint32_t unknown = 0;
		inline constexpr std::uint32_t msvc = 1;
		inline constexpr std::uint32_t gcc = 2;
		inline constexpr std::uint32_t clang = 3;
	}

	constexpr std::uint32_t api_major(std::uint32_t version) noexcept { return version >> 16; }
	constexpr std::uint32_t api_minor(std::uint32_t version) noexcept { return version & 0xffffu; }

	// Evaluated from predefined macros, so it describes whichever binary compiles the call.
	constexpr snk_module_build_info make_build_info() noexcept
	{
		std::uint32_t compiler = 0;
		std::uint32_t flags = 0;
#if defined(__clang__)
		compiler = (compiler_id::clang << 16) | static_cast<std::uint32_t>(__clang_major__);
#elif defined(_MSC_VER)
		compiler = (compiler_id::msvc << 16) | static_cast<std::uint32_t>(_MSC_VER);
#elif defined(__GNUC__)
		compiler = (compiler_id::gcc << 16) | static_cast<std::uint32_t>(__GNUC__);
#endif

#if defined(_MSC_VER)
#if defined(_DEBUG)
		flags |= build_flag::debug;
#endif
#if !defined(_DLL)
		flags |= build_flag::static_crt;
#endif
#elif !defined(NDEBUG)
		flags |= build_flag::debug;
#endif
#if defined(_LIBCPP_VERSION)
		flags |= build_flag::libcxx;
#endif
#if defined(_GLIBCXX_USE_CXX11_ABI) && _GLIBCXX_USE_CXX11_ABI == 0
		flags |= build_flag::old_stl_abi;
#endif
		return snk_module_build_info{ SNK_MODULE_API_VERSION, compiler, flags };
	}

	// Deletes through the creating binary's destroy function when there is one.
	struct module_deleter
	{
		void (*destroy)(module_interface*) = nullptr;

		void operator()(module_interface* m) const
		{
			if (destroy) {
				destroy(m);
			} else {
				delete m;
			}
		}
	};
	using module_ptr = std::unique_ptr<module_interface, module_deleter>;

	// The four exported functions of a module binary, resolved by whoever loads it
	// (dlopen / LoadLibrary in EPIC-52 #21; the unit tests take the addresses directly).
	struct module_entry
	{
		std::uint32_t (*api_version)() = nullptr;
		const struct snk_module_build_info* (*build_info)() = nullptr;
		module_interface* (*create)() = nullptr;
		void (*destroy)(module_interface*) = nullptr;
	};

	// Empty string when the module can be loaded by this host, otherwise the reason it cannot.
	inline std::string check_module_entry(const module_entry& entry)
	{
		if (!entry.api_version || !entry.build_info || !entry.create || !entry.destroy) {
			return "entry point is incomplete (a snk_* function is missing)";
		}
		const std::uint32_t version = entry.api_version();
		const std::uint32_t host_version = SNK_MODULE_API_VERSION;
		if (api_major(version) != api_major(host_version)) {
			return "api major version " + std::to_string(api_major(version)) + " does not match host major "
				+ std::to_string(api_major(host_version));
		}
		if (api_minor(version) > api_minor(host_version)) {
			return "module needs api minor " + std::to_string(api_minor(version)) + ", host provides "
				+ std::to_string(api_minor(host_version));
		}

		const snk_module_build_info* info = entry.build_info();
		if (!info) {
			return "module returned no build info";
		}
		if (info->api != version) {
			return "build info api does not match snk_module_api_version";
		}
		const snk_module_build_info host = make_build_info();
		// Same compiler family; for MSVC every 19.x toolset is binary compatible, otherwise the major must match.
		const bool same_compiler = (info->compiler >> 16) == (host.compiler >> 16)
			&& ((host.compiler >> 16) == compiler_id::msvc || info->compiler == host.compiler);
		if (!same_compiler) {
			return "built with a different compiler (module " + std::to_string(info->compiler)
				+ ", host " + std::to_string(host.compiler) + ")";
		}
		if (info->flags != host.flags) {
			return "build flags differ (module " + std::to_string(info->flags) + ", host "
				+ std::to_string(host.flags) + "): debug/release, CRT or standard library mismatch";
		}
		return {};
	}

}

// Generates the four exported functions of a module binary under the given prefix:
// <prefix>module_api_version, <prefix>module_build_info, <prefix>create_module, <prefix>destroy_module.
// Use SNK_DECLARE_MODULE in a module binary; the prefixed form exists so one binary (a unit test) can
// hold several declarations without clashing C symbols.
#define SNK_DECLARE_MODULE_PREFIXED(prefix, Type) \
	extern "C" { \
	SNK_MODULE_EXPORT std::uint32_t prefix##module_api_version() \
	{ \
		return SNK_MODULE_API_VERSION; \
	} \
	SNK_MODULE_EXPORT const struct snk_module_build_info* prefix##module_build_info() \
	{ \
		static const struct snk_module_build_info info = ::app::modules::make_build_info(); \
		return &info; \
	} \
	SNK_MODULE_EXPORT ::app::modules::module_interface* prefix##create_module() \
	{ \
		return new Type(); \
	} \
	SNK_MODULE_EXPORT void prefix##destroy_module(::app::modules::module_interface* module) \
	{ \
		delete module; \
	} \
	}

#define SNK_DECLARE_MODULE(Type) SNK_DECLARE_MODULE_PREFIXED(snk_, Type)
