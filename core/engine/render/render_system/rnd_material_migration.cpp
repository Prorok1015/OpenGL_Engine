#include <snk/render/rnd_material_migration.h>

#include <array>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <boost/json/serialize.hpp>

#include <snk/desc/desc_system.h>
#include <snk/model_codec/res_asset_id.h>
#include <snk/model_codec/res_reference.h>

namespace
{
	constexpr std::string_view base_material_path = "res://base_material.desc";
	constexpr std::string_view base_material_guid = "f90d3c03-865c-4193-aa09-cb2f98315634";
	constexpr std::string_view base_vertex = "shaders/scene.vert";

	struct engine_shader
	{
		std::string_view vertex;
		std::string_view fragment;
		std::string_view file;
	};

	constexpr std::array<engine_shader, 3> engine_shaders{ {
		{ "shaders/scene.vert", "shaders/scene.frag", "res://shaders/scene.shader.desc" },
		{ "shaders/sky.vert", "shaders/sky.frag", "res://shaders/sky.shader.desc" },
		{ "shaders/scene.vert", "shaders/mix_opaque_trans_scene.frag", "res://shaders/mix_opaque_trans_scene.shader.desc" },
	} };

	constexpr std::array<std::string_view, 3> stage_keys{ "shader_vertex", "shader_fragment", "shader_geometry" };

	bool stage_is_empty(const json::value& value)
	{
		return value.is_null() || (value.is_string() && value.as_string().empty());
	}

	// The path of a stage below the engine's folder, whichever way it is written: "res://x", "engine://x" or {guid, path}.
	// A form that names no path (a reference by ID only, a number) gives a text no engine stage has, so it is refused.
	std::string stage_path(const json::value& value)
	{
		if (stage_is_empty(value)) {
			return {};
		}
		const std::optional<res::reference> reference = res::reference_from_json(value);
		if (!reference || reference->path.view().empty()) {
			return std::format("<{}>", json::serialize(value));
		}
		const std::string_view path = reference->path.view();
		for (const std::string_view protocol : { std::string_view{ "res://" }, std::string_view{ "engine://" } }) {
			if (path.starts_with(protocol) && path.size() > protocol.size()) {
				return std::string{ path.substr(protocol.size()) };
			}
		}
		return std::string{ path };
	}

	// The parent is the engine's base material only when the file the project serves at its path has the engine's ID: a
	// project file of the same path wins over a reference by ID too (R28), and a parent between them may set other stages.
	bool parent_is_engine_base(const json::object& material, const desc::migration_context& context)
	{
		const json::value* parent = material.if_contains("__parent");
		if (!parent) {
			return false;
		}
		const std::optional<res::asset_id> engine_id = res::asset_id_from_text(base_material_guid);
		if (parent->is_object()) {
			const std::optional<res::reference> reference = res::reference_from_json(*parent);
			const bool names_base = reference && reference->guid == engine_id
				&& (reference->path.view().empty() || reference->path.view() == base_material_path);
			if (!names_base) {
				return false;
			}
		} else if (!parent->is_string() || parent->as_string() != base_material_path) {
			return false;
		}
		return context.id_of(base_material_path) == engine_id;
	}

	std::string described(std::string_view vertex, std::string_view fragment, std::string_view geometry)
	{
		auto or_none = [](std::string_view path) { return path.empty() ? std::string_view{ "none" } : path; };
		return std::format("vertex {}, fragment {}, geometry {}", or_none(vertex), or_none(fragment), or_none(geometry));
	}
}

void rnd::migrate_material_v1(boost::json::object& material, desc::migration_context& context)
{
	bool any_stage = false;
	std::array<std::string, 3> stages;
	for (std::size_t index = 0; index < stage_keys.size(); ++index) {
		if (const json::value* value = material.if_contains(stage_keys[index])) {
			any_stage = true;
			stages[index] = stage_path(*value);
		}
	}
	if (!any_stage) {
		return;
	}
	if (stages[0].empty() && stages[1].empty() && stages[2].empty()) {
		for (const std::string_view key : stage_keys) {
			material.erase(key);
		}
		if (material.contains("__parent")) {
			context.warn("every shader stage was empty, which cleared the parent's: the material now takes the parent's shader");
		}
		return;
	}

	std::string vertex = stages[0];
	const std::string& fragment = stages[1];
	const std::string& geometry = stages[2];
	const bool vertex_inherited = !material.contains("shader_vertex");
	if (vertex_inherited && parent_is_engine_base(material, context)) {
		vertex = base_vertex;
	}

	for (const engine_shader& shader : engine_shaders) {
		if (geometry.empty() && vertex == shader.vertex && fragment == shader.fragment) {
			for (const std::string_view key : stage_keys) {
				material.erase(key);
			}
			material["shader"] = shader.file;
			return;
		}
	}
	throw std::runtime_error(std::format(
		"the shader stages ({}) are not one of the engine's shaders, or a parent completes them: "
		"run the project migration (Project > Migrate Project, or --migrate), it writes a .shader.desc for them",
		described(vertex_inherited ? std::string_view{ "inherited" } : std::string_view{ vertex }, fragment, geometry)));
}
