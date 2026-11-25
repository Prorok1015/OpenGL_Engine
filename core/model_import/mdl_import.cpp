#include <snk/model_import/mdl_import.h>
#include "mdl_assimp_io.h"

#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/ProgressHandler.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>
#include <snk/desc/desc_node_id.h>
#include <snk/desc/desc_pretty_json.h>
#include <snk/model_codec/rnd_geometry_encode.h>
#include <snk/resource/res_desc_binary.h>
#include <snk/resource/res_image_format.h>
#include <snk/resource/res_image_header.h>
#include <snk/resource/res_model_reference.h>
#include <snk/resource/res_path_sanitize.h>
#include <snk/model_codec/res_tag_json.h>
#include <snk/model_codec/scn_glm_json_convert.h>
#include <snk/scene/scn_import_vertex_layout.h>

#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <boost/json.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstring>
#include <format>
#include <iterator>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace json = boost::json;

namespace {

constexpr std::string_view k_log = "import";

using mdl::detail::logged;

void checkpoint(const mdl::import_options& options)
{
	if (options.task) options.task->throw_if_stopped();
}

// Assimp polls it while it reads a file; false aborts the parse (the importer then returns no scene).
class stop_progress_handler final : public Assimp::ProgressHandler
{
public:
	stop_progress_handler(ds::task_context* task, std::chrono::steady_clock::time_point deadline) : m_task(task), m_deadline(deadline) {}
	bool Update(float) override
	{
		if (m_task && m_task->stop.stop_requested()) return false;
		return !mdl::past_deadline(m_deadline);
	}

private:
	ds::task_context* m_task;
	std::chrono::steady_clock::time_point m_deadline;
};

// The importer reading `model_name` through `files`; null scene: see importer.GetErrorString().
const aiScene* read_scene(Assimp::Importer& importer, std::string_view model_name, mdl::file_source& files,
	const mdl::import_options& options, unsigned int flags)
{
	if (options.task || options.deadline != std::chrono::steady_clock::time_point{}) {
		importer.SetProgressHandler(new stop_progress_handler(options.task, options.deadline));
	}
	importer.SetIOHandler(new mdl::detail::source_io(files, options.display_name));
	// FBX names its embedded textures by the file name they had, so two of the same name ("a/tex.png", "b/tex.png") would
	// be one texture to GetEmbeddedTexture; the legacy names are "*<index>" (#1206).
	importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_EMBEDDED_TEXTURES_LEGACY_NAMING, true);
	return importer.ReadFile(std::string{ model_name }, flags);
}

glm::quat convert_to_glm(const aiQuaternion& v)
{
	return glm::quat(v.w, v.x, v.y, v.z);
}

glm::vec3 convert_to_glm(const aiVector3D& v)
{
	return glm::vec3(v.x, v.y, v.z);
}

glm::mat4 convert_to_glm(const aiMatrix4x4& m)
{
	glm::mat4 local(1);
	local[0] = glm::vec4(m.a1, m.b1, m.c1, m.d1);
	local[1] = glm::vec4(m.a2, m.b2, m.c2, m.d2);
	local[2] = glm::vec4(m.a3, m.b3, m.c3, m.d3);
	local[3] = glm::vec4(m.a4, m.b4, m.c4, m.d4);
	return local;
}

struct trs_result { glm::vec3 position; glm::vec3 rotation_deg; glm::vec3 scale; };

trs_result decompose_aimatrix(const aiMatrix4x4& m)
{
	aiVector3D pos, scale;
	aiQuaternion rot;
	m.Decompose(scale, rot, pos);
	glm::quat q = convert_to_glm(rot);
	// prefab_node rebuilds rotation as yawPitchRoll(y, x, z) (Y*X*Z order), so decompose in the same order.
	float yaw = 0.f, pitch = 0.f, roll = 0.f;
	glm::extractEulerAngleYXZ(glm::mat4_cast(q), yaw, pitch, roll);
	return { convert_to_glm(pos), glm::degrees(glm::vec3{ pitch, yaw, roll }), convert_to_glm(scale) };
}

// What the one geometry of `scene` keeps (#1092): triangles when any mesh has them, else lines, else points. Faces of
// another kind are dropped; aiProcess_Triangulate leaves lines and points as they are.
rnd::geometry_topology kept_topology_of(const aiScene* scene)
{
	bool lines = false;
	bool points = false;
	for (unsigned int i = 0; i < scene->mNumMeshes; ++i) {
		const unsigned int types = scene->mMeshes[i]->mPrimitiveTypes;
		if ((types & (aiPrimitiveType_TRIANGLE | aiPrimitiveType_POLYGON)) != 0) return rnd::geometry_topology::triangles;
		lines = lines || (types & aiPrimitiveType_LINE) != 0;
		points = points || (types & aiPrimitiveType_POINT) != 0;
	}
	if (lines) return rnd::geometry_topology::lines;
	if (points) return rnd::geometry_topology::points;
	return rnd::geometry_topology::triangles;
}

unsigned int kept_face_size(rnd::geometry_topology topology)
{
	switch (topology) {
	case rnd::geometry_topology::lines: return 2;
	case rnd::geometry_topology::points: return 1;
	default: return 3;
	}
}

// The vertex attributes of the geometry (#1100): the union over the meshes that keep a face of `face_size`; a mesh
// whose faces are all dropped writes no vertex and adds nothing.
scn::import_layout::vertex_attributes kept_attributes_of(const aiScene* scene, unsigned int face_size)
{
	scn::import_layout::vertex_attributes out;
	for (unsigned int m = 0; m < scene->mNumMeshes; ++m) {
		const aiMesh* mesh = scene->mMeshes[m];
		bool keeps = false;
		for (unsigned int f = 0; f < mesh->mNumFaces && !keeps; ++f) {
			keeps = mesh->mFaces[f].mNumIndices == face_size;
		}
		if (keeps) {
			out |= scn::import_layout::vertex_attributes{ mesh->HasNormals(), mesh->HasTextureCoords(0), mesh->HasTangentsAndBitangents(), false };
		}
	}
	return out;
}

