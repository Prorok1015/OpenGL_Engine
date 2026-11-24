#include <snk/core/core_project_modules.h>
#include <snk/application/app_module_loader.hpp>
#include <snk/application/app_module_source.h>
#include <snk/common/ds/ds_store.hpp>
#include <snk/common/fs_paths.h>
#include <snk/common/hash/sha256.h>
#include <snk/common/logger/engine_log.h>
#include <snk/platform/plt_beneath.h>
#include <snk/platform/plt_process.h>
#include <snk/project/prj_modules.h>
#include <snk/project/prj_project.h>
#include <snk/resource/res_shipping_dirs.h>
#include <snk/resource/res_system.h>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
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

	// Where the copies are (#1294): `below` named literally from `base`, the project root or the run's scratch folder,
	// and every folder of it opened from the one above it, never resolved first: a link swapped in at .cache or
	// .cache/modules is refused, never followed to put a copy, or remove one, where it leads.
	struct copies_home
	{
		fs::path base;
		fs::path below;

		fs::path folder() const { return base / below; }
	};

	copies_home home_of(const prj::project_context& project, const fs::path* scratch)
	{
		if (scratch) {
			return { *scratch, "modules" };
		}
		return { project.root, module_cache_dir(project).lexically_relative(project.root) };
	}

	// The copies of a module are a few libraries and their symbols: a file past this is refused, not copied.
	constexpr std::uint64_t k_max_copied_file_bytes = std::uint64_t{ 2 } << 30;

	// `folder` (home.folder() / <pid>-<n>) removed with all it holds; a folder of a module a process still maps cannot
	// go on Windows and stays.
	void remove_copy_folder(const copies_home& home, const fs::path& folder)
	{
		if (folder.parent_path() != home.folder() || !folder.has_filename()) {
			egLOG_WARN("project", "'{}' is not a module copy of '{}' and is not removed", utf8(folder),
				utf8(home.folder()));
			return;
		}
		platform::beneath::pinned_root pinned{ home.base };
		const std::string error = platform::beneath::remove_tree_beneath(pinned, home.below, folder.filename());
		if (!error.empty()) {
			egLOG_WARN("project", "the module copy '{}' stays: {}", utf8(folder), error);
		}
	}

	// `source` (canonical, a regular file) copied to `leaf` in the copy's folder, streamed and never replacing a file.
	std::string copy_into(const copies_home& home, const fs::path& source, const fs::path& folder, const fs::path& leaf)
	{
		std::string problem;
		const platform::beneath::chunk_source stream = [&](const platform::beneath::chunk_sink& sink) {
			platform::beneath::read_request read;
			read.root = source.parent_path();
			read.leaf = source.filename();
			read.max_bytes = k_max_copied_file_bytes;
			bool sink_failed = false;
			const platform::beneath::read_result result =
				platform::beneath::read_beneath_stream(read, [&](std::span<const std::byte> chunk) {
					sink_failed = !sink(chunk);
					return !sink_failed;
				});
			if (result.status == platform::beneath::read_status::too_large) {
				problem = std::format("'{}' is larger than {} bytes, the most a module copy takes", utf8(source),
					k_max_copied_file_bytes);
				return false;
			}
			if (result.status != platform::beneath::read_status::ok) {
				problem = result.error.empty() ? std::format("'{}' cannot be read", utf8(source)) : result.error;
				return false;
			}
			return !sink_failed;
		};
		platform::beneath::pinned_root pinned{ home.base };
		platform::beneath::write_request write;
		write.folder = home.below / folder.filename();
		write.leaf = leaf;
		write.tmp_leaf = fs::path{ utf8(leaf) + ".copying" };
		write.tmp_path = folder / write.tmp_leaf;
		write.stream = &stream;
		write.replace = false;
		// A copy in the cache: a crash leaves a folder the next start removes, so nothing is flushed.
		write.durable = false;
		const std::string error = platform::beneath::write_beneath(pinned, write);
		return error.empty() || problem.empty() ? error : problem;
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
		// Libraries copied along with the module, by file name: size and time of the build's file. They are loaded by
		// name, so a reload does not replace one already loaded.
		std::map<std::string, std::pair<std::uintmax_t, fs::file_time_type>> siblings;
		fs::path source;
		std::uintmax_t source_size = 0;
		fs::file_time_type source_time{};
		// #1304: the hold on the folder, the path the library loads by through it and the digest read through it.
		std::shared_ptr<platform::beneath::held_folder> hold;
		fs::path load_path;
		std::string digest;
	};

	// A read-only project (a shipped game, a migration: #593, #757) gets nothing written into it: the copies go to the
	// run's scratch folder, which resource_term removes. Null for a project the editor writes into.
	const fs::path* run_scratch(ds::app_data_storage& data)
	{
		if (!::res::resource_system::read_only_project() || !data.has_value<core::res::run_scratch_folder>()) {
			return nullptr;
		}
		return &data.require<core::res::run_scratch_folder>().path;
	}

#if !defined(_WIN32)
	// The folder of this user outside the project for the copies the project cannot hold (#1304): `scratch`, made
	// on the first call (core::res::make_scratch_dir, 0700 and checked), the copies in its "modules".
	constexpr std::string_view k_private_copies = "module-copies";

	std::string private_home(fs::path& scratch, copies_home& out)
	{
		if (scratch.empty()) {
			if (const std::size_t stale = core::res::remove_stale_scratch_dirs(k_private_copies); stale > 0) {
				egLOG_INFO("project", "removed {} folder(s) of module copies of ended runs", stale);
			}
			const core::res::scratch_folder_result made = core::res::make_scratch_dir(k_private_copies);
			if (made.path.empty()) {
				return made.error;
			}
			scratch = made.path;
		}
		out = { scratch, "modules" };
		return {};
	}
