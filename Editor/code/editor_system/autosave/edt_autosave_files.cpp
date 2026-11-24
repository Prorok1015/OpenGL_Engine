#include "edt_autosave_files.h"
#include <snk/common/ds/ds_type_id.hpp>
#include <snk/common/fs_paths.h>
#include <snk/platform/plt_beneath.h>
#include <snk/project/prj_json_text.h>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <exception>
#include <format>
#include <random>
#include <system_error>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cstring>
#include <unistd.h>
#endif

namespace json = boost::json;
namespace fs = std::filesystem;

namespace
{
	// File names stay well below the 255 bytes most file systems allow, with room for the meta suffix and a temp name.
	constexpr std::size_t k_max_name = 160;

	std::string one_file_name(std::string_view path)
	{
		std::string out;
		out.reserve(path.size());
		for (const char c : path) {
			const auto byte = static_cast<unsigned char>(c);
			const bool plain = byte >= 0x20 && byte != 0x7f && std::string_view{ "%/\\:<>|?*\"" }.find(c) == std::string_view::npos;
			if (plain) {
				out.push_back(c);
			} else {
				out += std::format("%{:02X}", static_cast<unsigned>(byte));
			}
		}
		if (out.size() > k_max_name) {
			// The tail names the file best; the hash of the whole path keeps two long names apart.
			std::size_t start = out.size() - (k_max_name - 17);
			const auto inside = [&out](std::size_t at) {
				const bool continuation = (static_cast<unsigned char>(out[at]) & 0xC0) == 0x80;
				const bool escape_digit = out[at - 1] == '%' || (at >= 2 && out[at - 2] == '%');
				return continuation || escape_digit;
			};
			while (start < out.size() && inside(start)) ++start;
			out = std::format("{}~{:016x}", out.substr(start), ds::fnv1a_64(path));
		}
		// A leading dot would hide the file, and "." or ".." is no file name at all.
		if (!out.empty() && out.front() == '.') {
			out = "%2E" + out.substr(1);
		}
		return out;
	}

	std::string temp_leaf_of(const fs::path& leaf)
	{
		thread_local std::mt19937_64 random{ std::random_device{}() };
		return std::format(".{}.{:016x}.tmp", fs_paths::utf8_of(leaf.filename()), random());
	}

	std::string write_one(platform::beneath::pinned_root& root, const fs::path& relative, std::span<const std::byte> data)
	{
		platform::beneath::write_request request;
		request.folder = relative.parent_path();
		request.leaf = relative.filename();
		request.tmp_leaf = fs_paths::path_from_utf8(temp_leaf_of(request.leaf));
		request.data = data;
		request.durable = true;
		request.replace = true;
		request.tmp_path = root.path() / request.folder / request.tmp_leaf;
		return platform::beneath::write_beneath(root, request);
	}

	std::string remove_one(platform::beneath::pinned_root& root, const fs::path& relative)
	{
		std::error_code ec;
		if (fs::symlink_status(root.path() / relative, ec).type() == fs::file_type::not_found) {
			return {};
		}
		return platform::beneath::remove_beneath(root, relative.parent_path(), relative.filename(), true);
	}
}

std::string edt::autosave_source_text(const res::tag& source)
{
	if (!source.is_valid()) return {};
	return source.protocol() == res::tag::default_protocol() ? std::string(source.relative()) : std::string(source.view());
}

bool edt::is_autosave_kind(std::string_view kind)
{
	return std::ranges::find(k_autosave_kinds, kind) != k_autosave_kinds.end();
}