// The texture types the import reads, and those it lists in the log.
constexpr aiTextureType k_read_types[] = { aiTextureType_DIFFUSE, aiTextureType_BASE_COLOR, aiTextureType_SPECULAR, aiTextureType_NORMALS };

struct texture_kind { aiTextureType type; std::string_view name; };
constexpr texture_kind k_logged_types[] = { { aiTextureType_DIFFUSE, "diffuse" }, { aiTextureType_SPECULAR, "specular" },
	{ aiTextureType_HEIGHT, "height" }, { aiTextureType_NORMALS, "normals" }, { aiTextureType_BASE_COLOR, "base color" } };

// Standard padded base64 (RFC 4648), what texture_2d_desc's inline pixels hold; the decoder links no cmd::base64.
std::string base64_of(std::span<const std::byte> bytes)
{
	constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve((bytes.size() + 2) / 3 * 4);
	std::size_t i = 0;
	for (; i + 3 <= bytes.size(); i += 3) {
		const std::uint32_t v = (std::to_integer<std::uint32_t>(bytes[i]) << 16) | (std::to_integer<std::uint32_t>(bytes[i + 1]) << 8)
			| std::to_integer<std::uint32_t>(bytes[i + 2]);
		out += alphabet[(v >> 18) & 63];
		out += alphabet[(v >> 12) & 63];
		out += alphabet[(v >> 6) & 63];
		out += alphabet[v & 63];
	}
	if (const std::size_t rest = bytes.size() - i; rest > 0) {
		std::uint32_t v = std::to_integer<std::uint32_t>(bytes[i]) << 16;
		if (rest == 2) v |= std::to_integer<std::uint32_t>(bytes[i + 1]) << 8;
		out += alphabet[(v >> 18) & 63];
		out += alphabet[(v >> 12) & 63];
		out += rest == 2 ? alphabet[(v >> 6) & 63] : '=';
		out += '=';
	}
	return out;
}

// What the import names its embedded textures after: the geometry's "<stem>.geom.desc", whatever the model calls them.
std::string texture_stem_of(const res::tag& geometry_tag)
{
	constexpr std::string_view suffix = ".geom.desc";
	const std::string_view name = geometry_tag.name();
	if (name.size() > suffix.size() && name.ends_with(suffix)) return std::string(name.substr(0, name.size() - suffix.size()));
	return "model";
}

std::vector<std::byte> json_bytes(const json::object& object)
{
	const std::string text = json::serialize(object);
	std::vector<std::byte> bytes(text.size());
	std::memcpy(bytes.data(), text.data(), text.size());
	return bytes;
}

// The geometry of the whole model, as the loader's geometry_desc holds it; encoded through rnd::geometry_view (#1077).
struct geometry_data
{
	rnd::driver::BufferLayout layout;
	std::vector<unsigned int> indices;
	std::vector<std::byte> vertices;
	std::vector<std::pair<ds::bbox, std::uint32_t>> bounds;

	rnd::geometry_view view() const { return rnd::geometry_view{ layout, vertices, indices, bounds }; }
};

// One import: the scene, where it writes, what it refers to.
class scene_import
{
public:
	scene_import(const aiScene* scene, mdl::import_sink& sink, mdl::file_source& files, const mdl::import_options& options, mdl::import_result& out)
		: m_scene(scene), m_sink(sink), m_files(files), m_options(options), m_out(out), m_texture_stem(texture_stem_of(options.geometry_tag))
	{
	}

