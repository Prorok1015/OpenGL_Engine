#pragma once
#include <snk/model_codec/res_tag.h>
#include <snk/editor/edt_document.h>
#include <cstddef>
#include <functional>
#include <span>
#include <string>

namespace res { class resource_system; }

namespace edt
{
	class data_document;
	class document_manager;
	class mesh_document;
	class prefab_document;
	class shader_source_document;

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
	// The text of the shader (its ending and BOM stay the file's own). A file changed on disk since the autosave (`source_changed`)
	// comes back as a disk conflict the person settles with Reload / Overwrite. Refused for a read-only or locked shader.
	std::string restore_shader_source_bytes(shader_source_document& shader, std::span<const std::byte> bytes, bool source_changed);

	struct shader_restore_result
	{
		std::string error;
		document_id document{};
		std::uint64_t generation = 0;
	};
	// Opens `source` through `open` (a document of the manager, or the one already open), and restores the bytes into it. A shader that is
	// dirty, or holds typing, is refused: its edits are the person's.
	shader_restore_result restore_shader_source(document_manager& documents, const std::function<bool(const res::tag&)>& open,
		const res::tag& source, std::span<const std::byte> bytes, bool source_changed);
}
