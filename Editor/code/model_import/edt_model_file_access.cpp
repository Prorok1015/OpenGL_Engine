#include "edt_model_file_access.h"
#include <snk/common/engine_assert.h>
#include <snk/common/fs_paths.h>
#include <snk/common/text_escape.h>
#include <snk/platform/plt_beneath.h>
#include <snk/resource/res_path_sanitize.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <format>
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

	// ".png" of "tex/skin.PNG", from a generic UTF-8 name.
	std::string lowered_extension_of_name(std::string_view name)
	{
		const std::size_t slash = name.rfind('/');
		const std::string_view leaf = slash == std::string_view::npos ? name : name.substr(slash + 1);
		const std::size_t dot = leaf.rfind('.');
		if (dot == std::string_view::npos || dot == 0) return {};
		std::string text(leaf.substr(dot));
		std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return text;
	}

	std::string shown(std::string_view text)
	{
		return text_escape::escaped_text(text, 256);
	}

	bool image_bytes_match(const std::string& extension, const std::vector<std::byte>& bytes)
	{
		std::array<unsigned char, 18> head{};
		if (bytes.size() < head.size()) return false;
		std::copy_n(reinterpret_cast<const unsigned char*>(bytes.data()), head.size(), head.begin());
		return looks_like_image(extension, head);
	}

	std::string shown_leaf(const fs::path& path)
	{
		return shown(fs_paths::utf8_of(path.filename()));
	}

	std::string with_forward_slashes(std::string_view text)
	{
		std::string out(text);
		std::replace(out.begin(), out.end(), '\\', '/');
		return out;
	}

	// Names Windows reads as another machine's share, a device, or a stream of a file: refused on every system, before the
	// name reaches the file system (weakly_canonical of "//host/share" already goes to the network).
	bool is_spelling_the_rule_refuses(std::string_view reference)
	{
		if (reference.find('\0') != std::string_view::npos) return true;
		const std::string text = with_forward_slashes(reference);
		if (text.starts_with("//") || text.find("//?/") != std::string::npos || text.find("//./") != std::string::npos) return true;
		const bool drive = text.size() >= 3 && std::isalpha(static_cast<unsigned char>(text[0])) && text[1] == ':' && text[2] == '/';
		return text.find(':', drive ? std::size_t{ 2 } : std::size_t{ 0 }) != std::string::npos;
	}

	// `path` and `base` normalised, `path` is `base` or below it, compared by components.
	bool lexically_under(const fs::path& path, const fs::path& base)
	{
		if (base.empty()) return false;
		auto it = path.begin();
		for (const fs::path& part : base) {
			if (part.empty()) continue;
			if (it == path.end() || *it != part) return false;
			++it;
		}
		return std::none_of(it, path.end(), [](const fs::path& part) { return part == ".."; });
	}
}

edt::model_file_access::model_file_access(fs::path root_file, model_file_limits limits, const std::vector<referenced_file>* allowed,
	const fs::path& boundary, bool on_disk, model_read_rule may_read, model_file_reader read_file)
	: m_root_file(std::move(root_file)), m_limits(limits), m_allowed(allowed), m_may_read(std::move(may_read)),
	m_read_file(std::move(read_file)), m_on_disk(on_disk)
{
	m_root_extension = lowered_extension(m_root_file);
	m_dir_as_given = m_root_file.parent_path().lexically_normal();
	if (!m_on_disk) {
		m_dir = m_boundary = m_root_file.parent_path();
		return;
	}
	std::error_code ec;
	m_dir = fs::weakly_canonical(m_root_file.parent_path(), ec);
	if (ec) m_dir = m_root_file.parent_path();
	m_boundary = m_dir;
	if (!boundary.empty()) {
		fs::path wide = fs::weakly_canonical(boundary, ec);
		if (ec) wide = boundary.lexically_normal();
		const fs::path below = m_dir.lexically_relative(wide);
		if (!below.empty() && *below.begin() != "..") m_boundary = wide;
	}
}

edt::model_file_access::model_file_access(fs::path root_file, model_file_limits limits, const std::vector<provided_file>* provided, provided_mode mode)
	: m_root_file(std::move(root_file)), m_limits(limits), m_provided(provided), m_mode(mode)
{
	ASSERT_MSG(m_provided != nullptr, "model_file_access: the provided mode needs the list of provided files");
	m_root_extension = lowered_extension(m_root_file);
	m_dir = m_boundary = m_root_file.parent_path();
}

