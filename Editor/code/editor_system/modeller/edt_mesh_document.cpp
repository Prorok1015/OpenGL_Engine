#include "edt_mesh_document.h"
#include "edt_code_commands.h"
#include "edt_file_write_check.h"
#include "edt_mesh_shape_asset.h"
#include <snk/common/logger/engine_log.h>
#include <snk/desc/desc_system.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/resource/res_system.h>
#include <boost/json.hpp>
#include <atomic>
#include <vector>
#include <format>
#include <utility>

namespace json = boost::json;

namespace
{
	std::string_view without_suffix(std::string_view text, std::string_view suffix)
	{
		return text.substr(0, text.size() - suffix.size());
	}

	std::vector<std::byte> bytes_of(std::string_view text)
	{
		const auto* first = reinterpret_cast<const std::byte*>(text.data());
		return std::vector<std::byte>(first, first + text.size());
	}

	// The preview takes the geometry as the arrays form of its desc: a memory:// file has no binary file beside it.
	std::vector<std::byte> preview_bytes(const rnd::geometry_desc& geometry)
	{
		json::object body;
		geometry.serialize(body);
		body["__type"] = "geometry_desc";
		return bytes_of(json::serialize(body));
	}
}

edt::mesh_document::mesh_document(editor_services& services)
	: m_services(services)
{
}

edt::mesh_document::~mesh_document()
{
	close();
}

void edt::mesh_document::open(const res::tag& source)
{
	if (m_open) {
		close();
	}
	m_error.clear();

	const std::string_view text = source.view();
	std::string poly_text;
	if (text.ends_with(mesh::geometry_file_suffix)) {
		poly_text = std::string(without_suffix(text, mesh::geometry_file_suffix)) + std::string(mesh::poly_file_suffix);
	} else if (text.ends_with(mesh::poly_file_suffix)) {
		poly_text = std::string(text);
	} else {
		m_error = std::format("{} is neither a {} nor a {} file", text, mesh::poly_file_suffix, mesh::geometry_file_suffix);
		return;
	}
	const std::string_view stem = without_suffix(poly_text, mesh::poly_file_suffix);
	m_poly_tag = res::tag(poly_text);
	m_geometry_tag = res::tag(std::string(stem) + std::string(mesh::geometry_file_suffix));

	if (!m_services.resources() || !m_services.writable_resources() || !m_services.descs() || !m_files.top_layer) {
		m_error = "the project files are not known";
		return;
	}
	if (!load_source(m_error)) {
		if (text.ends_with(mesh::geometry_file_suffix) && m_error.starts_with("missing:")) {
			m_error = std::format("{} has no source to edit ({} is missing): it was not made by the editor's shape tools", text, m_poly_tag.view());
		}
		return;
	}

	static std::atomic<std::uint64_t> s_opened{ 0 };
	m_preview_tag = res::tag(std::format("memory://editor/mesh/{}.geom.desc", ++s_opened));
	m_title = std::string(stem.substr(stem.rfind('/') == std::string_view::npos ? 0 : stem.rfind('/') + 1));
	m_geometry_stale = false;
	m_stale_message.clear();
	m_open = true;
	rebake();
	watch_source();
	egLOG("Editor/Mesh", "Opened mesh '{}' for editing", m_poly_tag.view());
}

bool edt::mesh_document::load_source(std::string& error)
{
	const desc_file_read file = read_desc_file(*m_services.resources(), *m_services.descs(), m_files, m_poly_tag);
	if (file.missing) {
		error = "missing: " + std::string(m_poly_tag.view());
		return false;
	}
	if (!file.error.empty()) {
		error = std::format("{} cannot be read: {}", m_poly_tag.view(), file.error);
		return false;
	}
	mesh::poly_mesh_desc loaded;
	try {
		json::parse_options options;
		options.max_depth = 128;
		const json::value parsed = json::parse(std::string_view(reinterpret_cast<const char*>(file.bytes.data()), file.bytes.size()), {}, options);
		if (!parsed.is_object()) {
			error = std::format("{} is not a JSON object", m_poly_tag.view());
			return false;
		}
		mesh::poly_mesh_from_json(parsed.as_object(), loaded);
	} catch (const std::exception& failure) {
		error = std::format("{} is not a mesh: {}", m_poly_tag.view(), failure.what());
		return false;
	}
	m_working = std::move(loaded);
	m_saved_text = working_text();
	m_disk_version = file.version;
	m_dirty = false;
	m_changed_on_disk = false;
	return true;
}