	void run()
	{
		const rnd::geometry_topology topology = kept_topology_of(m_scene);
		m_topology = topology;
		m_face_size = kept_face_size(topology);
		if (m_scene->HasMeshes()) {
			m_geometry.layout = scn::import_layout::layout_of(kept_attributes_of(m_scene, m_face_size));
		}

		// Bones: name -> global index.
		int bone_idx = 0;
		for (unsigned int mi = 0; mi < m_scene->mNumMeshes; ++mi) {
			for (unsigned int bi = 0; bi < m_scene->mMeshes[mi]->mNumBones; ++bi) {
				std::string bname = m_scene->mMeshes[mi]->mBones[bi]->mName.C_Str();
				if (!m_bone_index.count(bname))
					m_bone_index[bname] = bone_idx++;
			}
		}

		// Animation clips: anim_name -> { node_name -> animation_node JSON }, all channels of one animation.
		struct clip_data {
			double duration = 0.0;
			double ticks_per_second = 25.0;
			json::object channels;
		};
		std::unordered_map<std::string, clip_data> clip_map;
		for (unsigned int ai = 0; ai < m_scene->mNumAnimations; ++ai) {
			auto* anim = m_scene->mAnimations[ai];
			auto& clip = clip_map[anim->mName.C_Str()];
			clip.duration = anim->mDuration;
			clip.ticks_per_second = anim->mTicksPerSecond;

			for (unsigned int ci = 0; ci < anim->mNumChannels; ++ci) {
				auto* chan = anim->mChannels[ci];
				json::array pos_keys, rot_keys, scale_keys;
				for (unsigned int k = 0; k < chan->mNumPositionKeys; ++k) {
					pos_keys.push_back(json::object{
						{"value", json::value_from(convert_to_glm(chan->mPositionKeys[k].mValue))},
						{"time", chan->mPositionKeys[k].mTime}
					});
				}
				for (unsigned int k = 0; k < chan->mNumRotationKeys; ++k) {
					rot_keys.push_back(json::object{
						{"value", json::value_from(convert_to_glm(chan->mRotationKeys[k].mValue))},
						{"time", chan->mRotationKeys[k].mTime}
					});
				}
				for (unsigned int k = 0; k < chan->mNumScalingKeys; ++k) {
					scale_keys.push_back(json::object{
						{"value", json::value_from(convert_to_glm(chan->mScalingKeys[k].mValue))},
						{"time", chan->mScalingKeys[k].mTime}
					});
				}
				json::object anim_node;
				anim_node["position"] = pos_keys;
				anim_node["rotation"] = rot_keys;
				anim_node["scale"] = scale_keys;
				clip.channels[chan->mNodeName.C_Str()] = anim_node;
			}
		}
		for (auto const& [anim_name, clip] : clip_map) {
			for (auto const& [node_name, _] : clip.channels) {
				m_animated.insert(std::string(node_name));
			}
		}
		checkpoint(m_options);

		json::object prefab_root = build_node(m_scene->mRootNode, m_scene->mRootNode->mName.C_Str());
		checkpoint(m_options);

		if (!clip_map.empty()) {
			json::array clips_arr;
			for (auto const& [anim_name, clip] : clip_map) {
				json::object clip_obj;
				clip_obj["__type"] = "animation_clip_desc";
				clip_obj["name"] = anim_name;
				clip_obj["duration"] = clip.duration;
				clip_obj["ticks_per_second"] = clip.ticks_per_second;
				clip_obj["channels"] = clip.channels;
				clips_arr.push_back(std::move(clip_obj));
			}
			json::object collection_comp;
			collection_comp["__type"] = "animation_collection_desc";
			collection_comp["clips"] = std::move(clips_arr);
			add_root_component(prefab_root, "animation_collection_desc", std::move(collection_comp));
		}

		// Registered during assembly.
		if (!m_out.skin_weights.empty()) {
			json::object sw_comp;
			sw_comp["__type"] = "skin_weights_desc";
			sw_comp["tag"] = json::value_from(m_options.prefab_tag);
			json::array sw_arr;
			sw_arr.reserve(m_out.skin_weights.size());
			for (auto const& column : m_out.skin_weights) {
				json::array vtx_arr;
				vtx_arr.reserve(column.size());
				for (uint32_t v : column) {
					vtx_arr.push_back(static_cast<uint64_t>(v));
				}
				sw_arr.push_back(std::move(vtx_arr));
			}
			sw_comp["weights"] = std::move(sw_arr);
			add_root_component(prefab_root, "skin_weights_desc", std::move(sw_comp));
		}

		// Last safe point: from here the geometry is stored.
		checkpoint(m_options);
		if (!store_geometry()) return;

		m_out.prefab["__type"] = "prefab_desc";
		for (auto& kv : prefab_root) {
			m_out.prefab[kv.key()] = kv.value();
		}
	}

private:
	static void add_root_component(json::object& root, std::string_view key, json::object component)
	{
		if (json::value* components = root.if_contains("components"); components && components->is_object()) {
			components->as_object()[key] = std::move(component);
		} else {
			json::object created;
			created[key] = std::move(component);
			root["components"] = std::move(created);
		}
	}

	void refer(const res::tag& tag)
	{
		if (m_referred.insert(tag).second) m_out.tags.push_back(tag);
	}

	void store_desc(const res::tag& tag, const json::object& desc)
	{
		m_sink.store(tag, desc::pretty_json_bytes(desc));
	}

	// Once per name: a glTF material names one texture as diffuse and as base color. An embedded texture's name ("*3") says
	// nothing without its reason, so the reason is listed with it.
	void missing_texture(std::string_view name, std::string_view why, bool with_reason = false)
	{
		std::string listed = cut_on_code_point(with_reason ? std::format("{}: {}", name, why) : std::string(name),
			mdl::k_max_missing_texture_name_bytes);
		if (!m_missing_seen.insert(listed).second) return;
		if (m_out.missing_textures.size() >= mdl::k_max_missing_textures) {
			if (m_out.missing_textures_not_listed++ == 0) {
				egLOG_WARN(k_log, "model '{}': more than {} textures were not loaded: the rest is not listed",
					logged(m_options.display_name), mdl::k_max_missing_textures);
			}
			return;
		}
		egLOG_WARN(k_log, "model '{}': texture '{}' {}{}: no sampler for it", logged(m_options.display_name),
			logged(cut_on_code_point(name, mdl::k_max_missing_texture_name_bytes)), with_reason ? "was skipped: " : "", why);
		m_out.missing_textures.push_back(std::move(listed));
	}

	static std::string cut_on_code_point(std::string_view text, std::size_t max_bytes)
	{
		if (text.size() <= max_bytes) return std::string(text);
		std::size_t cut = max_bytes;
		while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
		return std::string(text.substr(0, cut));
	}

	// The embedded texture `index` (its place in aiScene::mTextures), once per import whichever materials name it; null when
	// it was skipped. `name` is how the material names it, for the list of textures not loaded.
	json::value embedded_texture(const aiTexture& texture, unsigned int index, std::string_view name)
	{
		if (const auto done = m_embedded.find(index); done != m_embedded.end()) return done->second;
		json::value desc_tag = store_embedded(texture, index, name);
		m_embedded.emplace(index, desc_tag);
		return desc_tag;
	}

