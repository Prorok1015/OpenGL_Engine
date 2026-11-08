#include "edt_filesystem_assimp_io.h"
#include <assimp/MemoryIOWrapper.h>
#include <snk/common/logger/engine_log.h>
#include <snk/resource/res_path_sanitize.h>
#include <stdexcept>

edt::filesystem_assimp_io::filesystem_assimp_io(
	const std::filesystem::path& root_path,
	const std::vector<std::byte>& root_data,
	model_file_access* access)
	: m_root_path(root_path)
	, m_base_dir(root_path.parent_path())
	, m_root_data(root_data)
	, m_access(access)
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

	if (m_access) return m_access->resolve(pFile, model_file_kind::side_file).found();

	// Model files name their dependencies (mtllib, buffer uris): never look outside the model directory.
	const auto full = res::path_within_dir(m_base_dir, pFile, true);
	return full.has_value() && std::filesystem::exists(*full, ec);
}

char edt::filesystem_assimp_io::getOsSeparator() const
{
	return '/';
}

Assimp::IOStream* edt::filesystem_assimp_io::Open(const char* pFile, const char* pMode)
{
	std::filesystem::path p(pFile);

	// Root file — serve from memory
	std::error_code ec;
	if (p == m_root_path || std::filesystem::equivalent(p, m_root_path, ec)) {
		return new Assimp::MemoryIOStream(
			reinterpret_cast<const uint8_t*>(m_root_data.data()),
			m_root_data.size());
	}

	if (m_access) {
		const resolved_reference reference = m_access->resolve(pFile, model_file_kind::side_file);
		if (!reference.error.empty()) {
			egLOG_WARN("edt/import", "Refused to open '{}': {}", pFile, reference.error);
			throw std::runtime_error(reference.error);
		}
		if (!reference.found()) return nullptr;
		auto& cached = m_file_cache[reference.path.string()];
		if (cached.empty()) {
			auto bytes = m_access->read(reference);
			if (!bytes) return nullptr;
			cached = std::move(*bytes);
		}
		return new Assimp::MemoryIOStream(reinterpret_cast<const uint8_t*>(cached.data()), cached.size());
	}

	// Subsidiary file — read from model's directory
	const auto full = res::path_within_dir(m_base_dir, pFile, true);
	if (!full.has_value()) {
		egLOG_WARN("edt/import", "Refused to open '{}': outside the model directory", pFile);
		return nullptr;
	}
	std::string key = full->string();

	auto it = m_file_cache.find(key);
	if (it == m_file_cache.end()) {
		auto bytes = res::read_regular_file_capped(*full);
		if (!bytes.has_value())
			return nullptr;

		auto [ins, _] = m_file_cache.emplace(key, std::move(*bytes));
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
