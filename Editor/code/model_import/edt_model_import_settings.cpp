#include "edt_model_import_settings.h"
#include <algorithm>
#include <array>
#include <format>

namespace json = boost::json;

namespace {

constexpr std::array<std::string_view, 2> k_normals_names = { "smooth", "flat" };

// A key or value from outside, cut and without control characters, for an error text.
std::string shown(std::string_view text)
{
	std::string out;
	for (const char c : text.substr(0, 64)) {
		out.push_back(static_cast<unsigned char>(c) < 0x20 || c == 0x7F ? '?' : c);
	}
	if (text.size() > 64) out += "...";
	return out;
}

bool is_lower_hex(std::string_view text)
{
	return std::ranges::all_of(text, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// Why `version` is not one this editor writes; empty when it is 1.
std::string version_problem(const json::value& version)
{
	const bool one = (version.is_int64() && version.as_int64() == edt::k_model_settings_version)
		|| (version.is_uint64() && version.as_uint64() == static_cast<std::uint64_t>(edt::k_model_settings_version));
	if (one) return {};
	const bool newer = (version.is_int64() && version.as_int64() > edt::k_model_settings_version)
		|| version.is_uint64();
	if (newer) {
		return std::format("settings.version is newer than this editor reads ({}): written by a newer editor",
			edt::k_model_settings_version);
	}
	return std::format("settings.version must be {}", edt::k_model_settings_version);
}

std::string source_problem(const json::value& value, edt::model_source_record& out)
{
	if (!value.is_object()) return "settings.source must be an object {name, sha256}";
	bool named = false;
	bool hashed = false;
	for (const json::key_value_pair& entry : value.as_object()) {
		const std::string_view key = entry.key();
		if (key == "name") {
			if (!entry.value().is_string()) return "settings.source.name must be a file name";
			const std::string_view name = entry.value().as_string();
			const bool plain = !name.empty() && name.size() <= edt::k_max_model_source_name_bytes
				&& name.find_first_of("/\\:") == std::string_view::npos && name != "." && name != ".."
				&& std::ranges::none_of(name, [](char c) { return static_cast<unsigned char>(c) < 0x20; });
			if (!plain) return "settings.source.name must be a file name, not a path";
			out.name = std::string(name);
			named = true;
		} else if (key == "sha256") {
			const json::value& hash = entry.value();
			if (!hash.is_string() || hash.as_string().size() != 64 || !is_lower_hex(hash.as_string())) {
				return "settings.source.sha256 must be 64 lowercase hex digits";
			}
			out.sha256 = std::string(hash.as_string());
			hashed = true;
		} else {
			return std::format("settings.source.{} is not a key of the source (name, sha256)", shown(key));
		}
	}
	if (!named || !hashed) return "settings.source needs both name and sha256";
	return {};
}

}

std::string_view edt::model_normals_name(model_normals normals)
{
	return k_normals_names[static_cast<std::size_t>(normals)];
}

edt::model_settings_parse edt::parse_model_settings(const json::value& value)
{
	model_settings_parse out;
	if (value.is_null()) return out;
	if (!value.is_object()) {
		out.error = "settings must be an object";
		return out;
	}
	for (const json::key_value_pair& entry : value.as_object()) {
		const std::string_view key = entry.key();
		const json::value& item = entry.value();
		if (key == "version") {
			out.error = version_problem(item);
		} else if (key == "source") {
			model_source_record source;
			out.error = source_problem(item, source);
			if (out.error.empty()) out.source = std::move(source);
		} else if (key == "normals") {
			const auto found = item.is_string() ? std::ranges::find(k_normals_names, std::string_view(item.as_string()))
				: k_normals_names.end();
			if (found == k_normals_names.end()) {
				out.error = "settings.normals must be one of smooth, flat";
			} else {
				out.settings.normals = static_cast<model_normals>(found - k_normals_names.begin());
			}
		} else if (key == "tangents") {
			if (item.is_bool()) {
				out.settings.tangents = item.as_bool();
			} else {
				out.error = "settings.tangents must be true or false";
			}
		} else {
			out.error = std::format("settings.{} is not a setting of a model (version, source, normals, tangents)",
				shown(key));
		}
		if (!out.error.empty()) return out;
	}
	return out;
}

json::object edt::model_settings_json(const model_import_settings& settings,
	const std::optional<model_source_record>& source)
{
	json::object out{ { "version", k_model_settings_version } };
	if (source) out["source"] = json::object{ { "name", source->name }, { "sha256", source->sha256 } };
	out["normals"] = model_normals_name(settings.normals);
	out["tangents"] = settings.tangents;
	return out;
}

std::string edt::rewrite_model_settings(const json::value& existing, const model_import_settings& settings,
	const model_source_record& source, json::object& out)
{
	if (existing.is_null()) {
		out = model_settings_json(settings, source);
		return {};
	}
	if (!existing.is_object()) return "its settings are no object";
	json::object kept = existing.as_object();
	if (const json::value* version = kept.if_contains("version")) {
		if (std::string why = version_problem(*version); !why.empty()) return why;
	}
	const json::object written = model_settings_json(settings, source);
	for (const json::key_value_pair& entry : written) kept[entry.key()] = entry.value();
	out = std::move(kept);
	return {};
}

std::string edt::model_settings_text(const model_import_settings& settings)
{
	return std::format("normals {}, tangents {}", model_normals_name(settings.normals),
		settings.tangents ? "on" : "off");
}