edt::resolved_reference edt::model_file_access::resolve_provided(std::string_view reference, model_file_kind kind)
{
	resolved_reference out;
	out.kind = kind;
	for (std::size_t index = 0; index < m_provided->size(); ++index) {
		const provided_file& entry = (*m_provided)[index];
		if (entry.name != reference) continue;
		if (entry.kind != kind) {
			out.error = std::format("the model asks for '{}' as another kind of file", shown(reference));
			return out;
		}
		if (!entry.bytes) return out;
		out.path = fs::path("provided") / std::to_string(index);
		out.size = entry.bytes->size();
		return out;
	}
	if (m_mode == provided_mode::record) {
		const bool known = std::any_of(m_requested.begin(), m_requested.end(),
			[&](const requested_file& asked) { return asked.kind == kind && asked.name == reference; });
		if (!known) m_requested.push_back({ std::string(reference), kind });
		return out;
	}
	// Not handed over is missing: nothing is read for it. A side file the scan found missing (an .obj whose .mtl is gone)
	// is not among the listed files, so refusing it here would refuse a model that imports with a default material.
	return out;
}

const edt::provided_file* edt::model_file_access::provided_entry(const resolved_reference& file) const
{
	if (!m_provided || !file.found() || file.path.parent_path() != fs::path("provided")) return nullptr;
	const std::string text = file.path.filename().string();
	std::size_t index = 0;
	const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), index);
	if (error != std::errc{} || end != text.data() + text.size() || index >= m_provided->size()) return nullptr;
	const provided_file& entry = (*m_provided)[index];
	return entry.bytes ? &entry : nullptr;
}

