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

	// path::string() throws on Windows for a name outside the ANSI code page.
	std::string utf8(const fs::path& path)
	{
		const std::u8string text = path.u8string();
		return { text.begin(), text.end() };
	}

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
			return std::format("'{}' cannot be resolved: {}", utf8(dir), ec.message());
		}
		if (real == root || !fs_paths::is_inside(root, real)) {
			return std::format("'{}' resolves to '{}', outside the project", utf8(dir), utf8(real));
		}
		if (!fs::is_directory(real, ec)) {
			return std::format("'{}' is not a folder", utf8(real));
		}
		out = real;
		return {};
	}

	// `file` made canonical, or why it is not a file inside the canonical folder `dir`.
	std::string file_inside(const fs::path& dir, const fs::path& file, fs::path& out)
	{
		std::error_code ec;
		const fs::path real = fs::canonical(file, ec);
		if (ec || real == dir || !fs_paths::is_inside(dir, real) || !fs::is_regular_file(real, ec)) {
			return std::format("'{}' is not a file inside '{}'", utf8(file), utf8(dir));
		}
		out = real;
		return {};
	}

	bool is_copy_of(const std::string& name, const fs::path& file)
	{
		const std::string file_name = utf8(file.filename());
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

	// The library's debug symbols next to its copy, if there are any inside the build output: best effort.
	void copy_symbols(const fs::path& output, const fs::path& library, const fs::path& copy, module_copy& out)
	{
		std::error_code ec;
		const fs::path symbols = fs::path{ library }.replace_extension(".pdb");
		fs::path real;
		if (!fs::exists(symbols, ec) || !file_inside(output, symbols, real).empty()) {
			return;
		}
		const fs::path target = fs::path{ copy }.replace_extension(".pdb");
		if (fs::copy_file(real, target, fs::copy_options::none, ec)) {
			out.files.push_back(target);
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
		fs::path library;
		if (std::string why = file_inside(output, found, library); !why.empty()) {
			return why;
		}

		std::error_code ec;
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

		const fs::path extension = library.extension();
		for (int n = 0; n < k_max_copies; ++n) {
			fs::path target = out.folder / std::format("{}.{}", name, n);
			target += extension;
			if (fs::copy_file(library, target, fs::copy_options::none, ec)) {
				out.library = target;
				out.files.push_back(target);
				copy_symbols(output, library, target, out);
				return {};
			}
			if (ec != std::errc::file_exists) {
				return std::format("cannot copy '{}' to '{}': {}", utf8(library), utf8(target), ec.message());
			}
		}
		return std::format("no free name for a copy of '{}' in '{}'", utf8(library), utf8(out.folder));
	}

	// Empty string once the module is loaded. The module is checked against the manifest's name before it gets a
	// layer; when refused it is gone by the time this returns.
	std::string load_module(app::modules::module_loader& loader, ds::app_data_storage& data,
		std::unique_ptr<app::modules::module_source> source, const std::string& name)
	{
		auto prepared = app::modules::module_loader::prepare(std::move(source));
		if (prepared.name() != name) {
			return std::format("{} is the module '{}'", prepared.describe(), prepared.name());
		}
		loader.load(std::move(prepared), data);
		return {};
	}

	// Empty string once loaded, else why not; on failure the copy is gone.
	std::string load_one(app::modules::module_loader& loader, ds::app_data_storage& data, const prj::project_context& project,
		const core::project_modules_options& options, const std::string& name, module_copy& copy)
	{
		std::unique_ptr<app::modules::module_source> source;
		if (const auto it = options.compiled_in.find(name); it != options.compiled_in.end()) {
			source = std::make_unique<app::modules::static_module_source>(name, it->second);
		} else if (const auto found = find_library(project, name)) {
			if (std::string why = copy_library(project, name, *found, copy); !why.empty()) {
				return why;
			}
			source = std::make_unique<app::modules::native_library_source>(copy.library, copy.folder);
		} else {
			return "its library is no longer in the build output";
		}
		return load_module(loader, data, std::move(source), name);
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
			if (ref.kind != prj::module_kind::editor) {
				continue;
			}
			if (ref.optional) {
				egLOG_DEBUG("project", "optional editor module '{}' is not loaded: editor modules are not supported yet (#22)", ref.name);
			} else {
				egLOG_WARN("project", "editor module '{}' is not loaded: editor modules are not supported yet (#22)", ref.name);
			}
		}
	}

	for (const auto& ref : resolution.resolved) {
		module_copy copy;
		std::string problem;
		try {
			problem = load_one(loader, data, project, options, ref.name, copy);
		} catch (const std::exception& e) {
			problem = e.what();
		} catch (...) {
			problem = "unknown exception";
		}
		if (problem.empty()) {
			egLOG_INFO("project", "module '{}' loaded{}", ref.name, copy.library.empty() ? std::string{} : " from " + utf8(copy.library));
			report.loaded.push_back(ref.name);
			continue;
		}
		egLOG_ERROR("project", "module '{}' refused: {}", ref.name, problem);
		remove_copy(copy);
		report.refused.push_back(ref.name);
	}
	return report;
}
