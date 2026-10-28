#include "edt_settings.h"
#include "edt_data_protocol.h"
#include <snk/common/logger/engine_log.h>
#include <snk/resource/resources/res_resource_text.h>
#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <vector>

res::tag edt::settings_tag(std::string_view name)
{
	std::string path{ "settings/" };
	path.append(name);
	return res::tag{ DATA_PROTOCOL, path };
}

bool edt::write_setting(res::resource_system& res, std::string_view name, std::string_view text)
{
	const res::tag tag = settings_tag(name);
	std::vector<std::byte> bytes(text.size());
	std::transform(text.begin(), text.end(), bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
	try {
		if (!res.store_sync(tag, bytes)) {
			egLOG_WARN("editor/settings", "cannot store '{}'", tag.view());
			return false;
		}
	} catch (const std::logic_error& e) {
		// A resource layer limited to a module's memory:// prefix refuses every other tag by throwing.
		egLOG_WARN("editor/settings", "cannot store '{}': {}", tag.view(), e.what());
		return false;
	}
	res.signal_changed(tag);
	return true;
}

std::optional<std::string> edt::read_setting(res::resource_system& res, std::string_view name)
{
	const res::tag tag = settings_tag(name);
	if (!res.exists(tag)) {
		return std::nullopt;
	}
	auto handle = res.require_sync<res::text_resource>(tag);
	if (handle.has_error()) {
		return std::nullopt;
	}
	const auto text = handle.get_sync();
	if (!text) {
		return std::nullopt;
	}
	return text->str();
}
