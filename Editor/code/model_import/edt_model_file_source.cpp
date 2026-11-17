#include "edt_model_file_source.h"
#include <snk/common/logger/engine_log.h>
#include <snk/resource/res_path_sanitize.h>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace {
	constexpr std::string_view k_unread_texture = "is missing, outside the model's folder, not a regular file or too big";
}

edt::model_file_source::model_file_source(
	const std::filesystem::path& root_path,
	const std::vector<std::byte>& root_data,
	model_file_access* access)
	: m_root_path(root_path)
	, m_base_dir(root_path.parent_path())
	, m_root_data(root_data)
	, m_access(access)
{
}

bool edt::model_file_source::is_root(const std::filesystem::path& path) const
{
	// The decoder process (#1043) touches no disk: the root is only its own path, compared as text.
	if (m_access && m_access->provided()) return path.lexically_normal() == m_root_path.lexically_normal();
	std::error_code ec;
	return path == m_root_path || ((!m_access || m_access->on_disk()) && std::filesystem::equivalent(path, m_root_path, ec));
}

bool edt::model_file_source::exists(std::string_view name)
{
	const std::filesystem::path path(name);
	if (path == m_root_path)
		return true;

	if (m_access && !m_access->on_disk()) {
		// A model given only as bytes fails on any file it names instead of loading without it: open() refuses through
		// model_file_access, and a loader that asks exists() first (and skips a missing file) is refused here.
		throw std::runtime_error(m_access->resolve(name, model_file_kind::side_file).error);
	}

	if (m_access && m_access->provided())
		return is_root(path) || m_access->resolve(name, model_file_kind::side_file).found();

	std::error_code ec;
	if (std::filesystem::equivalent(path, m_root_path, ec))
		return true;

	if (m_access) return m_access->resolve(name, model_file_kind::side_file).found();

	// Model files name their dependencies (mtllib, buffer uris): never look outside the model directory.
	const auto full = res::path_within_dir(m_base_dir, name, true);
	return full.has_value() && std::filesystem::exists(*full, ec);
}

mdl::opened_file edt::model_file_source::open(std::string_view name)
{
	if (is_root(std::filesystem::path(name))) {
		return { &m_root_data, {} };
	}

	if (m_access) {
		const resolved_reference reference = m_access->resolve(name, model_file_kind::side_file);
		if (!reference.error.empty()) {
			egLOG_WARN("edt/import", "Refused to open '{}': {}", name, reference.error);
			throw std::runtime_error(reference.error);
		}
		if (!reference.found()) return {};
		auto& cached = m_file_cache[reference.path.string()];
		if (cached.empty()) {
			auto bytes = m_access->read(reference);
			if (!bytes) return {};
			cached = std::move(*bytes);
		}
		return { &cached, {} };
	}

	const auto full = res::path_within_dir(m_base_dir, name, true);
	if (!full.has_value()) {
		return { nullptr, "it is outside the model's folder" };
	}
	const std::string key = full->string();
	auto it = m_file_cache.find(key);
	if (it == m_file_cache.end()) {
		auto bytes = res::read_regular_file_capped(*full);
		if (!bytes.has_value())
			return {};
		it = m_file_cache.emplace(key, std::move(*bytes)).first;
	}
	return { &it->second, {} };
}

std::string edt::model_file_source::texture_key(std::string_view name)
{
	if (!m_access) return std::string{ name };
	resolved_reference reference = m_access->resolve(name, model_file_kind::texture);
	if (!reference.error.empty()) throw std::runtime_error(reference.error);
	std::string key = reference.found() ? m_access->relative_name(reference) : std::string{ name };
	m_textures.insert_or_assign(std::string{ name }, std::move(reference));
	return key;
}

mdl::texture_bytes edt::model_file_source::read_texture(std::string_view name)
{
	mdl::texture_bytes out;
	if (m_access) {
		const auto it = m_textures.find(std::string{ name });
		if (it != m_textures.end()) out.bytes = m_access->read(it->second);
	} else {
		// The name comes from the model file: only regular files inside the model's folder are read.
		out.bytes = res::read_file_within_dir(m_base_dir, name);
	}
	if (!out.bytes) out.reason = std::string{ k_unread_texture };
	return out;
}
