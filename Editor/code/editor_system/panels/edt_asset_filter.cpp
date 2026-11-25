#include "edt_asset_filter.h"
#include "edt_asset_database.h"
#include "edt_asset_kind_names.h"
#include <snk/editor/edt_asset_kind_registry.h>
#include <snk/platform/plt_beneath.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <limits>
#include <span>

namespace {

	std::string lowered(std::string_view text)
	{
		std::string out{ text };
		std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return out;
	}

	struct suffix_kind
	{
		std::string_view suffix;
		std::string_view kind;
	};

	constexpr std::array<suffix_kind, 8> k_suffix_kinds{ {
		{ ".level.desc", edt::k_asset_kind_level },
		{ ".world.desc", edt::k_asset_kind_world },
		{ ".geom.desc", edt::k_asset_kind_mesh },
		{ ".poly.desc", edt::k_asset_kind_mesh },
		{ ".prefab.desc", edt::k_asset_kind_prefab },
		{ ".shader.desc", edt::k_asset_kind_shader_desc },
		{ ".mat.desc", edt::k_asset_kind_material },
		{ ".txm.desc", "texture" },
	} };

	std::string_view kind_of_desc_type(std::string_view type)
	{
		if (type == "level_desc") return edt::k_asset_kind_level;
		if (type == "world_desc") return edt::k_asset_kind_world;
		if (type == "prefab_desc") return edt::k_asset_kind_prefab;
		return {};
	}

	// The kind of the browser's filter for a kind of the registry: the modeller's poly is a mesh here, the rest as agents see it.
	std::string_view filter_kind_of_registered(std::string_view id)
	{
		if (id == "poly") return edt::k_asset_kind_mesh;
		return edt::outward_kind_name(id);
	}

	std::string_view label_of(std::string_view id)
	{
		if (id == edt::k_asset_kind_level) return "Level";
		if (id == edt::k_asset_kind_world) return "World";
		if (id == edt::k_asset_kind_mesh) return "Mesh";
		if (id == edt::k_asset_kind_prefab) return "Prefab";
		if (id == edt::k_asset_kind_shader_desc) return "Shader desc";
		if (id == edt::k_asset_kind_material) return "Material";
		if (id == edt::k_asset_kind_data) return "Data";
		if (id == "texture") return "Texture";
		if (id == "model") return "Model";
		if (id == "shader") return "Shader source";
		if (id == "file") return "Other file";
		return id;
	}

}

std::string edt::asset_kind_of(const asset_kind_registry* kinds, std::string_view path, std::string_view desc_type)
{
	const std::string lower = lowered(path);
	for (const auto& [suffix, kind] : k_suffix_kinds) {
		if (lower.ends_with(suffix)) {
			return std::string{ kind };
		}
	}
	if (kinds) {
		if (const asset_kind_desc* registered = kinds->kind_for_open(path)) {
			if (!registered->id.empty()) {
				return std::string{ filter_kind_of_registered(registered->id) };
			}
		}
	}
	const std::string type = asset_type_of(path);
	if (type == "desc") {
		const std::string_view by_type = kind_of_desc_type(desc_type);
		return std::string{ by_type.empty() ? k_asset_kind_data : by_type };
	}
	return type;
}

std::string edt::desc_type_in(std::string_view head)
{
	if (head.starts_with("\xEF\xBB\xBF")) {
		head.remove_prefix(3);
	}
	const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
	// The end of the string whose opening quote is at `open` (the index of its closing quote), npos when the text ends first.
	const auto string_end = [&head](std::size_t open) {
		for (std::size_t i = open + 1; i < head.size(); ++i) {
			if (head[i] == '\\') {
				++i;
			} else if (head[i] == '"') {
				return i;
			}
		}
		return std::string_view::npos;
	};
	int depth = 0;
	for (std::size_t i = 0; i < head.size(); ++i) {
		const char c = head[i];
		if (c == '{' || c == '[') {
			++depth;
		} else if (c == '}' || c == ']') {
			--depth;
		} else if (c == '"') {
			const std::size_t end = string_end(i);
			if (end == std::string_view::npos) {
				return {};
			}
			std::size_t after = end + 1;
			while (after < head.size() && blank(head[after])) {
				++after;
			}
			const bool is_key = after < head.size() && head[after] == ':';
			if (depth == 1 && is_key && head.substr(i + 1, end - i - 1) == "__type") {
				++after;
				while (after < head.size() && blank(head[after])) {
					++after;
				}
				if (after >= head.size() || head[after] != '"') {
					return {};
				}
				const std::size_t value_end = string_end(after);
				return value_end == std::string_view::npos ? std::string{} : std::string{ head.substr(after + 1, value_end - after - 1) };
			}
			i = end;
		}
	}
	return {};
}

bool edt::asset_kind_needs_desc_type(const asset_kind_registry* kinds, std::string_view path)
{
	return lowered(path).ends_with(".desc") && asset_kind_of(kinds, path) == k_asset_kind_data;
}

namespace {

