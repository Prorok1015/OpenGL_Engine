#pragma once
#include "edt_mesh_document.h"
#include <snk/editor/edt_document_manager.h>
#include <string>
#include <string_view>
#include <vector>

namespace edt
{
	// The open mesh documents in the order they were opened. The manager keeps every preview as a previewable_document.
	inline std::vector<mesh_document*> meshes_of(document_manager& documents)
	{
		std::vector<mesh_document*> out;
		for (previewable_document* preview : documents.previews()) {
			if (auto* mesh = dynamic_cast<mesh_document*>(preview); mesh && mesh->is_open()) out.push_back(mesh);
		}
		return out;
	}

	inline std::vector<mesh_document*> dirty_meshes_of(document_manager& documents)
	{
		std::vector<mesh_document*> out = meshes_of(documents);
		std::erase_if(out, [](const mesh_document* mesh) { return !mesh->is_dirty(); });
		return out;
	}

	// The open mesh whose source is the poly file `source` names, or the poly file beside the geometry file it names: one source is one
	// document, however it was opened (two documents on one preview key would destroy each other's world and fight over the file).
	inline mesh_document* mesh_of_source(document_manager& documents, const res::tag& source)
	{
		std::string poly(source.view());
		constexpr std::string_view geometry_suffix = ".geom.desc";
		if (poly.ends_with(geometry_suffix)) {
			poly.resize(poly.size() - geometry_suffix.size());
			poly += ".poly.desc";
		}
		const res::tag wanted{ poly };
		for (mesh_document* mesh : meshes_of(documents)) {
			if (mesh->source() == wanted) return mesh;
		}
		return nullptr;
	}

	inline mesh_document* mesh_by_id(document_manager& documents, document_id id)
	{
		for (mesh_document* mesh : meshes_of(documents)) {
			if (mesh->id() == id) return mesh;
		}
		return nullptr;
	}

	// The id of the previews that make a mesh_document, one per suffix (a module's preview with a longer suffix cannot take their place).
	inline constexpr std::string_view k_mesh_preview_id = "mesh";
	inline constexpr std::string_view k_mesh_geometry_preview_id = "mesh_geometry";
}
