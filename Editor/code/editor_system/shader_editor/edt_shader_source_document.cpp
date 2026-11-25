#include "edt_shader_source_document.h"
#include "edt_code_commands.h"
#include "edt_file_menu.h"
#include "edt_shader_problems.h"
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/resource/res_system.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <utility>

namespace fs = std::filesystem;

namespace
{
	constexpr std::chrono::seconds k_read_timeout{ 5 };

	// The path a res:// tag names, when it stays inside the project: no drive, no backslash, no control character, no empty, '.' or '..' segment.
	std::string relative_of(const res::tag& tag, std::string& error)
	{
		if (tag.protocol() != res::tag::default_protocol()) {
			error = std::format("'{}': only res:// files of the project can be edited", tag.view());
			return {};
		}
		std::string_view path = tag.view();
		path.remove_prefix(std::string_view("res://").size());
		if (path.empty() || path.front() == '/') {
			error = std::format("'{}' names no file", tag.view());
			return {};
		}
		std::size_t begin = 0;
		while (begin <= path.size()) {
			const std::size_t end = std::min(path.find('/', begin), path.size());
			const std::string_view part = path.substr(begin, end - begin);
			if (part.empty() || part == "." || part == "..") {
				error = std::format("'{}' is not a path inside the project", tag.view());
				return {};
			}
			begin = end + 1;
		}
		for (char c : path) {
			if (c == '\\' || c == ':' || static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
				error = std::format("'{}' is not a path inside the project", tag.view());
				return {};
			}
		}
		return std::string(path);
	}

	// The editor's text has LF breaks only: a CRLF pair or a lone CR (a paste from another system) is one break.
	std::string fold_line_breaks(std::string text)
	{
		if (text.find('\r') == std::string::npos) return text;
		std::string out;
		out.reserve(text.size());
		for (std::size_t i = 0; i < text.size(); ++i) {
			if (text[i] != '\r') {
				out.push_back(text[i]);
				continue;
			}
			out.push_back('\n');
			if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
		}
		return out;
	}

	std::vector<std::byte> bytes_of(std::string_view text)
	{
		const auto* first = reinterpret_cast<const std::byte*>(text.data());
		return std::vector<std::byte>(first, first + text.size());
	}
}

edt::shader_source_document::shader_source_document(editor_services& services)
	: m_services(services)
{
}

edt::shader_source_document::~shader_source_document()
{
	close();
}

edt::shader_source_document::located edt::shader_source_document::locate(const res::tag& tag) const
{
	located out;
	out.relative = relative_of(tag, out.error);
	if (!out.error.empty()) return out;
	if (!is_shader_source(out.relative)) {
		out.error = std::format("'{}' is not a shader source", tag.view());
		return out;
	}
	std::vector<fs::path> roots = m_files.layers ? m_files.layers() : std::vector<fs::path>{};
	if (roots.empty() && m_files.top_layer) roots.push_back(m_files.top_layer());
	if (roots.empty()) {
		out.error = "the project files are not known";
		return out;
	}
	for (std::size_t i = roots.size(); i-- > 0;) {
		switch (inspect_level_file(roots[i], out.relative)) {
		case level_file_state::unsafe:
			out.error = std::format("'{}' is not a plain file inside the project", tag.view());
			return out;
		case level_file_state::file:
			out.file = roots[i] / fs_paths::path_from_utf8(out.relative);
			out.top_layer = i + 1 == roots.size();
			return out;
		case level_file_state::missing:
			break;
		}
	}
	out.error = std::format("no file '{}'", tag.view());
	out.missing = true;
	return out;
}

