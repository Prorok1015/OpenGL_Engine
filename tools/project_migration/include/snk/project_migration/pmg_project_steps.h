#pragma once
#include <snk/project_migration/pmg_assets.h>
#include <snk/project_migration/pmg_scan.h>
#include <boost/json/object.hpp>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Steps of a whole project, beside the steps of each type (EPIC-58 3.3, #754): what no single file can do on its own.
namespace prj::migration
{
	// pre runs before the files are migrated, post after them.
	enum class project_phase { pre, post };

	std::string_view to_string(project_phase phase);

	// The project's files as a step sees and changes them (EPIC-58 3.3, S5 #756). Paths are relative to the project
	// root with '/', inside a content root; project.snkproj is the context's `manifest`, never a file here. Every
	// change is backed up and in the journal before it is made, so a rollback undoes it; a failure throws, and the
	// whole migration is rolled back.
	class project_step_files
	{
	public:
		// The file as this migration left it so far (a step's or a type's write), else as it is on the disk. Throws when
		// it is not there.
		virtual std::string read(std::string_view path) = 0;
		virtual bool exists(std::string_view path) = 0;
		// Replaces an existing file (to drop a key, write the file without it).
		virtual void write(std::string_view path, std::string_view text) = 0;
		// A new file; refused when anything is at its name. The rollback deletes it.
		virtual void create(std::string_view path, std::string_view text) = 0;
		// Deletes an existing file; the rollback puts it back.
		virtual void remove(std::string_view path) = 0;

	protected:
		~project_step_files() = default;
	};

	// What a project step may read and change: project.snkproj, held in memory and written once after the post
	// steps with the stamp of the migration, the project's files and its assets.
	struct project_step_context
	{
		const std::filesystem::path& root;
		const scan_result& scan;
		boost::json::object& manifest;
		// Lines for the report.
		std::vector<std::string>& notes;
		project_step_files& files;
		// The project's assets as the .resource files on the disk say; rebuilt after the pre steps (before the
		// files are migrated). A step that changes .resource files and reads them back calls rebuild() itself.
		project_assets& assets;
		std::string_view step_id;
		// The game layer the migration runs with: the schemas a step reads typed objects by (assets.refs).
		const desc::desc_system* descs = nullptr;

		// A line of the report under the step's id.
		void warn(std::string_view text) { notes.push_back(std::string(step_id) + ": " + std::string(text)); }
	};

	struct project_step
	{
		// "<owner>.<what>", unique; it is what project.snkproj keeps in applied_steps.
		std::string id;
		project_phase phase = project_phase::pre;
		std::string summary;
		// Whether the project needs the step, from its scan alone; it must be false once the step ran.
		bool (*needed)(const scan_result& scan) = nullptr;
		void (*apply)(project_step_context& context) = nullptr;
	};

	// The steps in the order they were added: the engine's first, then each module's (EPIC-58 3.3).
	class project_step_registry
	{
	public:
		// Refuses an id that is taken or a step without needed/apply (std::invalid_argument).
		void add(project_step step);
		const project_step* find(std::string_view id) const;
		const std::vector<project_step>& steps() const { return m_steps; }

	private:
		std::vector<project_step> m_steps;
	};

	// Whether the desc `root` (the file at `path`) is one engine.geometry_blobs converts: a geometry_desc whose vertices
	// and indices are JSON arrays with at least one vertex, of triangles (no "topology" other than that) and named
	// <name>.<ext>.desc, so that a binary file can stand beside it (EPIC-79, #1129).
	bool holds_convertible_geometry(std::string_view path, const boost::json::object& root);

	// What a migration writes at most for one file (scan_options::max_file_bytes, the applier's limit).
	inline constexpr std::uint64_t k_geometry_binary_max_bytes = std::uint64_t{ 64 } << 20;

	// engine.manifest_format (pre): brings format_version of project.snkproj to prj::k_manifest_format_version.
	// engine.geometry_blobs (post, EPIC-79 #1129): every desc the scan marks geometry_arrays gets its vertices and indices
	// in <name>.<ext>.bin beside it (created) and keeps their ranges (written), as rnd::encode_geometry_files makes the
	// pair. A desc it cannot convert (a .bin or .bin.new already at the name, data the loader would refuse, a binary file
	// over k_geometry_binary_max_bytes) stays as it is with a line "<path>: not migrated: <why>" in the report.
	void register_engine_project_steps(project_step_registry& registry);
}
