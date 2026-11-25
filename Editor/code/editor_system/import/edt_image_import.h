#pragma once
#include "edt_file_identity.h"
#include "edt_model_import_limits.h"
#include <snk/command/cmd_types.h>
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/model_codec/res_asset_id.h>
#include <snk/model_codec/res_tag.h>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The import of a picture into the project (#1193, docs/epics/editor/EPIC-62.md): the file is copied as it is,
// its `.resource` carries importer "texture" and the settings, and a `<name>.txm.desc` beside it (with a `.resource`
// of its own) holds the texture header the settings make. No ImGui here: the agent's command, the asset browser's
// window and the tests share it.
namespace edt
{
	class job_service;

	enum class image_wrap : std::uint8_t { clamp_to_edge, repeat, repeat_mirror, clamp_to_border };
	enum class image_filter : std::uint8_t { nearest, linear };

	// The `settings` of a `.resource` whose importer is "texture", version 1. The defaults are base_texture.desc's.
	struct image_import_settings
	{
		image_wrap wrap = image_wrap::clamp_to_edge;
		image_filter filter = image_filter::nearest;
		bool mips = false;

		bool operator==(const image_import_settings&) const = default;
	};

	inline constexpr std::string_view k_image_importer = "texture";
	inline constexpr std::int64_t k_image_settings_version = 1;
	// The loader clamps mip_levels to the full chain of the picture and generates the levels.
	inline constexpr std::int64_t k_image_full_mip_chain = 32;
	inline constexpr std::string_view k_image_default_folder = "textures";
	inline constexpr std::string_view k_image_texture_suffix = ".txm.desc";

	std::string_view image_wrap_name(image_wrap wrap);
	std::string_view image_filter_name(image_filter filter);

	struct image_settings_parse
	{
		image_import_settings settings;
		std::string error;
		bool ok() const { return error.empty(); }
	};

	// Strict: an object; a key it does not know, a wrong type or value is an error naming the key; a missing key takes
	// its default; `version`, when given, must be 1. A null value is all defaults.
	image_settings_parse parse_image_settings(const boost::json::value& value);
	// What the `.resource` holds: {"version", "wrap", "filter", "mips"}.
	boost::json::object image_settings_json(const image_import_settings& settings);
	// The texture desc beside the picture: never header.type, usage, extent or format (the loader takes them from the
	// picture, #926).
	boost::json::object image_texture_desc_json(const res::tag& image, std::string_view name,
		const image_import_settings& settings);

	// DRIVE_REMOVABLE and DRIVE_FIXED of GetDriveTypeW: the only drive types an agent's source may lie on.
	inline constexpr unsigned k_drive_removable = 2;
	inline constexpr unsigned k_drive_fixed = 3;

	// GetDriveTypeW of "X:\" (the local drive table only, no network round trip); 0 (DRIVE_UNKNOWN) off Windows.
	unsigned system_drive_type(char letter);
	// The text of /proc/self/mounts; nullopt when it cannot be read (and on Windows).
	std::optional<std::string> system_mount_table();

	// How agent_source_path_problem tells a local volume from another one, without touching the path.
	struct source_volume_ports
	{
#if defined(_WIN32)
		bool windows = true;
#else
		bool windows = false;
#endif
		// Windows rules: the type of a drive by its letter; null: unknown.
		std::function<unsigned(char letter)> drive_type = system_drive_type;
		// POSIX rules: the mount table; null or nullopt: it cannot be read, every path is refused.
		std::function<std::optional<std::string>()> mount_table = system_mount_table;
	};

	// The allowed source roots of an agent's import: only local fixed or removable volumes. Why `path` (as an agent
	// gave it) may not be read for an agent; empty when it may. The text alone decides, plus the drive table or the
	// mount table: the path itself is never looked at. Refused: UNC and device paths (two leading separators,
	// `\??\`), a `.` or `..` component; Windows: no drive letter, a drive that is not DRIVE_FIXED/DRIVE_REMOVABLE;
	// POSIX: a path whose longest mount point is a network or automount one (agent_log_mounts).
	std::string agent_source_path_problem(std::string_view path, const source_volume_ports& ports);
	std::string agent_source_path_problem(std::string_view path);