std::string edt::mesh_document::working_text() const
{
	return mesh::poly_file_text(m_working);
}

void edt::mesh_document::close()
{
	if (!m_open) return;
	unwatch_source();
	if (res::resource_system* writer = m_services.writable_resources()) {
		writer->erase(m_preview_tag);
	}
	m_open = false;
	m_dirty = false;
	m_geometry_stale = false;
	m_changed_on_disk = false;
	m_stale_message.clear();
	m_working = {};
	m_baked = {};
	m_stats = {};
}

void edt::mesh_document::rebake()
{
	m_baked = mesh::bake(m_working.mesh);
	m_stats = {};
	m_stats.vertices = m_working.mesh.vertices().size();
	m_stats.faces = m_working.mesh.faces().size();
	m_stats.triangles = m_baked.geometry.indices.size() / 3;
	m_stats.skipped_faces = m_baked.skipped_faces.size();
	m_preview_problem.clear();

	res::resource_system* writer = m_services.writable_resources();
	if (!writer) {
		m_preview_problem = "the project resources are not available";
		return;
	}
	if (m_baked.geometry.indices.empty()) {
		m_preview_problem = "the mesh has no triangle to draw";
		writer->erase(m_preview_tag);
		writer->signal_changed(m_preview_tag);
		return;
	}
	try {
		writer->store(m_preview_tag, preview_bytes(m_baked.geometry));
	} catch (const std::exception& failure) {
		m_preview_problem = std::format("the mesh cannot be previewed: {}", failure.what());
		return;
	}
	writer->signal_changed(m_preview_tag);
}

void edt::mesh_document::announce()
{
	if (m_events) {
		m_events->document_changed.emit(id(), m_generation);
	}
}

bool edt::mesh_document::edit(const std::function<bool(mesh::poly_mesh&)>& change)
{
	if (!m_open || edits_locked()) {
		return false;
	}
	if (!change(m_working.mesh)) {
		return false;
	}
	++m_generation;
	rebake();
	m_dirty = working_text() != m_saved_text;
	announce();
	return true;
}

std::string edt::mesh_document::geometry_file_problem() const
{
	const desc_file_read geometry = read_desc_file(*m_services.resources(), *m_services.descs(), m_files, m_geometry_tag);
	if (geometry.in_open_level) {
		return std::format("{} belongs to the open level now: nothing was written", m_geometry_tag.view());
	}
	if (!geometry.missing && !geometry.error.empty()) {
		return std::format("{} cannot be checked before it is written: {}", m_geometry_tag.view(), geometry.error);
	}
	if (!geometry.missing && !geometry.top_layer) {
		return std::format("{} belongs to a lower layer (the engine or a package), not to the project's own res/: nothing was written", m_geometry_tag.view());
	}
	// Only while the source is about to change, and not after a failed pair: that file is ours and older than the source on purpose.
	if (m_dirty && !m_geometry_stale && !geometry.missing) {
		const std::string existing(reinterpret_cast<const char*>(geometry.bytes.data()), geometry.bytes.size());
		std::string unused;
		if (!mesh::geometry_is_bake_of(m_saved_text, existing, unused)) {
			return std::format("{} exists and was not made from {} (an imported mesh or another source): nothing was written. Rename one of the files or delete the geometry file.",
				m_geometry_tag.view(), m_poly_tag.view());
		}
	}
	return {};
}