edt::shader_source_document::disk_read edt::shader_source_document::read_disk(const res::tag& tag, const fs::path& path) const
{
	disk_read out;
	res::resource_system* resources = m_services.resources();
	if (!resources) {
		out.error = "the project resources are not available";
		return out;
	}
	std::error_code ec;
	const auto size = fs::file_size(path, ec);
	if (ec) {
		out.error = std::format("'{}' could not be read", tag.view());
		return out;
	}
	if (size > k_max_shader_text_bytes) {
		out.error = std::format("'{}' is larger than 4 MiB", tag.view());
		return out;
	}
	const auto raw = resources->fetch_data(tag);
	if (!raw) {
		out.error = "the project resources are not mounted";
		return out;
	}
	if (!raw->wait_for(k_read_timeout)) {
		out.error = std::format("'{}' could not be read in {} s", tag.view(), k_read_timeout.count());
		return out;
	}
	if (!raw->is_ready()) {
		out.error = std::format("'{}' could not be read", tag.view());
		return out;
	}
	out.bytes = raw->data;
	return out;
}

void edt::shader_source_document::open(const res::tag& source)
{
	if (m_open) {
		close();
	}
	m_error.clear();
	if (!m_services.resources() || !m_services.writable_resources() || !m_files.top_layer) {
		m_error = "the project files are not known";
		return;
	}
	const located where = locate(source);
	if (!where.error.empty()) {
		m_error = where.error;
		return;
	}
	if (!load(source, where.file, m_error)) {
		return;
	}
	m_tag = source;
	m_disk_path = where.file;
	m_top_layer = where.top_layer;
	const std::string_view path = where.relative;
	const std::size_t slash = path.rfind('/');
	m_title = std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
	m_open = true;
	watch();
	egLOG("Editor/Shader", "Opened shader source '{}' for editing", m_tag.view());
}

bool edt::shader_source_document::load(const res::tag& tag, const fs::path& path, std::string& error)
{
	const disk_read read = read_disk(tag, path);
	if (!read.error.empty()) {
		error = read.error;
		return false;
	}
	std::string why;
	std::optional<shader_text_file> parsed = read_shader_text(read.bytes, why);
	if (!parsed) {
		error = std::format("{}: {}", tag.view(), why);
		return false;
	}
	m_file = std::move(*parsed);
	m_text = m_file.text;
	m_saved_text = m_text;
	m_disk_version = code_version_of(read.bytes);
	m_dirty = false;
	m_changed_on_disk = false;
	m_missing_on_disk = false;
	return true;
}

void edt::shader_source_document::close()
{
	unwatch();
	m_open = false;
	m_file = {};
	m_tag = res::tag{};
	m_disk_path.clear();
	m_text.clear();
	m_saved_text.clear();
	m_dirty = false;
	m_changed_on_disk = false;
	m_missing_on_disk = false;
}

void edt::shader_source_document::announce()
{
	if (m_events) {
		m_events->document_changed.emit(id(), m_generation);
	}
}

void edt::shader_source_document::mark_missing()
{
	if (m_missing_on_disk && m_changed_on_disk) return;
	m_missing_on_disk = true;
	m_changed_on_disk = true;
	announce();
}

bool edt::shader_source_document::step_history(bool redo)
{
	if (!m_open || !m_top_layer || edits_locked()) return false;
	const std::function<bool()>& can = redo ? m_history.can_redo : m_history.can_undo;
	const std::function<void()>& step = redo ? m_history.redo : m_history.undo;
	if (!can || !step) return false;
	if (m_flush_edits) m_flush_edits();
	if (!can()) return false;
	step();
	return true;
}

std::optional<edt::history_entry> edt::shader_source_document::history_entry_of(bool redo) const
{
	if (!m_open || !m_top_layer || edits_locked()) return std::nullopt;
	const std::function<bool()>& can = redo ? m_history.can_redo : m_history.can_undo;
	if (!can || !can()) return std::nullopt;
	return history_entry{ "Typing", cmd::origin::ui() };
}

int edt::shader_source_document::line_count() const
{
	return static_cast<int>(std::ranges::count(m_text, '\n')) + 1;
}