	json::value store_embedded(const aiTexture& texture, unsigned int index, std::string_view name)
	{
		const auto skip = [this, name](std::string_view why) {
			missing_texture(name, why, true);
			return json::value{};
		};
		if (index >= mdl::k_max_embedded_textures) {
			return skip(std::format("embedded texture {} is past the first {} an import stores", index, mdl::k_max_embedded_textures));
		}
		if (texture.pcData == nullptr || texture.mWidth == 0) return skip("an embedded texture without data");
		const std::string base = std::format("{}{}.texture_{}", m_options.embedded_folder, m_texture_stem, index);
		const res::tag desc_tag{ res::tag::memory, base + ".txm.desc" };
		json::object desc;
		desc["__type"] = "texture_2d_desc";
		desc["__parent"] = "res://base_texture.desc";
		desc["name"] = std::format("texture_{}", index);

		std::vector<std::byte> bytes;
		std::string extension;
		if (texture.mHeight != 0) {
			const std::uint64_t width = texture.mWidth;
			const std::uint64_t height = texture.mHeight;
			constexpr std::uint64_t max_side = res::image_format::max_picture_side;
			if (width > max_side || height > max_side) return skip(std::format("{}x{}: a side is past {}", width, height, max_side));
			// Both sides at most 16384: the product fits, and so does every index below.
			const std::uint64_t pixel_bytes = width * height * 4;
			if (pixel_bytes > res::image_format::max_picture_decoded_bytes) {
				return skip(std::format("{}x{}: past {} bytes of pixels", width, height, res::image_format::max_picture_decoded_bytes));
			}
			if (!take_embedded_bytes(pixel_bytes)) return skip(total_bytes_reason());
			// The texels go as bytes b, g, r, a: the fields of aiTexel, whatever achFormatHint says.
			static_assert(sizeof(aiTexel) == 4 && offsetof(aiTexel, b) == 0 && offsetof(aiTexel, g) == 1 && offsetof(aiTexel, r) == 2
				&& offsetof(aiTexel, a) == 3);
			mdl::stored_texels stored = mdl::store_texels(std::span(reinterpret_cast<const std::byte*>(texture.pcData),
				static_cast<std::size_t>(pixel_bytes)), static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
			if (stored.raw.empty() && stored.pixels.empty()) return skip("texels the import cannot read");
			if (stored.raw.empty()) {
				desc["pixels"] = std::move(stored.pixels);
				store_embedded_desc(desc_tag, desc);
				return json::value_from(desc_tag);
			}
			bytes = std::move(stored.raw);
			extension = res::image_format::raw_extension;
		} else {
			const std::span<const std::byte> file(reinterpret_cast<const std::byte*>(texture.pcData), texture.mWidth);
			const std::optional<res::image_header::picture_size> size = res::image_header::read_picture_size(file);
			if (!size) return skip("an embedded picture that is no PNG, JPEG, BMP or TGA");
			const std::uint64_t max_side = res::image_format::max_picture_side;
			if (size->width > max_side || size->height > max_side
				|| std::uint64_t{ size->width } * size->height * 4 > res::image_format::max_picture_decoded_bytes) {
				return skip(std::format("an embedded picture of {}x{}: past {} a side or {} bytes of pixels", size->width, size->height,
					max_side, res::image_format::max_picture_decoded_bytes));
			}
			if (!take_embedded_bytes(file.size())) return skip(total_bytes_reason());
			bytes.assign(file.begin(), file.end());
			// Whatever the picture is, the pct adapter decodes it by its content; no other extension is ever written.
			extension = res::image_format::picture_extensions[0];
		}

		// Stored even when the tag is there: the names are the same for every model of this stem, and an earlier import's
		// bytes must never stand for this one's.
		const res::tag data_tag{ res::tag::memory, std::format("{}.{}", base, extension) };
		m_sink.store(data_tag, bytes);
		refer(data_tag);
		desc["data"] = json::value_from(data_tag);
		store_embedded_desc(desc_tag, desc);
		return json::value_from(desc_tag);
	}

	bool take_embedded_bytes(std::uint64_t bytes)
	{
		if (bytes > mdl::k_max_embedded_total_bytes - m_embedded_bytes) return false;
		m_embedded_bytes += bytes;
		return true;
	}

	static std::string total_bytes_reason()
	{
		return std::format("it would pass the {} bytes of embedded textures an import stores", mdl::k_max_embedded_total_bytes);
	}

	void store_embedded_desc(const res::tag& desc_tag, const json::object& desc)
	{
		store_desc(desc_tag, desc);
		refer(desc_tag);
	}

	// A texture desc naming the file beside the model (the engine's adapter).
	json::value referenced_texture(std::string_view name)
	{
		const res::tag desc_tag{ res::tag::memory, std::format("{}{}.txm.desc", m_options.texture_folder, res::sanitize_relative_path(name)) };
		if (!m_sink.exists(desc_tag)) {
			const std::string data_name = name.find("://") == std::string_view::npos ? std::string(name) : res::sanitize_relative_path(name);
			const std::optional<res::tag> data_tag = res::model_reference_tag(m_options.model_tag, data_name);
			if (!data_tag) {
				missing_texture(name, "is absolute, in another protocol or above its root");
				return {};
			}
			json::object desc;
			desc["__type"] = "texture_2d_desc";
			desc["__parent"] = "res://base_texture.desc";
			desc["name"] = name;
			desc["data"] = json::value_from(*data_tag);
			store_desc(desc_tag, desc);
		}
		refer(desc_tag);
		return json::value_from(desc_tag);
	}

	// The texture's bytes copied into memory:// with a desc of their own (the editor's import).
	json::value copied_texture(std::string_view name)
	{
		const std::string key = res::sanitize_relative_path(m_files.texture_key(name));
		const res::tag data_tag{ res::tag::memory, m_options.texture_folder + key };
		if (!m_sink.exists(data_tag)) {
			mdl::texture_bytes read = m_files.read_texture(name);
			if (!read.bytes) {
				missing_texture(name, read.reason.empty() ? std::string_view{ "is missing, not a regular file or too big" } : std::string_view{ read.reason });
				return {};
			}
			m_sink.store(data_tag, *read.bytes);
		}
		refer(data_tag);

		const res::tag desc_tag{ res::tag::memory, std::format("{}{}.txm.desc", m_options.texture_folder, key) };
		if (!m_sink.exists(desc_tag)) {
			json::object desc;
			desc["__type"] = "texture_2d_desc";
			desc["__parent"] = "res://base_texture.desc";
			desc["name"] = name;
			desc["data"] = json::value_from(data_tag);
			store_desc(desc_tag, desc);
		}
		refer(desc_tag);
		return json::value_from(desc_tag);
	}

	// The desc of the first texture of `type` (null: none, or not loaded).
	json::value material_texture(aiMaterial* material, aiTextureType type)
	{
		if (material->GetTextureCount(type) == 0) return {};
		aiString str;
		material->GetTexture(type, 0, &str);
		const std::string_view name = str.C_Str();
		if (const auto [embedded, index] = m_scene->GetEmbeddedTextureAndIndex(str.C_Str()); embedded != nullptr && index >= 0) {
			return embedded_texture(*embedded, static_cast<unsigned int>(index), name);
		}
		return m_options.textures == mdl::texture_mode::copy ? copied_texture(name) : referenced_texture(name);
	}

	json::value process_material(aiMaterial* material)
	{
		for (const texture_kind& logged_type : k_logged_types) {
			if (material->GetTextureCount(logged_type.type) > 0) {
				aiString str;
				material->GetTexture(logged_type.type, 0, &str);
				egLOG_DEBUG(k_log, "model '{}': {} texture: {} ({} in all)", logged(m_options.display_name), logged_type.name, logged(str.C_Str()),
					material->GetTextureCount(logged_type.type));
			}
		}

		const std::string mat_name = material->GetName().C_Str();
		json::object jsmaterial;
		jsmaterial["__type"] = "material_desc";
		jsmaterial["__parent"] = "res://base_material.desc";
		jsmaterial["name"] = mat_name;

		json::array samplers;
		json::array defines{ "LIGHTS_ENABLED" };
		json::value diffuse = material_texture(material, aiTextureType_DIFFUSE);
		if (diffuse.is_null()) diffuse = material_texture(material, aiTextureType_BASE_COLOR);
		if (!diffuse.is_null()) {
			samplers.push_back(diffuse);
			defines.push_back("USE_TXM_AS_DIFFUSE");
		}
		if (json::value spec = material_texture(material, aiTextureType_SPECULAR); !spec.is_null()) {
			samplers.resize(2);
			samplers[1] = spec;
			defines.push_back("USE_SPECULAR_MAP");
		}
		if (json::value normal = material_texture(material, aiTextureType_NORMALS); !normal.is_null()) {
			samplers.resize(3);
			samplers[2] = normal;
			defines.push_back("USE_NORMAL_MAP");
		}
		if (!samplers.empty()) {
			jsmaterial["samplers"] = samplers;
		}

		json::object uniforms;
		aiColor4D color;
		if (AI_SUCCESS == material->Get(AI_MATKEY_COLOR_DIFFUSE, color)) {
			uniforms["albedo"] = json::value_from(glm::vec4(color.r, color.g, color.b, color.a));
		}
		if (AI_SUCCESS == material->Get(AI_MATKEY_COLOR_EMISSIVE, color)) {
			uniforms["emissive"] = json::value_from(glm::vec4(color.r, color.g, color.b, color.a));
		}
		float shininess = 0;
		if (AI_SUCCESS == material->Get(AI_MATKEY_SHININESS, shininess)) {
			uniforms["shininess"] = json::value_from(shininess);
		}
		if (!uniforms.empty()) {
			jsmaterial["uniforms"] = uniforms;
		}

		jsmaterial["defines"] = defines;
		jsmaterial["queue"] = "mix";
		jsmaterial["shader"] = "res://shaders/mix_opaque_trans_scene.shader.desc";
		if (m_options.materials == mdl::material_mode::inline_desc) {
			return jsmaterial;
		}

		const res::tag mat_tag{ res::tag::memory, std::format("{}{}.mat.desc", m_options.material_folder, res::sanitize_relative_path(mat_name)) };
		if (!m_sink.exists(mat_tag)) {
			store_desc(mat_tag, jsmaterial);
		}
		refer(mat_tag);
		return json::value_from(mat_tag);
	}

	// Appends the vertices and the faces of the kept size of `mesh`; false (nothing appended) when it has none. The faces
	// dropped are counted in one warning naming the model and the mesh.
	bool process_mesh_geometry(const aiMesh* mesh)
	{
		unsigned int kept = 0;
		unsigned int dropped_lines = 0;
		unsigned int dropped_points = 0;
		unsigned int dropped_other = 0;
		for (unsigned int i = 0; i < mesh->mNumFaces; ++i) {
			const unsigned int count = mesh->mFaces[i].mNumIndices;
			if (count == m_face_size) ++kept;
			else if (count == 2) ++dropped_lines;
			else if (count == 1) ++dropped_points;
			else ++dropped_other;
		}
		if (dropped_lines + dropped_points + dropped_other > 0) {
			egLOG_WARN(k_log, "model '{}': mesh '{}': dropped {} line(s) and {} point(s){}: the geometry holds {} only",
				logged(m_options.display_name), logged(mesh->mName.C_Str()), dropped_lines, dropped_points,
				dropped_other > 0 ? std::format(" and {} other face(s)", dropped_other) : std::string{},
				rnd::topology_name(m_topology));
		}
		if (kept == 0) {
			return false;
		}

		// Every vertex in the geometry's layout (#1100): what this mesh lacks takes the default of scn::import_layout.
		const scn::import_layout::vertex_attributes layout = scn::import_layout::attributes_of(m_geometry.layout);
		if (layout.normal && !mesh->HasNormals()) {
			egLOG_WARN(k_log, "model '{}': mesh '{}' has no normals: written as (0, 0, 1)", logged(m_options.display_name), logged(mesh->mName.C_Str()));
		}
		for (unsigned int i = 0; i < mesh->mNumVertices; i++) {
			scn::import_layout::vertex_values vertex;
			vertex.position = convert_to_glm(mesh->mVertices[i]);
			if (mesh->HasNormals()) {
				vertex.normal = convert_to_glm(mesh->mNormals[i]);
			}
			if (mesh->HasTextureCoords(0)) {
				vertex.uv = glm::vec2(convert_to_glm(mesh->mTextureCoords[0][i]));
			}
			if (mesh->HasTangentsAndBitangents()) {
				vertex.tangent = convert_to_glm(mesh->mTangents[i]);
				vertex.bitangent = convert_to_glm(mesh->mBitangents[i]);
			}
			const std::size_t begin = m_geometry.vertices.size();
			scn::import_layout::append_vertex(m_geometry.vertices, layout, vertex);
			ASSERT_MSG(m_geometry.vertices.size() - begin == m_geometry.layout.get_stride(), "Stride mismatch");
		}

		// Only a triangle with uvs has uv bounds.
		const bool uv_bounds = mesh->HasTextureCoords(0) && m_face_size == 3;
		for (unsigned int i = 0; i < mesh->mNumFaces; i++) {
			const aiFace& face = mesh->mFaces[i];
			if (face.mNumIndices != m_face_size) continue;
			for (unsigned int j = 0; j < face.mNumIndices; j++) {
				m_geometry.indices.push_back(face.mIndices[j]);
			}
			if (!uv_bounds) continue;
			ds::bbox box;
			for (unsigned int j = 0; j < 3; ++j) {
				ds::expand(box, glm::vec2(convert_to_glm(mesh->mTextureCoords[0][face.mIndices[j]])));
			}
			m_geometry.bounds.push_back({ box, static_cast<uint32_t>(m_geometry.bounds.size()) });
		}
		return true;
	}

	// Skinning weights of `mesh` as skinning_manager reads them:
	// out[vertex] = [bone_idx_0, bone_idx_1, ..., weight_0_as_uint32, weight_1_as_uint32, ...]
	void build_mesh_skin_weights(const aiMesh* mesh, std::size_t vx_begin)
	{
		std::vector<std::vector<uint32_t>>& out_weights = m_out.skin_weights;
		const std::size_t num_vx = mesh->mNumVertices;
		std::vector<std::vector<std::pair<int, float>>> vtx_bones(num_vx);
		for (unsigned int bi = 0; bi < mesh->mNumBones; ++bi) {
			const aiBone* b = mesh->mBones[bi];
			int bidx = 0;
			if (auto it = m_bone_index.find(b->mName.C_Str()); it != m_bone_index.end())
				bidx = it->second;
			for (unsigned int wi = 0; wi < b->mNumWeights; ++wi) {
				const unsigned int vi = b->mWeights[wi].mVertexId;
				if (vi < vtx_bones.size())
					vtx_bones[vi].emplace_back(bidx, b->mWeights[wi].mWeight);
			}
		}

		const std::size_t required_size = vx_begin + num_vx;
		if (out_weights.size() < required_size)
			out_weights.resize(required_size);
		for (std::size_t vi = 0; vi < num_vx; ++vi) {
			const auto& vw = vtx_bones[vi];
			std::vector<uint32_t> column(vw.size() * 2);
			for (std::size_t k = 0; k < vw.size(); ++k) {
				column[k] = static_cast<uint32_t>(vw[k].first);
				column[vw.size() + k] = std::bit_cast<uint32_t>(vw[k].second);
			}
			out_weights[vx_begin + vi] = std::move(column);
		}
	}

	// Reserves a sibling name that is not taken yet (children keep distinct names even though v2 could store duplicates:
	// animation channels and the editor address nodes by name).
	static std::string unique_child_name(std::unordered_set<std::string>& used_names, const std::string& name)
	{
		std::string candidate = name;
		for (unsigned int suffix = 1; !used_names.insert(candidate).second; ++suffix)
			candidate = std::format("{}_{}", name, suffix);
		return candidate;
	}

	json::object build_node(const aiNode* node, const std::string& node_path, std::string_view node_key = {})
	{
		json::object jsnode;
		const std::string node_name = node->mName.C_Str();
		// Hash of the name path: re-importing the same model yields the same ids.
		jsnode["__id"] = desc::format_node_id(desc::derive_node_id(node_path));
		// The root of the file has no name of its own; every other node is named by its (unique among siblings) key.
		if (!node_key.empty())
			jsnode["name"] = std::string{ node_key };

		const trs_result trs = decompose_aimatrix(node->mTransformation);
		{
			json::object transform;
			bool has_transform = false;
			if (trs.position != glm::vec3(0)) { transform["position"] = json::value_from(trs.position); has_transform = true; }
			if (trs.rotation_deg != glm::vec3(0)) { transform["rotation"] = json::value_from(trs.rotation_deg); has_transform = true; }
			if (trs.scale != glm::vec3(1)) { transform["scale"] = json::value_from(trs.scale); has_transform = true; }
			if (has_transform)
				jsnode["transform"] = transform;
		}

		json::object components;
		if (const aiBone* bone = m_scene->findBone(aiString(node_name))) {
			if (auto it = m_bone_index.find(node_name); it != m_bone_index.end()) {
				json::object bone_comp;
				bone_comp["__type"] = "bone_desc";
				bone_comp["offset_matrix"] = json::value_from(convert_to_glm(bone->mOffsetMatrix));
				bone_comp["index"] = it->second;
				components["bone_desc"] = bone_comp;
			}
		}
		// Nodes with animation channels.
		if (m_animated.contains(node_name)) {
			components["animated_node_desc"] = json::object{ {"__type", "animated_node_desc"} };
		}
		if (!components.empty())
			jsnode["components"] = components;

		json::array children;
		std::unordered_set<std::string> used_child_names;

		// One child per mesh of this node.
		for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
			const aiMesh* mesh = m_scene->mMeshes[node->mMeshes[i]];
			const std::size_t vx_begin = m_geometry.vertices.size() / m_geometry.layout.get_stride();
			const std::size_t ind_begin = m_geometry.indices.size();
			if (!process_mesh_geometry(mesh)) {
				continue; // nothing of it is drawn (#1092)
			}
			const std::size_t vx_end = m_geometry.vertices.size() / m_geometry.layout.get_stride();
			const std::size_t ind_end = m_geometry.indices.size();

			json::object mesh_node_comp;
			mesh_node_comp["__type"] = "mesh_node_desc";
			mesh_node_comp["geometry"] = json::value_from(m_options.geometry_tag);
			mesh_node_comp["vx_begin"] = vx_begin;
			mesh_node_comp["vx_end"] = vx_end;
			mesh_node_comp["ind_begin"] = ind_begin;
			mesh_node_comp["ind_end"] = ind_end;
			mesh_node_comp["material"] = process_material(m_scene->mMaterials[mesh->mMaterialIndex]);

			json::object mesh_components;
			mesh_components["mesh_node_desc"] = mesh_node_comp;
			if (mesh->mNumBones > 0) {
				json::object skinning_comp;
				skinning_comp["__type"] = "skinning_desc";
				skinning_comp["bone_count"] = static_cast<int>(mesh->mNumBones);
				skinning_comp["skinning_tag"] = json::value_from(m_options.prefab_tag);
				mesh_components["skinning_desc"] = skinning_comp;
				build_mesh_skin_weights(mesh, vx_begin);
			}

			// Same-named meshes under one node must not overwrite each other.
			const std::string mesh_key = unique_child_name(used_child_names, "mesh_" + std::string(mesh->mName.C_Str()));
			json::object mesh_child;
			mesh_child["__id"] = desc::format_node_id(desc::derive_node_id(node_path + "/" + mesh_key));
			mesh_child["name"] = mesh_key;
			mesh_child["components"] = mesh_components;
			children.push_back(std::move(mesh_child));
		}

		for (unsigned int i = 0; i < node->mNumChildren; ++i) {
			const aiNode* child_node = node->mChildren[i];
			// Same-named siblings (and a node named like a mesh child) must not collapse into one key.
			const std::string child_key = unique_child_name(used_child_names, child_node->mName.C_Str());
			children.push_back(build_node(child_node, node_path + "/" + child_key, child_key));
		}

		if (!children.empty())
			jsnode["children"] = children;
		return jsnode;
	}

