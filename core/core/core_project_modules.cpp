#include <snk/core/core_project_modules.h>
#include <snk/application/app_module_loader.hpp>
#include <snk/application/app_module_source.h>
#include <snk/common/ds/ds_store.hpp>
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>
#include <snk/project/prj_modules.h>
#include <snk/project/prj_project.h>
#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

	namespace fs = std::filesystem;

	// Copies of the same file under a new name each load: Windows maps one image per file name, and the build output
	// is never held open, so it can be rebuilt while the module runs (hot reload, #435).
	constexpr int k_max_copies = 1000;

	// <root>/build/bin: the project's build output (build_dir, #426).
	fs::path build_output_dir(const prj::project_context& project)
	{
		return project.root / "build" / "bin";
	}

	fs::path module_cache_dir(const prj::project_context& project)
	{
		return project.cache_dir() / "modules";
	}

	std::vector<std::string> library_names(const std::string& name)
	{
#if defined(_WIN32)
		return { name + ".dll" };
#else
		return { "lib" + name + ".so", name + ".so" };
#endif
	}

	std::optional<fs::path> find_library(const prj::project_context& project, const std::string& name)
	{
		std::error_code ec;
		for (const auto& file : library_names(name)) {
			const fs::path path = build_output_dir(project) / file;
			if (fs::exists(path, ec)) {
				return path;
			}
		}
		return std::nullopt;
	}

	// `dir` made canonical, or why it is not a folder inside the (canonical) project root.
	std::string folder_inside(const fs::path& root, const fs::path& dir, fs::path& out)
	{
		std::error_code ec;
		const fs::path real = fs::canonical(dir, ec);
		if (ec) {
			return std::format("'{}' cannot be resolved: {}", dir.string(), ec.message());
		}
		if (real == root || !fs_paths::is_inside(root, real)) {
			return std::format("'{}' resolves to '{}', outside the project", dir.string(), real.string());
		}
		if (!fs::is_directory(real, ec)) {
			return std::format("'{}' is not a folder", real.string());
		}
		out = real;
		return {};
	}

	bool is_copy_of(const std::string& name, const fs::path& file)
	{
		const std::string file_name = file.filename().string();
		if (!file_name.starts_with(name + ".")) {
			return false;
		}
		const std::string_view rest = std::string_view{ file_name }.substr(name.size() + 1);
		const std::string_view number = rest.substr(0, rest.find('.'));
		return !number.empty() && number.size() < rest.size()
			&& std::ranges::all_of(number, [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
	}

	// Copies left by earlier runs. One another editor still has loaded cannot be removed on Windows and stays.
	void remove_old_copies(const fs::path& cache, const std::string& name)
	{
		std::error_code ec;
		std::vector<fs::path> old;
		for (fs::directory_iterator it{ cache, ec }, end; !ec && it != end; it.increment(ec)) {
			if (is_copy_of(name, it->path())) {
				old.push_back(it->path());
			}
		}
		for (const auto& file : old) {
			fs::remove(file, ec);
		}
	}

	struct module_copy
	{
		fs::path library;
		fs::path folder; // canonical <root>/.cache/modules
		std::vector<fs::path> files; // the library and what came with it
	};

	void remove_copy(const module_copy& copy)
	{
		std::error_code ec;
		for (const auto& file : copy.files) {
			fs::remove(file, ec);
		}
	}

	// Empty string on success.
	std::string copy_library(const prj::project_context& project, const std::string& name, const fs::path& found, module_copy& out)
	{
		const fs::path& root = project.root;
		fs::path output;
		if (std::string why = folder_inside(root, build_output_dir(project), output); !why.empty()) {
			return why;
		}
		std::error_code ec;
		const fs::path library = fs::canonical(found, ec);
		if (ec || !fs_paths::is_inside(output, library) || library == output || !fs::is_regular_file(library, ec)) {
			return std::format("'{}' is not a file inside '{}'", found.string(), output.string());
		}

		// Checked before anything is created in it: a .cache linked out of the project gets no folders.
		if (fs::path cache; fs::exists(project.cache_dir(), ec)) {
			if (std::string why = folder_inside(root, project.cache_dir(), cache); !why.empty()) {
				return why;
			}
		}
		fs::create_directories(module_cache_dir(project), ec);
		if (std::string why = folder_inside(root, module_cache_dir(project), out.folder); !why.empty()) {
			return why;
		}
		remove_old_copies(out.folder, name);

		const std::string extension = library.extension().string();
		for (int n = 0; n < k_max_copies; ++n) {
			const fs::path target = out.folder / std::format("{}.{}{}", name, n, extension);
			if (fs::copy_file(library, target, fs::copy_options::none, ec)) {
				out.library = target;
				out.files.push_back(target);
				const fs::path symbols = fs::path{ library }.replace_extension(".pdb");
				if (fs::is_regular_file(symbols, ec)) {
					const fs::path symbols_copy = fs::path{ target }.replace_extension(".pdb");
					if (fs::copy_file(symbols, symbols_copy, fs::copy_options::overwrite_existing, ec)) {
						out.files.push_back(symbols_copy);
					}
				}
				return {};
			}
			if (ec != std::errc::file_exists) {
				return std::format("cannot copy '{}' to '{}': {}", library.string(), target.string(), ec.message());
			}
		}
		return std::format("no free name for a copy of '{}' in '{}'", library.string(), out.folder.string());
	}

	// Empty string once the module is loaded. The module is checked against the manifest's name before it gets a
	// layer; when refused it is gone by the time this returns.
	std::string load_module(app::modules::module_loader& loader, ds::app_data_storage& data,
		std::unique_ptr<app::modules::module_source> source, const std::string& name)
	{
		try {
			auto prepared = app::modules::module_loader::prepare(std::move(source));
			if (prepared.name() != name) {
				return std::format("{} is the module '{}'", prepared.describe(), prepared.name());
			}
			loader.load(std::move(prepared), data);
			return {};
		} catch (const std::exception& e) {
			return e.what();
		} catch (...) {
			return "unknown exception";
		}
	}

}

core::project_modules_report core::load_project_modules(app::modules::module_loader& loader, ds::app_data_storage& data,
	const project_modules_options& options)
{
	project_modules_report report;
	if (!data.has_value<prj::project_context>()) {
		return report;
	}
	const auto& project = data.require<prj::project_context>();

	std::vector<std::string> known;
	for (const auto& ref : project.data.modules) {
		if (ref.kind == prj::module_kind::runtime && (options.compiled_in.contains(ref.name) || find_library(project, ref.name))) {
			known.push_back(ref.name);
		}
	}
	const prj::module_resolution resolution = prj::resolve_modules(project.data, known, false);
	report.missing_required = resolution.missing_required;
	report.missing_optional = resolution.missing_optional;

	if (options.with_editor_modules) {
		for (const auto& ref : project.data.modules) {
			if (ref.kind == prj::module_kind::editor) {
				egLOG_WARN("project", "editor module '{}' is not loaded: editor modules are not supported yet (#22)", ref.name);
			}
		}
	}

	for (const auto& ref : resolution.resolved) {
		std::unique_ptr<app::modules::module_source> source;
		module_copy copy;
		std::string problem;
		if (const auto it = options.compiled_in.find(ref.name); it != options.compiled_in.end()) {
			source = std::make_unique<app::modules::static_module_source>(ref.name, it->second);
		} else if (const auto found = find_library(project, ref.name)) {
			problem = copy_library(project, ref.name, *found, copy);
			if (problem.empty()) {
				source = std::make_unique<app::modules::native_library_source>(copy.library, copy.folder);
			}
		} else {
			problem = "its library is no longer in the build output";
		}
		if (source) {
			problem = load_module(loader, data, std::move(source), ref.name);
		}
		if (problem.empty()) {
			egLOG_INFO("project", "module '{}' loaded{}", ref.name, copy.library.empty() ? std::string{} : " from " + copy.library.string());
			report.loaded.push_back(ref.name);
			continue;
		}
		egLOG_ERROR("project", "module '{}' refused: {}", ref.name, problem);
		remove_copy(copy);
		report.refused.push_back(ref.name);
	}
	return report;
}
