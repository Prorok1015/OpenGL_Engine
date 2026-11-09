#include "edt_model_file_access.h"
#include <snk/resource/res_path_sanitize.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <fstream>
#include <initializer_list>
#include <utility>
#include <stdexcept>
#include <system_error>

namespace fs = std::filesystem;

namespace {
	std::string lowered_extension(const fs::path& path)
	{
		std::string text = path.extension().string();
		std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return text;
	}

	bool is_image_extension(const std::string& extension)
	{
		return extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".tga" || extension == ".bmp";
	}

	bool starts_with(const std::array<unsigned char, 18>& head, std::initializer_list<unsigned char> prefix)
	{
		return std::equal(prefix.begin(), prefix.end(), head.begin());
	}

	bool looks_like_image(const std::string& extension, const std::array<unsigned char, 18>& head)
	{
		if (extension == ".png") return starts_with(head, { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A });
		if (extension == ".jpg" || extension == ".jpeg") return starts_with(head, { 0xFF, 0xD8, 0xFF });
		if (extension == ".bmp") return starts_with(head, { 'B', 'M' });
		const unsigned char type = head[2];
		const unsigned char depth = head[16];
		const bool known_type = type == 1 || type == 2 || type == 3 || type == 9 || type == 10 || type == 11;
		const bool known_depth = depth == 8 || depth == 15 || depth == 16 || depth == 24 || depth == 32;
		return head[1] <= 1 && known_type && known_depth;
	}

	bool has_hidden_segment(const fs::path& relative)
	{
		return std::any_of(relative.begin(), relative.end(), [](const fs::path& part) {
			const std::string name = part.string();
			return !name.empty() && name.front() == '.';
		});
	}

	std::string key_of(const fs::path& path)
	{
		return path.generic_string();
	}
}

edt::model_file_access::model_file_access(fs::path root_file, model_file_limits limits, const std::vector<referenced_file>* allowed)
	: m_root_file(std::move(root_file)), m_limits(limits), m_allowed(allowed)
{
	std::error_code ec;
	m_dir = fs::weakly_canonical(m_root_file.parent_path(), ec);
	if (ec) m_dir = m_root_file.parent_path();
	m_root_extension = lowered_extension(m_root_file);
}

edt::resolved_reference edt::model_file_access::resolve(std::string_view reference, model_file_kind kind)
{
	resolved_reference out;
	out.kind = kind;
	const auto refuse = [&](std::string why) {
		out.path.clear();
		out.error = std::format("the model refers to '{}': {}", std::string(reference).substr(0, 200), why);
		return out;
	};

	const auto inside = res::path_within_dir(m_dir, reference, kind == model_file_kind::side_file);
	if (!inside) return refuse("outside the folder of the model");
	std::error_code ec;
	const fs::path relative = inside->lexically_relative(m_dir);
	if (relative.empty() || relative.native().starts_with(fs::path("..").native())) return refuse("outside the folder of the model");
	if (has_hidden_segment(relative)) return refuse("a hidden file or folder");

	const std::string extension = lowered_extension(*inside);
	if (kind == model_file_kind::texture) {
		if (!is_image_extension(extension)) return refuse("not an image (.png, .jpg, .tga or .bmp)");
	} else {
		const bool allowed_side = (m_root_extension == ".gltf" && extension == ".bin") || (m_root_extension == ".obj" && extension == ".mtl");
		if (!allowed_side) return refuse("a file this kind of model may not read");
	}

	if (!fs::exists(*inside, ec)) return out;
	if (fs::symlink_status(*inside, ec).type() != fs::file_type::regular) return refuse("not a plain file");
	out.size = fs::file_size(*inside, ec);
	if (ec) return refuse("its size cannot be read");
	out.modified = fs::last_write_time(*inside, ec);
	if (ec) return refuse("its time cannot be read");
	out.id = file_id_of(*inside);
	if (out.size > m_limits.max_file_bytes) return refuse(std::format("too big ({} bytes, the limit is {})", out.size, m_limits.max_file_bytes));

	if (kind == model_file_kind::texture) {
		std::array<unsigned char, 18> head{};
		std::ifstream in(*inside, std::ios::binary);
		if (!in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size())) || !looks_like_image(extension, head)) {
			return refuse("the bytes of the file are no image");
		}
	}

	if (m_allowed) {
		const auto listed = std::find_if(m_allowed->begin(), m_allowed->end(), [&](const referenced_file& file) { return file.path == *inside; });
		if (listed == m_allowed->end()) return refuse("not among the files the person was shown");
		if (listed->size != out.size || listed->modified != out.modified || listed->id != out.id) return refuse("the file changed since the person was shown it");
	} else if (m_seen.insert(key_of(*inside)).second) {
		if (m_files.size() >= m_limits.max_files) return refuse(std::format("the model refers to more than {} files", m_limits.max_files));
		if (m_listed_bytes + out.size > m_limits.max_total_bytes) {
			return refuse(std::format("the files of the model together are bigger than {} bytes", m_limits.max_total_bytes));
		}
		m_listed_bytes += out.size;
		m_files.push_back({ *inside, out.size, out.modified, out.id });
	}
	out.path = *inside;
	return out;
}

std::optional<std::vector<std::byte>> edt::model_file_access::read(const resolved_reference& file)
{
	if (!file.found()) return std::nullopt;
	m_bytes_read += file.size;
	if (m_bytes_read > m_limits.max_total_bytes) throw std::runtime_error("the import would read more bytes from the disk than it was allowed");
	auto bytes = res::read_regular_file_capped(file.path, file.size);
	if (!bytes || bytes->size() != file.size) return std::nullopt;
	if (file.kind == model_file_kind::texture) {
		std::array<unsigned char, 18> head{};
		std::copy_n(reinterpret_cast<const unsigned char*>(bytes->data()), (std::min)(head.size(), bytes->size()), head.begin());
		if (bytes->size() < head.size() || !looks_like_image(lowered_extension(file.path), head)) {
			throw std::runtime_error(std::format("'{}' is no image", file.path.filename().string()));
		}
	}
	std::error_code ec;
	if (fs::file_size(file.path, ec) != file.size || ec || fs::last_write_time(file.path, ec) != file.modified || ec || file_id_of(file.path) != file.id) {
		throw std::runtime_error(std::format("'{}' changed while it was read", file.path.filename().string()));
	}
	const content_digest digest = digest_of(*bytes);
	if (m_allowed) {
		const auto listed = std::find_if(m_allowed->begin(), m_allowed->end(), [&](const referenced_file& entry) { return entry.path == file.path; });
		if (listed != m_allowed->end() && listed->hashed && listed->digest != digest) {
			throw std::runtime_error(std::format("'{}' is not the file the person was shown: its content changed", file.path.filename().string()));
		}
	} else {
		const auto listed = std::find_if(m_files.begin(), m_files.end(), [&](const referenced_file& entry) { return entry.path == file.path; });
		if (listed != m_files.end()) {
			listed->digest = digest;
			listed->hashed = true;
		}
	}
	return bytes;
}

std::string edt::model_file_access::relative_name(const resolved_reference& file) const
{
	return file.path.lexically_relative(m_dir).generic_string();
}