	// Triangles as the desc and its binary file (EPIC-79), the binary file first; lines, points and a model without
	// meshes as JSON arrays with the topology named. False: the loader would refuse the triangles.
	bool store_geometry()
	{
		const res::tag& geom_tag = m_options.geometry_tag;
		rnd::geometry_file_options geometry_options;
		geometry_options.header["__parent"] = "res://base_geometry.desc";
		geometry_options.bounds = !m_geometry.bounds.empty();
		geometry_options.topology = m_topology;
		const bool has_meshes = !m_geometry.vertices.empty();
		if (has_meshes && m_topology == rnd::geometry_topology::triangles) {
			const rnd::geometry_files files = rnd::encode_geometry_files(geom_tag, m_geometry.view(), geometry_options);
			if (!files) {
				m_out.failed = mdl::failure::geometry;
				m_out.error = files.error;
				return false;
			}
			// store() registers the bytes for readers before it returns and queues the write on the resolver's one io
			// thread: the binary file is served and written before the desc, so the lock need not wait for the disk.
			const res::desc_binary::save_lock lock(geom_tag);
			m_sink.store(*res::desc_binary::binary_tag(geom_tag), files.binary);
			m_sink.store(geom_tag, files.desc);
		} else {
			json::object jsgeometry;
			jsgeometry["__type"] = "geometry_desc";
			jsgeometry["__parent"] = "res://base_geometry.desc";
			if (has_meshes) {
				egLOG_WARN(k_log, "model '{}': geometry '{}' stays in JSON arrays: not triangles ({})", logged(m_options.display_name),
					geom_tag.view(), rnd::topology_name(m_topology));
				jsgeometry[rnd::geometry_topology_key] = rnd::topology_name(m_topology);
			}
			rnd::write_geometry_arrays(geom_tag, m_geometry.view(), jsgeometry);
			m_sink.store(geom_tag, json_bytes(jsgeometry));
		}
		refer(geom_tag);
		return true;
	}