std::string edt::shader_source_document::read_only_reason() const
{
	if (!m_open) return {};
	if (!m_top_layer) {
		return std::format("{} belongs to a layer below the project: it can be read here, and saved only as a file of the project's own", m_tag.view());
	}
	if (edits_locked()) return "Stop Play to edit the shader";
	return {};
}

bool edt::shader_source_document::set_text(std::string text)
{
	if (!m_open || !m_top_layer || edits_locked()) {
		return false;
	}
	text = fold_line_breaks(std::move(text));
	if (text == m_text) {
		return false;
	}
	m_text = std::move(text);
	m_dirty = m_text != m_saved_text;
	++m_generation;
	announce();
	return true;
}

edt::save_result edt::shader_source_document::save()
{
	if (!m_open) {
		return save_result::failure("the shader is not open");
	}
	if (m_flush_edits) {
		m_flush_edits();
	}
	if (edits_locked()) {
		return save_result::failure("Stop Play to save the shader");
	}
	if (!m_top_layer) {
		return save_result::failure(read_only_reason());
	}
	if (!m_dirty) {
		return save_result::ok();
	}
	if (const std::optional<std::string> why = m_files.write_blocker ? m_files.write_blocker() : std::nullopt) {
		return save_result::failure(*why);
	}
	res::resource_system* writer = m_services.writable_resources();
	if (!writer) {
		return save_result::failure("the project resources are not available");
	}
	const located where = locate(m_tag);
	if (!where.error.empty()) {
		if (where.missing) {
			mark_missing();
			return save_result::failure(std::format("{} was deleted or renamed outside the editor: Overwrite writes it again", m_tag.view()));
		}
		return save_result::failure(where.error);
	}
	if (!where.top_layer) {
		return save_result::failure(std::format("{} is now held by a layer below the project: nothing was written", m_tag.view()));
	}
	const disk_read now = read_disk();
	if (!now.error.empty()) {
		return save_result::failure(std::format("{} cannot be checked before it is written: {}", m_tag.view(), now.error));
	}
	m_missing_on_disk = false;
	// A conflict the window raised (restored autosave, typing over a reload) stands though the file is as it was read.
	if (m_changed_on_disk || code_version_of(now.bytes) != m_disk_version) {
		if (!m_changed_on_disk) {
			m_changed_on_disk = true;
			announce();
		}
		return save_result::failure(std::format("{} changed on disk since it was opened: Reload takes the file, Overwrite replaces it", m_tag.view()));
	}
	shader_text_file out = m_file;
	out.text = m_text;
	const std::vector<std::byte> bytes = bytes_of(write_shader_text(out));
	if (bytes.size() > k_max_shader_text_bytes) {
		return save_result::failure("the text is larger than 4 MiB: nothing was written");
	}
	const std::string previous_version = std::exchange(m_disk_version, code_version_of(bytes));
	const bool previous_changed = std::exchange(m_changed_on_disk, false);
	if (!writer->store_sync(m_tag, bytes)) {
		m_disk_version = previous_version;
		m_changed_on_disk = previous_changed;
		return save_result::failure(std::format("{} could not be written", m_tag.view()));
	}
	m_saved_text = m_text;
	m_dirty = false;
	writer->signal_changed(m_tag);
	++m_generation;
	announce();
	return save_result::ok();
}

void edt::shader_source_document::raise_conflict()
{
	if (m_open && !m_changed_on_disk) {
		m_changed_on_disk = true;
		announce();
	}
}

std::string edt::shader_source_document::revert()
{
	if (!m_open) {
		return "the shader is not open";
	}
	if (edits_locked()) {
		return "Stop Play to revert the shader";
	}
	std::string error;
	const located where = locate(m_tag);
	if (!where.error.empty()) {
		return where.error;
	}
	if (!load(m_tag, where.file, error)) {
		return error;
	}
	m_disk_path = where.file;
	m_top_layer = where.top_layer;
	++m_generation;
	announce();
	return {};
}

