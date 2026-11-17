#include <snk/resource/res_reference.h>
#include <string_view>

namespace
{
	res::tag tag_of(std::string_view text)
	{
		return text.empty() ? res::tag{} : res::tag{ text };
	}
}

std::optional<res::reference> res::reference_from_json(const json::value& value)
{
	if (value.is_string()) {
		return reference{ asset_id{}, tag_of(value.get_string()) };
	}
	const json::object* object = value.if_object();
	if (!object || object->empty()) {
		return std::nullopt;
	}
	reference result;
	for (const auto& [key, field] : *object) {
		if (key == "guid") {
			const auto id = field.is_string() ? asset_id_from_text(field.get_string()) : std::nullopt;
			if (!id || id->is_nil()) return std::nullopt;
			result.guid = *id;
		} else if (key == "path") {
			if (!field.is_string()) return std::nullopt;
			result.path = tag_of(field.get_string());
		} else {
			return std::nullopt;
		}
	}
	return result;
}