	const aiScene* m_scene;
	mdl::import_sink& m_sink;
	mdl::file_source& m_files;
	const mdl::import_options& m_options;
	mdl::import_result& m_out;
	geometry_data m_geometry;
	rnd::geometry_topology m_topology = rnd::geometry_topology::triangles;
	unsigned int m_face_size = 3;
	std::unordered_map<std::string, int> m_bone_index;
	std::unordered_set<std::string> m_animated;
	std::unordered_set<res::tag> m_referred;
	std::unordered_set<std::string> m_missing_seen;
	const std::string m_texture_stem;
	std::unordered_map<unsigned int, json::value> m_embedded;
	std::uint64_t m_embedded_bytes = 0;
};

// The size of `scene` without recursion: a model file chooses how deep its tree is.
mdl::scene_size size_of(const aiScene& scene)
{
	mdl::scene_size size;
	size.meshes = scene.mNumMeshes;
	for (unsigned int m = 0; m < scene.mNumMeshes; ++m) {
		const aiMesh* mesh = scene.mMeshes[m];
		size.vertices += mesh->mNumVertices;
		for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
			size.indices += mesh->mFaces[f].mNumIndices;
		}
	}
	std::vector<std::pair<const aiNode*, std::uint64_t>> pending;
	if (scene.mRootNode) pending.emplace_back(scene.mRootNode, 1);
	while (!pending.empty()) {
		const auto [node, depth] = pending.back();
		pending.pop_back();
		++size.nodes;
		size.depth = std::max(size.depth, depth);
		for (unsigned int c = 0; c < node->mNumChildren; ++c) {
			pending.emplace_back(node->mChildren[c], depth + 1);
		}
	}
	return size;
}

}

