#include "edt_agent_model_guard.h"
#include <snk/resource/res_system.h>
#include <snk/scene/adapters/scn_model_importer_adapter.h>
#include <boost/json.hpp>
#include <algorithm>
#include <cctype>
#include <format>
#include <string>
#include <vector>

namespace json = boost::json;

namespace {
	constexpr std::string_view k_import_command = "file.import_model";
	constexpr std::size_t k_shown_bytes = 120;

	std::string pointer_token(std::string_view key)
	{
		std::string out;
		for (const char c : key) {
			if (c == '~') out += "~0";
			else if (c == '/') out += "~1";
			else out += c;
		}
		return out;
	}

	std::optional<edt::named_model> find_in(const json::value& value, const std::string& pointer, std::span<const std::string> extensions)
	{
		if (value.is_string()) {
			const std::string_view text = value.as_string();
			const std::string ext = edt::model_check_extension(text);
			if (!ext.empty() && std::ranges::find(extensions, ext) != extensions.end()) {
				return edt::named_model{ pointer, std::string(text), ext };
			}
			return std::nullopt;
		}
		if (value.is_object()) {
			for (const auto& item : value.as_object()) {
				if (auto found = find_in(item.value(), pointer + "/" + pointer_token(item.key()), extensions)) return found;
			}
		} else if (value.is_array()) {
			const json::array& items = value.as_array();
			for (std::size_t i = 0; i < items.size(); ++i) {
				if (auto found = find_in(items[i], pointer + "/" + std::to_string(i), extensions)) return found;
			}
		}
		return std::nullopt;
	}
}

std::string edt::model_check_extension(std::string_view text)
{
	while (!text.empty() && (text.back() == ' ' || text.back() == '.')) text.remove_suffix(1);
	const std::size_t segment = text.find_last_of("/\\");
	const std::string_view name = segment == std::string_view::npos ? text : text.substr(segment + 1);
	// As res::tag::extension(): a name without a dot is its own extension (res://models/glb is served by the "glb" adapter).
	const std::size_t dot = name.find_last_of('.');
	std::string ext(dot == std::string_view::npos ? name : name.substr(dot + 1));
	std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return ext;
}

std::optional<edt::named_model> edt::find_named_model(const json::value& args, std::span<const std::string> model_extensions)
{
	if (model_extensions.empty()) return std::nullopt;
	return find_in(args, std::string{}, model_extensions);
}

cmd::command_registry::agent_argument_check edt::make_agent_model_check(const res::resource_system& resources)
{
	return [&resources](const cmd::command_desc& command, const cmd::call& request) -> std::optional<cmd::result> {
		if (command.id == k_import_command) return std::nullopt;
		std::vector<std::string> extensions = resources.registered_extensions(scn::model_importer_adapter::INFO);
		for (std::string& ext : extensions) {
			std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		}
		const std::optional<named_model> found = find_named_model(request.args, extensions);
		if (!found) return std::nullopt;
		std::size_t cut = std::min(found->value.size(), k_shown_bytes);
		while (cut < found->value.size() && cut > 0 && (static_cast<unsigned char>(found->value[cut]) & 0xC0) == 0x80) --cut;
		const std::string_view shown = std::string_view(found->value).substr(0, cut);
		return cmd::result::failure(cmd::codes::denied, std::format(
			"'{}' names a model file at {} ('{}'{}): an agent brings a model into the project only with {}, which a person approves (#681)",
			command.id, found->pointer.empty() ? std::string{ "the arguments" } : found->pointer, shown,
			shown.size() < found->value.size() ? "..." : "", k_import_command));
	};
}