#endif

	// The home `folder` (a <pid>-<n> folder) was made in: the private one when it lies there.
	copies_home home_of_copy(ds::app_data_storage& data, const fs::path& private_scratch, const fs::path& folder)
	{
		if (!private_scratch.empty() && folder.parent_path() == private_scratch / "modules") {
			return { private_scratch, "modules" };
		}
		return home_of(data.require<prj::project_context>(), run_scratch(data));
	}

	enum class copy_outcome { copied, not_held_here, failed };

	// Copies into a new folder of `home`, closed to others from its making, holds it and hashes the libraries through
	// the hold (#1304). `need_load_path`: a copy no other path than the hold's reaches safely (in the project) is
	// `not_held_here` without one. On anything but `copied` nothing of the copy is left and `why` says why.
	copy_outcome copy_and_hold(const copies_home& home, const fs::path& output, const fs::path& library,
		bool need_load_path, module_copy& out, std::string& why)
	{
		const fs::path cache = home.folder();
		platform::beneath::pinned_root pinned{ home.base };
		if (std::string problem = platform::beneath::make_folders_beneath(pinned, home.below); !problem.empty()) {
			why = std::format("cannot make '{}': {}", utf8(cache), problem);
			return copy_outcome::failed;
		}

		std::error_code ec;
		const std::int64_t pid = platform::current_process_id();
		fs::path folder;
		for (int n = 0; n < k_max_copies && folder.empty(); ++n) {
			const fs::path candidate = cache / std::format("{}-{}", pid, n);
			std::error_code status_ec;
			if (fs::symlink_status(candidate, status_ec).type() != fs::file_type::not_found) {
				continue;
			}
			const fs::path below = home.below / candidate.filename();
			// Closed to others before anything is in it: nobody else can open, add or rename an entry in it.
			std::string problem = platform::beneath::make_new_folder_beneath(pinned, below, 0700);
			if (!problem.empty()) {
				why = std::format("cannot make '{}': {}", utf8(candidate), problem);
				return copy_outcome::failed;
			}
			folder = candidate;
		}
		if (folder.empty()) {
			why = std::format("no free folder for a copy of '{}' in '{}'", utf8(library), utf8(cache));
			return copy_outcome::failed;
		}

		const auto fail = [&](copy_outcome outcome, std::string problem) {
			// Held files cannot be removed on Windows: the hold goes first.
			out.hold.reset();
			remove_copy_folder(home, folder);
			why = std::move(problem);
			return outcome;
		};
		const fs::path target = folder / library.filename();
		// Before the copy: a build that writes meanwhile leaves a newer time, so the copy is not taken for current.
		out.source = library;
		out.source_size = fs::file_size(library, ec);
		out.source_time = fs::last_write_time(library, ec);
		out.siblings.clear();
		if (std::string problem = copy_into(home, library, folder, library.filename()); !problem.empty()) {
			return fail(copy_outcome::failed,
				std::format("cannot copy '{}' to '{}': {}", utf8(library), utf8(target), problem));
		}
		std::vector<fs::path> copied{ library.filename() };
		for (fs::directory_iterator it{ output, ec }, end; !ec && it != end; it.increment(ec)) {
			fs::path real;
			if (it->path().filename() == library.filename() || !travels_with_module(it->path())
				|| !file_inside(output, it->path(), real).empty()) {
				continue;
			}
			if (!copy_into(home, real, folder, it->path().filename()).empty()) {
				continue;
			}
			copied.push_back(it->path().filename());
			if (is_library(real)) {
				std::error_code stamp_ec;
				out.siblings[utf8(it->path().filename())] = { fs::file_size(real, stamp_ec), fs::last_write_time(real, stamp_ec) };
			}
		}

		// Held, then hashed through the hold: what the gate is told is what the loader reads.
		platform::beneath::hold_result held =
			platform::beneath::hold_folder_beneath(pinned, home.below / folder.filename(), copied);
		if (held.status == platform::beneath::hold_status::not_private) {
			return fail(copy_outcome::not_held_here, std::move(held.error));
		}
		if (held.status != platform::beneath::hold_status::held) {
			return fail(copy_outcome::failed, std::format("cannot hold the copy '{}': {}", utf8(folder), held.error));
		}
		out.hold = std::move(held.held);
		std::vector<std::pair<std::string, std::string>> hashed;
		for (const fs::path& name : copied) {
			if (!is_library(name)) {
				continue;
			}
			const std::optional<std::string> hash = cmn::sha256_stream_hex([&](const auto& sink) {
				return out.hold->read(name, [&sink](std::span<const std::byte> piece) {
					sink(piece);
					return true;
				});
			});
			if (!hash) {
				return fail(copy_outcome::failed, std::format("cannot read the held copy '{}'", utf8(folder / name)));
			}
			hashed.emplace_back(utf8(name), *hash);
		}
		std::optional<std::string> digest = core::module_library_set_digest_of(std::move(hashed));
		if (!digest) {
			return fail(copy_outcome::failed, std::format("the copy '{}' holds no library", utf8(folder)));
		}
		out.load_path = out.hold->load_path(library.filename());
		if (out.load_path.empty()) {
			if (need_load_path) {
				return fail(copy_outcome::not_held_here, "the held copy cannot be loaded through its hold here (no "
					"usable /proc/self/fd)");
			}
			// A folder of this user only, that nobody else can rename or write into: its path is as safe as the hold.
			out.load_path = target;
		}
		out.digest = std::move(*digest);
		out.library = target;
		out.folder = folder;
		return copy_outcome::copied;
	}

	// Empty string on success; on failure nothing of the copy is left. The copy goes to the project's .cache/modules
	// (a read-only project: `scratch`, the run's folder), or where the project cannot hold it, to the folder of this
	// user `private_scratch` (made here when needed, #1304).
	std::string copy_library(const prj::project_context& project, const fs::path* scratch,
		[[maybe_unused]] fs::path& private_scratch, const fs::path& found, module_copy& out)
	{
		fs::path output;
		if (std::string why = folder_inside(project.root, build_output_dir(project), output); !why.empty()) {
			return why;
		}
		fs::path library;
		if (std::string why = file_inside(output, found, library); !why.empty()) {
			return why;
		}
		if (!scratch && ::res::resource_system::read_only_project()) {
			return "a read-only project has no folder for the copies of its modules";
		}
		std::string why;
		if (scratch) {
			// The run's folder is this user's only: a path into it is safe where no hold path exists.
			return copy_and_hold(home_of(project, scratch), output, library, false, out, why) == copy_outcome::copied
				? std::string{}
				: why;
		}
#if defined(__APPLE__)
		// No /proc/self/fd: a copy in the project could be loaded only by a path someone else may change.
		why = "macOS has no path that reaches a held folder";
#else
		const copy_outcome here = copy_and_hold(home_of(project, nullptr), output, library, true, out, why);
		if (here == copy_outcome::copied) {
			return {};
		}
		if (here == copy_outcome::failed) {
			return why;
		}
#endif
#if defined(_WIN32)
		return why;
#else
		egLOG_INFO("project", "the copy of '{}' goes to a folder of this user outside the project: {}", utf8(library),
			why);
		copies_home home;
		if (std::string problem = private_home(private_scratch, home); !problem.empty()) {
			return std::format("the project cannot hold a copy safely ({}) and there is no private folder for one: {}",
				why, problem);
		}
		std::string problem;
		if (copy_and_hold(home, output, library, false, out, problem) != copy_outcome::copied) {
			return std::format("the private copy failed: {}", problem);
		}
		return {};
#endif
	}

	// The source of a held copy, loaded by its hold's path.
	std::unique_ptr<app::modules::module_source> held_source(const module_copy& copy)
	{
		return std::make_unique<app::modules::held_library_source>(copy.load_path, copy.library, copy.hold);
	}

	// The source of a copy loaded before, for a rollback or a revert: only while the host still holds its folder.
	std::unique_ptr<app::modules::module_source> held_again(const fs::path& library, const fs::path& load_path,
		const std::weak_ptr<void>& hold)
	{
		std::shared_ptr<void> kept = hold.lock();
		if (!kept) {
			throw std::runtime_error(std::format("the copy '{}' is no longer held", utf8(library)));
		}
		return std::make_unique<app::modules::held_library_source>(load_path, library, std::move(kept));
	}

	// A source whose entry is checked right after it opens, before the module is created: the editor API version of an
	// editor module (#588). When the check refuses, the source is closed again, as a failed open leaves it.
	class checked_module_source final : public app::modules::module_source {
	public:
		using check_fn = std::function<std::string(const app::modules::module_entry&)>;

		checked_module_source(std::unique_ptr<app::modules::module_source> inner, check_fn check)
			: m_inner(std::move(inner)), m_check(std::move(check)) {}

		std::string open() override
		{
			if (std::string problem = m_inner->open(); !problem.empty()) {
				return problem;
			}
			if (std::string problem = m_check(m_inner->entry()); !problem.empty()) {
				m_inner->close();
				return problem;
			}
			return {};
		}
		const app::modules::module_entry& entry() const override { return m_inner->entry(); }
		void close() override { m_inner->close(); }
		std::string describe() const override { return m_inner->describe(); }

	private:
		std::unique_ptr<app::modules::module_source> m_inner;
		check_fn m_check;
	};

	// The source checked against the engine's layout key once it is open, before its module is created (#614).
	std::unique_ptr<app::modules::module_source> with_layout_check(std::unique_ptr<app::modules::module_source> source,
		std::uint64_t layout_key)
	{
		if (layout_key == 0) {
			return source;
		}
		return std::make_unique<checked_module_source>(std::move(source), [layout_key](const app::modules::module_entry& entry) {
			// The version, the compiler and the flags first: prepare reports their own message (debug/release, another
			// major), which says what to change; a layout message would only say "rebuild".
			if (!app::modules::check_module_entry(entry).empty()) {
				return std::string{};
			}
			return app::modules::check_module_layout(entry, layout_key);
		});
	}

	// Empty string once the module is loaded. The module is checked against the manifest's name before it gets a
	// layer; when refused it is gone by the time this returns.
	std::string load_module(app::modules::module_loader& loader, ds::app_data_storage& data,
		std::unique_ptr<app::modules::module_source> source, const std::string& name, std::uint64_t layout_key)
	{
		auto prepared = app::modules::module_loader::prepare(with_layout_check(std::move(source), layout_key));
		if (prepared.name() != name) {
			return std::format("{} is the module '{}'", prepared.describe(), prepared.name());
		}
		loader.load(std::move(prepared), data);
		return {};
	}

	// The host's answer for the copy of `name` about to be loaded: no gate, or one that throws, is a no (#921).
	bool trusted(const core::project_modules_options& options, const std::string& name, const module_copy& copy)
	{
		if (!options.trust_library) {
			return false;
		}
		try {
			return options.trust_library(name, core::library_copy{ copy.library, copy.digest });
		} catch (const std::exception& e) {
			egLOG_ERROR("project", "module '{}': the trust check failed: {}", name, e.what());
		} catch (...) {
			egLOG_ERROR("project", "module '{}': the trust check failed", name);
		}
		return false;
	}

	std::string untrusted_message(const std::string& name)
	{
		return std::format("the project is not trusted: its build of '{}' is not loaded", name);
	}

	void add_once(std::vector<std::string>& names, const std::string& name)
	{
		if (std::ranges::find(names, name) == names.end()) {
			names.push_back(name);
		}
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

	// Only the libraries the running copy loaded too and that the build changed since: the system keeps those.
	// While a reload runs, from its start to its after_swap: unloading the module from inside the reload (a hook, or the
	// module's own code calling back into the editor) would destroy code that is still on the stack.
	struct reloading_scope
	{
		bool& flag;
		explicit reloading_scope(bool& reloading) : flag(reloading) { flag = true; }
		~reloading_scope() { flag = false; }
		reloading_scope(const reloading_scope&) = delete;
		reloading_scope& operator=(const reloading_scope&) = delete;
	};

	void warn_about_siblings(const std::string& name, const std::map<std::string, std::pair<std::uintmax_t, fs::file_time_type>>& now,
		const std::map<std::string, std::pair<std::uintmax_t, fs::file_time_type>>& before)
	{
		std::string list;
		for (const auto& [file, stamp] : now) {
			if (const auto it = before.find(file); it != before.end() && it->second != stamp) {
				list += (list.empty() ? "" : ", ") + file;
			}
		}
		if (list.empty()) {
			return;
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

core::project_module_host::~project_module_host()
{
	if (m_private_scratch.empty()) {
		return;
	}
	// Only POSIX makes one: a library still mapped from it keeps running once its file is gone.
	std::error_code ec;
	if (!core::res::remove_tree_without_following(m_private_scratch, ec)) {
		egLOG_WARN("project", "the folder of module copies '{}' stays: {}", utf8(m_private_scratch), ec.message());
	}
}

void core::project_module_host::remove_copy(const std::filesystem::path& folder) const
{
	if (m_data.has_value<prj::project_context>()) {
		remove_copy_folder(home_of_copy(m_data, m_private_scratch, folder), folder);
	}
}

std::optional<std::filesystem::path> core::project_module_host::build_output(const std::string& name) const
{
	if (m_options.compiled_only || !m_data.has_value<prj::project_context>()) {
		return std::nullopt;
	}
	return find_library(m_data.require<prj::project_context>(), name);
}

std::vector<std::string> core::project_module_host::failed_required(const project_modules_report& report) const
{
	std::vector<std::string> failed;
	if (!m_data.has_value<prj::project_context>()) {
		return failed;
	}
	for (const auto& ref : m_data.require<prj::project_context>().data.modules) {
		if (ref.kind == prj::module_kind::runtime && !ref.optional && std::ranges::find(report.loaded, ref.name) == report.loaded.end()) {
			failed.push_back(ref.name);
		}
	}
	return failed;
}

bool core::project_module_host::is_current(const std::string& name) const
{
	if (m_options.compiled_in.contains(name)) {
		return true;
	}
	const auto it = m_copies.find(name);
	if (it == m_copies.end() || !it->second.current) {
		return false;
	}
	const auto found = build_output(name);
	if (!found) {
		return false;
	}
	const loaded_copy& copy = *it->second.current;
	std::error_code ec;
	if (!fs::equivalent(*found, copy.source, ec) || ec) {
		return false;
	}
	const std::uintmax_t size = fs::file_size(*found, ec);
	if (ec) {
		return false;
	}
	const fs::file_time_type time = fs::last_write_time(*found, ec);
	return !ec && size == copy.source_size && time == copy.source_time;
}

void core::project_module_host::remove_unused_copies() const
{
	// A static host makes no copies and writes nothing into the project.
	if (m_options.compiled_only || !m_data.has_value<prj::project_context>()) {
		return;
	}
	std::vector<copies_home> homes;
	// A read-only one keeps them outside it, in the run's folder that goes with the run.
	if (!::res::resource_system::read_only_project()) {
		homes.push_back(home_of(m_data.require<prj::project_context>(), nullptr));
	}
	if (!m_private_scratch.empty()) {
		homes.push_back({ m_private_scratch, "modules" });
	}
	const auto in_use = [&](const fs::path& folder) {
		return std::ranges::any_of(m_copies, [&](const auto& entry) {
			const module_copies& copies = entry.second;
			return (copies.current && copies.current->folder == folder) || (copies.previous && copies.previous->folder == folder);
		});
	};
	const std::int64_t self = platform::current_process_id();
	for (const copies_home& home : homes) {
		const fs::path cache = home.folder();
		std::error_code ec;
		// Only listed when no link stands in the way (nor a junction, which MSVC calls neither a folder nor a link); a
		// link swapped in after this is met by the removal, which refuses it.
		bool plain = true;
		for (fs::path step = home.base; const fs::path& part : home.below) {
			step /= part;
			plain = plain && fs::symlink_status(step, ec).type() == fs::file_type::directory;
		}
		if (!plain) {
			continue;
		}
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
			remove_copy_folder(home, entry);
		}
	}
}

core::project_modules_report core::project_module_host::load_all()
{
	project_modules_report report;
	// What a reload before this load left to take back or refused names builds of modules loaded anew now.
	m_revert.clear();
	m_rejected.clear();
	m_untrusted_runtime.clear();
	m_untrusted_editor.clear();
	if (!m_data.has_value<prj::project_context>()) {
		return report;
	}
	const auto& project = m_data.require<prj::project_context>();
	remove_unused_copies();

	std::vector<std::string> known;
	for (const auto& ref : project.data.modules) {
		if (ref.kind != prj::module_kind::runtime) {
			continue;
		}
		if (m_options.compiled_in.contains(ref.name)) {
			known.push_back(ref.name);
		} else if (m_options.compiled_only) {
			egLOG_WARN("project", "module '{}' is not loaded: the static host loads only compiled-in modules", ref.name);
		} else if (find_library(project, ref.name)) {
			known.push_back(ref.name);
		}
	}
	const prj::module_resolution resolution = prj::resolve_modules(project.data, known, false);
	report.missing_required = resolution.missing_required;
	report.missing_optional = resolution.missing_optional;

	for (const auto& ref : resolution.resolved) {
		load_runtime_module(ref.name, report);
	}
	return report;
}

void core::project_module_host::load_runtime_module(const std::string& name, project_modules_report& report)
{
	const auto& project = m_data.require<prj::project_context>();
	std::erase(m_untrusted_runtime, name);
	module_copy copy;
	bool untrusted = false;
	const std::string problem = guarded([&]() -> std::string {
		std::unique_ptr<app::modules::module_source> source;
		if (const auto it = m_options.compiled_in.find(name); it != m_options.compiled_in.end()) {
			source = std::make_unique<app::modules::static_module_source>(name, it->second);
		} else if (const auto found = build_output(name)) {
			std::string why = copy_library(project, run_scratch(m_data), m_private_scratch, *found, copy);
			if (!why.empty()) {
				return why;
			}
			// Asked of the held copy: what is judged is what loads, whatever the build output or the cache do meanwhile.
			if (!trusted(m_options, name, copy)) {
				untrusted = true;
				return untrusted_message(name);
			}
			source = held_source(copy);
		} else {
			return "its library is no longer in the build output";
		}
		return load_module(m_loader, m_data, std::move(source), name, m_options.layout_key);
	});
	if (problem.empty()) {
		egLOG_INFO("project", "module '{}' loaded{}", name,
			copy.library.empty() ? std::string{} : " from " + utf8(copy.library));
		if (!copy.folder.empty()) {
			m_copies[name].current = loaded_copy{ copy.library, copy.folder, copy.siblings, copy.source,
				copy.source_size, copy.source_time, copy.load_path, std::move(copy.hold) };
		}
		report.loaded.push_back(name);
		return;
	}
	if (!copy.folder.empty()) {
		copy.hold.reset();
		remove_copy(copy.folder);
	}
	if (untrusted) {
		egLOG_WARN("project", "module '{}' not loaded: {}", name, problem);
		add_once(m_untrusted_runtime, name);
		report.untrusted.push_back(name);
		return;
	}
	egLOG_ERROR("project", "module '{}' refused: {}", name, problem);
	report.refused.push_back(name);
}

core::project_modules_report core::project_module_host::load_waiting()
{
	project_modules_report report;
	if (!m_data.has_value<prj::project_context>()) {
		return report;
	}
	const std::vector<std::string> runtime = m_untrusted_runtime;
	for (const auto& name : runtime) {
		if (m_loader.state(name) == app::modules::module_state::initialized) {
			std::erase(m_untrusted_runtime, name);
			continue;
		}
		load_runtime_module(name, report);
	}
	if (!m_options.with_editor_modules || m_untrusted_editor.empty()) {
		return report;
	}
	const std::vector<std::string> editor = m_untrusted_editor;
	const project_modules_report editor_report = load_editor([&](const std::string& name) {
		return std::ranges::find(editor, name) != editor.end();
	});
	const auto append = [](std::vector<std::string>& to, const std::vector<std::string>& from) {
		to.insert(to.end(), from.begin(), from.end());
	};
	append(report.loaded, editor_report.loaded);
	append(report.missing_required, editor_report.missing_required);
	append(report.missing_optional, editor_report.missing_optional);
	append(report.refused, editor_report.refused);
	append(report.untrusted, editor_report.untrusted);
	return report;
}

core::reload_report core::project_module_host::reload(const std::string& name, const reload_hooks& hooks, bool retry_taken_back)
{
	const auto refused = [&](std::string why) {
		egLOG_ERROR("project", "module '{}' not reloaded: {}", name, why);
		return reload_report{ reload_status::refused, std::move(why) };
	};
	if (m_reloading) {
		return refused("a reload is already running");
	}
	if (is_editor_module(name)) {
		return reload_editor_module(name, hooks);
	}
	const reloading_scope scope{ m_reloading };
	if (!m_data.has_value<prj::project_context>()) {
		return refused("no project is open");
	}
	if (m_options.compiled_in.contains(name)) {
		return refused("it is compiled into the host");
	}
	if (m_options.compiled_only) {
		return refused("the static host loads only compiled-in modules");
	}
	const auto& project = m_data.require<prj::project_context>();
	const auto found = find_library(project, name);
	if (!found) {
		return refused(std::format("there is no build of it in '{}'", utf8(build_output_dir(project))));
	}
	if (const auto rejected = m_rejected.find(name); rejected != m_rejected.end()) {
		if (retry_taken_back || m_loader.state(name) != app::modules::module_state::initialized) {
			// A person asks for it again, or nothing runs that it could be taken back to.
			m_rejected.erase(rejected);
		} else {
			std::error_code equal_ec;
			std::error_code size_ec;
			std::error_code time_ec;
			const bool same_file = fs::equivalent(*found, rejected->second.source, equal_ec);
			const std::uintmax_t size = fs::file_size(*found, size_ec);
			const fs::file_time_type time = fs::last_write_time(*found, time_ec);
			if (equal_ec || size_ec || time_ec) {
				// Kept: a file locked while it is written could otherwise pass as a new build once.
				return refused(std::format("'{}' cannot be compared with the build taken back: {}", utf8(*found),
					(equal_ec ? equal_ec : size_ec ? size_ec : time_ec).message()));
			}
			if (same_file && size == rejected->second.size && time == rejected->second.time) {
				return refused("this build was taken back after it was loaded (the editor could not go on with it); build the module again");
			}
			m_rejected.erase(rejected);
		}
	}
	remove_unused_copies();
	module_copy copy;
	const auto copied = [&] { return copy_library(project, run_scratch(m_data), m_private_scratch, *found, copy); };
	if (std::string why = guarded(copied); !why.empty()) {
		return refused(std::move(why));
	}
	// Before anything is unloaded: the running module stays.
	if (!trusted(m_options, name, copy)) {
		copy.hold.reset();
		remove_copy(copy.folder);
		if (m_loader.state(name) != app::modules::module_state::initialized) {
			add_once(m_untrusted_runtime, name);
		}
		std::string why = untrusted_message(name);
		egLOG_WARN("project", "module '{}' not reloaded: {}", name, why);
		return reload_report{ reload_status::refused, std::move(why) };
	}
	std::erase(m_untrusted_runtime, name);

	std::optional<loaded_copy> running;
	std::function<std::unique_ptr<app::modules::module_source>()> previous;
	if (const auto it = m_copies.find(name); it != m_copies.end() && it->second.current) {
		running = it->second.current;
		previous = [library = running->library, load = running->load_path, hold = std::weak_ptr<void>(running->hold)] {
			return held_again(library, load, hold);
		};
	}
	std::unique_ptr<app::modules::module_source> next = held_source(copy);
	// The hold moves on with the copy: a swap that drops the copy lets it go before the folder is removed.
	loaded_copy made{ copy.library, copy.folder, copy.siblings, copy.source, copy.source_size, copy.source_time,
		copy.load_path, std::move(copy.hold) };
	const reload_report report = swap(name, std::move(next), previous, hooks, std::move(made));
	if (report.status == reload_status::reloaded && running) {
		warn_about_siblings(name, copy.siblings, running->siblings);
	}
	return report;
}

core::reload_report core::project_module_host::reload(const std::string& name, std::unique_ptr<app::modules::module_source> next,
	std::function<std::unique_ptr<app::modules::module_source>()> previous, const reload_hooks& hooks)
{
	if (m_reloading) {
		egLOG_ERROR("project", "module '{}' not reloaded: a reload is already running", name);
		return { reload_status::refused, "a reload is already running" };
	}
	if (is_editor_module(name)) {
		const reloading_scope scope{ m_reloading };
		return swap_editor(name, std::move(next), previous, hooks, std::nullopt);
	}
	const reloading_scope scope{ m_reloading };
	return swap(name, std::move(next), previous, hooks, std::nullopt);
}

void core::project_module_host::forget_revert(const std::string& name)
{
	m_revert.erase(name);
}

core::reload_report core::project_module_host::revert(const std::string& name, const reload_hooks& hooks)
{
	const auto refused = [&](std::string why) {
		egLOG_ERROR("project", "module '{}' not taken back: {}", name, why);
		return reload_report{ reload_status::refused, std::move(why) };
	};
	if (m_reloading) {
		return refused("a reload is already running");
	}
	if (is_editor_module(name)) {
		return refused("it is an editor module: it has no reload to take back");
	}
	const auto it = m_revert.find(name);
	if (it == m_revert.end()) {
		return refused("no reload of it to take back");
	}
	revert_point point = std::move(it->second);
	m_revert.erase(it);
	if (m_loader.state(name) != app::modules::module_state::initialized) {
		return refused("it is not loaded");
	}
	std::error_code ec;
	if (point.previous && !fs::is_regular_file(point.previous->library, ec)) {
		return refused(std::format("the copy of the previous build is gone: '{}'", utf8(point.previous->library)));
	}
	const reloading_scope scope{ m_reloading };
	std::unique_ptr<app::modules::module_source> source;
	if (std::string why = guarded([&] { source = point.source(); return std::string{}; }); !why.empty()) {
		return refused(std::move(why));
	}
	// No rollback: a previous build that fails now leaves no module running, and its copy stays for a look.
	const reload_report report = swap(name, std::move(source), nullptr, hooks, point.previous, true);
	if (report.status == reload_status::reloaded && point.reverted) {
		m_rejected[name] = rejected_build{ point.reverted->source, point.reverted->source_size, point.reverted->source_time };
		if (point.previous) {
			warn_about_siblings(name, point.previous->siblings, point.reverted->siblings);
		}
	}
	return report;
}

core::reload_report core::project_module_host::swap(const std::string& name, std::unique_ptr<app::modules::module_source> next,
	const std::function<std::unique_ptr<app::modules::module_source>()>& previous, const reload_hooks& hooks,
	std::optional<loaded_copy> copy, bool keep_copy)
{
	const auto drop_copy = [&] {
		if (copy && !keep_copy) {
			copy->hold.reset();
			remove_copy(copy->folder);
		}
	};

	// Checked next to the running module: nothing of the running one is touched until the new one passed.
	std::optional<app::modules::module_loader::prepared_module> prepared;
	std::string problem = guarded([&]() -> std::string {
		prepared.emplace(app::modules::module_loader::prepare(with_layout_check(std::move(next), m_options.layout_key)));
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

	// Everything that can refuse is asked before anything is touched: the game module's layer (one of its callbacks is on
	// the stack) and the editor's modules (idem).
	if (std::string why = m_loader.layer_unload_refusal(name); !why.empty()) {
		prepared.reset();
		drop_copy();
		egLOG_ERROR("project", "module '{}' not reloaded: the running module could not be unloaded: {}", name, why);
		return { reload_status::refused, std::format("the running module could not be unloaded: {}", why) };
	}
	// The editor modules registered into the editor on top of the running game module: they go first and come back after
	// it, so a reload registers everything again with the old entries gone.
	if (std::string why = unload_editor_modules(); !why.empty()) {
		prepared.reset();
		drop_copy();
		egLOG_ERROR("project", "module '{}' not reloaded: the editor modules could not be unloaded: {}", name, why);
		return { reload_status::refused, std::format("the editor modules could not be unloaded: {}", why) };
	}
	if (hooks.before_swap) {
		hooks.before_swap();
	}
	const auto finish = [&](reload_report report) {
		report.module = name;
		// Only a new build in place can be taken back, once.
		if (report.status != reload_status::reloaded) {
			m_revert.erase(name);
		}
		if (report.status == reload_status::reloaded) {
			egLOG_INFO("project", "module '{}' reloaded{}", name, copy ? " from " + utf8(copy->library) : std::string{});
		} else {
			egLOG_ERROR("project", "module '{}' not reloaded: {}", name, report.message);
		}
		if (report.status == reload_status::lost) {
			egLOG_WARN("project", "editor modules not loaded again: no game module runs");
		} else {
			load_editor_modules();
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

	// Made before the load: nothing that can throw is left between a module that loaded and finish (after_swap).
	auto& copies = m_copies[name];
	const bool can_revert = previous && was_loaded;
	revert_point next_revert = can_revert ? revert_point{ previous, copies.current, copy } : revert_point{};
	revert_point* revert_slot = can_revert ? &m_revert[name] : nullptr;

	problem = guarded([&] { m_loader.load(std::move(*prepared), m_data); return std::string{}; });
	prepared.reset();
	if (problem.empty()) {
		if (copy) {
			copies.previous = std::exchange(copies.current, *copy);
		}
		if (revert_slot) {
			*revert_slot = std::move(next_revert);
		} else {
			m_revert.erase(name);
		}
		return finish({ reload_status::reloaded, {} });
	}
	// Its hold goes before the folder: the revert point would keep it.
	next_revert = revert_point{};
	drop_copy();

	if (!was_loaded) {
		// Nothing ran before, so nothing was lost.
		return finish({ reload_status::refused, std::format("the new build failed: {}", problem) });
	}
	if (!previous) {
		// No module runs: the copy that was current is unloaded and must not count as the build in use (is_current).
		// A copy revert() kept stays as the previous one.
		if (keep_copy && copy) {
			copies.previous = *copy;
		}
		copies.current.reset();
		return finish({ reload_status::lost, std::format("the new build failed: {}", problem) });
	}
	const std::string rollback = guarded([&] { return load_module(m_loader, m_data, previous(), name, m_options.layout_key); });
	if (rollback.empty()) {
		return finish({ reload_status::rolled_back, std::format("the new build failed, the previous one runs again: {}", problem) });
	}
	m_copies.erase(name);
	return finish({ reload_status::lost, std::format("the new build failed: {}; the previous one failed to load again: {}", problem, rollback) });
}

core::reload_report core::project_module_host::reload_editor_module(const std::string& name, const reload_hooks& hooks)
{
	const auto refused = [&](std::string why) {
		egLOG_ERROR("project", "editor module '{}' not reloaded: {}", name, why);
		return reload_report{ reload_status::refused, std::move(why) };
	};
	if (m_reloading) {
		return refused("a reload is already running");
	}
	if (!is_editor_module(name)) {
		return refused("it is not an editor module of the project");
	}
	const reloading_scope scope{ m_reloading };
	if (!m_data.has_value<prj::project_context>()) {
		return refused("no project is open");
	}
	if (!m_options.with_editor_modules || !m_options.check_editor_module) {
		return refused("the host does not load editor modules");
	}
	if (m_options.compiled_in.contains(name)) {
		return refused("it is compiled into the host");
	}
	if (m_options.compiled_only) {
		return refused("the static host loads only compiled-in modules");
	}
	const auto& project = m_data.require<prj::project_context>();
	const auto found = find_library(project, name);
	if (!found) {
		return refused(std::format("there is no build of it in '{}'", utf8(build_output_dir(project))));
	}
	remove_unused_copies();
	module_copy copy;
	const auto copied = [&] { return copy_library(project, run_scratch(m_data), m_private_scratch, *found, copy); };
	if (std::string why = guarded(copied); !why.empty()) {
		return refused(std::move(why));
	}
	if (!trusted(m_options, name, copy)) {
		copy.hold.reset();
		remove_copy(copy.folder);
		if (m_loader.state(name) != app::modules::module_state::initialized) {
			add_once(m_untrusted_editor, name);
		}
		std::string why = untrusted_message(name);
		egLOG_WARN("project", "editor module '{}' not reloaded: {}", name, why);
		return reload_report{ reload_status::refused, std::move(why) };
	}
	std::erase(m_untrusted_editor, name);
	std::function<std::unique_ptr<app::modules::module_source>()> previous;
	if (const auto it = m_copies.find(name); it != m_copies.end() && it->second.current) {
		const loaded_copy& running = *it->second.current;
		previous = [library = running.library, load = running.load_path, hold = std::weak_ptr<void>(running.hold)] {
			return held_again(library, load, hold);
		};
	}
	std::unique_ptr<app::modules::module_source> next = held_source(copy);
	loaded_copy made{ copy.library, copy.folder, copy.siblings, copy.source, copy.source_size, copy.source_time,
		copy.load_path, std::move(copy.hold) };
	return swap_editor(name, std::move(next), previous, hooks, std::move(made));
}

core::reload_report core::project_module_host::swap_editor(const std::string& name, std::unique_ptr<app::modules::module_source> next,
	const std::function<std::unique_ptr<app::modules::module_source>()>& previous, const reload_hooks& hooks,
	std::optional<loaded_copy> copy)
{
	const auto drop_copy = [&] {
		if (copy) {
			copy->hold.reset();
			remove_copy(copy->folder);
		}
	};
	if (!m_options.with_editor_modules || !m_options.check_editor_module) {
		drop_copy();
		egLOG_ERROR("project", "editor module '{}' not reloaded: the host does not load editor modules", name);
		return { reload_status::refused, "the host does not load editor modules" };
	}
	const auto checked = [&](std::unique_ptr<app::modules::module_source> source) {
		return std::make_unique<checked_module_source>(std::move(source), m_options.check_editor_module);
	};

	// Checked next to the running module: nothing of the running one is touched until the new one passed.
	std::optional<app::modules::module_loader::prepared_module> prepared;
	std::string problem = guarded([&]() -> std::string {
		prepared.emplace(app::modules::module_loader::prepare(with_layout_check(checked(std::move(next)), m_options.layout_key)));
		if (prepared->name() != name) {
			std::string why = std::format("{} is the module '{}'", prepared->describe(), prepared->name());
			prepared.reset();
			return why;
		}
		return {};
	});
	if (!problem.empty()) {
		drop_copy();
		egLOG_ERROR("project", "editor module '{}' not reloaded: {}", name, problem);
		return { reload_status::refused, std::move(problem) };
	}
	const bool was_loaded = m_loader.state(name) == app::modules::module_state::initialized;
	// Everything that can refuse is asked before anything is touched (no before_swap yet): one of the module's callbacks is
	// on the stack (unloading would destroy the code that runs it), its layer refuses, or a loaded module depends on it.
	if (was_loaded) {
		std::string refusal;
		if (m_options.editor_unload_refusal) {
			refusal = m_options.editor_unload_refusal();
		}
		if (refusal.empty()) {
			refusal = m_loader.layer_unload_refusal(name);
		}
		if (!refusal.empty()) {
			prepared.reset();
			drop_copy();
			egLOG_ERROR("project", "editor module '{}' not reloaded: it could not be unloaded: {}", name, refusal);
			return { reload_status::refused, std::format("the editor module could not be unloaded: {}", refusal) };
		}
		if (const std::string dependent = m_loader.dependent_of(name); !dependent.empty()) {
			prepared.reset();
			drop_copy();
			egLOG_ERROR("project", "editor module '{}' not reloaded: the module '{}' depends on it", name, dependent);
			return { reload_status::refused, std::format("the module '{}' depends on it", dependent) };
		}
	}
	// Made before anything is unloaded: nothing that can throw is left between a module that loaded and finish (after_swap).
	// The module goes to the end of the list: a new build may depend on a module that was loaded after the old one, and
	// unload_editor_modules unloads the last loaded first.
	m_editor_loaded.reserve(m_editor_loaded.size() + 1);
	auto& copies = m_copies[name];
	const auto move_to_the_end = [&] {
		std::erase(m_editor_loaded, name);
		m_editor_loaded.push_back(name);
	};

	if (hooks.before_swap) {
		hooks.before_swap();
	}
	const auto finish = [&](reload_report report) {
		report.module = name;
		if (report.status == reload_status::reloaded) {
			egLOG_INFO("project", "editor module '{}' reloaded{}", name, copy ? " from " + utf8(copy->library) : std::string{});
		} else {
			egLOG_ERROR("project", "editor module '{}' not reloaded: {}", name, report.message);
		}
		if (hooks.after_swap) {
			hooks.after_swap(report);
		}
		remove_unused_copies();
		return report;
	};

	if (was_loaded) {
		problem = guarded([&] { m_loader.unload(name, m_data); return std::string{}; });
		if (m_loader.state(name) == app::modules::module_state::initialized) {
			prepared.reset();
			drop_copy();
			return finish({ reload_status::refused, std::format("the editor module could not be unloaded: {}",
				problem.empty() ? std::string{ "unknown reason" } : problem) });
		}
		if (!problem.empty()) {
			egLOG_ERROR("project", "editor module '{}': {}", name, problem);
		}
	}
	problem = guarded([&] { m_loader.load(std::move(*prepared), m_data); return std::string{}; });
	prepared.reset();
	if (problem.empty()) {
		if (copy) {
			copies.previous = std::move(copies.current);
			copies.current = copy;
		}
		move_to_the_end();
		return finish({ reload_status::reloaded, {} });
	}
	drop_copy();
	if (!was_loaded) {
		return finish({ reload_status::refused, std::format("the new build failed: {}", problem) });
	}
	const auto lost = [&](std::string why) {
		// Unloaded and not back: it must not be unloaded again with the others, nor count as the build in use.
		std::erase(m_editor_loaded, name);
		copies.current.reset();
		return finish({ reload_status::lost, std::move(why) });
	};
	if (!previous) {
		return lost(std::format("the new build failed: {}", problem));
	}
	const std::string rollback = guarded([&] {
		return load_module(m_loader, m_data, checked(previous()), name, m_options.layout_key);
	});
	if (rollback.empty()) {
		move_to_the_end();
		return finish({ reload_status::rolled_back, std::format("the new build failed, the previous one runs again: {}", problem) });
	}
	return lost(std::format("the new build failed: {}; the previous one failed to load again: {}", problem, rollback));
}

bool core::project_module_host::is_editor_module(const std::string& name) const
{
	if (std::ranges::find(m_editor_loaded, name) != m_editor_loaded.end()) {
		return true;
	}
	if (!m_data.has_value<prj::project_context>()) {
		return false;
	}
	const auto& modules = m_data.require<prj::project_context>().data.modules;
	return std::ranges::any_of(modules, [&](const auto& ref) { return ref.kind == prj::module_kind::editor && ref.name == name; });
}

core::project_modules_report core::project_module_host::load_editor_modules()
{
	return load_editor({});
}

core::project_modules_report core::project_module_host::load_editor(
	const std::function<bool(const std::string&)>& wanted)
{
	project_modules_report report;
	if (!m_options.with_editor_modules || !m_options.check_editor_module || !m_data.has_value<prj::project_context>()) {
		return report;
	}
	const auto& project = m_data.require<prj::project_context>();
	for (const auto& ref : project.data.modules) {
		if (ref.kind != prj::module_kind::editor || (wanted && !wanted(ref.name))
			|| std::ranges::find(m_editor_loaded, ref.name) != m_editor_loaded.end()) {
			continue;
		}
		std::erase(m_untrusted_editor, ref.name);
		const auto missing = [&] {
			if (ref.optional) {
				egLOG_DEBUG("project", "optional editor module '{}' is not loaded: not built", ref.name);
				report.missing_optional.push_back(ref.name);
			} else {
				egLOG_WARN("project", "editor module '{}' is not loaded: not built", ref.name);
				report.missing_required.push_back(ref.name);
			}
		};

		module_copy copy;
		bool untrusted = false;
		const std::string problem = guarded([&]() -> std::string {
			std::unique_ptr<app::modules::module_source> source;
			if (const auto it = m_options.compiled_in.find(ref.name); it != m_options.compiled_in.end()) {
				source = std::make_unique<app::modules::static_module_source>(ref.name, it->second);
			} else if (m_options.compiled_only) {
				return "the static host loads only compiled-in modules";
			} else if (const auto found = find_library(project, ref.name)) {
				if (std::string why = copy_library(project, run_scratch(m_data), m_private_scratch, *found, copy);
					!why.empty()) {
					return why;
				}
				if (!trusted(m_options, ref.name, copy)) {
					untrusted = true;
					return untrusted_message(ref.name);
				}
				source = held_source(copy);
			} else {
				return {};
			}
			source = std::make_unique<checked_module_source>(std::move(source), m_options.check_editor_module);
			return load_module(m_loader, m_data, std::move(source), ref.name, m_options.layout_key);
		});
		if (problem.empty() && m_loader.state(ref.name) == app::modules::module_state::initialized) {
			egLOG_INFO("project", "editor module '{}' loaded{}", ref.name, copy.library.empty() ? std::string{} : " from " + utf8(copy.library));
			if (!copy.folder.empty()) {
				m_copies[ref.name].current = loaded_copy{ copy.library, copy.folder, copy.siblings, copy.source,
					copy.source_size, copy.source_time, copy.load_path, std::move(copy.hold) };
			}
			m_editor_loaded.push_back(ref.name);
			report.loaded.push_back(ref.name);
			continue;
		}
		if (problem.empty()) {
			missing();
			continue;
		}
		if (!copy.folder.empty()) {
			copy.hold.reset();
			remove_copy(copy.folder);
		}
		if (untrusted) {
			egLOG_WARN("project", "editor module '{}' is not loaded: {}", ref.name, problem);
			add_once(m_untrusted_editor, ref.name);
			report.untrusted.push_back(ref.name);
			continue;
		}
		if (ref.optional) {
			egLOG_WARN("project", "optional editor module '{}' is not loaded: {}", ref.name, problem);
		} else {
			egLOG_ERROR("project", "editor module '{}' is not loaded: {}", ref.name, problem);
		}
		report.refused.push_back(ref.name);
	}
	return report;
}

std::string core::project_module_host::unload_editor_modules()
{
	if (m_editor_loaded.empty()) {
		return {};
	}
	if (m_options.editor_unload_refusal) {
		if (std::string why = m_options.editor_unload_refusal(); !why.empty()) {
			egLOG_ERROR("project", "editor modules not unloaded: {}", why);
			return why;
		}
	}
	while (!m_editor_loaded.empty()) {
		const std::string name = m_editor_loaded.back();
		const std::string problem = guarded([&] { m_loader.unload(name, m_data); return std::string{}; });
		if (m_loader.state(name) == app::modules::module_state::initialized) {
			// Still there (the loader refused): the ones unloaded before it come back, so a refusal leaves the editor as it was.
			egLOG_ERROR("project", "editor module '{}' not unloaded: {}", name, problem);
			load_editor_modules();
			return problem.empty() ? "the module could not be unloaded" : problem;
		}
		if (!problem.empty()) {
			// Unloaded, but it left services behind (logged by the loader).
			egLOG_ERROR("project", "editor module '{}': {}", name, problem);
		}
		m_editor_loaded.pop_back();
	}
	return {};
}

core::project_modules_report core::load_project_modules(app::modules::module_loader& loader, ds::app_data_storage& data,
	const project_modules_options& options)
{
	project_module_host host(loader, data, options);
	return host.load_all();
}

std::optional<std::string> core::module_library_set_digest(const std::filesystem::path& folder)
{
	// The same filter as copy_library, so build/bin and a copy of it give one digest.
	std::vector<std::pair<std::string, fs::path>> files;
	std::error_code ec;
	for (fs::directory_iterator it{ folder, ec }, end; !ec && it != end; it.increment(ec)) {
		std::error_code kind;
		if (!travels_with_module(it->path()) || !is_library(it->path()) || !fs::is_regular_file(it->path(), kind)) {
			continue;
		}
		files.emplace_back(utf8(it->path().filename()), it->path());
	}
	if (ec || files.empty()) {
		return std::nullopt;
	}
	std::vector<std::pair<std::string, std::string>> hashed;
	hashed.reserve(files.size());
	for (const auto& [name, path] : files) {
		std::optional<std::string> hash = cmn::sha256_file_hex(path);
		if (!hash) {
			return std::nullopt;
		}
		hashed.emplace_back(name, std::move(*hash));
	}
	return module_library_set_digest_of(std::move(hashed));
}

std::optional<std::string> core::module_library_set_digest_of(
	std::vector<std::pair<std::string, std::string>> libraries)
{
	if (libraries.empty()) {
		return std::nullopt;
	}
	std::ranges::sort(libraries);
	std::string listing;
	for (const auto& [name, hash] : libraries) {
		listing += name;
		listing += '\0';
		listing += hash;
		listing += '\n';
	}
	return cmn::sha256_hex(std::as_bytes(std::span{ listing.data(), listing.size() }));
}