	// What a caller asks for: the person's window or file.import_image.
	struct image_import_request
	{
		std::string path;	// absolute, on the disk, anywhere
		// Below res://; nullopt: k_image_default_folder; "": the root.
		std::optional<std::string> folder;
		// The file name in the project; empty: the source's name. Without a picture extension the source's is added.
		std::string name;
		// The settings as JSON (parse_image_settings); null: the defaults.
		boost::json::value settings;
	};

	// The request resolved: the file a person approves and the four files the import writes. An agent's plan made
	// for the question holds the source as given and no size, time or identity: nothing on its disk was looked at.
	struct image_import_plan
	{
		std::filesystem::path source;	// absolute; on the disk: resolved, no link on the way
		bool on_disk = false;
		std::uintmax_t size = 0;
		std::filesystem::file_time_type modified{};
		file_id id{};
		std::string folder;	// below res://, no protocol; empty: the root
		std::string name;	// file name of the picture in the project
		res::tag image;
		res::tag meta;
		res::tag texture;
		res::tag texture_meta;	// the texture desc's own `.resource`
		image_import_settings settings;
		// Set when refused: the command code (invalid_args, not_found, needs_input, busy) and why.
		std::string code;
		std::string error;

		bool ok() const { return error.empty(); }
		// The same files with the same settings.
		bool same_as(const image_import_plan& other) const;
	};

	struct image_import_written
	{
		res::tag image;
		res::tag meta;
		res::tag texture;
		res::tag texture_meta;
		res::asset_id guid;
		res::asset_id texture_guid;
		std::string error;
		bool ok() const { return error.empty(); }
	};

	struct image_import_options
	{
		// The folders res:// is read from, lowest first: a name taken in any of them is refused.
		std::function<std::vector<std::filesystem::path>()> resource_layers;
		// The project's own (top) layer: the files are written there.
		std::function<std::filesystem::path()> resources_path;
		// Whether a project is open; null or false: every import is refused (never into the engine's layer).
		std::function<bool()> project_open;
		// Why nothing may be imported now (Play, a migration, the model import's dialog); null or nullopt: it may.
		std::function<std::optional<std::string>()> blocker;
		// Main thread, after a job wrote the four files: the asset database and the resource cache are told.
		std::function<void(const std::vector<res::tag>&)> written;
		// For the lifetime of what a person was shown; null: steady_clock.
		std::function<std::chrono::steady_clock::time_point()> now;
		// The largest picture an agent may import; a person's call may bring k_max_person_source_bytes.
		std::uintmax_t max_agent_bytes = k_max_agent_source_bytes;
		std::uintmax_t max_person_bytes = k_max_person_source_bytes;
		// Where an agent's source may lie (agent_source_path_problem); tests fake a remote drive or mount.
		std::function<unsigned(char letter)> drive_type = system_drive_type;
		std::function<std::optional<std::string>()> mount_table = system_mount_table;
		// Tests only: called before each write ("meta", "texture_meta", "image", "texture"); true fails that write.
		std::function<bool(std::string_view step)> fail_step;
	};

	class image_importer
	{
	public:
		image_importer(job_service& jobs, image_import_options options);
		~image_importer();
		image_importer(const image_importer&) = delete;
		image_importer& operator=(const image_importer&) = delete;

		bool running() const;

		// Resolves the request without reading the picture (a stat and the folders of every layer): the source, the
		// settings, the names and whether they are free. `person`: the person's own size limit and no source rule;
		// else agent_source_path_problem first, on the path given and on the path resolved. It looks at the source on
		// the disk: for an agent's call only once a person approved it (start).
		image_import_plan resolve(const image_import_request& request, bool person) const;

		// What the person is asked for an agent's call: the source as given, the four files and the settings; or why
		// the call will be refused. Never looks at the source on the disk (only the project's layers). Remembers the
		// plan for the start that follows. A person's call is described from the disk.
		std::optional<std::string> describe(const image_import_request& request, const cmd::origin& caller);

