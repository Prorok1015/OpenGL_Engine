#include <snk/render/rnd_render_service_init.h>
#include <snk/render/rnd_render_system.h>
#include <snk/render/rnd_capture_commands.h>
#include <snk/render/rnd_capture_service.h>
#include <snk/command/cmd_registry.h>
#include <snk/window/wnd_window_system.h>
#include <snk/driver_gl/rnd_gl_swapchain.h>
#include <snk/driver_null/rnd_null_swapchain.h>
#include <snk/driver_validation/rnd_validation_driver.h>
#include <snk/config/cfg_variables.h>
#include <snk/common/logger/engine_log.h>
#include <array>
#include <functional>
#include <string>
#include <string_view>
#include <snk/desc/desc_describe.h>
#include <snk/desc/desc_system.h>
#include <snk/resource/res_system.h>
#include <snk/render/geom/rnd_geometry_desc.h>
#include "geom/rnd_geometry_desc_adapter.h"
#include <snk/render/texture/rnd_texture_desc.h>
#include <snk/render/rnd_shader_desc.h>
#include "texture/rnd_texture_2d_desc.h"
#include "texture/rnd_texture_cubemap_desc.h"
#include "passes/rnd_frame_upload_pass.h"
#include "passes/rnd_z_prepass.h"
#include "passes/rnd_opaque_pass.h"
#include "passes/rnd_transparent_pass.h"
#include "passes/rnd_composition_pass.h"
#include "passes/rnd_skybox_pass.h"
#include "passes/rnd_debug_pass.h"
#include <snk/render/shader/rnd_material_compiler.h>
#include <utility>

namespace {
	// What the Inspector, validate_desc_tree and MCP read of a geometry desc (EPIC-79 PR5). Built by hand: geometry_desc
	// reads and writes its JSON itself (read_geometry_json, serialize) and its members are not that form, so it has no
	// Boost.Describe of them. "vertices" and "indices" are ranges of the binary file, or arrays in an older file.
	desc::type_schema geometry_schema(int version)
	{
		desc::type_schema schema;
		schema.name = "geometry_desc";
		schema.id = desc::schema_id(schema.name);
		schema.version = version;
		schema.kind = desc::schema_kind::desc;
		const auto add = [&schema](std::string name, desc::field_kind kind, std::string tooltip) -> desc::field_schema& {
			desc::field_schema& field = schema.fields.emplace_back();
			field.key = "/" + name;
			field.name = std::move(name);
			field.kind = kind;
			field.tooltip = std::move(tooltip);
			return field;
		};
		add("layout", desc::field_kind::custom, "Vertex attributes by name: {\"position\": \"vec3\", \"uv\": \"vec2\"}").read_only = true;
		add("indices", desc::field_kind::binary_ref, "u32 indices: a range of the binary file beside the desc");
		add("vertices", desc::field_kind::binary_ref, "Interleaved vertex bytes: a range of the binary file beside the desc");
		add("bounds", desc::field_kind::custom, "Built again from the data when loaded").editor_hidden = true;
		desc::field_schema& topology = add("topology", desc::field_kind::string, "lines or points; a desc without it holds triangles");
		topology.optional = true;
		topology.read_only = true;
		topology.default_value = boost::json::value(std::string_view{});
		return schema;
	}

	// What the editor reads of a texture_2d_desc (#1205): texture_desc reads its JSON itself, so the schema is written by
	// hand. "header" and "pixels" are custom values the texture window draws; the file format is unchanged.
	desc::type_schema texture_2d_schema(int version)
	{
		desc::type_schema schema;
		schema.name = "texture_2d_desc";
		schema.id = desc::schema_id(schema.name);
		schema.version = version;
		schema.kind = desc::schema_kind::desc;
		const auto add = [&schema](std::string name, desc::field_kind kind, std::string tooltip) -> desc::field_schema& {
			desc::field_schema& field = schema.fields.emplace_back();
			field.key = "/" + name;
			field.name = std::move(name);
			field.kind = kind;
			field.tooltip = std::move(tooltip);
			field.optional = true;
			return field;
		};
		add("name", desc::field_kind::string, "A name for the texture; the file's path is what refers to it").default_value =
			boost::json::value(std::string_view{});
		add("header", desc::field_kind::custom, "Sampling: wrap, min/mag filter, mip levels (only these apply to a 2D texture)")
			.editor_hidden = true;
		add("data", desc::field_kind::res_tag, "The picture file").filter = "png,jpg,jpeg,tga,bmp,pct,raw";
		add("pixels", desc::field_kind::custom, "Inline pixels (#1204)").read_only = true;
		return schema;
	}

