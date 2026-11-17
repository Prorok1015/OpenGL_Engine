#include "edt_reference_rewrite.h"
#include <snk/resource/res_asset_walk.h>
#include <snk/resource/res_pretty_json.h>
#include <boost/json.hpp>
#include <cstddef>
#include <format>
#include <optional>
#include <utility>
#include <vector>

namespace json = boost::json;

namespace
{
	// Deeper than any file the editor writes (the references are collected down to 128).
	constexpr std::size_t k_max_depth = 256;

	std::string_view view_of(const json::string& text)
	{
		return std::string_view(text.data(), text.size());
	}

	std::string text_of(const json::value& document)
	{
		const std::vector<std::byte> bytes = res::pretty_json_bytes(document);
		return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
	}

	std::optional<json::value> parse(std::string_view text, std::string& error)
	{
		json::parse_options options;
		options.max_depth = k_max_depth;
		boost::system::error_code ec;
		json::value parsed = json::parse(text, ec, json::storage_ptr{}, options);
		if (ec) {
			error = ec.message();
			return std::nullopt;
		}
		return parsed;
	}
}

std::string_view edt::rewrite_status_name(rewrite_status status)
{
	switch (status) {
	case rewrite_status::rewritten: return "rewritten";
	case rewrite_status::nothing_to_do: return "nothing_to_do";
	case rewrite_status::unreadable: return "unreadable";
	case rewrite_status::not_canonical: return "not_canonical";
	case rewrite_status::pointer_missing: return "pointer_missing";
	case rewrite_status::not_a_reference: return "not_a_reference";
	}
	return "unreadable";
}

std::string edt::canonical_json_text(std::string_view text, std::string& error)
{
	const std::optional<json::value> parsed = parse(text, error);
	return parsed ? text_of(*parsed) : std::string{};
}

edt::rewrite_outcome edt::rewrite_references(std::string_view text, std::span<const std::string> pointers, std::string_view old_path,
	std::string_view new_path, bool case_insensitive)
{
	rewrite_outcome out;
	const auto fail = [&out](rewrite_status status, std::string reason) {
		out.status = status;
		out.reason = std::move(reason);
		out.pointers.clear();
		return std::move(out);
	};

	std::string error;
	std::optional<json::value> document = parse(text, error);
	if (!document) return fail(rewrite_status::unreadable, std::move(error));
	if (text_of(*document) != text) {
		return fail(rewrite_status::not_canonical, "the file is not laid out as the editor writes it, so saving it again would change more than the references");
	}

	const std::string old_key = res::asset_name_key(old_path, case_insensitive);
	const auto names_old = [&](const json::value& value) {
		const json::string* path = value.if_string();
		return path && res::asset_name_key(view_of(*path), case_insensitive) == old_key;
	};

	for (const std::string& pointer : pointers) {
		boost::system::error_code ec;
		json::value* slot = document->find_pointer(pointer, ec);
		if (ec || !slot) return fail(rewrite_status::pointer_missing, std::format("{} is not in the file any more", pointer));
		if (slot->is_string()) {
			if (!names_old(*slot)) return fail(rewrite_status::not_a_reference, std::format("{} names another file", pointer));
			if (view_of(slot->as_string()) == new_path) continue;
			*slot = json::value(json::string_view(new_path.data(), new_path.size()));
			out.pointers.push_back(pointer);
		} else if (json::object* object = slot->if_object()) {
			json::value* path = object->if_contains("path");
			if (!path || !path->is_string()) continue;
			if (!object->contains("guid") && !names_old(*path)) {
				return fail(rewrite_status::not_a_reference, std::format("{} names another file", pointer));
			}
			if (view_of(path->as_string()) == new_path) continue;
			*path = json::value(json::string_view(new_path.data(), new_path.size()));
			out.pointers.push_back(pointer);
		} else {
			return fail(rewrite_status::not_a_reference, std::format("{} is neither a path nor a reference", pointer));
		}
	}

	if (out.pointers.empty()) {
		out.status = rewrite_status::nothing_to_do;
		return out;
	}
	out.status = rewrite_status::rewritten;
	out.text = text_of(*document);
	return out;
}
