#include "edt_filesystem_assimp_io.h"
#include <assimp/MemoryIOWrapper.h>
#include <fstream>

edt::filesystem_assimp_io::filesystem_assimp_io(
	const std::filesystem::path& root_path,
	const std::vector<std::byte>& root_data)
	: m_root_path(root_path)
	, m_base_dir(root_path.parent_path())
	, m_root_data(root_data)
{
}

bool edt::filesystem_assimp_io::Exists(const char* pFile) const
{
	std::filesystem::path p(pFile);
	if (p == m_root_path)
		return true;

	std::error_code ec;
	if (std::filesystem::equivalent(p, m_root_path, ec))
		return true;

	auto full = m_base_dir / p;
	return std::filesystem::exists(full);
}

char edt::filesystem_assimp_io::getOsSeparator() const
{
	return '/';
}

Assimp::IOStream* edt::filesystem_assimp_io::Open(const char* pFile, const char* pMode)
{
	std::filesystem::path p(pFile);

	// Root file — serve from memory
	if (p == m_root_path || std::filesystem::equivalent(p, m_root_path)) {
		return new Assimp::MemoryIOStream(
			reinterpret_cast<const uint8_t*>(m_root_data.data()),
			m_root_data.size());
	}

	// Subsidiary file — read from model's directory
	auto full = m_base_dir / p;
	std::string key = full.string();

	auto it = m_file_cache.find(key);
	if (it == m_file_cache.end()) {
		std::ifstream in(full, std::ios::binary | std::ios::ate);
		if (!in)
			return nullptr;

		auto size = static_cast<std::streamsize>(in.tellg());
		in.seekg(0);
		std::vector<std::byte> bytes(static_cast<size_t>(size));
		in.read(reinterpret_cast<char*>(bytes.data()), size);

		auto [ins, _] = m_file_cache.emplace(key, std::move(bytes));
		it = ins;
	}

	return new Assimp::MemoryIOStream(
		reinterpret_cast<const uint8_t*>(it->second.data()),
		it->second.size());
}

void edt::filesystem_assimp_io::Close(Assimp::IOStream* pFile)
{
	delete pFile;
}