		struct started
		{
			std::uint64_t job = 0;
			std::string code;	// empty when the job started
			std::string error;
			bool ok() const { return code.empty(); }
		};
		// Anyone but a person needs the plan describe remembered for exactly this call: the command registry runs a
		// call the policy asks about only with an approved ticket, and describes every allowed call first
		// (describe_before_allowed_invoke). The job reads the picture, proves it decodes and writes the files; its
		// result is {image, resource, texture, texture_resource, guid, texture_guid}.
		started start(const image_import_request& request, const cmd::origin& caller);

		// Empty when `bytes` are a png or jpeg (matching `extension`) that decodes within the picture adapter's limits.
		static std::string image_problem(std::span<const std::byte> bytes, std::string_view extension);

		// Writes the picture's `.resource` (a new guid, the recipe), the texture desc's `.resource` (another new guid,
		// no importer: an output), the picture's bytes and the texture desc, in that order, below `top`; each one new
		// (never over a file) and never through a link. On a failure the files already written are removed, newest
		// first. An error names a file below `top` as res:// and holds no absolute path.
		static image_import_written write_files(const std::filesystem::path& top, const image_import_plan& plan,
			std::span<const std::byte> bytes, const std::function<bool(std::string_view)>& fail_step = {});

		// `text` with every spelling of each of `roots` read as res:// and of `source` as its file name: what an error
		// that reaches an agent may hold of this machine's paths.
		static std::string without_paths(std::string text, const std::vector<std::filesystem::path>& roots,
			const std::filesystem::path& source);

		static constexpr std::string_view k_extensions[] = { ".png", ".jpg", ".jpeg" };
		static constexpr std::size_t k_max_shown = 32;
		static constexpr std::chrono::minutes k_shown_lifetime{ 5 };

	private:
		struct shown_plan
		{
			std::string key;
			std::string session;
			image_import_plan plan;
			std::chrono::steady_clock::time_point at;
		};

		std::chrono::steady_clock::time_point now() const;
		source_volume_ports volumes() const;
		// Everything of a plan but the source: settings, names, free names, the blockers.
		void plan_targets(const image_import_request& request, std::string_view source_name,
			image_import_plan& plan) const;
		// An agent's plan for the question, from the text of the request only.
		image_import_plan text_plan(const image_import_request& request) const;
		void remember(std::string key, std::string session, image_import_plan plan);
		std::optional<image_import_plan> take(const std::string& key);

		job_service& m_jobs;
		image_import_options m_options;
		std::vector<shown_plan> m_shown;
		std::uint64_t m_job = 0;
		// Last: destroyed first, so a job callback cannot run on a half-destroyed importer.
		ds::lifetime m_lifetime{ ds::lifetime_option::cancel_on_destroy };
	};

	// What the asset browser's "Import Image" window holds between frames, without ImGui: the picked file, the folder,
	// the settings, the file name and what is wrong with them.
	class image_import_form
	{
	public:
		explicit image_import_form(image_importer& importer) : m_importer(importer) {}

		void open(std::string folder, std::filesystem::path source);
		const std::string& folder() const { return m_folder; }
		const std::filesystem::path& source() const { return m_source; }

		const image_import_settings& settings() const { return m_settings; }
		void set_wrap(image_wrap wrap) { m_settings.wrap = wrap; }
		void set_filter(image_filter filter) { m_settings.filter = filter; }
		void set_mips(bool mips) { m_settings.mips = mips; }

		const std::string& name() const { return m_name; }
		// Checks the names again (the folders are walked then, not at every frame).
		void set_name(std::string name);

		// The four files the import writes, as res:// tags; empty while the name is refused.
		std::vector<std::string> final_names() const;
		const std::string& problem() const { return m_problem; }
		bool can_submit() const { return !m_source.empty() && m_problem.empty(); }
		const std::string& error() const { return m_error; }

		// Starts the import as the person's own call; the job's id, else error() says why.
		std::optional<std::uint64_t> submit();

	private:
		image_import_request request() const;
		void check();

		image_importer& m_importer;
		std::string m_folder;
		std::filesystem::path m_source;
		image_import_settings m_settings;
		std::string m_name;
		image_import_plan m_plan;
		std::string m_problem;
		std::string m_error;
	};
}
