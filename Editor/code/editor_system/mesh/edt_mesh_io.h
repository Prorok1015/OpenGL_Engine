#pragma once
#include "edt_mesh_model.h"
#include "edt_mesh_shapes.h"
#include <snk/desc/desc_base.hpp>
#include <snk/render/geom/rnd_geometry_desc.h>
#include <snk/resource/res_tag.h>
#include <cstddef>
#include <optional>
#include <string>

namespace res { class resource_system; }

namespace edt::mesh
{
	// What a *.poly.desc may hold. The file comes from outside (a project, a repository): anything over these fails the load
	// with a message that names the place in the file. A parameter of poly_mesh_from_json so that tests can use small ones.
	struct poly_read_limits
	{
		std::size_t vertices = 2'000'000;
		std::size_t faces = 1'000'000;
		std::size_t corners = 8'000'000;
		std::size_t face_corners = 1024;
		std::size_t edges = 8'000'000;
		std::uint32_t material_slot = 255;
	};

	// The editable source of a mesh (x.poly.desc). Only the editor registers this type; the engine never reads it.
	// `generator` is the shape the mesh was made from while nobody has edited it by hand.
	class poly_mesh_desc : public desc::desc_base
	{
	public:
		using base_type = desc::desc_base;

		void copy_to(desc::desc_base& other) const override
		{
			copy_to_same(*this, other);
		}

		void deserialize(desc::desc_system& system, const json::object& resource) override;
		void serialize(json::object& resource) const override;

		poly_mesh mesh;
		std::optional<shape_params> generator;
	};

	// Throw res::load_error on a value that is not what the format says. Free of a desc_system, for tests and tools.
	void poly_mesh_from_json(const json::object& resource, poly_mesh_desc& out, const poly_read_limits& limits = {});
	json::object poly_mesh_to_json(const poly_mesh_desc& source);

	struct geometry_file_options
	{
		// `__parent` of the game's base geometry desc, as the importer writes it.
		bool write_parent = true;
	};

	// Writes `file` (*.geom.desc) with its binary file (EPIC-79) through rnd::write_geometry_files: compact JSON, no `bounds`
	// (the runtime does not use them). Replacing a file in JSON arrays logs its conversion (Editor/Save). Empty when written.
	std::string write_mesh_geometry(res::resource_system& system, const res::tag& file, const rnd::geometry_desc& geometry,
		const geometry_file_options& options = {});

	// Whether `file` exists and holds its vertices as JSON arrays (the form before EPIC-79).
	bool file_holds_geometry_arrays(res::resource_system& system, const res::tag& file);

	void register_mesh_descs(desc::desc_system& system);
	void unregister_mesh_descs(desc::desc_system& system);
}
