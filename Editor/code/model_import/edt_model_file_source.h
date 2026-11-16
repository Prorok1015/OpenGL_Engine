#pragma once
#include "edt_model_file_access.h"
#include <snk/model_import/mdl_import.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace edt {

	// What the editor's import reads (#1108): the model file from memory (already read), the files it names from the
	// model's folder. With `access` (an agent's import, the scans, the decoder process) model_file_access decides what
	// may be read and refuses the rest by throwing, which stops the import; without it only regular files inside the
	// model's folder are read.
	class model_file_source final : public mdl::file_source {
	public:
		model_file_source(
			const std::filesystem::path& root_path,
			const std::vector<std::byte>& root_data,
			model_file_access* access = nullptr);

		mdl::opened_file open(std::string_view name) override;
		bool exists(std::string_view name) override;
		std::string texture_key(std::string_view name) override;
		mdl::texture_bytes read_texture(std::string_view name) override;

	private:
		bool is_root(const std::filesystem::path& path) const;

		std::filesystem::path m_root_path;
		std::filesystem::path m_base_dir;
		const std::vector<std::byte>& m_root_data;
		// Set for an agent's import: the files the model may read are decided there.
		model_file_access* m_access;
		// Side files, read once.
		std::unordered_map<std::string, std::vector<std::byte>> m_file_cache;
		// The textures texture_key resolved, by the name the model wrote.
		std::unordered_map<std::string, resolved_reference> m_textures;
	};

} // namespace edt
