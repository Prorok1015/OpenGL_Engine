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

	// Why engine.geometry_blobs would leave such a desc as it is, by the writer's own checks: the loader's checks of
	// rnd::encode_geometry_files refuse its data (no indices, a count not a multiple of 3, an index past the vertices,
	// a value that is not finite, no "layout" of its own), its stored bounds are not those the loader builds, the binary
	// file would be over k_geometry_binary_max_bytes, or the pair would not read back. Empty when it converts. The scan
	// asks it, so such a desc is "not migrated" with this reason and never keeps the step needed (#1129).
	std::string geometry_conversion_problem(std::string_view path, const boost::json::object& root);

	// The JSON pointers of the material_desc objects at version 1 under `root` (in the order of a walk of the file) whose
	// shader stages rnd::migrate_material_v1 refuses, `id_of` answering its migration_context::id_of (#1200).
	std::vector<std::string> refused_material_stages(const boost::json::object& root, const desc::migration_id_lookup& id_of);

	// Why engine.material_shaders would leave `material` (one refused_material_stages lists, in the desc at `path` of the
	// project under `root`) as it is, resolved as the step resolves it through `assets` and the files on the disk: its
	// __parent chain does not read, a vertex or a fragment stage is missing, or its triple needs a new file and the asset
	// walk does not list the desc. Empty when it converts. The scan makes such a desc invalid with it, so --force leaves
	// it and the rest of the project migrates.
	std::string material_shaders_problem(const std::filesystem::path& root, const project_assets& assets, const std::string& path,
		const boost::json::object& material);

	// What a migration writes at most for one file (scan_options::max_file_bytes, the applier's limit).
	inline constexpr std::uint64_t k_geometry_binary_max_bytes = std::uint64_t{ 64 } << 20;

	// engine.manifest_format (pre): brings format_version of project.snkproj to prj::k_manifest_format_version.
	// engine.geometry_blobs (post, EPIC-79 #1129): every desc the scan marks geometry_arrays gets its vertices and indices
	// in <name>.<ext>.bin beside it (created) and keeps their ranges (written), as rnd::encode_geometry_files makes the
	// pair. A .bin already at the name with exactly the bytes it would write (a run stopped between creating it and its
	// journal) is taken as it is. A desc it cannot convert then (a .bin.new or another .bin at the name, a check that the
	// type migrations made fail) stays as it is with a line "<path>: not migrated: <why>" in the report.
	// engine.material_shaders (pre, #1200): every material the scan marks material_stages (a material_desc file or an
	// inline one, e.g. a mesh node's in a prefab) gets its effective stages (its own shader_* keys, the others from its
	// __parent chain through the project and the read-only layers) as a reference "shader" instead of its shader_* keys;
	// __version stays, the type step then stamps v2. A stage is written as the material wrote it ({guid, path} kept). One
	// .shader.desc per distinct triple, beside the first material (by path) that uses it, <desc stem>.shader.desc, with
	// -2, -3... while the res:// path (or its .resource) is taken in any content root or read-only layer: nothing is
	// overwritten or shadowed. Each gets a .resource of a new ID; a triple that is one of the engine's shaders takes the
	// engine's file. The report lists every file and the materials that point to it. A material the step could not
	// convert is invalid in the scan already (material_shaders_problem), so it never reaches the step.
	void register_engine_project_steps(project_step_registry& registry);
}