	// The custom fields of texture_2d_schema: texture_desc reads and writes them as JSON.
	constexpr std::array<std::string_view, 2> k_texture_2d_codec_fields{ "header", "pixels" };

	// "gl" (default) or "null". A window without a GL surface selects null regardless.
	// Unknown values warn and fall through to the surface check.
	CFG_VAR_DEF_STRING(cfg_render_backend, "render/backend", "gl");

	// Wraps the backend in the validation decorator (rule violations go to the "rnd.validate" log category).
	// Over GL only in builds without NDEBUG; over null in every build.
	CFG_VAR_DEF_BOOL(cfg_render_validate, "render/validate", false);
	// Per-frame creation budgets of the validation; 0 leaves a budget unlimited.
	CFG_VAR_DEF_INT(cfg_validate_max_frame_buffers, "render/validate_max_frame_buffers", 0);
	CFG_VAR_DEF_INT(cfg_validate_max_programs, "render/validate_max_programs", 0);
	CFG_VAR_DEF_INT(cfg_validate_max_textures, "render/validate_max_textures", 0);

	// The commands the renderer offers; they leave the registry with this holder.
	struct render_commands
	{
		cmd::registration capture;
		cmd::registration compare;
	};

	std::size_t budget_of(int configured)
	{
		return configured > 0 ? static_cast<std::size_t>(configured) : rnd::driver::validation::limits::unlimited;
	}

	rnd::driver::validation::limits validation_budget()
	{
		return { budget_of(*cfg_validate_max_frame_buffers), budget_of(*cfg_validate_max_programs), budget_of(*cfg_validate_max_textures) };
	}

	bool validation_wanted(bool null_backend)
	{
		if (!*cfg_render_validate) {
			return false;
		}
#ifdef NDEBUG
		if (!null_backend) {
			egLOG_WARN("render", "render/validate over the GL backend needs a build without NDEBUG; ignored");
			return false;
		}
#else
		(void)null_backend;
#endif
		return true;
	}
}

void engine::render::register_render_descs(desc::desc_system& desc_sys)
{
	desc_sys.register_desc<rnd::geometry_desc>("geometry_desc");
	// Version 2 may keep "vertices" and "indices" in the binary file next to the desc (EPIC-79): an older engine
	// refuses such a file instead of reading ranges as arrays. Arrays stay valid, so the step changes nothing.
	desc_sys.register_migration("geometry_desc", 1, "nothing: arrays stay valid, version 2 may also hold binary ranges",
		[](json::object&, desc::migration_context&) {});
	desc_sys.register_schema(geometry_schema(desc_sys.current_version("geometry_desc")));
	desc_sys.register_desc<rnd::texture_desc>("texture_desc");
	desc_sys.register_desc<rnd::texture_2d_desc>("texture_2d_desc");
	// A document (#1205): the editor opens a .txm.desc in the texture window.
	desc_sys.register_schema(texture_2d_schema(desc_sys.current_version("texture_2d_desc")));
	desc_sys.mark_document("texture_2d_desc", k_texture_2d_codec_fields);
	desc_sys.register_desc<rnd::texture_cubemap_desc>("texture_cubemap_desc");
	// A document: every field is on the schema, so the editor opens a .shader.desc as data. The structure of "backends"
	// (#1203) comes first: a document's nested schemas must be known when it is marked.
	desc_sys.register_schema(desc::schema_generator<rnd::shader_stage_sources>::make("shader_stage_sources", 1, desc::schema_kind::structure));
	desc_sys.register_document<rnd::shader_desc>("shader_desc");
}

void engine::render::unregister_render_descs(desc::desc_system& desc_sys)
{
	desc_sys.unregister_desc("shader_desc");
	desc_sys.unregister_desc("shader_stage_sources");
	desc_sys.unregister_desc("texture_cubemap_desc");
	desc_sys.unregister_desc("texture_2d_desc");
	desc_sys.unregister_desc("texture_desc");
	desc_sys.unregister_migrations("geometry_desc");
	desc_sys.unregister_desc("geometry_desc");
}

