#pragma once
#include "mdl_api.h"
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/model_codec/res_desc_binary_codec.h>
#include <snk/model_codec/res_tag.h>
#include <boost/json/object.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The one reading of a model file (assimp) into the engine's descs (#1108): the engine's resource adapter and the
// editor's import (in the decoder process) both call it, each with its own files, its own place to write and its names.
namespace mdl
{
	// A model can name any number of textures it does not ship: the import lists this many, and cuts each name to
	// this many bytes (on a code point), so the answer of the decoder stays small (edt_decode_codec takes the same limits).
	inline constexpr std::size_t k_max_missing_textures = 256;
	inline constexpr std::size_t k_max_missing_texture_name_bytes = 1024;

	// What a model may hold (#1119): a scene past any of these is refused with failure::too_large before anything is written.
	inline constexpr std::uint64_t k_max_scene_nodes = 65536;
	// A desc is parsed with at most 128 JSON levels (max_depth in core/core/resource/res_json_body.cpp and
	// core/core/desc/desc_system.cpp). A prefab node adds about two (its object and its children array) plus its
	// components, so a deeper tree could be written but not read back. The root is level 1.
	inline constexpr std::uint64_t k_max_scene_depth = 48;
	inline constexpr std::uint64_t k_max_scene_meshes = 65536;
	// The geometry file's limit alone would refuse a big model only after the whole geometry was built in memory, and
	// vertices of position only (12 bytes, the smallest scn::import_layout) fit it far past this count.
	inline constexpr std::uint64_t k_max_scene_vertices = 16'777'216;
	// Face indices of every mesh; each is a u32 in the geometry's binary file.
	inline constexpr std::uint64_t k_max_scene_indices = 48ull * 1024 * 1024;
	static_assert(k_max_scene_indices * sizeof(std::uint32_t) <= res::desc_binary::max_file_bytes);
	static_assert(k_max_scene_vertices * 3 * sizeof(float) <= res::desc_binary::max_file_bytes);

	// A parsed scene as the limits above count it.
	struct scene_size
	{
		std::uint64_t nodes = 0;    // the root included
		std::uint64_t depth = 0;    // of the deepest node
		std::uint64_t meshes = 0;
		std::uint64_t vertices = 0;
		std::uint64_t indices = 0;
	};

	// Empty: `size` is within every limit. Else the first limit it passes, with the count: "too many nodes: 70112 (limit 65536)".
	SNK_MODEL_IMPORT_API std::string scene_size_problem(const scene_size& size);

	// A file the model names, opened for assimp.
	struct opened_file
	{
		// Null: not readable. Owned by the source, valid as long as the source lives.
		const std::vector<std::byte>* bytes = nullptr;
		// Why not, for the log; empty: the file is simply not there.
		std::string reason;
	};

	// The bytes of an external texture the import copies (texture_mode::copy).
	struct texture_bytes
	{
		// Nullopt: missing or not readable.
		std::optional<std::vector<std::byte>> bytes;
		std::string reason;
	};

	// How the import reads the model and the files beside it. Every call may throw to stop the import (the editor
	// refuses a file it may not read that way): the exception leaves import_model as it is.
	class file_source
	{
	public:
		virtual ~file_source() = default;

		// The model itself (by the name import_model was given) or a file it names, as written in the model.
		virtual opened_file open(std::string_view name) = 0;
		// Whether `name` can be opened: some loaders ask first and skip a file that is not there.
		virtual bool exists(std::string_view name) = 0;
		// texture_mode::copy: the name the texture is stored under (the folder of the copies + it); `name` as written.
		virtual std::string texture_key(std::string_view name) { return std::string{ name }; }
		// texture_mode::copy: the bytes of the texture texture_key was asked for.
		virtual texture_bytes read_texture(std::string_view name) { static_cast<void>(name); return {}; }
	};

	// Where the import writes its resources (memory://).
	class import_sink
	{
	public:
		virtual ~import_sink() = default;

