#pragma once
#include <snk/model_codec/res_tag.h>
#include <boost/json/object.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <entt/entt.hpp>

namespace scn
{
	class prefab_desc;
	struct camera_component;
}

namespace edt
{
	class data_document;
	struct editor_services;
	struct material_shader;

	// The shape the material preview draws (the unit shapes of core/res/shapes).
	enum class preview_shape
	{
		sphere,
		cube,
		plane,
	};

	std::string_view preview_shape_label(preview_shape shape);
	// res://shapes/<name>.geom.desc of the shape.
	std::string preview_shape_geometry(preview_shape shape);

	// The material uses the engine's sky shader: a mesh drawn with it shows nothing useful, so it has no preview.
	bool is_sky_material(const material_shader& shader);

	// The live preview of a material window (#1196 PR 2), the mesh preview's pattern (mesh_document): a preview set of the
	// context manager made from edt://templates/prefab_preview.desc (a light and the sky), an orbit camera drawing into
	// memory:// targets, and one mesh node of the chosen shape whose material is a memory:// copy of the document as it is
	// shown, unsaved edits included. The copy is written again when the document's generation moves. Main thread only.
	class material_preview
	{
	public:
		material_preview(data_document& document, editor_services& services);
		~material_preview();
		material_preview(const material_preview&) = delete;
		material_preview& operator=(const material_preview&) = delete;

		// Once per frame while the window is shown: makes the world when there is none, writes the material copy again when
		// the document changed. A sky material gives everything up and shows no preview.
		void update(const material_shader& shader);

		preview_shape shape() const { return m_shape; }
		void set_shape(preview_shape shape);
		// The template's sky (true) or none: the camera's clear shows behind the mesh.
		bool sky() const { return m_sky; }
		void set_sky(bool shown);
		bool sky_material() const { return m_sky_material; }

		// The window is hidden or collapsed: the camera draws nothing until it is shown again.
		void park();
		// The game layer goes: the world (a set of its context manager) is dropped, the material copy and the targets stay;
		// rebuild_world makes it again in the new layer.
		void release_world();
		void rebuild_world();
		// Gives up the world, the render targets, the world template and the material copy (also done by the destructor).
		void close();

		// The render targets are the renderer's: this gives their tags up when the preview closes.
		using targets_release = std::function<void(const std::vector<res::tag>&)>;
		void set_targets_release(targets_release release) { m_release_targets = std::move(release); }

		static constexpr std::string_view context_name() { return "__material_preview"; }
		bool has_world() const { return m_has_world; }
		// Null without a world.
		entt::registry* registry();
		scn::camera_component* camera();
		// The entity of the mesh node; null without a world.
		entt::entity shape_entity();
		const std::string& set_name() const { return m_set_name; }
		const std::string& render_target() const { return m_rt_color; }
		// The memory:// copy of the material the mesh node draws with.
		const res::tag& material_tag() const { return m_material_tag; }
		const res::tag& world_template() const { return m_world_tag; }

	private:
		bool write_material();
		void erase_material();
		void open_world();
		void apply_sky();
		void sync_node();
		boost::json::object node_fields() const;
		void release_resources();

		data_document& m_document;
		editor_services& m_services;
		targets_release m_release_targets;

		std::uint64_t m_serial = 0;
		preview_shape m_shape = preview_shape::sphere;
		// The shape the mesh node was given last.
		preview_shape m_node_shape = preview_shape::sphere;
		bool m_sky = true;
		bool m_sky_material = false;
		// The world could not be made (no context manager, no template): not tried again until rebuild_world.
		bool m_failed = false;
		// Between release_world and rebuild_world (a game layer change).
		bool m_layer_parked = false;
		bool m_has_world = false;
		bool m_material_written = false;
		std::uint64_t m_written_generation = 0;

		res::tag m_material_tag;
		res::tag m_world_tag;
		std::string m_key;
		std::string m_set_name;
		std::string m_rt_color;
		std::string m_rt_depth;
		std::string m_rt_accum;
		std::string m_rt_reveal;
		std::string m_working_tag;
		std::unique_ptr<scn::prefab_desc> m_prefab;
		entt::entity m_root = entt::null;
	};
}