void engine::render::render_init(ds::app_data_storage& data)
{
	auto& window_system = data.require<wnd::window_system>();
	auto& desc_sys = data.require<desc::desc_system>();
	auto& res_sys = data.require<res::resource_system>();
	const auto window = window_system.get_active_window();
	rnd::driver::gl_surface* surface = window ? window->gl_surface_ptr() : nullptr;
	const std::string& backend = *cfg_render_backend;
	if (backend != "gl" && backend != "null") {
		egLOG_WARN("render", "Unknown render/backend '{}', expected 'gl' or 'null'", backend);
	}
	// Destroyed in render_term after the render system, before the window (core shutdown).
	rnd::driver::swapchain_interface* swapchain_ptr = nullptr;
	const bool use_null = backend == "null" || !surface;
	if (use_null) {
		if (backend == "null") {
			egLOG_INFO("render", "Render backend: null (render/backend = null)");
		} else {
			egLOG_WARN("render", "render/backend is gl but the window has no GL surface; falling back to the null backend");
		}
	}
	if (validation_wanted(use_null)) {
		egLOG_INFO("render", "Render validation is on (render/validate)");
		std::unique_ptr<rnd::driver::swapchain_interface> inner;
		if (use_null) {
			inner = std::make_unique<rnd::driver::null::swapchain>();
		} else {
			inner = std::make_unique<rnd::driver::gl::swapchain>(*surface);
		}
		swapchain_ptr = &data.construct<rnd::driver::swapchain_interface, rnd::driver::validation::swapchain>(std::move(inner), validation_budget());
	} else if (use_null) {
		swapchain_ptr = &data.construct<rnd::driver::swapchain_interface, rnd::driver::null::swapchain>();
	} else {
		swapchain_ptr = &data.construct<rnd::driver::swapchain_interface, rnd::driver::gl::swapchain>(*surface);
	}
	auto& swapchain = *swapchain_ptr;
	auto& render_system = data.construct<rnd::render_system>(swapchain.create_driver(), desc_sys, res_sys);
	// Per-frame render data shared by the extract and render stages; reset at the end of each frame.
	data.construct<rnd::frame_context>();
	register_render_descs(desc_sys);
	res_sys.registrate_adapter<rnd::geometry_desc_adapter>(rnd::geometry_desc_adapter::INFO, std::ref(res_sys),
		std::ref(desc_sys));
	render_system.add_render_pass(std::make_shared<rnd::frame_upload_pass>());
	render_system.add_render_pass(std::make_shared<rnd::z_prepass>(res_sys.engine_file("shaders/z_prepass.vert"), res_sys.engine_file("shaders/z_prepass.frag")));
	render_system.add_render_pass(std::make_shared<rnd::opaque_pass>());
	render_system.add_render_pass(std::make_shared<rnd::skybox_pass>());
	render_system.add_render_pass(std::make_shared<rnd::transparent_pass>());
	render_system.add_render_pass(std::make_shared<rnd::debug_pass>(res_sys));
	render_system.add_render_pass(std::make_shared<rnd::composition_pass>(res_sys.engine_file("shaders/composition.vert"), res_sys.engine_file("shaders/composition.frag")));
	render_system.set_material_compiler(std::make_shared<rnd::default_material_compiler>());
	// Views are named by their owners (the editor registers "scene" and "game"); the command is there for the
	// game without an editor too.
	auto& capture = data.construct<rnd::capture_service>(*render_system.get_driver());
	auto& commands = data.construct<render_commands>();
	if (data.has_value<cmd::command_registry>()) {
		commands.capture = rnd::register_capture_commands(data.require<cmd::command_registry>(), capture);
		commands.compare = rnd::register_capture_compare_command(data.require<cmd::command_registry>(), capture, res_sys);
	}
}

void engine::render::render_term(ds::app_data_storage& data)
{
	auto& desc_sys = data.require<desc::desc_system>();
	data.require<res::resource_system>().unregistrate_adapter(rnd::geometry_desc_adapter::INFO);
	unregister_render_descs(desc_sys);

	data.destruct<render_commands>();
	data.destruct<rnd::capture_service>();
	data.destruct<rnd::frame_context>();
	data.destruct<rnd::render_system>();
	data.destruct<rnd::driver::swapchain_interface>();
}