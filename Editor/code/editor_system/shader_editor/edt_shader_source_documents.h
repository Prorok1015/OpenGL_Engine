#pragma once
#include "edt_shader_source_document.h"
#include <snk/editor/edt_document_manager.h>
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace edt
{
	// The open shader sources in the order they were opened. The manager keeps every preview as a previewable_document.
	inline std::vector<shader_source_document*> shader_sources_of(document_manager& documents)
	{
		std::vector<shader_source_document*> out;
		for (previewable_document* preview : documents.previews()) {
			if (auto* shader = dynamic_cast<shader_source_document*>(preview); shader && shader->is_open()) out.push_back(shader);
		}
		return out;
	}

	inline std::vector<shader_source_document*> dirty_shader_sources_of(document_manager& documents)
	{
		std::vector<shader_source_document*> out = shader_sources_of(documents);
		std::erase_if(out, [](const shader_source_document* shader) { return !shader->is_dirty(); });
		return out;
	}

	inline shader_source_document* shader_source_by_id(document_manager& documents, document_id id)
	{
		for (shader_source_document* shader : shader_sources_of(documents)) {
			if (shader->id() == id) return shader;
		}
		return nullptr;
	}

	// One kind and one preview per suffix: the registry matches a suffix as written (case-sensitive), and asset_type_of says "shader" for
	// these (is_shader_source). The ids start with k_shader_source_id_prefix, which the asset browser's filter reads as the kind "shader".
	inline constexpr std::string_view k_shader_source_id_prefix = "shader_source";
	inline constexpr std::array<std::string_view, 8> k_shader_source_suffixes = { ".glsl", ".vert", ".frag", ".geom", ".comp", ".tesc", ".tese", ".shader" };

	inline std::string shader_source_id(std::string_view suffix)
	{
		return std::string(k_shader_source_id_prefix).append(suffix);
	}

	// The id of the preview that makes the document of `path` (a res:// tag or a relative path), empty when no registered suffix ends it.
	inline std::string shader_preview_id_of(std::string_view path)
	{
		for (const std::string_view suffix : k_shader_source_suffixes) {
			if (path.ends_with(suffix)) return shader_source_id(suffix);
		}
		return {};
	}
}
