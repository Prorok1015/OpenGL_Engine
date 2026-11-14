#include <snk/project/prj_project.h>
#include <snk/common/fs_paths.h>
#include <snk/project/prj_code.h>
#include <algorithm>
#include <array>
#include <format>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;
using fs_paths::is_inside;

prj::open_result prj::open_project(const fs::path& path)
{
	open_result result;

	std::error_code ec;
	const fs::path absolute = fs::absolute(path, ec);
	if (ec) {
		result.error = std::format("cannot resolve project path '{}': {}", fs_paths::utf8_of(path), ec.message());
		return result;
	}
	const fs::path manifest_file = fs::is_directory(absolute, ec) ? absolute / k_manifest_file_name : absolute;

	auto loaded = load_manifest(manifest_file);
	if (!loaded.data) {
		result.error = std::format("cannot open project '{}': {}", fs_paths::utf8_of(manifest_file),
			loaded.error ? to_string(*loaded.error) : std::string{ "unknown error" });
		return result;
	}

	project_context context;
	context.root = fs::canonical(manifest_file.parent_path(), ec);
	if (ec) {
		result.error = std::format("cannot resolve project directory '{}': {}", fs_paths::utf8_of(manifest_file.parent_path()), ec.message());
		return result;
	}
	context.manifest_file = context.root / manifest_file.filename();
	context.data = std::move(*loaded.data);
	context.engine_match = compare_engine_versions(context.data.engine, current_engine_version());

	for (const auto& rel : context.data.content_roots) {
		context.content_roots.push_back((context.root / rel).lexically_normal());
	}
	if (auto problem = verify_project_mounts(context)) {
		result.error = std::format("project '{}': {}", context.data.name, *problem);
		return result;
	}
	for (auto& content_root : context.content_roots) {
		content_root = fs::canonical(content_root, ec);
	}

	result.context = std::move(context);
	return result;
}

int prj::exit_code_for(const held_project& project)
{
	switch (project.answer) {
	case held_project::decision::continue_without_project: return 0;
	case held_project::decision::no_project: return k_exit_project_error;
	case held_project::decision::none:
	case held_project::decision::quit: break;
	}
	return k_exit_engine_version;
}

bool prj::refuse_without_asking(version_action action, bool non_interactive)
{
	const bool held = action == version_action::ask_open_or_quit || action == version_action::refuse;
	return held && non_interactive;
}

namespace {

	constexpr std::array<std::string_view, 4> k_own_top_dirs{ prj::k_code_dir_name, prj::k_snk_dir_name, prj::k_cache_dir_name,
		prj::k_build_dir_name };

	std::string folded(std::string_view text)
	{
		std::string out{ text };
		for (char& ch : out) {
			if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
		}
		return out;
	}

	// `candidate` is `dir` or lies in it: by its parts with letter case folded, and, where `dir` exists, by fs::equivalent
	// of `dir` and each folder from `candidate` up (another spelling of the same folder: .SNK, a short 8.3 name).
	bool lies_in(const fs::path& dir, const fs::path& candidate)
	{
		if (fs_paths::is_inside_any_case(dir, candidate)) {
			return true;
		}
		std::error_code ec;
		if (!fs::exists(dir, ec)) {
			return false;
		}
		for (fs::path at = candidate; !at.empty(); at = at.parent_path()) {
			if (fs::exists(at, ec) && fs::equivalent(dir, at, ec)) {
				return true;
			}
			if (at == at.parent_path()) {
				break;
			}
		}
		return false;
	}

	bool overlap(const fs::path& a, const fs::path& b)
	{
		return lies_in(a, b) || lies_in(b, a);
	}

	// The private dirs as children of the canonical root: project.root may be spelled otherwise.
	std::vector<fs::path> real_private_dirs(const prj::project_context& project, const fs::path& root)
	{
		std::vector<fs::path> out;
		for (const auto& dir : project.private_dirs()) {
			out.push_back(root / dir.filename());
		}
		return out;
	}

	// MSVC's std::filesystem has a file type of its own for a junction.
	bool is_junction(const fs::file_status& status)
	{
#ifdef _MSC_VER
		return status.type() == fs::file_type::junction;
#else
		(void)status;
		return false;
#endif
	}

	// A link at code/ (a symlink or a junction) would send every code:// write wherever it leads.
	std::optional<std::string> verify_code_dir(const prj::project_context& project, const fs::path& root,
		const std::vector<fs::path>& content_roots)
	{
		std::error_code ec;
		const fs::path dir = project.code_dir();
		const auto status = fs::symlink_status(dir, ec);
		if (status.type() == fs::file_type::not_found) {
			return std::nullopt;
		}
		if (ec) {
			return std::format("'{}' cannot be checked: {}", fs_paths::generic_utf8_of(dir), ec.message());
		}
		if (fs::is_symlink(status) || is_junction(status)) {
			return std::format("'{}' is a link; the project's code folder must be a real folder", fs_paths::generic_utf8_of(dir));
		}
		if (!fs::is_directory(status)) {
			return std::format("'{}' is not a directory", fs_paths::generic_utf8_of(dir));
		}
		const fs::path real = fs::canonical(dir, ec);
		if (ec || !is_inside(root, real)) {
			return std::format("'{}' leads outside the project", fs_paths::generic_utf8_of(dir));
		}
		for (const auto& content_root : content_roots) {
			if (overlap(content_root, real)) {
				return std::format("the code folder '{}' overlaps the content root '{}': res:// would serve code or code:// assets",
					fs_paths::generic_utf8_of(dir), fs_paths::generic_utf8_of(content_root));
			}
		}
		for (const auto& private_dir : real_private_dirs(project, root)) {
			if (overlap(private_dir, real)) {
				return std::format("the code folder '{}' overlaps the project's private folder '{}'", fs_paths::generic_utf8_of(dir),
					fs_paths::generic_utf8_of(private_dir));
			}
		}
		return std::nullopt;
	}

}