edt::resolved_reference edt::model_file_access::resolve(std::string_view reference, model_file_kind kind)
{
	if (m_provided) return resolve_provided(reference, kind);
	resolved_reference out;
	out.kind = kind;
	const auto refuse = [&](refusal_kind why_kind, std::string why) {
		out.path.clear();
		out.error = std::format("the model refers to '{}': {}", shown(reference), why);
		out.refusal = why_kind;
		return out;
	};
	if (!m_on_disk) return refuse(refusal_kind::rule, "the model is not a file on the disk, so it cannot read other files");

	const char* const outside = m_boundary == m_dir ? "outside the folder of the model" : "outside the content root of the model";
	constexpr const char* not_allowed = "a name the rule does not allow";
	if (reference.find('\0') != std::string_view::npos) return refuse(refusal_kind::rule, not_allowed);
	const fs::path given = fs_paths::path_from_utf8(with_forward_slashes(reference));
	const bool absolute_name = given.has_root_name() || given.has_root_directory();
	if (absolute_name) {
		// The model's own folder may be a share (//server/share): Assimp names its side files from it. Only what lies below
		// that folder is the model's spelling.
		const fs::path normal = given.lexically_normal();
		const fs::path* base = lexically_under(normal, m_dir_as_given) ? &m_dir_as_given : lexically_under(normal, m_boundary) ? &m_boundary : nullptr;
		if (!base) return refuse(refusal_kind::rule, outside);
		if (is_spelling_the_rule_refuses(fs_paths::generic_utf8_of(normal.lexically_relative(*base)))) return refuse(refusal_kind::rule, not_allowed);
	} else if (is_spelling_the_rule_refuses(reference)) {
		return refuse(refusal_kind::rule, not_allowed);
	} else if (!lexically_under((m_dir / given).lexically_normal(), m_boundary)) {
		// A ".." that leaves the boundary is refused here, so weakly_canonical below never asks the disk about a path outside it.
		return refuse(refusal_kind::rule, outside);
	}
	// With a rule of the caller, the path judged is the path resolved: `link/../x` must not reach the disk as written,
	// where the `..` would be taken after the link is followed.
	std::string judged;
	if (m_may_read) {
		// A lone '.' (glTF's "./tex.png") cannot pass a link and lexically_normal drops it; a '..' could.
		if (std::any_of(given.begin(), given.end(), [](const fs::path& part) { return part == ".."; })) {
			return refuse(refusal_kind::rule, "a '..' in a name an agent's import reads");
		}
		if (absolute_name && kind != model_file_kind::side_file) return refuse(refusal_kind::rule, outside);
		const fs::path candidate = absolute_name ? given.lexically_normal() : (m_dir / given).lexically_normal();
		if (std::string why = m_may_read(candidate); !why.empty()) return refuse(refusal_kind::rule, std::move(why));
		judged = fs_paths::generic_utf8_of(candidate);
	}
	const auto inside = m_may_read ? res::path_within_dir_from(m_boundary, m_dir, judged, true)
		: res::path_within_dir_from(m_boundary, m_dir, reference, kind == model_file_kind::side_file);
	if (!inside) return refuse(refusal_kind::rule, outside);
	std::error_code ec;
	const fs::path relative = inside->lexically_relative(m_boundary);
	if (relative.empty() || relative.native().starts_with(fs::path("..").native())) return refuse(refusal_kind::rule, outside);
	if (has_hidden_segment(relative)) return refuse(refusal_kind::rule, "a hidden file or folder");

	const std::string extension = lowered_extension(*inside);
	if (kind == model_file_kind::texture) {
		if (!is_image_extension(extension)) return refuse(refusal_kind::rule, "not an image (.png, .jpg, .tga or .bmp)");
	} else {
		const bool allowed_side = (m_root_extension == ".gltf" && extension == ".bin") || (m_root_extension == ".obj" && extension == ".mtl");
		if (!allowed_side) return refuse(refusal_kind::rule, "a file this kind of model may not read");
	}

	// Only stat-like calls here: the file is opened by read(), below the boundary and without blocking (a FIFO, a device).
	if (!fs::exists(*inside, ec)) return out;
	if (fs::symlink_status(*inside, ec).type() != fs::file_type::regular) return refuse(refusal_kind::other, "not a plain file");
	out.size = fs::file_size(*inside, ec);
	if (ec) return refuse(refusal_kind::other, "its size cannot be read");
	out.modified = fs::last_write_time(*inside, ec);
	if (ec) return refuse(refusal_kind::other, "its time cannot be read");
	out.id = file_id_of(*inside);
	if (!out.id.valid) return refuse(refusal_kind::other, "its identity on the disk cannot be read");
	if (out.size > m_limits.max_file_bytes) {
		return refuse(refusal_kind::limit, std::format("a side file of {} bytes exceeds {} per file ({})", out.size, limit_text(m_limits.max_file_bytes), k_step_side_files));
	}

	if (m_allowed) {
		const auto listed = std::find_if(m_allowed->begin(), m_allowed->end(), [&](const referenced_file& file) { return file.path == *inside; });
		if (listed == m_allowed->end()) return refuse(refusal_kind::rule, "not among the files the person was shown");
		if (listed->size != out.size || listed->modified != out.modified || !listed->id.same_as(out.id)) {
			return refuse(refusal_kind::other, "the file changed since the person was shown it");
		}
	} else if (m_seen.insert(key_of(*inside)).second) {
		if (m_files.size() >= m_limits.max_files) {
			m_seen.erase(key_of(*inside));
			return refuse(refusal_kind::limit, std::format("the model refers to more than {} files ({})", m_limits.max_files, k_step_side_files));
		}
		if (m_listed_bytes + out.size > m_limits.max_total_bytes) {
			m_seen.erase(key_of(*inside));
			return refuse(refusal_kind::limit, std::format("the side files exceed {} in total ({})", limit_text(m_limits.max_total_bytes), k_step_side_files));
		}
		m_listed_bytes += out.size;
		referenced_file listed{ *inside, out.size, out.modified, out.id };
		listed.names.emplace_back(reference);
		listed.kind = kind;
		m_files.push_back(std::move(listed));
	} else {
		const auto listed = std::find_if(m_files.begin(), m_files.end(), [&](const referenced_file& file) { return file.path == *inside; });
		if (listed != m_files.end() && std::find(listed->names.begin(), listed->names.end(), reference) == listed->names.end()) {
			if (listed->names.size() >= k_max_names_per_file) {
				return refuse(refusal_kind::limit, std::format("the model names '{}' in more than {} ways ({})", shown(fs_paths::generic_utf8_of(relative)), k_max_names_per_file, k_step_side_files));
			}
			listed->names.emplace_back(reference);
		}
	}
	out.path = *inside;
	return out;
}

