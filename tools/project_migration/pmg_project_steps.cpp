#include <snk/project_migration/pmg_project_steps.h>
#include <snk/project/prj_manifest.h>
#include <snk/render/geom/rnd_geometry_files.h>
#include <snk/resource/res_desc_binary.h>
#include <snk/resource/res_tag.h>
#include <boost/json.hpp>
#include <algorithm>
#include <cstddef>
#include <exception>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
	using namespace prj::migration;

	bool manifest_format_needed(const scan_result& scan)
	{
		return scan.manifest.format_version < prj::k_manifest_format_version;
	}

	void manifest_format_apply(project_step_context& context)
	{
		const boost::json::value* before = context.manifest.if_contains("format_version");
		const std::string was = before ? boost::json::serialize(*before) : std::string{ "none" };
		context.manifest["format_version"] = prj::k_manifest_format_version;
		context.notes.push_back(std::format("project.snkproj: format_version {} -> {}", was, prj::k_manifest_format_version));
	}

	bool geometry_blobs_needed(const scan_result& scan)
	{
		return std::ranges::any_of(scan.files, &scanned_file::geometry_arrays);
	}

	// The pair engine.geometry_blobs writes for the desc `root` (the file at `path`), or why it leaves the desc as it is.
	struct prepared_geometry
	{
		rnd::geometry_desc geometry;
		rnd::geometry_files pair;
		std::string problem;
	};

	std::string_view text_of(const std::vector<std::byte>& bytes)
	{
		return { reinterpret_cast<const char*>(bytes.data()), bytes.size() };
	}

	// Why the pair would not give the data it was made of back, as the loader decodes it; empty when it does.
	std::string read_back_problem(const res::tag& tag, const rnd::geometry_files& pair, const rnd::geometry_desc& geometry)
	{
		boost::system::error_code error;
		const boost::json::value desc = boost::json::parse(text_of(pair.desc), error);
		if (error || !desc.is_object()) return "its new desc would not be a JSON object";
		rnd::geometry_desc read;
		if (std::string problem = rnd::decode_geometry_files(tag, desc.as_object(), pair.binary, read); !problem.empty()) {
			return std::format("its new pair would not read back: {}", problem);
		}
		if (read.vertices != geometry.vertices || read.indices != geometry.indices || read.layout.get_stride() != geometry.layout.get_stride()) {
			return "its new pair would read back other data";
		}
		return {};
	}

	prepared_geometry prepare_geometry(std::string_view path, const boost::json::object& root)
	{
		prepared_geometry out;
		const res::tag tag{ path };
		if (std::string problem = rnd::decode_geometry_files(tag, root, {}, out.geometry); !problem.empty()) {
			out.problem = std::move(problem);
			return out;
		}
		// A desc with ranges stores no bounds, the loader builds them: stored ones that differ would be lost silently.
		if (!out.geometry.bounds.empty() && out.geometry.bounds != rnd::built_geometry_bounds(out.geometry)) {
			out.problem = "its stored bounds are not those the loader builds from its data, which a desc with a binary file keeps";
			return out;
		}
		rnd::geometry_file_options file_options;
		// The header (__guid, __parent, ...) as the file has it; the data stays out of the copy, the writer drops it anyway.
		for (const auto& member : root) {
			const std::string_view key = member.key();
			if (key == "vertices" || key == "indices" || key == "bounds" || key == "layout") continue;
			file_options.header[key] = member.value();
		}
		file_options.bounds = !out.geometry.bounds.empty();
		// The checks of the loader (rnd::geometry_write_problem) run in the encoder: refused data writes nothing.
		out.pair = rnd::encode_geometry_files(tag, out.geometry, file_options);
		if (!out.pair) {
			out.problem = out.pair.error;
		} else if (out.pair.binary.size() > k_geometry_binary_max_bytes) {
			out.problem = std::format("its binary file would be {} bytes, over the {} a migration writes", out.pair.binary.size(),
				k_geometry_binary_max_bytes);
		} else {
			out.problem = read_back_problem(tag, out.pair, out.geometry);
		}
		return out;
	}

	// Whether anything is at `path`; a name that cannot be checked (too large, unreadable) counts as taken.
	std::optional<std::string> taken(project_step_files& files, const std::string& path)
	{
		try {
			if (!files.exists(path)) return std::nullopt;
			return std::format("{} is already beside it", path);
		} catch (const std::exception& e) {
			return std::format("{} cannot be checked: {}", path, e.what());
		}
	}

	enum class geometry_outcome { converted, binary_kept };

	struct converted_geometry
	{
		geometry_outcome outcome = geometry_outcome::converted;
		// Why the desc stays as it is; empty when converted.
		std::string reason;
	};

	// Writes the pair of one desc. Nothing is written before every check passed.
	converted_geometry convert_geometry(project_step_files& files, const std::string& path)
	{
		const res::tag tag{ path };
		const std::optional<res::tag> binary = res::desc_binary::binary_tag(tag);
		const std::optional<res::tag> staged = res::desc_binary::staged_tag(tag);
		if (!binary || !staged) return { {}, "not named <name>.<ext>.desc: no binary file can stand beside it" };
		const std::string binary_path(binary->view());
		if (std::optional<std::string> reason = taken(files, std::string(staged->view()))) return { {}, *reason };

		const std::string read = files.read(path);
		std::string_view text = read;
		if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
		boost::system::error_code error;
		boost::json::parse_options options;
		options.max_depth = 128;
		const boost::json::value root = boost::json::parse(text, error, {}, options);
		if (error || !root.is_object()) return { {}, "no JSON object after the migration of its type" };
		const boost::json::object& desc = root.as_object();
		// The type migrations ran before: what the scan saw may be gone.
		if (!holds_convertible_geometry(path, desc)) return { {}, "no longer triangles in JSON arrays after the migration of its type" };
		const prepared_geometry prepared = prepare_geometry(path, desc);
		if (!prepared.problem.empty()) return { {}, prepared.problem };

		// The applier creates a file before its journal names it: a run stopped between the two leaves this very binary file
		// beside an unchanged desc, which the next run takes as it is.
		bool keep_binary = false;
		if (std::optional<std::string> reason = taken(files, binary_path)) {
			std::string beside;
			try {
				beside = files.read(binary_path);
			} catch (const std::exception&) {
				return { {}, *reason };
			}
			if (beside != text_of(prepared.pair.binary)) return { {}, *reason };
			keep_binary = true;
		}
		if (!keep_binary) files.create(binary_path, text_of(prepared.pair.binary));
		files.write(path, text_of(prepared.pair.desc));
		return { keep_binary ? geometry_outcome::binary_kept : geometry_outcome::converted, {} };
	}

	void geometry_blobs_apply(project_step_context& context)
	{
		std::size_t converted = 0;
		std::size_t left = 0;
		for (const scanned_file& file : context.scan.files) {
			if (!file.geometry_arrays) continue;
			const converted_geometry done = convert_geometry(context.files, file.path);
			if (!done.reason.empty()) {
				context.warn(std::format("{}: not migrated: {}", file.path, done.reason));
				++left;
				continue;
			}
			const std::string_view binary = res::desc_binary::binary_tag(res::tag{ file.path })->view();
			context.warn(done.outcome == geometry_outcome::binary_kept
				? std::format("{}: vertices and indices moved to {}, which an interrupted run had already written", file.path, binary)
				: std::format("{}: vertices and indices moved to {}", file.path, binary));
			++converted;
		}
		context.warn(std::format("{} geometry desc files with a binary file, {} not migrated", converted, left));
	}
}