edt::save_result edt::mesh_document::save()
{
	if (!m_open) {
		return save_result::failure("the mesh is not open");
	}
	if (edits_locked()) {
		return save_result::failure("Stop Play to save the mesh");
	}
	if (!is_dirty()) {
		return save_result::ok();
	}
	if (const std::optional<std::string> why = m_files.write_blocker ? m_files.write_blocker() : std::nullopt) {
		return save_result::failure(*why);
	}
	if (!m_preview_problem.empty()) {
		return save_result::failure(std::format("{}: nothing was written", m_preview_problem));
	}
	res::resource_system* reader = m_services.resources();
	res::resource_system* writer = m_services.writable_resources();
	desc::desc_system* descs = m_services.descs();
	if (!reader || !writer || !descs) {
		return save_result::failure("the project resources are not available");
	}

	const std::string geometry_problem = geometry_file_problem();
	if (!geometry_problem.empty()) {
		return save_result::failure(geometry_problem);
	}
	if (m_dirty) {
		const file_write_check allowed = check_file_write(*reader, *descs, m_files, m_poly_tag, m_disk_version);
		if (allowed.changed_on_disk && !m_changed_on_disk) {
			m_changed_on_disk = true;
			announce();
		}
		if (!allowed.error.empty()) {
			return save_result::failure(allowed.error);
		}
		const std::vector<std::byte> bytes = bytes_of(working_text());
		if (!writer->store_sync(m_poly_tag, bytes)) {
			return save_result::failure(std::format("{} could not be written", m_poly_tag.view()));
		}
		writer->signal_changed(m_poly_tag);
		m_saved_text = working_text();
		m_disk_version = code_version_of(bytes);
		m_dirty = false;
		m_changed_on_disk = false;
		m_geometry_stale = true;
	}

	const std::string written = m_write_geometry ? m_write_geometry(*writer, m_geometry_tag, m_baked.geometry)
		: mesh::write_mesh_geometry(*writer, m_geometry_tag, m_baked.geometry);
	if (!written.empty()) {
		m_stale_message = std::format("{} was saved, but {} could not be written ({}). The game still loads the old geometry: press Save again.",
			m_poly_tag.view(), m_geometry_tag.view(), written);
		announce();
		return save_result::failure(m_stale_message);
	}
	m_geometry_stale = false;
	m_stale_message.clear();
	announce();
	return save_result::ok();
}

std::string edt::mesh_document::revert()
{
	if (!m_open) {
		return "the mesh is not open";
	}
	if (edits_locked()) {
		return "Stop Play to revert the mesh";
	}
	std::string error;
	if (!load_source(error)) {
		return error;
	}
	++m_generation;
	rebake();
	announce();
	return {};
}

edt::save_result edt::mesh_document::overwrite()
{
	if (!m_open) {
		return save_result::failure("the mesh is not open");
	}
	if (!m_dirty) {
		const std::string error = revert();
		return error.empty() ? save_result::ok() : save_result::failure(error);
	}
	if (!m_services.resources() || !m_services.descs() || !m_files.top_layer) {
		return save_result::failure("the project files are not known: nothing was written");
	}
	const desc_file_read on_disk = read_desc_file(*m_services.resources(), *m_services.descs(), m_files, m_poly_tag);
	if (!on_disk.error.empty() && !on_disk.located) {
		return save_result::failure(std::format("{} cannot be checked before it is written: {}", m_poly_tag.view(), on_disk.error));
	}
	const std::string previous_version = std::exchange(m_disk_version, on_disk.version);
	const save_result saved = save();
	// Still dirty means the source was not written (a written source makes it clean even when the geometry pair failed).
	if (!saved.saved && m_dirty) {
		m_disk_version = previous_version;
		m_changed_on_disk = true;
	}
	return saved;
}

void edt::mesh_document::watch_source()
{
	unwatch_source();
	m_watching = m_services.writable_resources();
	if (m_watching) {
		m_watching->watch(m_poly_tag, this, [this](const res::tag& changed) { on_source_changed(changed); });
	}
}

void edt::mesh_document::unwatch_source()
{
	if (m_watching) {
		m_watching->unwatch(m_poly_tag, this);
		m_watching = nullptr;
	}
}

void edt::mesh_document::on_source_changed(const res::tag& changed)
{
	if (!m_open || changed != m_poly_tag) {
		return;
	}
	const desc_file_read on_disk = read_desc_file(*m_services.resources(), *m_services.descs(), m_files, m_poly_tag);
	if (on_disk.version == m_disk_version) {
		if (m_changed_on_disk) {
			m_changed_on_disk = false;
			announce();
		}
		return;
	}
	if (!m_dirty && !edits_locked()) {
		if (const std::string error = revert(); error.empty()) {
			return;
		} else {
			egLOG_WARN("Editor/Mesh", "{}", error);
		}
	}
	if (!m_changed_on_disk) {
		m_changed_on_disk = true;
		announce();
	}
}