bool edt::is_autosave_session_name(std::string_view name)
{
	return name.size() == 32 && std::ranges::all_of(name, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string edt::autosave_session_folder(std::string_view session)
{
	return std::format("{}/{}", k_autosave_folder, one_file_name(session));
}

std::string edt::autosave_file_of(std::string_view session, std::string_view kind, const res::tag& source)
{
	const std::string text = autosave_source_text(source);
	return std::format("{}/{}/{}", autosave_session_folder(session), one_file_name(kind),
		text.empty() ? std::string{ "untitled" } : one_file_name(text));
}

std::string_view edt::autosave_leaf_of(std::string_view file)
{
	const std::size_t slash = file.rfind('/');
	return slash == std::string_view::npos ? file : file.substr(slash + 1);
}

std::optional<res::tag> edt::autosave_source_tag(std::string_view text)
{
	if (text.empty() || text.find("://") != std::string_view::npos || text.front() == '/') return std::nullopt;
	for (const char c : text) {
		const auto byte = static_cast<unsigned char>(c);
		if (byte < 0x20 || byte == 0x7f || c == '\\' || c == ':') return std::nullopt;
	}
	std::size_t start = 0;
	while (start <= text.size()) {
		const std::size_t end = (std::min)(text.find('/', start), text.size());
		const std::string_view part = text.substr(start, end - start);
		if (part.empty() || part == "." || part == "..") return std::nullopt;
		start = end + 1;
	}
	res::tag tag = res::tag::make(text);
	if (!tag.is_valid() || autosave_source_text(tag) != text) return std::nullopt;
	return tag;
}

std::string edt::autosave_meta_text(const autosave_meta& meta)
{
	json::object root;
	root["format"] = meta.format;
	root["source"] = meta.source;
	root["kind"] = meta.kind;
	root["generation"] = meta.generation;
	root["saved_at"] = meta.saved_at;
	root["session"] = meta.session;
	root["host"] = meta.host;
	root["payload_size"] = meta.payload_size;
	root["payload_sha256"] = meta.payload_sha256;
	if (meta.source_size) {
		root["source_size"] = *meta.source_size;
		root["source_sha256"] = meta.source_sha256;
	}
	return prj::to_indented_json(root);
}

std::optional<edt::autosave_meta> edt::parse_autosave_meta(std::string_view text)
{
	boost::system::error_code ec;
	const json::value root = json::parse(text, ec);
	if (ec || !root.is_object()) return std::nullopt;
	const json::object& object = root.as_object();
	const auto string_of = [&object](std::string_view key) -> const json::string* {
		const json::value* found = object.if_contains(key);
		return found && found->is_string() ? &found->as_string() : nullptr;
	};
	const auto number_of = [&object](std::string_view key) -> std::optional<std::uint64_t> {
		const json::value* found = object.if_contains(key);
		if (!found) return std::nullopt;
		if (found->is_uint64()) return found->as_uint64();
		if (found->is_int64() && found->as_int64() >= 0) return static_cast<std::uint64_t>(found->as_int64());
		return std::nullopt;
	};
	const std::optional<std::uint64_t> format = number_of("format");
	const json::string* source = string_of("source");
	const json::string* kind = string_of("kind");
	const std::optional<std::uint64_t> generation = number_of("generation");
	const json::string* saved_at = string_of("saved_at");
	const json::string* session = string_of("session");
	const json::string* host = string_of("host");
	const std::optional<std::uint64_t> payload_size = number_of("payload_size");
	const json::string* payload_sha256 = string_of("payload_sha256");
	if (!format || *format != static_cast<std::uint64_t>(k_autosave_format) || !source || !kind || !generation || !saved_at || !session || !host
		|| !payload_size || !payload_sha256) {
		return std::nullopt;
	}
	autosave_meta meta;
	meta.source = std::string(*source);
	meta.kind = std::string(*kind);
	meta.generation = *generation;
	meta.saved_at = std::string(*saved_at);
	meta.session = std::string(*session);
	meta.host = std::string(*host);
	meta.payload_size = *payload_size;
	meta.payload_sha256 = std::string(*payload_sha256);
	meta.source_size = number_of("source_size");
	if (const json::string* sha = string_of("source_sha256")) meta.source_sha256 = std::string(*sha);
	return meta;
}

const std::string& edt::autosave_session_id()
{
	static const std::string session = autosave_new_session_id();
	return session;
}

std::string edt::autosave_new_session_id()
{
	std::random_device random;
	std::string out;
	for (int part = 0; part < 4; ++part) {
		out += std::format("{:08x}", static_cast<std::uint32_t>(random()));
	}
	return out;
}

std::string edt::this_host_name()
{
	std::string name;
#if defined(_WIN32)
	wchar_t buffer[256]{};
	DWORD size = static_cast<DWORD>(std::size(buffer));
	bool named = ::GetComputerNameExW(ComputerNameDnsHostname, buffer, &size) != FALSE;
	if (!named) {
		size = static_cast<DWORD>(std::size(buffer));
		named = ::GetComputerNameW(buffer, &size) != FALSE;
	}
	if (named && size < std::size(buffer)) {
		name = fs_paths::utf8_of(fs::path(std::wstring(buffer, size)));
	}
#else
	char buffer[256]{};
	if (::gethostname(buffer, sizeof(buffer) - 1) == 0) {
		name = std::string(buffer, ::strnlen(buffer, sizeof(buffer) - 1));
	}
#endif
	if (name.size() > 255) {
		std::size_t end = 255;
		while (end > 0 && (static_cast<unsigned char>(name[end]) & 0xC0) == 0x80) --end;
		name.resize(end);
	}
	return name;
}

std::string edt::autosave_session_text(const autosave_session_info& info)
{
	json::object root;
	root["format"] = info.format;
	root["session"] = info.session;
	root["host"] = info.host;
	root["pid"] = info.pid;
	root["started_at"] = info.started_at;
	root["locked"] = info.locked;
	return prj::to_indented_json(root);
}

std::optional<edt::autosave_session_info> edt::parse_autosave_session(std::string_view text)
{
	boost::system::error_code ec;
	json::parse_options options;
	options.max_depth = 8;
	const json::value root = json::parse(text, ec, {}, options);
	if (ec || !root.is_object()) return std::nullopt;
	const json::object& object = root.as_object();
	const json::value* format = object.if_contains("format");
	const json::value* session = object.if_contains("session");
	const json::value* host = object.if_contains("host");
	const json::value* pid = object.if_contains("pid");
	const json::value* started_at = object.if_contains("started_at");
	const json::value* locked = object.if_contains("locked");
	const bool format_ok = format && ((format->is_int64() && format->as_int64() == k_autosave_format)
		|| (format->is_uint64() && format->as_uint64() == static_cast<std::uint64_t>(k_autosave_format)));
	if (!format_ok || !session || !session->is_string() || !host || !host->is_string() || !locked || !locked->is_bool()) {
		return std::nullopt;
	}
	autosave_session_info info;
	info.session = std::string(session->as_string());
	info.host = std::string(host->as_string());
	if (pid && pid->is_int64()) info.pid = pid->as_int64();
	else if (pid && pid->is_uint64() && pid->as_uint64() <= static_cast<std::uint64_t>(INT64_MAX)) info.pid = static_cast<std::int64_t>(pid->as_uint64());
	if (started_at && started_at->is_string()) info.started_at = std::string(started_at->as_string());
	info.locked = locked->as_bool();
	return info;
}

std::string edt::autosave_time_now()
{
	return std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
}

std::string edt::write_autosave_files(const fs::path& cache_root, std::string_view file, std::span<const std::byte> data,
	std::string_view meta)
{
	const fs::path relative = fs_paths::path_from_utf8(file);
	platform::beneath::pinned_root root{ cache_root };
	if (std::string error = platform::beneath::make_folders_beneath(root, relative.parent_path()); !error.empty()) {
		return error;
	}
	if (std::string error = write_one(root, relative, data); !error.empty()) {
		return error;
	}
	const auto* first = reinterpret_cast<const std::byte*>(meta.data());
	return write_one(root, fs_paths::path_from_utf8(autosave_meta_of(file)), std::span<const std::byte>(first, meta.size()));
}

std::string edt::write_autosave_file(const fs::path& cache_root, std::string_view file, std::string_view text)
{
	try {
		const fs::path relative = fs_paths::path_from_utf8(file);
		platform::beneath::pinned_root root{ cache_root };
		if (std::string error = platform::beneath::make_folders_beneath(root, relative.parent_path()); !error.empty()) {
			return error;
		}
		const auto* first = reinterpret_cast<const std::byte*>(text.data());
		return write_one(root, relative, std::span<const std::byte>(first, text.size()));
	} catch (const std::exception& error) {
		return error.what();
	} catch (...) {
		return "an unknown error";
	}
}

std::string edt::remove_autosave_file(const fs::path& cache_root, std::string_view file)
{
	try {
		platform::beneath::pinned_root root{ cache_root };
		return remove_one(root, fs_paths::path_from_utf8(file));
	} catch (const std::exception& error) {
		return error.what();
	} catch (...) {
		return "an unknown error";
	}
}

std::string edt::remove_autosave_tree(const fs::path& cache_root, std::string_view name)
{
	try {
		platform::beneath::pinned_root root{ cache_root };
		return platform::beneath::remove_tree_beneath(root, fs_paths::path_from_utf8(k_autosave_folder), fs_paths::path_from_utf8(name));
	} catch (const std::exception& error) {
		return error.what();
	} catch (...) {
		return "an unknown error";
	}
}

std::string edt::remove_autosave_files(const fs::path& cache_root, std::string_view file)
{
	try {
		platform::beneath::pinned_root root{ cache_root };
		if (std::string error = remove_one(root, fs_paths::path_from_utf8(autosave_meta_of(file))); !error.empty()) {
			return error;
		}
		return remove_one(root, fs_paths::path_from_utf8(file));
	} catch (const std::exception& error) {
		return error.what();
	} catch (...) {
		return "an unknown error";
	}
}
