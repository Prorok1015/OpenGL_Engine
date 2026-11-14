#include <snk/project_migration/pmg_asset_steps.h>
#include <snk/desc/desc_pretty_json.h>
#include <snk/resource/res_asset_id.h>
#include <snk/resource/res_asset_meta.h>
#include <snk/resource/res_asset_walk.h>
#include <boost/json.hpp>
#include <algorithm>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json = boost::json;

namespace
{
	using namespace prj::migration;

	bool ids_needed(const scan_result& scan)
	{
		return std::ranges::any_of(scan.files,
			[](const scanned_file& file) { return !file.has_resource && file.kind != file_class::invalid; });
	}

	bool is_desc_path(std::string_view path)
	{
		constexpr std::string_view suffix = ".desc";
		if (path.size() <= suffix.size()) return false;
		for (std::size_t i = 0; i < suffix.size(); ++i) {
			const char c = path[path.size() - suffix.size() + i];
			if ((c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c) != suffix[i]) return false;
		}
		return true;
	}

	// The root __guid of a desc as an ID; none for a file that is not a desc, not JSON or has no usable one.
	std::optional<res::asset_id> guid_of(project_step_files& files, const std::string& path)
	{
		if (!is_desc_path(path)) return std::nullopt;
		return desc_root_guid(files.read(path));
	}

	void ids_apply(project_step_context& context)
	{
		// Who holds an ID already: every .resource on the disk, in a content root a higher one covers too, then what
		// this step gives out. Named by the file the .resource belongs to.
		std::map<res::asset_id, std::string> holders;
		for (const scanned_file& file : context.scan.files) {
			if (!res::is_asset_meta_sidecar(file.path)) continue;
			const res::asset_meta_result meta = res::read_asset_meta(context.files.read(file.path));
			if (meta) holders.emplace(meta.meta->guid, std::string(res::asset_meta_owner(file.path)));
		}

		std::size_t created = 0;
		for (const scanned_file& file : context.scan.files) {
			if (file.has_resource || file.kind == file_class::invalid) continue;

			res::asset_meta meta;
			if (const std::optional<res::asset_id> guid = guid_of(context.files, file.path)) {
				if (const auto taken = holders.find(*guid); taken == holders.end()) {
					meta.guid = *guid;
					context.warn(std::format("{}: ID {} from its __guid", file.path, res::to_text(*guid)));
				} else {
					meta.guid = res::generate_asset_id();
					context.warn(std::format("{}: its __guid {} is already the ID of {}; it gets the new ID {} (assets.ids_drop_desc_guid takes the __guid out)",
						file.path, res::to_text(*guid), taken->second, res::to_text(meta.guid)));
				}
			} else {
				meta.guid = res::generate_asset_id();
			}
			holders.emplace(meta.guid, file.path);
			context.files.create(file.path + ".resource", res::write_asset_meta(meta));
			++created;
		}
		context.warn(std::format("{} .resource files created", created));
	}

	bool drop_guid_needed(const scan_result& scan)
	{
		return std::ranges::any_of(scan.files, [](const scanned_file& file) { return file.root_guid && file.kind != file_class::invalid; });
	}

	void drop_guid_apply(project_step_context& context)
	{
		std::size_t dropped = 0;
		for (const scanned_file& file : context.scan.files) {
			if (!file.root_guid || file.kind == file_class::invalid) continue;
			const std::optional<res::asset_id> id = context.assets.id_of(file.path);
			if (!id) {
				context.warn(std::format("{}: keeps its __guid, it has no .resource with an ID", file.path));
				continue;
			}
			const std::string read = context.files.read(file.path);
			std::string_view text = read;
			if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
			boost::system::error_code error;
			json::parse_options options;
			options.max_depth = 128;
			json::value root = json::parse(text, error, {}, options);
			if (error || !root.is_object()) {
				context.warn(std::format("{}: keeps its __guid, it is no JSON object now", file.path));
				continue;
			}
			json::object& object = root.as_object();
			const json::value* guid = object.if_contains("__guid");
			if (!guid) continue;
			const std::optional<res::asset_id> as_id = guid->is_string() ? res::asset_id_from_text(std::string_view(guid->as_string())) : std::nullopt;
			if (as_id != id) {
				context.warn(std::format("{}: its __guid {} is taken out; the ID of the file is {}", file.path, json::serialize(*guid), res::to_text(*id)));
			}
			object.erase("__guid");
			const std::vector<std::byte> bytes = desc::pretty_json_bytes(root);
			context.files.write(file.path, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
			++dropped;
		}
		context.warn(std::format("{} desc files without their __guid", dropped));
	}
}

std::optional<res::asset_id> prj::migration::desc_root_guid(std::string_view text)
{
	if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
	boost::system::error_code error;
	const json::value root = json::parse(text, error);
	if (error || !root.is_object()) return std::nullopt;
	const json::value* guid = root.as_object().if_contains("__guid");
	if (!guid || !guid->is_string()) return std::nullopt;
	std::optional<res::asset_id> id = res::asset_id_from_text(std::string_view(guid->as_string()));
	if (!id || id->is_nil()) return std::nullopt;
	return id;
}

void prj::migration::register_asset_project_steps(project_step_registry& registry)
{
	project_step ids;
	ids.id = "assets.ids";
	ids.phase = project_phase::pre;
	ids.summary = "a .resource with an ID for every content file without one";
	ids.needed = &ids_needed;
	ids.apply = &ids_apply;
	registry.add(std::move(ids));

	project_step drop_guid;
	drop_guid.id = "assets.ids_drop_desc_guid";
	drop_guid.phase = project_phase::post;
	drop_guid.summary = "the root __guid out of every desc: its ID is its .resource";
	drop_guid.needed = &drop_guid_needed;
	drop_guid.apply = &drop_guid_apply;
	registry.add(std::move(drop_guid));
}

void prj::migration::register_project_steps(project_step_registry& registry)
{
	register_engine_project_steps(registry);
	register_asset_project_steps(registry);
}
