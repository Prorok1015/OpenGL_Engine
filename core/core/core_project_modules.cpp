#include <snk/core/core_project_modules.h>
#include <snk/application/app_module_loader.hpp>
#include <snk/application/app_module_source.h>
#include <snk/common/ds/ds_store.hpp>
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>
#include <snk/platform/plt_process.h>
#include <snk/project/prj_modules.h>
#include <snk/project/prj_project.h>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

	namespace fs = std::filesystem;

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

	// What goes with the module into its copy: its debug symbols and the libraries it may load from its own folder.
	bool travels_with_module(const fs::path& file)
	{
#if defined(_WIN32)
		const std::string extension = utf8(file.extension());
		return extension == ".dll" || extension == ".pdb" || extension == ".DLL" || extension == ".PDB";
#else
		const std::string name = utf8(file.filename());
		return name.ends_with(".so") || name.find(".so.") != std::string::npos;
#endif
	}

	bool is_library(const fs::path& file)
	{
		const std::string extension = utf8(file.extension());
		return extension != ".pdb" && extension != ".PDB";
	}

	// The process a copy folder <pid>-<n> belongs to; nullopt for a name of another kind.
	std::optional<std::int64_t> owner_of(const fs::path& folder)
	{
		const std::string name = utf8(folder.filename());
		const auto dash = name.find('-');
		std::int64_t pid = 0;
		if (dash == std::string::npos || dash == 0 || dash + 1 == name.size()) {
			return std::nullopt;
		}
		const auto [end, error] = std::from_chars(name.data(), name.data() + dash, pid);
		if (error != std::errc{} || end != name.data() + dash
			|| !std::ranges::all_of(std::string_view{ name }.substr(dash + 1), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) {
			return std::nullopt;
		}
		return pid;
	}

	struct module_copy
	{
		fs::path library;
		fs::path folder; // <root>/.cache/modules/<pid>-<n>, canonical
		// Libraries copied along with the module: loaded by name, so a reload does not replace one already loaded.
		std::vector<fs::path> siblings;
	};

	// The checked, canonical <root>/.cache/modules, made if missing; empty string on success.
	std::string cache_folder(const prj::project_context& project, fs::path& out)
	{
		std::error_code ec;
		// Checked before anything is created in it: a .cache linked out of the project gets no folders.
		if (fs::path cache; fs::exists(project.cache_dir(), ec)) {
			if (std::string why = folder_inside(project.root, project.cache_dir(), cache); !why.empty()) {
				return why;
			}
		}
		fs::create_directories(module_cache_dir(project), ec);
		return folder_inside(project.root, module_cache_dir(project), out);
	}

	// Empty string on success; on failure nothing of the copy is left.
	std::string copy_library(const prj::project_context& project, const fs::path& found, module_copy& out)
	{
		fs::path output;
		if (std::string why = folder_inside(project.root, build_output_dir(project), output); !why.empty()) {
			return why;
		}
		fs::path library;
		if (std::string why = file_inside(output, found, library); !why.empty()) {
			return why;
		}
		fs::path cache;
		if (std::string why = cache_folder(project, cache); !why.empty()) {
			return why;
		}

		std::error_code ec;
		const std::int64_t pid = platform::current_process_id();
		fs::path folder;
		for (int n = 0; n < k_max_copies && folder.empty(); ++n) {
			const fs::path candidate = cache / std::format("{}-{}", pid, n);
			if (fs::create_directory(candidate, ec)) {
				folder = candidate;
			} else if (ec) {
				return std::format("cannot make '{}': {}", utf8(candidate), ec.message());
			}
		}
		if (folder.empty()) {
			return std::format("no free folder for a copy of '{}' in '{}'", utf8(library), utf8(cache));
		}

		const auto fail = [&](std::string why) {
			fs::remove_all(folder, ec);
			return why;
		};
		const fs::path target = folder / library.filename();
		if (!fs::copy_file(library, target, fs::copy_options::none, ec)) {
			return fail(std::format("cannot copy '{}' to '{}': {}", utf8(library), utf8(target), ec.message()));
		}
		for (fs::directory_iterator it{ output, ec }, end; !ec && it != end; it.increment(ec)) {
			fs::path real;
			if (it->path().filename() == library.filename() || !travels_with_module(it->path())
				|| !file_inside(output, it->path(), real).empty()) {
				continue;
			}
			std::error_code copy_ec;
			if (fs::copy_file(real, folder / it->path().filename(), fs::copy_options::none, copy_ec) && is_library(real)) {
				out.siblings.push_back(it->path().filename());
			}
		}
		out.library = target;
		out.folder = folder;
		return {};
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

	template<class F>
	std::string guarded(F&& body)
	{
		try {
			return body();
		} catch (const std::exception& e) {
			return e.what();
		} catch (...) {
			return "unknown exception";
		}
	}

	void warn_about_siblings(const std::string& name, const module_copy& copy)
	{
		if (copy.siblings.empty()) {
			return;
		}
		std::string list;
		for (const auto& file : copy.siblings) {
			list += (list.empty() ? "" : ", ") + utf8(file);
		}
		egLOG_WARN("project", "module '{}': the libraries next to it are not reloaded, the system keeps the ones already "
			"loaded; restart the editor to pick up changes to {}", name, list);
	}

}

core::project_module_host::project_module_host(app::modules::module_loader& loader, ds::app_data_storage& data,
	project_modules_options options)
	: m_loader(loader)
	, m_data(data)
	, m_options(std::move(options))
{
}

std::optional<std::filesystem::path> core::project_module_host::build_output(const std::string& name) const
{
	if (!m_data.has_value<prj::project_context>()) {
		return std::nullopt;
	}
	return find_library(m_data.require<prj::project_context>(), name);
}

void core::project_module_host::remove_unused_copies() const
{
	if (!m_data.has_value<prj::project_context>()) {
		return;
	}
	const auto& project = m_data.require<prj::project_context>();
	std::error_code ec;
	if (!fs::exists(module_cache_dir(project), ec)) {
		return;
	}
	fs::path cache;
	if (!cache_folder(project, cache).empty()) {
		return;
	}
	const auto in_use = [&](const fs::path& folder) {
		return std::ranges::any_of(m_copies, [&](const auto& entry) {
			const module_copies& copies = entry.second;
			return (copies.current && copies.current->folder == folder) || (copies.previous && copies.previous->folder == folder);
		});
	};
	const std::int64_t self = platform::current_process_id();
	std::vector<fs::path> unused;
	for (fs::directory_iterator it{ cache, ec }, end; !ec && it != end; it.increment(ec)) {
		std::error_code type_ec;
		if (!it->is_directory(type_ec)) {
			// A copy of the layout before #435 (<name>.<n>.<ext>).
			unused.push_back(it->path());
			continue;
		}
		const auto owner = owner_of(it->path());
		if (!owner || (*owner == self ? in_use(it->path()) : platform::process_is_running(*owner))) {
			continue;
		}
		unused.push_back(it->path());
	}
	// One still mapped by a process cannot go on Windows and stays.
	for (const auto& entry : unused) {
		fs::remove_all(entry, ec);
	}
}

core::project_modules_report core::project_module_host::load_all()
{
	project_modules_report report;
	if (!m_data.has_value<prj::project_context>()) {
		return report;
	}
	const auto& project = m_data.require<prj::project_context>();
	remove_unused_copies();

	std::vector<std::string> known;
	for (const auto& ref : project.data.modules) {
		if (ref.kind == prj::module_kind::runtime && (m_options.compiled_in.contains(ref.name) || find_library(project, ref.name))) {
			known.push_back(ref.name);
		}
	}
	const prj::module_resolution resolution = prj::resolve_modules(project.data, known, false);
	report.missing_required = resolution.missing_required;
	report.missing_optional = resolution.missing_optional;

	if (m_options.with_editor_modules) {
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
		const std::string problem = guarded([&]() -> std::string {
			std::unique_ptr<app::modules::module_source> source;
			if (const auto it = m_options.compiled_in.find(ref.name); it != m_options.compiled_in.end()) {
				source = std::make_unique<app::modules::static_module_source>(ref.name, it->second);
			} else if (const auto found = find_library(project, ref.name)) {
				if (std::string why = copy_library(project, *found, copy); !why.empty()) {
					return why;
				}
				source = std::make_unique<app::modules::native_library_source>(copy.library, copy.folder);
			} else {
				return "its library is no longer in the build output";
			}
			return load_module(m_loader, m_data, std::move(source), ref.name);
		});
		if (problem.empty()) {
			egLOG_INFO("project", "module '{}' loaded{}", ref.name, copy.library.empty() ? std::string{} : " from " + utf8(copy.library));
			if (!copy.folder.empty()) {
				m_copies[ref.name].current = loaded_copy{ copy.library, copy.folder };
			}
			report.loaded.push_back(ref.name);
			continue;
		}
		egLOG_ERROR("project", "module '{}' refused: {}", ref.name, problem);
		std::error_code ec;
		if (!copy.folder.empty()) {
			fs::remove_all(copy.folder, ec);
		}
		report.refused.push_back(ref.name);
	}
	return report;
}

core::reload_report core::project_module_host::reload(const std::string& name, const reload_hooks& hooks)
{
	const auto refused = [&](std::string why) {
		egLOG_ERROR("project", "module '{}' not reloaded: {}", name, why);
		return reload_report{ reload_status::refused, std::move(why) };
	};
	if (!m_data.has_value<prj::project_context>()) {
		return refused("no project is open");
	}
	if (m_options.compiled_in.contains(name)) {
		return refused("it is compiled into the host");
	}
	const auto& project = m_data.require<prj::project_context>();
	const auto found = find_library(project, name);
	if (!found) {
		return refused(std::format("there is no build of it in '{}'", utf8(build_output_dir(project))));
	}
	remove_unused_copies();
	module_copy copy;
	if (std::string why = guarded([&] { return copy_library(project, *found, copy); }); !why.empty()) {
		return refused(std::move(why));
	}

	std::function<std::unique_ptr<app::modules::module_source>()> previous;
	if (const auto it = m_copies.find(name); it != m_copies.end() && it->second.current) {
		previous = [running = *it->second.current] {
			return std::make_unique<app::modules::native_library_source>(running.library, running.folder);
		};
	}
	const reload_report report = swap(name, std::make_unique<app::modules::native_library_source>(copy.library, copy.folder),
		previous, hooks, loaded_copy{ copy.library, copy.folder });
	if (report.status == reload_status::reloaded) {
		warn_about_siblings(name, copy);
	}
	return report;
}

core::reload_report core::project_module_host::reload(const std::string& name, std::unique_ptr<app::modules::module_source> next,
	std::function<std::unique_ptr<app::modules::module_source>()> previous, const reload_hooks& hooks)
{
	return swap(name, std::move(next), previous, hooks, std::nullopt);
}

core::reload_report core::project_module_host::swap(const std::string& name, std::unique_ptr<app::modules::module_source> next,
	const std::function<std::unique_ptr<app::modules::module_source>()>& previous, const reload_hooks& hooks,
	const std::optional<loaded_copy>& copy)
{
	std::error_code ec;
	const auto drop_copy = [&] {
		if (copy) {
			fs::remove_all(copy->folder, ec);
		}
	};

	// Checked next to the running module: nothing of the running one is touched until the new one passed.
	std::optional<app::modules::module_loader::prepared_module> prepared;
	std::string problem = guarded([&]() -> std::string {
		prepared.emplace(app::modules::module_loader::prepare(std::move(next)));
		if (prepared->name() != name) {
			std::string why = std::format("{} is the module '{}'", prepared->describe(), prepared->name());
			prepared.reset();
			return why;
		}
		return {};
	});
	if (!problem.empty()) {
		drop_copy();
		egLOG_ERROR("project", "module '{}' not reloaded: {}", name, problem);
		return { reload_status::refused, std::move(problem) };
	}

	if (hooks.before_swap) {
		hooks.before_swap();
	}
	const auto finish = [&](reload_report report) {
		if (report.status == reload_status::reloaded) {
			egLOG_INFO("project", "module '{}' reloaded{}", name, copy ? " from " + utf8(copy->library) : std::string{});
		} else {
			egLOG_ERROR("project", "module '{}' not reloaded: {}", name, report.message);
		}
		if (hooks.after_swap) {
			hooks.after_swap(report);
		}
		remove_unused_copies();
		return report;
	};

	const bool was_loaded = m_loader.state(name) == app::modules::module_state::initialized;
	if (was_loaded) {
		problem = guarded([&] { m_loader.unload(name, m_data); return std::string{}; });
		if (!problem.empty() && m_loader.state(name) == app::modules::module_state::initialized) {
			prepared.reset();
			drop_copy();
			return finish({ reload_status::refused, std::format("the running module could not be unloaded: {}", problem) });
		}
		if (!problem.empty()) {
			// Unloaded, but it left services behind (logged by the loader): go on with the new one.
			egLOG_ERROR("project", "module '{}': {}", name, problem);
		}
	}

	problem = guarded([&] { m_loader.load(std::move(*prepared), m_data); return std::string{}; });
	prepared.reset();
	if (problem.empty()) {
		auto& copies = m_copies[name];
		if (copy) {
			copies.previous = std::exchange(copies.current, *copy);
		}
		return finish({ reload_status::reloaded, {} });
	}
	drop_copy();

	if (!was_loaded || !previous) {
		return finish({ reload_status::lost, std::format("the new build failed: {}", problem) });
	}
	const std::string rollback = guarded([&] { return load_module(m_loader, m_data, previous(), name); });
	if (rollback.empty()) {
		return finish({ reload_status::rolled_back, std::format("the new build failed, the previous one runs again: {}", problem) });
	}
	m_copies.erase(name);
	return finish({ reload_status::lost, std::format("the new build failed: {}; the previous one failed to load again: {}", problem, rollback) });
}

core::project_modules_report core::load_project_modules(app::modules::module_loader& loader, ds::app_data_storage& data,
	const project_modules_options& options)
{
	project_module_host host(loader, data, options);
	return host.load_all();
}
