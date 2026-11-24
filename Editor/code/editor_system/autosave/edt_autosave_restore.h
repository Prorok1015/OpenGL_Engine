#pragma once
#include <snk/model_codec/res_tag.h>
#include <cstddef>
#include <span>
#include <string>

namespace res { class resource_system; }

namespace edt
{
	class data_document;
	class mesh_document;
	class prefab_document;

	// Crash recovery (#1187): the verified bytes of an autosave go into an open document through the kind's own reader,
	// as an edit (the document is dirty, its source file is not written). Each returns why not, empty when it worked.

	// Stores the bytes as the working copy `copy` (memory://), reads it as a prefab through the resource system and takes it
	// into the document's working copy; the copy is erased again.
	std::string restore_prefab_bytes(res::resource_system& writable, res::resource_system& resources, prefab_document& prefab,
		const res::tag& copy, std::span<const std::byte> bytes);
	// The poly file read by the mesh module's own reader.
	std::string restore_mesh_bytes(mesh_document& mesh, std::span<const std::byte> bytes);
	// The file's object as the document's own fields (data_document::restore_own: another __type or __parent is refused).
	std::string restore_data_bytes(data_document& data, std::span<const std::byte> bytes);
}
