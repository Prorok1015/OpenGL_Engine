#include <snk/resource/res_asset_meta.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <limits>
#include <sstream>
#include <boost/json/parse.hpp>
#include <snk/resource/res_pretty_json.h>

namespace
{
	constexpr std::string_view type_name = "resource_meta";
	constexpr std::array<std::string_view, 6> known_keys{ "__type", "__version", "__guid", "importer", "settings", "flags" };

	res::asset_meta_result fail(std::string field_path, std::string message,
		res::asset_meta_error_kind kind = res::asset_meta_error_kind::malformed)
	{
		return res::asset_meta_result{ std::nullopt, res::asset_meta_error{ kind, std::move(field_path), std::move(message) } };
	}

	std::optional<std::int64_t> integer_of(const json::value& value)
	{
		if (value.is_int64()) return value.get_int64();
		if (value.is_uint64() && value.get_uint64() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
			return static_cast<std::int64_t>(value.get_uint64());
		}
		return std::nullopt;
	}
}

res::asset_meta_result res::read_asset_meta(std::string_view text)
{
	boost::system::error_code ec;
	const json::value document = json::parse(text, ec);
	if (ec) {
		return fail("", std::format("not JSON: {}", ec.message()));
	}
	const json::object* root = document.if_object();
	if (!root) {
		return fail("", "the document is not an object");
	}

	const json::value* type = root->if_contains("__type");
	if (!type) return fail("__type", "missing");
	if (!type->is_string() || type->get_string() != type_name) {
		return fail("__type", std::format("expected \"{}\"", type_name));
	}

	const json::value* version_value = root->if_contains("__version");
	if (!version_value) return fail("__version", "missing");
	const auto version = integer_of(*version_value);
	if (!version || *version < 1) {
		return fail("__version", "expected a positive integer");
	}
	if (*version > asset_meta_version) {
		return fail("__version", std::format("version {} is newer than this engine reads ({})", *version, asset_meta_version),
			asset_meta_error_kind::newer_than_engine);
	}

	for (const auto& entry : *root) {
		if (std::ranges::find(known_keys, std::string_view{ entry.key() }) == known_keys.end()) {
			return fail(std::string{ entry.key() }, "unknown key");
		}
	}

	asset_meta meta;

	const json::value* guid = root->if_contains("__guid");
	if (!guid) return fail("__guid", "missing");
	const auto id = guid->is_string() ? asset_id_from_text(guid->get_string()) : std::nullopt;
	if (!id) return fail("__guid", "expected a UUID (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx)");
	if (id->is_nil()) return fail("__guid", "the nil UUID is not an asset id");
	meta.guid = *id;

	const json::value* importer = root->if_contains("importer");
	if (!importer) return fail("importer", "missing");
	if (!importer->is_string()) return fail("importer", "expected a string");
	meta.importer = importer->get_string();

	const json::value* settings = root->if_contains("settings");
	if (!settings) return fail("settings", "missing");
	if (!settings->is_object()) return fail("settings", "expected an object");
	meta.settings = settings->get_object();

	const json::value* flags = root->if_contains("flags");
	if (!flags) return fail("flags", "missing");
	if (!flags->is_array()) return fail("flags", "expected an array");
	const json::array& flag_array = flags->get_array();
	meta.flags.reserve(flag_array.size());
	for (std::size_t i = 0; i < flag_array.size(); ++i) {
		if (!flag_array[i].is_string()) {
			return fail(std::format("flags[{}]", i), "expected a string");
		}
		meta.flags.emplace_back(flag_array[i].get_string());
	}

	return asset_meta_result{ std::move(meta), {} };
}

std::string res::write_asset_meta(const asset_meta& meta)
{
	json::object document;
	document["__type"] = type_name;
	document["__version"] = asset_meta_version;
	document["__guid"] = to_text(meta.guid);
	document["importer"] = meta.importer;
	document["settings"] = meta.settings;
	json::array flags;
	flags.reserve(meta.flags.size());
	for (const auto& flag : meta.flags) {
		flags.emplace_back(flag);
	}
	document["flags"] = std::move(flags);

	std::ostringstream os;
	write_pretty_json(os, document);
	return std::move(os).str();
}
