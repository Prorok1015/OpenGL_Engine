#pragma once
#include "edt_preview_stage.h"
#include <snk/model_codec/res_tag.h>
#include <boost/json/object.hpp>
#include <cstdint>
#include <string>
#include <string_view>

namespace edt
{
	class data_document;
	class material_window_model;
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

	// The material window's preview content (#1196 PR 2): one mesh node of the chosen shape whose material is a memory://
	// copy of the document as it is shown, unsaved edits included, written again when the document's generation moves.
	// A sky material shows nothing. Main thread only.
	class material_preview_content final : public preview_content
	{
	public:
		material_preview_content(data_document& document, material_window_model& model, editor_services& services);
		~material_preview_content() override;
		material_preview_content(const material_preview_content&) = delete;
		material_preview_content& operator=(const material_preview_content&) = delete;

		state update() override;
		void build(scn::prefab_desc::prefab_node& root) override;
		void refresh(scn::prefab_desc::prefab_node& root) override;
		void release() override;

		preview_shape shape() const { return m_shape; }
		// The stage's sync() shows it.
		void set_shape(preview_shape shape) { m_shape = shape; }
		bool sky_material() const { return m_sky_material; }
		// The memory:// copy of the material the mesh node draws with.
		const res::tag& material_tag() const { return m_material_tag; }

	private:
		bool write_material();
		boost::json::object node_fields() const;

		data_document& m_document;
		material_window_model& m_model;
		editor_services& m_services;

		preview_shape m_shape = preview_shape::sphere;
		// The shape the mesh node was given last.
		preview_shape m_node_shape = preview_shape::sphere;
		bool m_sky_material = false;
		bool m_material_written = false;
		std::uint64_t m_written_generation = 0;
		res::tag m_material_tag;
	};
}