edt::save_result edt::shader_source_document::recreate()
{
	res::resource_system* writer = m_services.writable_resources();
	if (!writer) {
		return save_result::failure("the project resources are not available");
	}
	shader_text_file out = m_file;
	out.text = m_text;
	const std::vector<std::byte> bytes = bytes_of(write_shader_text(out));
	if (bytes.size() > k_max_shader_text_bytes) {
		return save_result::failure("the text is larger than 4 MiB: nothing was written");
	}
	if (!writer->store_sync(m_tag, bytes)) {
		return save_result::failure(std::format("{} could not be written", m_tag.view()));
	}
	const located written = locate(m_tag);
	if (!written.error.empty() || !written.top_layer) {
		return save_result::failure(std::format("{} was written, but it is not a file of the project's own layer now", m_tag.view()));
	}
	m_disk_path = written.file;
	m_disk_version = code_version_of(bytes);
	m_saved_text = m_text;
	m_dirty = false;
	m_changed_on_disk = false;
	m_missing_on_disk = false;
	writer->signal_changed(m_tag);
	++m_generation;
	announce();
	return save_result::ok();
}

edt::save_result edt::shader_source_document::overwrite()
{
	if (!m_open) {
		return save_result::failure("the shader is not open");
	}
	if (edits_locked()) {
		return save_result::failure("Stop Play to save the shader");
	}
	if (!m_top_layer) {
		return save_result::failure(read_only_reason());
	}
	if (locate(m_tag).missing) {
		if (const std::optional<std::string> why = m_files.write_blocker ? m_files.write_blocker() : std::nullopt) {
			return save_result::failure(*why);
		}
		return recreate();
	}
	if (!m_dirty) {
		const std::string error = revert();
		return error.empty() ? save_result::ok() : save_result::failure(error);
	}
	if (const std::optional<std::string> why = m_files.write_blocker ? m_files.write_blocker() : std::nullopt) {
		return save_result::failure(*why);
	}
	const disk_read now = read_disk();
	if (!now.error.empty()) {
		return save_result::failure(std::format("{} cannot be checked before it is written: {}", m_tag.view(), now.error));
	}
	const std::string previous_version = std::exchange(m_disk_version, code_version_of(now.bytes));
	const bool previous_changed = std::exchange(m_changed_on_disk, false);
	const save_result saved = save();
	if (!saved.saved) {
		m_disk_version = previous_version;
		m_changed_on_disk = previous_changed;
	}
	return saved;
}

edt::autosave_snapshot edt::shader_source_document::autosave_capture() const
{
	autosave_snapshot out;
	if (!m_open) {
		out.error = "the shader is not open";
		return out;
	}
	shader_text_file file = m_file;
	const bool typing_waits = m_pending_edits && m_peek_edits && m_pending_edits();
	file.text = typing_waits ? fold_line_breaks(m_peek_edits()) : m_text;
	out.bytes = bytes_of(write_shader_text(file));
	return out;
}

void edt::shader_source_document::watch()
{
	unwatch();
	m_watching = m_services.writable_resources();
	if (m_watching) {
		m_watching->watch(m_tag, this, [this](const res::tag& changed) { on_source_changed(changed); });
	}
}

void edt::shader_source_document::unwatch()
{
	if (m_watching) {
		m_watching->unwatch(m_tag, this);
		m_watching = nullptr;
	}
}

void edt::shader_source_document::on_source_changed(const res::tag& changed)
{
	if (!m_open || changed != m_tag) {
		return;
	}
	const disk_read now = read_disk();
	if (!now.error.empty()) {
		if (locate(m_tag).missing) {
			mark_missing();
		}
		return;
	}
	if (m_missing_on_disk) {
		// The file is back: the banner is about its content now (a different one is the usual conflict).
		m_missing_on_disk = false;
		announce();
	}
	if (code_version_of(now.bytes) == m_disk_version) {
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
			egLOG_WARN("Editor/Shader", "{}", error);
		}
	}
	if (!m_changed_on_disk) {
		m_changed_on_disk = true;
		announce();
	}
}
