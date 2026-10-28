#include <snk/project/prj_project.h>
#include <snk/common/fs_paths.h>
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
		result.error = std::format("cannot resolve project path '{}': {}", path.string(), ec.message());
		return result;
	}
	const fs::path manifest_file = fs::is_directory(absolute, ec) ? absolute / k_manifest_file_name : absolute;

	auto loaded = load_manifest(manifest_file);
	if (!loaded.data) {
		result.error = std::format("cannot open project '{}': {}", manifest_file.string(),
			loaded.error ? to_string(*loaded.error) : std::string{ "unknown error" });
		return result;
	}

	project_context context;
	context.root = fs::canonical(manifest_file.parent_path(), ec);
	if (ec) {
		result.error = std::format("cannot resolve project directory '{}': {}", manifest_file.parent_path().string(), ec.message());
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
	return project.answer == held_project::decision::continue_without_project ? 0 : k_exit_engine_version;
}

bool prj::refuse_without_asking(version_action action, bool non_interactive)
{
	const bool held = action == version_action::ask_open_or_quit || action == version_action::refuse;
	return held && non_interactive;
}

std::optional<std::string> prj::verify_project_mounts(const project_context& project, const fs::path& engine_base)
{
	std::error_code ec;
	const fs::path root = fs::canonical(project.root, ec);
	if (ec) {
		return std::format("project directory '{}' cannot be resolved: {}", project.root.string(), ec.message());
	}

	// The manifest already refused "..", absolute paths, drives and device names; canonical() also catches a
	// symlink that leads out of the project, which a resolver rooted at the content root would then follow.
	std::vector<fs::path> roots;
	for (const auto& dir : project.content_roots) {
		const fs::path content_root = fs::canonical(dir, ec);
		if (ec || !fs::is_directory(content_root, ec)) {
			return std::format("content root '{}' is not a directory", dir.generic_string());
		}
		if (!is_inside(root, content_root)) {
			return std::format("content root '{}' leads outside the project", dir.generic_string());
		}
		// One file would be reachable under two tags, and a store through the outer root would land in the inner one.
		for (const auto& other : roots) {
			if (is_inside(other, content_root) || is_inside(content_root, other)) {
				return std::format("content roots '{}' and '{}' overlap", other.generic_string(), content_root.generic_string());
			}
		}
		roots.push_back(content_root);
	}

	// A content root over the engine's resources would make res:// stores write into the engine.
	if (!engine_base.empty()) {
		const fs::path base = fs::weakly_canonical(engine_base, ec);
		if (ec) {
			return std::format("engine resources '{}' cannot be resolved: {}", engine_base.string(), ec.message());
		}
		for (const auto& content_root : roots) {
			if (is_inside(base, content_root) || is_inside(content_root, base)) {
				return std::format("content root '{}' overlaps the engine resources '{}'", content_root.generic_string(),
					base.generic_string());
			}
		}
	}

	for (const auto& dir : project.private_dirs()) {
		for (const auto& content_root : roots) {
			if (is_inside(dir, content_root) || is_inside(content_root, dir)) {
				return std::format("content root '{}' overlaps the project's private folder '{}'", content_root.generic_string(),
					dir.generic_string());
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
			return std::format("'{}' cannot be checked: {}", dir.generic_string(), ec.message());
		}
		if (fs::is_symlink(status)) {
			return std::format("'{}' is a symlink; the project's writable folders must be real folders", dir.generic_string());
		}
		if (!fs::is_directory(status)) {
			return std::format("'{}' is not a directory", dir.generic_string());
		}
		const fs::path real = fs::canonical(dir, ec);
		if (ec || !is_inside(root, real)) {
			return std::format("'{}' leads outside the project", dir.generic_string());
		}
	}
	return std::nullopt;
}
