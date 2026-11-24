#pragma once
#include <snk/application/app_module_entry.h>
#include <snk/common/snk_api.h>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

// Where a module's code comes from (MODULE_LAYER.md sec. 6): compiled into the host or a shared library on disk.
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

	// A module in a shared library, entry points resolved by their SNK_DECLARE_MODULE names. Only a file that lies,
	// once made canonical (symlinks resolved), inside `module_dir` is loaded, by that absolute path: LoadLibraryExW
	// with the DLL's own directory and the system directories as the only search path for its dependencies, dlopen
	// with a path (never a bare name, which LD_LIBRARY_PATH and the loader's search order would resolve). A relative
	// path, a bare name or a file outside the directory is refused by open() before anything is loaded.
	// On Linux the image is unmapped by close() only if the module keeps hidden visibility (SPIKE-218 sec. 3.5).
	//
	// For module authors: the library's static initializers (and DllMain) run inside open(), before the host has
	// checked snk_module_build_info. They must not use the engine or EnTT; do that in the module's services.
	// Nothing of the library (an exception its code threw, an object, a string, a vtable) may outlive its unload: an
	// exception of a module that fails to load reaches the host as module_loader's own std::logic_error with its message.
	class SNK_API native_library_source final : public module_source {
	public:
		// `module_dir`: the project's (or the build's) directory of game modules, absolute.
		native_library_source(std::filesystem::path path, std::filesystem::path module_dir);
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
		std::filesystem::path m_module_dir;
		void* m_handle = nullptr;
		module_entry m_entry;
	};

	// A module in a shared library whose folder and files a host holds (#1304), loaded by `load_path`, the exact path
	// that reaches the held file (Linux /proc/self/fd/<folder>/<name>, Windows the final path of the file's handle),
	// never resolved first: on Linux $ORIGIN is then the held folder, so the libraries beside it come from there too.
	// Windows: the module the system loaded must lie at that path (GetModuleFileNameW), else it is unloaded and
	// refused. `keep` holds the folder and is let go with the source, after close(); while the system still maps the
	// image after close() it is never let go (a load of a later hold could get the same descriptor, so the same name,
	// and be handed this image). `shown`: the path logs name. Same rules for module authors as native_library_source.
	class SNK_API held_library_source final : public module_source {
	public:
		held_library_source(std::filesystem::path load_path, std::filesystem::path shown, std::shared_ptr<void> keep);
		~held_library_source() override;

		held_library_source(const held_library_source&) = delete;
		held_library_source& operator=(const held_library_source&) = delete;

		std::string open() override;
		const module_entry& entry() const override { return m_entry; }
		void close() override;
		std::string describe() const override;

		const std::filesystem::path& load_path() const { return m_load_path; }
		bool is_open() const { return m_handle != nullptr; }

	private:
		std::filesystem::path m_load_path;
		std::filesystem::path m_shown;
		std::shared_ptr<void> m_keep;
		void* m_handle = nullptr;
		module_entry m_entry;
	};

}