bool prj::migration::holds_convertible_geometry(std::string_view path, const boost::json::object& root)
{
	const boost::json::value* type = root.if_contains("__type");
	if (!type || !type->is_string() || type->as_string() != "geometry_desc") return false;
	const boost::json::value* vertices = root.if_contains("vertices");
	const boost::json::value* indices = root.if_contains("indices");
	// No vertices: no mesh to write (the writer refuses an empty one), the arrays stay valid as they are.
	if (!vertices || !vertices->is_array() || vertices->as_array().empty() || !indices || !indices->is_array()) return false;
	if (rnd::topology_of(root) != rnd::geometry_topology::triangles) return false;
	return res::desc_binary::binary_tag(res::tag{ path }).has_value();
}

std::string prj::migration::geometry_conversion_problem(std::string_view path, const boost::json::object& root)
{
	return prepare_geometry(path, root).problem;
}

std::string_view prj::migration::to_string(project_phase phase)
{
	return phase == project_phase::pre ? "pre" : "post";
}

void prj::migration::project_step_registry::add(project_step step)
{
	if (step.id.empty() || !step.needed || !step.apply) {
		throw std::invalid_argument(std::format("project step '{}' needs an id, needed and apply", step.id));
	}
	if (find(step.id)) throw std::invalid_argument(std::format("project step '{}' is already registered", step.id));
	m_steps.push_back(std::move(step));
}

const prj::migration::project_step* prj::migration::project_step_registry::find(std::string_view id) const
{
	const auto found = std::ranges::find(m_steps, id, &project_step::id);
	return found == m_steps.end() ? nullptr : &*found;
}

void prj::migration::register_engine_project_steps(project_step_registry& registry)
{
	project_step format;
	format.id = "engine.manifest_format";
	format.phase = project_phase::pre;
	format.summary = std::format("project.snkproj to format_version {}", prj::k_manifest_format_version);
	format.needed = &manifest_format_needed;
	format.apply = &manifest_format_apply;
	registry.add(std::move(format));

	// Post: the type migrations come first (geometry_desc 1 -> 2), the step reads the file as they left it.
	project_step geometry;
	geometry.id = "engine.geometry_blobs";
	geometry.phase = project_phase::post;
	geometry.summary = "vertices and indices of geometry descs from JSON arrays to a binary file beside the desc";
	geometry.needed = &geometry_blobs_needed;
	geometry.apply = &geometry_blobs_apply;
	registry.add(std::move(geometry));
}
