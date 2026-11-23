#pragma once
#include "edt_mesh_document.h"
#include <snk/editor/edt_document_manager.h>
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