std::string mdl::scene_size_problem(const scene_size& size)
{
	if (size.nodes > k_max_scene_nodes) return std::format("too many nodes: {} (limit {})", size.nodes, k_max_scene_nodes);
	if (size.depth > k_max_scene_depth) {
		return std::format("the node tree is too deep: {} levels (limit {})", size.depth, k_max_scene_depth);
	}
	if (size.meshes > k_max_scene_meshes) return std::format("too many meshes: {} (limit {})", size.meshes, k_max_scene_meshes);
	if (size.vertices > k_max_scene_vertices) {
		return std::format("too many vertices: {} (limit {})", size.vertices, k_max_scene_vertices);
	}
	if (size.indices > k_max_scene_indices) return std::format("too many indices: {} (limit {})", size.indices, k_max_scene_indices);
	return {};
}

mdl::stored_texels mdl::store_texels(std::span<const std::byte> bgra, std::uint32_t width, std::uint32_t height)
{
	constexpr auto max_side = static_cast<std::uint32_t>(res::image_format::max_picture_side);
	if (width < 1 || height < 1 || width > max_side || height > max_side) return {};
	// Both sides at most 16384: no product below overflows.
	const std::size_t row_bytes = std::size_t{ width } * 4;
	const std::size_t pixel_bytes = row_bytes * height;
	if (bgra.size() != pixel_bytes) return {};
	const bool inline_pixels = pixel_bytes <= k_max_inline_texture_bytes && width <= k_max_inline_texture_side && height <= k_max_inline_texture_side;
	const std::size_t offset = inline_pixels ? 0 : res::image_format::raw_header_size;

	std::vector<std::byte> out(offset + pixel_bytes);
	for (std::size_t row = 0; row < height; ++row) {
		// Inline pixels run top row first, as assimp's; a .raw file is read bottom row first.
		const std::size_t to_row = inline_pixels ? row : height - 1 - row;
		const std::byte* from = bgra.data() + row * row_bytes;
		std::byte* to = out.data() + offset + to_row * row_bytes;
		for (std::size_t x = 0; x < row_bytes; x += 4) {
			to[x + 0] = from[x + 2];
			to[x + 1] = from[x + 1];
			to[x + 2] = from[x + 0];
			to[x + 3] = from[x + 3];
		}
	}

	stored_texels stored;
	if (inline_pixels) {
		stored.pixels = json::object{ { "width", std::int64_t{ width } }, { "height", std::int64_t{ height } }, { "format", "RGBA8" },
			{ "base64", base64_of(out) } };
		return stored;
	}
	res::image_format::raw_header header;
	header.size = glm::ivec2(static_cast<int>(width), static_cast<int>(height));
	header.channels = 4;
	std::memcpy(out.data(), &header, res::image_format::raw_header_size);
	stored.raw = std::move(out);
	return stored;
}

