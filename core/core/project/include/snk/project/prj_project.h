#pragma once
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include <snk/common/snk_api.h>
#include <snk/project/prj_manifest.h>

namespace prj
{
	// The open project. Lives in app_data_storage from before resource_system (which mounts its content roots)
	// until after it, and is absent when the editor runs without a project.
	struct project_context
	{
		// Canonical directory holding the manifest.
		std::filesystem::path root;
		std::filesystem::path manifest_file;
		manifest data;

		// Absolute, highest priority first (the order of the manifest); every one exists and is inside `root`.
		std::vector<std::filesystem::path> content_roots;

		// <root>/.cache: regenerated data, never committed.
		std::filesystem::path cache_dir() const { return root / ".cache"; }
	};

	struct open_result
	{
		std::optional<project_context> context;
		// Set exactly when context is empty: what to tell the user.
		std::string error;
	};

	// `path` is a project.snkproj file or the directory holding one.
	SNK_API open_result open_project(const std::filesystem::path& path);

	// The project given on the command line could not be opened; the host exits with a dedicated code.
	class SNK_API project_error : public std::runtime_error
	{
	public:
		using std::runtime_error::runtime_error;
	};
}
