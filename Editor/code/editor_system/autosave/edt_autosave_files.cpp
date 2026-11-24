#include "edt_autosave_files.h"
#include <snk/common/ds/ds_type_id.hpp>
#include <snk/common/fs_paths.h>
#include <snk/platform/plt_beneath.h>
#include <snk/project/prj_json_text.h>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>
#include <chrono>
#include <cstdint>
#include <exception>
#include <format>
#include <random>
#include <system_error>
#include <vector>

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

std::string edt::autosave_file_of(std::string_view kind, const res::tag& source)
{
	const std::string text = autosave_source_text(source);
	return std::format("{}/{}/{}", k_autosave_folder, one_file_name(kind), text.empty() ? std::string{ "untitled" } : one_file_name(text));
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
	const std::optional<std::uint64_t> payload_size = number_of("payload_size");
	const json::string* payload_sha256 = string_of("payload_sha256");
	if (!format || *format != static_cast<std::uint64_t>(k_autosave_format) || !source || !kind || !generation || !saved_at || !session
		|| !payload_size || !payload_sha256) {
		return std::nullopt;
	}
	autosave_meta meta;
	meta.source = std::string(*source);
	meta.kind = std::string(*kind);
	meta.generation = *generation;
	meta.saved_at = std::string(*saved_at);
	meta.session = std::string(*session);
	meta.payload_size = *payload_size;
	meta.payload_sha256 = std::string(*payload_sha256);
	meta.source_size = number_of("source_size");
	if (const json::string* sha = string_of("source_sha256")) meta.source_sha256 = std::string(*sha);
	return meta;
}

const std::string& edt::autosave_session_id()
{
	static const std::string session = [] {
		std::random_device random;
		std::string out;
		for (int part = 0; part < 4; ++part) {
			out += std::format("{:08x}", static_cast<std::uint32_t>(random()));
		}
		return out;
	}();
	return session;
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
