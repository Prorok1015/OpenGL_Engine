#pragma once
#include "common.h"
#include <assimp/IOSystem.hpp>
#include <filesystem>
#include <vector>
#include <unordered_map>

namespace edt {

	// Assimp IO wrapper that reads files from the real filesystem.
	// The root file (already loaded into memory) is served from a buffer;
	// subsidiary files (textures etc.) are read from the model's directory.
	class filesystem_assimp_io : public Assimp::IOSystem {
	public:
		filesystem_assimp_io(
			const std::filesystem::path& root_path,
			const std::vector<std::byte>& root_data);

		~filesystem_assimp_io() override = default;

		bool Exists(const char* pFile) const override;
		char getOsSeparator() const override;
		Assimp::IOStream* Open(const char* pFile, const char* pMode) override;
		void Close(Assimp::IOStream* pFile) override;

	private:
		std::filesystem::path m_root_path;
		std::filesystem::path m_base_dir;
		const std::vector<std::byte>& m_root_data;
		// Cache for subsidiary files
		std::unordered_map<std::string, std::vector<std::byte>> m_file_cache;
	};

} // namespace edt
