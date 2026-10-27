#include <snk/project/prj_project.h>
#include <format>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace
{
	// Whole components: "/game/res2" is not inside "/game/res".
	bool is_inside(const fs::path& dir, const fs::path& candidate)
	{
		auto cand_it = candidate.begin();
		for (auto dir_it = dir.begin(); dir_it != dir.end(); ++dir_it, ++cand_it) {
			if (cand_it == candidate.end() || *dir_it != *cand_it) {
				return false;
			}
		}
		return true;
	}
}

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
	if (auto problem = verify_content_roots(context)) {
		result.error = std::format("project '{}': {}", context.data.name, *problem);
		return result;
	}
	for (auto& content_root : context.content_roots) {
		content_root = fs::canonical(content_root, ec);
	}

	result.context = std::move(context);
	return result;
}

std::optional<std::string> prj::verify_content_roots(const project_context& project)
{
	std::error_code ec;
	const fs::path root = fs::canonical(project.root, ec);
	if (ec) {
		return std::format("project directory '{}' cannot be resolved: {}", project.root.string(), ec.message());
	}
	// The manifest already refused "..", absolute paths, drives and device names; canonical() also catches a
	// symlink that leads out of the project, which a resolver rooted at the content root would then follow.
	for (const auto& dir : project.content_roots) {
		const fs::path content_root = fs::canonical(dir, ec);
		if (ec || !fs::is_directory(content_root, ec)) {
			return std::format("content root '{}' is not a directory", dir.generic_string());
		}
		if (!is_inside(root, content_root)) {
			return std::format("content root '{}' leads outside the project", dir.generic_string());
		}
	}
	return std::nullopt;
}