std::optional<std::vector<std::byte>> edt::model_file_access::read(const resolved_reference& file)
{
	if (!file.found()) return std::nullopt;
	if (m_provided) {
		const provided_file* entry = provided_entry(file);
		if (!entry) return std::nullopt;
		m_bytes_read += entry->bytes->size();
		if (m_bytes_read > m_limits.max_total_bytes) {
			throw std::runtime_error(std::format("the side files the import reads exceed {} in total ({})", limit_text(m_limits.max_total_bytes), k_step_side_files));
		}
		if (entry->kind == model_file_kind::texture && !image_bytes_match(lowered_extension_of_name(entry->relative), *entry->bytes)) {
			throw std::runtime_error(std::format("'{}' is no image", shown(entry->relative)));
		}
		return *entry->bytes;
	}
	m_bytes_read += file.size;
	if (m_bytes_read > m_limits.max_total_bytes) {
		throw std::runtime_error(std::format("the side files the import reads from the disk exceed {} in total ({})", limit_text(m_limits.max_total_bytes), k_step_side_files));
	}
	std::optional<std::vector<std::byte>> bytes(std::in_place);
	if (m_read_file) {
		*bytes = m_read_file(file);
	} else {
		// Opened below the boundary without following a link: a folder or the file swapped for one since resolve() is
		// not read.
		fs::path below = file.path.lexically_relative(m_boundary);
		if (below.empty() || *below.begin() == "..") {
			std::error_code ec;
			const fs::path folder = fs::weakly_canonical(file.path.parent_path(), ec);
			below = (folder / file.path.filename()).lexically_relative(m_boundary);
			if (ec || below.empty() || *below.begin() == "..") return std::nullopt;
		}
		platform::beneath::read_request request;
		request.root = m_boundary;
		request.folder = below.parent_path();
		request.leaf = below.filename();
		request.max_bytes = file.size;
		const platform::beneath::read_result read = platform::beneath::read_beneath(request, *bytes);
		if (read.status != platform::beneath::read_status::ok) return std::nullopt;
	}
	if (bytes->size() != file.size) return std::nullopt;
	if (file.kind == model_file_kind::texture) {
		std::array<unsigned char, 18> head{};
		std::copy_n(reinterpret_cast<const unsigned char*>(bytes->data()), (std::min)(head.size(), bytes->size()), head.begin());
		if (bytes->size() < head.size() || !looks_like_image(lowered_extension(file.path), head)) {
			throw std::runtime_error(std::format("'{}' is no image", shown_leaf(file.path)));
		}
	}
	// The reader checked the handle it read from; the path is not opened again (file_id_of opens it on Windows).
	const auto changed_on_disk = [&] {
		std::error_code ec;
		if (fs::file_size(file.path, ec) != file.size || ec) return true;
		if (fs::last_write_time(file.path, ec) != file.modified || ec) return true;
		return !file_id_of(file.path).same_as(file.id);
	};
	if (!m_read_file && changed_on_disk()) {
		throw std::runtime_error(std::format("'{}' changed while it was read", shown_leaf(file.path)));
	}
	const content_digest digest = digest_of(*bytes);
	if (m_allowed) {
		const auto listed = std::find_if(m_allowed->begin(), m_allowed->end(), [&](const referenced_file& entry) { return entry.path == file.path; });
		if (listed == m_allowed->end() || !listed->hashed) {
			throw std::runtime_error(std::format("'{}' was listed without its bytes being hashed", shown_leaf(file.path)));
		}
		if (listed->digest != digest) {
			throw std::runtime_error(std::format("'{}' is not the file the person was shown: its content changed", shown_leaf(file.path)));
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

void edt::model_file_access::hash_listed(const resolved_reference& file)
{
	if (!file.found() || m_allowed || m_provided) return;
	const auto listed = std::find_if(m_files.begin(), m_files.end(), [&](const referenced_file& entry) { return entry.path == file.path; });
	if (listed == m_files.end() || listed->hashed) return;
	if (!read(file)) throw std::runtime_error(std::format("'{}' cannot be read", shown_leaf(file.path)));
}

std::string edt::model_file_access::relative_name(const resolved_reference& file) const
{
	if (m_provided) {
		const provided_file* entry = provided_entry(file);
		return entry ? entry->relative : std::string{};
	}
	// With a boundary every name is taken from it, so a file in the model's folder and one elsewhere never share a name.
	return file.path.lexically_relative(m_boundary).generic_string();
}