		// True: the tag is there already; the import refers to it and does not write it.
		virtual bool exists(const res::tag& tag) = 0;
		virtual void store(const res::tag& tag, const std::vector<std::byte>& bytes) = 0;
	};

	enum class material_mode
	{
		inline_desc,    // in the mesh node's component (the engine's adapter)
		separate_files, // <material_folder><name>.mat.desc, referred to by tag (the editor's import)
	};

	enum class texture_mode
	{
		reference_beside_model, // <texture_folder><name>.txm.desc naming the file beside the model (model_tag)
		copy,                   // the bytes into <texture_folder><key>, its desc <texture_folder><key>.txm.desc
	};

	struct import_options
	{
		// Where the geometry desc goes (its binary file beside it) and the prefab the caller stores (skinning names it).
		res::tag geometry_tag;
		res::tag prefab_tag;
		// memory:// folders, each empty or ending in '/'.
		std::string embedded_folder;
		std::string texture_folder;
		std::string material_folder;
		material_mode materials = material_mode::inline_desc;
		texture_mode textures = texture_mode::reference_beside_model;
		// texture_mode::reference_beside_model: the model's tag, the textures it names are read beside it.
		res::tag model_tag;
		// How the log names the model: never an absolute path (agents read the log).
		std::string display_name;
		// Null: not cancellable. Checked between the steps and by assimp while it parses; a stop throws ds::task_cancelled.
		ds::task_context* task = nullptr;
		// Zero: no limit. Assimp is told to stop when it passes, and the import fails with failure::timed_out.
		std::chrono::steady_clock::time_point deadline{};
	};

	enum class failure
	{
		none,
		timed_out, // past import_options::deadline
		parse,     // assimp could not read the model; `error` is what it said
		geometry,  // the geometry would be refused by the loader; `error` names the check
		too_large, // past a k_max_scene_* limit; `error` names it and the count
	};

	struct import_result
	{
		// "__type": "prefab_desc" and the tree, not stamped: the caller's desc system stamps and stores it.
		boost::json::object prefab;
		res::tag geometry_tag;
		res::tag prefab_tag;
		// Every memory:// resource the import refers to (embedded textures, textures, materials, the geometry desc), once
		// each; not the geometry's binary file nor the prefab.
		std::vector<res::tag> tags;
		// result[vertex] = [bone indices..., weights as uint32...] (what skin_weights_desc holds).
		std::vector<std::vector<std::uint32_t>> skin_weights;
		// External textures as the model names them that were not loaded: the material has no sampler for them. At most
		// k_max_missing_textures, each cut to k_max_missing_texture_name_bytes; the rest only counted.
		std::vector<std::string> missing_textures;
		std::size_t missing_textures_not_listed = 0;
		failure failed = failure::none;
		// Empty: the import succeeded.
		std::string error;
	};

	// Reads the model `model_name` (opened through `files`) and writes its geometry, materials and textures through
	// `sink`. Never throws for a problem of the model (import_result::error); what `files`, `sink` or a stop request
	// throws passes through.
	SNK_MODEL_IMPORT_API import_result import_model(std::string_view model_name, file_source& files, import_sink& sink,
		const import_options& options);

	// The files a model refers to, read without post-processing (the editor's scans).
	struct referenced_files
	{
		failure failed = failure::none;
		std::string error;
		// External textures (diffuse, base color, specular, normals) as written, per material; embedded ones left out.
		std::vector<std::string> textures;
	};

	// Parses `model_name` (its side files through `files`) and lists the textures it refers to; stores nothing. Uses the
	// display name, task and deadline of `options`.
	SNK_MODEL_IMPORT_API referenced_files list_referenced_files(std::string_view model_name, file_source& files,
		const import_options& options);

	inline bool past_deadline(std::chrono::steady_clock::time_point deadline)
	{
		return deadline != std::chrono::steady_clock::time_point{} && std::chrono::steady_clock::now() > deadline;
	}
}