	platform::beneath::read_request request_of(const std::filesystem::path& root, const std::filesystem::path& relative)
	{
		platform::beneath::read_request request;
		request.root = root;
		request.folder = relative.parent_path();
		request.leaf = relative.filename();
		request.max_bytes = (std::numeric_limits<std::uint64_t>::max)();
		return request;
	}

	std::optional<edt::desc_type_cache::stamp> stat_beneath(const std::filesystem::path& root, const std::filesystem::path& relative)
	{
		platform::beneath::read_request request = request_of(root, relative);
		request.stamp_only = true;
		std::vector<std::byte> unused;
		const auto result = platform::beneath::read_beneath(request, unused);
		if (result.status != platform::beneath::read_status::ok || !result.stamp) {
			return std::nullopt;
		}
		return edt::desc_type_cache::stamp{ result.stamp->size, result.stamp->write_time_ns, result.stamp->file_id };
	}

	std::optional<std::string> read_head_beneath(const std::filesystem::path& root, const std::filesystem::path& relative)
	{
		std::string head;
		const auto result = platform::beneath::read_beneath_stream(request_of(root, relative),
			[&head](std::span<const std::byte> chunk) {
				const std::size_t room = edt::k_desc_sniff_bytes - head.size();
				head.append(reinterpret_cast<const char*>(chunk.data()), (std::min)(room, chunk.size()));
				return head.size() < edt::k_desc_sniff_bytes;
			});
		if (result.status != platform::beneath::read_status::ok) {
			return std::nullopt;
		}
		return head;
	}

}

std::optional<edt::desc_type_cache::stamp> edt::stat_asset_file(const std::filesystem::path& root, const std::filesystem::path& relative)
{
	return stat_beneath(root, relative);
}

edt::desc_type_cache::desc_type_cache()
	: m_ports{ &stat_beneath, &read_head_beneath }
{
}

std::optional<std::string> edt::desc_type_cache::type_of(const std::filesystem::path& root, const std::filesystem::path& relative, int& budget)
{
	const std::optional<stamp> now = m_ports.stat(root, relative);
	if (!now) {
		return std::string{};
	}
	const std::string key = (root / relative).generic_string();
	if (const auto found = m_entries.find(key); found != m_entries.end() && found->second.seen == *now) {
		return found->second.type;
	}
	if (budget <= 0) {
		return std::nullopt;
	}
	--budget;
	++m_reads;
	const std::optional<std::string> head = m_ports.read_head(root, relative);
	if (!head) {
		return std::string{};
	}
	std::string type = desc_type_in(*head);
	m_entries[key] = entry{ *now, type };
	return type;
}

std::vector<edt::asset_filter_kind> edt::asset_filter_kinds(const asset_kind_registry* kinds)
{
	std::vector<asset_filter_kind> out;
	for (const std::string_view id : { k_asset_kind_prefab, k_asset_kind_level, k_asset_kind_world, k_asset_kind_mesh,
			k_asset_kind_data, k_asset_kind_shader_desc, k_asset_kind_material, std::string_view{ "texture" }, std::string_view{ "model" },
			std::string_view{ "shader" }, std::string_view{ "file" } }) {
		out.push_back({ std::string{ id }, std::string{ label_of(id) } });
	}
	if (kinds) {
		for (const std::string& id : kinds->ids()) {
			const asset_kind_desc* registered = kinds->find(id);
			const bool opens = registered && registered->open;
			const bool known = filter_kind_of_registered(id) != id || id == "desc"
				|| std::ranges::any_of(out, [&](const asset_filter_kind& kind) { return kind.id == id; });
			if (opens && !known) {
				out.push_back({ id, id });
			}
		}
	}
	return out;
}

void edt::asset_filter::set_name(std::string_view text)
{
	m_name = lowered(text);
}

void edt::asset_filter::set_kind(std::string_view id, bool on)
{
	if (on) {
		m_kinds.emplace(id);
	} else if (const auto found = m_kinds.find(id); found != m_kinds.end()) {
		m_kinds.erase(found);
	}
}

bool edt::asset_filter::accepts_name(std::string_view name) const
{
	return m_name.empty() || lowered(name).find(m_name) != std::string::npos;
}

bool edt::asset_filter::accepts(bool is_dir, std::string_view name, std::string_view kind) const
{
	if (is_dir) {
		return true;
	}
	return accepts_name(name) && (m_kinds.empty() || m_kinds.contains(kind));
}

void edt::asset_filter::keep_only(const std::vector<asset_filter_kind>& offered)
{
	std::erase_if(m_kinds, [&offered](const std::string& id) {
		return std::ranges::none_of(offered, [&id](const asset_filter_kind& kind) { return kind.id == id; });
	});
}

std::string edt::asset_filter::summary() const
{
	if (m_kinds.empty()) {
		return "All";
	}
	if (m_kinds.size() == 1) {
		return std::string{ label_of(*m_kinds.begin()) };
	}
	return std::format("{} kinds", m_kinds.size());
}
