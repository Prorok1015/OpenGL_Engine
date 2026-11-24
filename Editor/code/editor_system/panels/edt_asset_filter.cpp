#include "edt_asset_filter.h"
#include "edt_asset_database.h"
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

	constexpr std::array<suffix_kind, 6> k_suffix_kinds{ {
		{ ".level.desc", edt::k_asset_kind_level },
		{ ".world.desc", edt::k_asset_kind_world },
		{ ".geom.desc", edt::k_asset_kind_mesh },
		{ ".poly.desc", edt::k_asset_kind_mesh },
		{ ".prefab.desc", edt::k_asset_kind_prefab },
		{ ".shader.desc", edt::k_asset_kind_shader_desc },
	} };

	std::string_view kind_of_desc_type(std::string_view type)
	{
		if (type == "level_desc") return edt::k_asset_kind_level;
		if (type == "world_desc") return edt::k_asset_kind_world;
		if (type == "prefab_desc") return edt::k_asset_kind_prefab;
		return {};
	}

	std::string_view label_of(std::string_view id)
	{
		if (id == edt::k_asset_kind_level) return "Level";
		if (id == edt::k_asset_kind_world) return "World";
		if (id == edt::k_asset_kind_mesh) return "Mesh";
		if (id == edt::k_asset_kind_prefab) return "Prefab";
		if (id == edt::k_asset_kind_shader_desc) return "Shader desc";
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
				return registered->id == "poly" ? std::string{ k_asset_kind_mesh } : registered->id;
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
	constexpr std::string_view key = "\"__type\"";
	const std::size_t at = head.find(key);
	if (at == std::string_view::npos) {
		return {};
	}
	std::size_t i = at + key.size();
	while (i < head.size() && (head[i] == ' ' || head[i] == '\t' || head[i] == '\r' || head[i] == '\n' || head[i] == ':')) {
		++i;
	}
	if (i >= head.size() || head[i] != '"') {
		return {};
	}
	const std::size_t end = head.find('"', i + 1);
	return end == std::string_view::npos ? std::string{} : std::string{ head.substr(i + 1, end - i - 1) };
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

	std::string read_head_beneath(const std::filesystem::path& root, const std::filesystem::path& relative)
	{
		std::string head;
		const auto result = platform::beneath::read_beneath_stream(request_of(root, relative),
			[&head](std::span<const std::byte> chunk) {
				const std::size_t room = edt::k_desc_sniff_bytes - head.size();
				head.append(reinterpret_cast<const char*>(chunk.data()), (std::min)(room, chunk.size()));
				return head.size() < edt::k_desc_sniff_bytes;
			});
		return result.status == platform::beneath::read_status::ok ? head : std::string{};
	}

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
	std::string type = desc_type_in(m_ports.read_head(root, relative));
	m_entries[key] = entry{ *now, type };
	return type;
}

std::vector<edt::asset_filter_kind> edt::asset_filter_kinds(const asset_kind_registry* kinds)
{
	std::vector<asset_filter_kind> out;
	for (const std::string_view id : { k_asset_kind_prefab, k_asset_kind_level, k_asset_kind_world, k_asset_kind_mesh,
			k_asset_kind_data, k_asset_kind_shader_desc, std::string_view{ "texture" }, std::string_view{ "model" },
			std::string_view{ "shader" }, std::string_view{ "file" } }) {
		out.push_back({ std::string{ id }, std::string{ label_of(id) } });
	}
	if (kinds) {
		for (const std::string& id : kinds->ids()) {
			const bool known = id == "poly" || id == "desc" || std::ranges::any_of(out, [&](const asset_filter_kind& kind) { return kind.id == id; });
			if (!known) {
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

bool edt::asset_filter::accepts(bool is_dir, std::string_view name, std::string_view kind) const
{
	if (is_dir) {
		return true;
	}
	if (!m_name.empty() && lowered(name).find(m_name) == std::string::npos) {
		return false;
	}
	return m_kinds.empty() || m_kinds.contains(kind);
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