mdl::import_result mdl::import_model(std::string_view model_name, file_source& files, import_sink& sink, const import_options& options)
{
	import_result out;
	out.geometry_tag = options.geometry_tag;
	out.prefab_tag = options.prefab_tag;
	checkpoint(options);

	Assimp::Importer importer;
	constexpr unsigned int flags = aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace;
	const aiScene* scene = read_scene(importer, model_name, files, options, flags);
	// An aborted parse also returns no scene: that is a stop, not an import error.
	checkpoint(options);
	if (past_deadline(options.deadline)) {
		out.failed = failure::timed_out;
		out.error = "the model took too long to read";
		return out;
	}
	if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode) {
		out.failed = failure::parse;
		out.error = importer.GetErrorString();
		return out;
	}
	if (std::string problem = scene_size_problem(size_of(*scene)); !problem.empty()) {
		out.failed = failure::too_large;
		out.error = std::move(problem);
		return out;
	}

	scene_import(scene, sink, files, options, out).run();
	return out;
}

mdl::referenced_files mdl::list_referenced_files(std::string_view model_name, file_source& files, const import_options& options)
{
	referenced_files out;
	Assimp::Importer importer;
	const aiScene* scene = read_scene(importer, model_name, files, options, 0);
	checkpoint(options);
	if (past_deadline(options.deadline)) {
		out.failed = failure::timed_out;
		out.error = "the model took too long to read";
		return out;
	}
	if (!scene) {
		out.failed = failure::parse;
		out.error = importer.GetErrorString();
		return out;
	}
	for (unsigned int m = 0; m < scene->mNumMaterials; ++m) {
		const aiMaterial* material = scene->mMaterials[m];
		for (const aiTextureType type : k_read_types) {
			for (unsigned int i = 0; i < material->GetTextureCount(type); ++i) {
				aiString name;
				material->GetTexture(type, i, &name);
				if (scene->GetEmbeddedTexture(name.C_Str())) continue;
				out.textures.emplace_back(name.C_Str());
			}
		}
	}
	return out;
}
