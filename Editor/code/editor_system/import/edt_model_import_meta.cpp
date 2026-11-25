#include "edt_model_import_meta.h"
#include "edt_agent_source_rule.h"
#include <snk/common/fs_paths.h>
#include <snk/common/text_escape.h>
#include <snk/platform/plt_beneath.h>
#include <snk/resource/res_asset_meta.h>
#include <boost/json/value.hpp>
#include <cstddef>
#include <cstdint>
#include <format>
#include <random>
#include <span>
#include <vector>

namespace fs = std::filesystem;

namespace {

// A sidecar is a few hundred bytes; a bigger file at its name is none this editor wrote.
constexpr std::uint64_t k_max_meta_bytes = 64 * 1024;

std::string shown(std::string_view text)
{
	return text_escape::escaped_text(text, 1024);
}

std::string private_text(std::string text, const fs::path& top)
{
	return shown(edt::without_paths(std::move(text), { top }, {}));
}

fs::path temp_leaf_for(const fs::path& leaf)
{
	std::random_device random;
	fs::path out{ "." };
	out += leaf;
	out += std::format(".{:08x}.tmp", random());
	return out;
}

struct read_meta
{
	platform::beneath::read_status status = platform::beneath::read_status::failed;
	std::string text;
	std::string error;
};

read_meta read_existing(const fs::path& top, const edt::model_meta_plan& plan)
{
	platform::beneath::read_request request;
	request.root = top;
	request.folder = plan.folder;
	request.leaf = plan.leaf;
	request.max_bytes = k_max_meta_bytes;
	std::vector<std::byte> bytes;
	const platform::beneath::read_result read = platform::beneath::read_beneath(request, bytes);
	read_meta out;
	out.status = read.status;
	out.error = read.error;
	if (read.status == platform::beneath::read_status::ok) {
		out.text.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
	}
	return out;
}

std::string write_text(const fs::path& top, const edt::model_meta_plan& plan, const std::string& text, bool replace)
{
	platform::beneath::pinned_root pinned{ top };
	if (!plan.folder.empty()) {
		if (std::string error = platform::beneath::make_folders_beneath(pinned, plan.folder); !error.empty()) {
			return error;
		}
	}
	platform::beneath::write_request request;
	request.folder = plan.folder;
	request.leaf = plan.leaf;
	request.tmp_leaf = temp_leaf_for(plan.leaf);
	request.data = std::as_bytes(std::span(text));
	request.replace = replace;
	request.tmp_path = top / plan.folder / request.tmp_leaf;
	return platform::beneath::write_beneath(pinned, request);
}

}

edt::model_meta_plan edt::plan_model_meta(const fs::path& top, const res::tag& prefab,
	const model_import_settings& settings, const model_source_record& source, bool may_replace)
{
	model_meta_plan plan;
	const std::string relative = std::string(prefab.relative()) + std::string(res::asset_meta_suffix);
	plan.tag = res::tag::make(relative);
	const fs::path path = fs_paths::path_from_utf8(relative);
	plan.folder = path.parent_path();
	plan.leaf = path.filename();
	const std::string named = shown(plan.tag.view());
	if (top.empty()) {
		plan.error = std::format("{} cannot be written: no project is open", named);
		return plan;
	}
	// What is written must read back by this editor's own rule.
	const boost::json::object written_settings = model_settings_json(settings, source);
	if (const model_settings_parse check = parse_model_settings(written_settings); !check.ok()) {
		plan.error = std::format("{} cannot be written: {}", named, shown(check.error));
		return plan;
	}

	const read_meta existing = read_existing(top, plan);
	res::asset_meta meta;
	switch (existing.status) {
	case platform::beneath::read_status::missing:
		meta.guid = res::generate_asset_id();
		meta.importer = std::string(k_model_importer);
		meta.settings = written_settings;
		break;
	case platform::beneath::read_status::ok: {
		if (!may_replace) {
			plan.error = std::format("{} exists: nothing was written", named);
			return plan;
		}
		const res::asset_meta_result read = res::read_asset_meta(existing.text);
		if (!read) {
			const bool newer = read.error.kind == res::asset_meta_error_kind::newer_than_engine;
			plan.error = newer
				? std::format("{} was written by a newer editor: it is not rewritten, nothing was written", named)
				: std::format("{} cannot be read ({}): it is not rewritten, nothing was written; fix or remove it",
					named, shown(read.error.message));
			return plan;
		}
		if (res::write_asset_meta(*read.meta) != existing.text) {
			plan.error = std::format("{} is not in the form the editor writes (edited by hand?): it is not rewritten, "
				"nothing was written; save it again in that form or remove it", named);
			return plan;
		}
		meta = *read.meta;
		if (meta.importer == k_model_importer) {
			boost::json::object kept;
			const std::string why = rewrite_model_settings(boost::json::value(meta.settings), settings, source, kept);
			if (!why.empty()) {
				plan.error = std::format("{}: {}; it is not rewritten, nothing was written", named, shown(why));
				return plan;
			}
			meta.settings = std::move(kept);
		} else {
			// Another importer's settings mean nothing to a model.
			meta.importer = std::string(k_model_importer);
			meta.settings = written_settings;
		}
		plan.before = existing.text;
		break;
	}
	case platform::beneath::read_status::not_regular:
		plan.error = std::format("{} is not a plain file: nothing was written", named);
		return plan;
	case platform::beneath::read_status::too_large:
		plan.error = std::format("{} is larger than a .resource can be: nothing was written", named);
		return plan;
	case platform::beneath::read_status::failed:
		plan.error = std::format("{} cannot be read: {}; nothing was written", named,
			private_text(existing.error, top));
		return plan;
	}
	plan.guid = meta.guid;
	plan.text = res::write_asset_meta(meta);
	return plan;
}

std::string edt::write_model_meta(const fs::path& top, const model_meta_plan& plan)
{
	const std::string named = shown(plan.tag.view());
	if (!plan.ok()) return plan.error;
	if (plan.before) {
		const read_meta now = read_existing(top, plan);
		if (now.status != platform::beneath::read_status::ok || now.text != *plan.before) {
			return std::format("{} changed since it was read: it is not rewritten", named);
		}
	}
	if (std::string error = write_text(top, plan, plan.text, plan.before.has_value()); !error.empty()) {
		return std::format("{} was not written: {}", named, private_text(std::move(error), top));
	}
	return {};
}

std::string edt::take_back_model_meta(const fs::path& top, const model_meta_plan& plan)
{
	const std::string named = shown(plan.tag.view());
	std::string error;
	if (plan.before) {
		error = write_text(top, plan, *plan.before, true);
	} else {
		platform::beneath::pinned_root pinned{ top };
		error = platform::beneath::remove_beneath(pinned, plan.folder, plan.leaf, true);
	}
	if (error.empty()) return {};
	return std::format("{} could not be taken back: {}", named, private_text(std::move(error), top));
}
