#pragma once
#include <snk/application/app_module_entry.h>
#include <snk/common/snk_api.h>
#include <filesystem>
#include <string>
#include <utility>

// Where a module's code comes from (MODULE_LAYER.md §6): compiled into the host or a shared library on disk.
// module_loader opens the source, creates the module through its entry and closes the source only after the
// module and its layer are gone, so no code of the module can run once the library is released.
namespace app::modules {

	class module_source {
	public:
		virtual ~module_source() = default;

		// Resolves the entry points. Empty string on success, otherwise why the module cannot be had;
		// a failed open leaves the source closed.
		virtual std::string open() = 0;
		// Valid between a successful open() and close().
		virtual const module_entry& entry() const = 0;
		// Releases the code. Called once the module created from the entry is destroyed.
		virtual void close() = 0;
		// For logs: a path, or the module's name for a compiled-in one.
		virtual std::string describe() const = 0;
	};

	// A module compiled into the host: the entry is the addresses of its functions (SNK_DECLARE_MODULE_PREFIXED).
	class static_module_source final : public module_source {
	public:
		static_module_source(std::string name, const module_entry& entry) : m_name(std::move(name)), m_entry(entry) {}

		std::string open() override { return {}; }
		const module_entry& entry() const override { return m_entry; }
		void close() override {}
		std::string describe() const override { return m_name; }

	private:
		std::string m_name;
		module_entry m_entry;
	};

	// A module in a shared library (LoadLibraryW / dlopen), entry points resolved by their SNK_DECLARE_MODULE names.
	// On Linux the image is unmapped by close() only if the module keeps hidden visibility (SPIKE-218 §3.5).
	class SNK_API native_library_source final : public module_source {
	public:
		explicit native_library_source(std::filesystem::path path);
		~native_library_source() override;

		native_library_source(const native_library_source&) = delete;
		native_library_source& operator=(const native_library_source&) = delete;

		std::string open() override;
		const module_entry& entry() const override { return m_entry; }
		void close() override;
		std::string describe() const override;

		const std::filesystem::path& path() const { return m_path; }
		bool is_open() const { return m_handle != nullptr; }

	private:
		std::filesystem::path m_path;
		void* m_handle = nullptr;
		module_entry m_entry;
	};

}
