#include "edt_prefab_file_edit.h"
#include "edt_override_edit.h"
#include "edt_prefab_document.h"
#include <snk/desc/desc_system.h>
#include <snk/scene/level/scn_prefab_layer.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/resource/res_system.h>
#include <boost/json.hpp>
#include <format>

std::vector<std::byte> edt::prefab_file_bytes(editor_services& services, const scn::prefab_desc& prefab, const res::tag& tag,
	std::string* why)
{
	desc::desc_system* descs = services.descs();
	res::resource_system* resources = services.resources();
	if (!descs || !resources) {
		return {};
	}
	namespace json = boost::json;
	json::object body;
	prefab.serialize(body);
	if (prefab.get_parent().is_valid()) {
		if (std::string reason = scn::write_prefab_layer(*descs, *resources, prefab, body); !reason.empty()) {
			if (why) *why = std::move(reason);
			return {};
		}
	}
	body["__type"] = std::string(scn::prefab_desc::__type);
	// A __guid the file had stays until the project's migration takes it out; none is made up (the adapter does the same).
	if (!prefab.get_guid().empty()) {
		body["__guid"] = prefab.get_guid();
	}
	descs->stamp_tree(body);
	json::value file{ std::move(body) };
	descs->write_references_over(file, tag);
	return descs->serialize_to_bytes(file);
}

std::string edt::edit_prefab_file(editor_services& services, const desc_file_layout& files, const res::tag& tag,
	const std::function<std::string(scn::prefab_desc&)>& change)
{
	desc::desc_system* descs = services.descs();
	res::resource_system* reader = services.resources();
	res::resource_system* writer = services.writable_resources();
	if (!descs || !reader || !writer || !files.top_layer) {
		return "the project files are not known: nothing was written";
	}
	if (const std::optional<std::string> why = files.write_blocker ? files.write_blocker() : std::nullopt) {
		return *why;
	}
	const desc_file_read on_disk = read_desc_file(*reader, *descs, files, tag);
	if (!on_disk.error.empty()) {
		return std::format("{} cannot be read: {}", tag.view(), on_disk.error);
	}
	if (!on_disk.top_layer) {
		return std::format("{} belongs to a lower layer (the engine or a package), not to the project's own res/: nothing was written", tag.view());
	}
	if (on_disk.in_open_level) {
		return std::format("{} belongs to the open level now: nothing was written", tag.view());
	}

	writer->evict_cached(tag);
	auto handle = reader->require_sync<scn::prefab_desc>(tag);
	if (!handle.is_ready()) {
		return std::format("{} could not be loaded", tag.view());
	}
	scn::prefab_desc edited;
	handle->copy_to(edited);
	edited.set_guid(handle->get_guid());
	edited.set_parent(handle->get_parent());
	if (std::string error = change(edited); !error.empty()) {
		return error;
	}
	std::string why;
	const std::vector<std::byte> bytes = prefab_file_bytes(services, edited, tag, &why);
	if (bytes.empty()) {
		return std::format("{} could not be turned into a file: {}. Nothing was written.", tag.view(), why.empty() ? "no file form" : why);
	}
	if (bytes.size() > k_desc_file_limit) {
		return std::format("{} would be {} bytes: a prefab holds at most {}", tag.view(), bytes.size(), k_desc_file_limit);
	}
	if (!writer->store_sync(tag, bytes)) {
		return std::format("{} could not be written", tag.view());
	}
	writer->signal_changed(tag);
	return {};
}

std::string edt::apply_override_records(editor_services& services, const desc_file_layout& files, const res::tag& tag,
	const std::vector<scn::override_record>& records, const prefab_document* open_dirty)
{
	if (open_dirty && open_dirty->source_tag() == tag) {
		return std::format("{} is open in the prefab editor with unapplied edits: apply or revert them first", tag.view());
	}
	return edit_prefab_file(services, files, tag, [&records] (scn::prefab_desc& prefab) {
		for (const scn::override_record& record : records) {
			if (std::string error = override_edit::fold_record(prefab, record); !error.empty()) {
				return error;
			}
		}
		return std::string{};
	});
}