std::optional<std::string> prj::verify_project_mounts(const project_context& project, const fs::path& engine_base)
{
	std::error_code ec;
	const fs::path root = fs::canonical(project.root, ec);
	if (ec) {
		return std::format("project directory '{}' cannot be resolved: {}", fs_paths::utf8_of(project.root), ec.message());
	}

	// The manifest already refused "..", absolute paths, drives and device names; canonical() also catches a
	// symlink that leads out of the project, which a resolver rooted at the content root would then follow.
	std::vector<fs::path> roots;
	for (const auto& dir : project.content_roots) {
		const fs::path content_root = fs::canonical(dir, ec);
		if (ec || !fs::is_directory(content_root, ec)) {
			return std::format("content root '{}' is not a directory", fs_paths::generic_utf8_of(dir));
		}
		if (!is_inside(root, content_root)) {
			return std::format("content root '{}' leads outside the project", fs_paths::generic_utf8_of(dir));
		}
		// The real path, symlinks resolved, by the rule the manifest checks the written one with: a link to .git too.
		const std::string real_relative = fs_paths::generic_utf8_of(content_root.lexically_relative(root));
		if (const auto problem = content_root_error(real_relative)) {
			const std::string written = fs_paths::generic_utf8_of(dir.lexically_relative(project.root));
			return written == real_relative ? *problem : std::format("{} (content root '{}' leads there)", *problem, written);
		}
		// One file would be reachable under two tags, and a store through the outer root would land in the inner one. The
		// manifest already refuses roots whose paths overlap (#856); here a link may join two that do not.
		for (const auto& other : roots) {
			if (overlap(other, content_root)) {
				return std::format("content roots '{}' and '{}' overlap", fs_paths::generic_utf8_of(other), fs_paths::generic_utf8_of(content_root));
			}
		}
		roots.push_back(content_root);
	}

	// A content root over the engine's resources would make res:// stores write into the engine.
	if (!engine_base.empty()) {
		const fs::path base = fs::weakly_canonical(engine_base, ec);
		if (ec) {
			return std::format("engine resources '{}' cannot be resolved: {}", fs_paths::utf8_of(engine_base), ec.message());
		}
		for (const auto& content_root : roots) {
			if (overlap(base, content_root)) {
				return std::format("content root '{}' overlaps the engine resources '{}'", fs_paths::generic_utf8_of(content_root),
					fs_paths::generic_utf8_of(base));
			}
		}
	}

	for (const auto& dir : real_private_dirs(project, root)) {
		for (const auto& content_root : roots) {
			if (overlap(dir, content_root)) {
				return std::format("content root '{}' overlaps the project's private folder '{}'", fs_paths::generic_utf8_of(content_root),
					fs_paths::generic_utf8_of(dir));
			}
		}
	}

	// A project that ships .cache (or a folder in it) as a symlink would send every memory:// write elsewhere; a
	// symlinked .snk would put the agent token elsewhere.
	auto real_dirs = project.writable_dirs();
	real_dirs.push_back(project.snk_dir());
	for (const auto& dir : real_dirs) {
		const auto status = fs::symlink_status(dir, ec);
		if (status.type() == fs::file_type::not_found) {
			continue;
		}
		if (ec) {
			return std::format("'{}' cannot be checked: {}", fs_paths::generic_utf8_of(dir), ec.message());
		}
		if (fs::is_symlink(status)) {
			return std::format("'{}' is a symlink; the project's writable folders must be real folders", fs_paths::generic_utf8_of(dir));
		}
		if (!fs::is_directory(status)) {
			return std::format("'{}' is not a directory", fs_paths::generic_utf8_of(dir));
		}
		const fs::path real = fs::canonical(dir, ec);
		if (ec || !is_inside(root, real)) {
			return std::format("'{}' leads outside the project", fs_paths::generic_utf8_of(dir));
		}
	}

	if (const auto problem = verify_code_dir(project, root, roots)) {
		return problem;
	}
	return std::nullopt;
}

std::span<const std::string_view> prj::own_top_dir_names()
{
	return k_own_top_dirs;
}

std::optional<std::string> prj::content_root_error(std::string_view relative)
{
	bool first = true;
	for (std::size_t pos = 0; pos < relative.size();) {
		std::size_t end = relative.find('/', pos);
		if (end == std::string_view::npos) {
			end = relative.size();
		}
		const std::string_view part = relative.substr(pos, end - pos);
		pos = end + 1;
		if (part.empty()) {
			continue;
		}
		if (part.front() == '.') {
			return std::format("content root '{}': '{}' is a hidden folder (its name starts with '.'), and a content root may "
				"not be or lie in one", relative, part);
		}
		if (first) {
			const std::string name = folded(part);
			if (std::ranges::any_of(own_top_dir_names(), [&name](std::string_view own) { return folded(own) == name; })) {
				return std::format("content root '{}': '{}' is the project's own folder (code, .snk, .cache, build), and a "
					"content root may not be or lie in one", relative, part);
			}
			if (is_code_ignored_dir(part)) {
				return std::format("content root '{}': '{}' is a build or dependency folder, and a content root may not be or "
					"lie in one", relative, part);
			}
			first = false;
		}
	}
	return std::nullopt;
}
