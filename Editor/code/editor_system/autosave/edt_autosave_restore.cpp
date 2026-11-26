#include "edt_autosave_restore.h"
#include "edt_data_document.h"
#include "edt_mesh_document.h"
#include "edt_mesh_io.h"
#include "edt_prefab_document.h"
#include "edt_shader_source_document.h"
#include "edt_shader_text.h"
#include <snk/editor/edt_document_manager.h>
#include <snk/resource/res_system.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>
#include <exception>
#include <format>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
	std::optional<boost::json::object> object_of(std::span<const std::byte> bytes, std::string& error)
	{
		boost::json::error_code ec;
		boost::json::parse_options options;
		options.max_depth = 256;
		boost::json::value parsed = boost::json::parse(
			std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), ec, {}, options);
		if (ec || !parsed.is_object()) {
			error = ec ? std::format("the autosave is no JSON object: {}", ec.message()) : std::string{ "the autosave is no JSON object" };
			return std::nullopt;
		}
		return std::move(parsed.as_object());
	}
}

std::string edt::restore_prefab_bytes(res::resource_system& writable, res::resource_system& resources, prefab_document& prefab,
	const res::tag& copy, std::span<const std::byte> bytes)
{
	if (!prefab.is_open()) return "the prefab is not open";
	if (!writable.store_sync(copy, std::vector<std::byte>(bytes.begin(), bytes.end()))) return "the working copy could not be stored";
	std::string error;
	{
		auto handle = resources.require_sync<scn::prefab_desc>(copy);
		const std::shared_ptr<scn::prefab_desc> read = handle.is_ready() ? handle.get_sync() : nullptr;
		if (!read) {
			error = "the autosave does not read as a prefab";
		} else if (!prefab.edit([&read](scn::prefab_desc& working) {
				read->copy_to(working);
				return true;
			}, "Restore autosave")) {
			error = "the prefab cannot be edited now";
		}
	}
	static_cast<void>(writable.erase(copy));
	return error;
}

std::string edt::restore_mesh_bytes(mesh_document& mesh, std::span<const std::byte> bytes)
{
	if (!mesh.is_open()) return "the mesh is not open";
	std::string error;
	const std::optional<boost::json::object> object = object_of(bytes, error);
	if (!object) return error;
	mesh::poly_mesh_desc read;
	try {
		mesh::poly_mesh_from_json(*object, read);
	} catch (const std::exception& failure) {
		return std::format("the autosave does not read as a mesh: {}", failure.what());
	}
	if (!mesh.edit([&read](mesh::poly_mesh& working) {
			working = std::move(read.mesh);
			return true;
		}, "Restore autosave")) {
		return "the mesh cannot be edited now";
	}
	return {};
}

std::string edt::restore_data_bytes(data_document& data, std::span<const std::byte> bytes)
{
	if (!data.is_open()) return "the document is not open";
	std::string error;
	std::optional<boost::json::object> object = object_of(bytes, error);
	if (!object) return error;
	if (!data.restore_own(std::move(*object))) return "the autosave is of another __type or __parent than the file";
	return {};
}

std::string edt::restore_shader_source_bytes(shader_source_document& shader, std::span<const std::byte> bytes, bool source_changed)
{
	if (!shader.is_open()) return "the shader is not open";
	if (std::string reason = shader.read_only_reason(); !reason.empty()) return reason;
	std::string error;
	const std::optional<shader_text_file> read = read_shader_text(bytes, error);
	if (!read) return error;
	if (shader.is_dirty()) return "the shader has unsaved changes of its own";
	// set_text is false for the text the shader already has: nothing to restore, and the record is then discarded as done.
	if (!shader.set_text(read->text) && shader.text() != read->text) return "the shader cannot be edited now";
	if (source_changed) shader.raise_conflict();
	return {};
}

edt::shader_restore_result edt::restore_shader_source(document_manager& documents, const std::function<bool(const res::tag&)>& open,
	const res::tag& source, std::span<const std::byte> bytes, bool source_changed)
{
	if (!open || !open(source)) return { "the shader did not open", {}, 0 };
	shader_source_document* shader = nullptr;
	documents.for_each([&shader, &source](document& candidate) {
		auto* found = dynamic_cast<shader_source_document*>(&candidate);
		if (found && found->is_open() && found->source() == source) shader = found;
	});
	if (!shader) return { "the shader did not open", {}, 0 };
	if (std::string error = restore_shader_source_bytes(*shader, bytes, source_changed); !error.empty()) return { std::move(error), {}, 0 };
	return { {}, shader->id(), shader->generation() };
}
