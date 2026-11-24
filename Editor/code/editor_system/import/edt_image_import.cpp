#include "edt_image_import.h"
#include "edt_agent_log_filter.h"
#include "edt_asset_picker.h"
#include "edt_file_menu.h"
#include <snk/common/fs_paths.h>
#include <snk/common/text_escape.h>
#include <snk/editor/edt_job_service.h>
#include <snk/platform/plt_beneath.h>
#include <snk/resource/adapters/res_pct_adapter.h>
#include <snk/resource/res_asset_meta.h>
#include <snk/resource/res_asset_walk.h>
#include <snk/resource/res_free_name.h>
#include <snk/resource/res_pretty_json.h>
#include <algorithm>
#include <array>
#include <boost/json/serialize.hpp>
#include <cctype>
#include <format>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace json = boost::json;
namespace fs = std::filesystem;

namespace
{
	constexpr std::array<std::string_view, 4> k_wrap_names = {
		"clamp_to_edge", "repeat", "repeat_mirror", "clamp_to_border" };
	constexpr std::array<std::string_view, 2> k_filter_names = { "nearest", "linear" };
	// The only mipmap minification the texture desc knows: the loader maps it to GL_LINEAR_MIPMAP_LINEAR.
	constexpr std::string_view k_mipmap_min = "linear_mipmap_nearest";

	fs::path from_utf8(const std::string& text)
	{
		return fs::path(std::u8string(text.begin(), text.end()));
	}

	std::string lowered(std::string text)
	{
		std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return text;
	}

	std::string shown(std::string_view text)
	{
		return text_escape::escaped_text(text, 1024);
	}

	std::string size_text(std::uintmax_t bytes)
	{
		return std::format("{} bytes ({:.1f} MiB)", bytes, static_cast<double>(bytes) / (1024.0 * 1024.0));
	}

	bool is_png_extension(std::string_view extension)
	{
		return extension == ".png";
	}

	bool known_extension(std::string_view extension)
	{
		const auto& known = edt::image_importer::k_extensions;
		return std::ranges::find(known, extension) != std::end(known);
	}

	// The extension of a name in lower case, with its dot; empty when it has none.
	std::string extension_of(std::string_view name)
	{
		const std::size_t dot = name.rfind('.');
		return dot == std::string_view::npos || dot == 0 ? std::string{} : lowered(std::string(name.substr(dot)));
	}

	std::string joined(const std::string& folder, std::string_view name)
	{
		return folder.empty() ? std::string(name) : std::format("{}/{}", folder, name);
	}

	// Why `relative` is not free in any layer, as a refusal (code, text); nullopt when it is.
	std::optional<std::pair<const char*, std::string>> name_held(const std::vector<fs::path>& layers,
		const std::string& relative)
	{
		const std::optional<res::name_refusal> taken = res::name_taken(layers, relative);
		if (!taken) {
			return std::nullopt;
		}
		const std::string layer = edt::picker::layer_label(taken->layer, layers.size());
		const std::string tag = shown(res::tag::make(relative).view());
		if (!taken->error.empty()) {
			const std::string why = edt::image_importer::without_paths(taken->error, layers, {});
			return std::pair{ cmd::codes::invalid_args,
				std::format("the folder of {} cannot be read in the {} layer: {}", tag, layer, shown(why)) };
		}
		if (taken->meta_left) {
			return std::pair{ cmd::codes::needs_input,
				std::format("a '.resource' of a deleted {} is left in the {} layer: remove it first, or choose another "
					"name", tag, layer) };
		}
		const std::string holder = shown(fs_paths::utf8_of(taken->existing.filename()));
		return std::pair{ cmd::codes::needs_input,
			std::format("{} is taken in the {} layer (by '{}'): choose another name or folder", tag, layer, holder) };
	}

	std::string identity_problem(const edt::image_import_plan& plan)
	{
		std::error_code ec;
		const fs::file_status status = fs::symlink_status(plan.source, ec);
		if (ec || status.type() != fs::file_type::regular) return "the file is gone or is no plain file any more";
		const fs::path resolved = fs::canonical(plan.source, ec);
		if (ec || resolved != plan.source) return "the file is reached by another path now";
		if (fs::file_size(plan.source, ec) != plan.size || ec) return "the size of the file changed";
		if (fs::last_write_time(plan.source, ec) != plan.modified || ec) return "the file was modified";
		if (!edt::file_id_of(plan.source).same_as(plan.id)) return "the path reaches another file now";
		return {};
	}

	// The picture read once: no more than the size that was shown, and the file must still be that file.
	std::vector<std::byte> read_checked_source(const edt::image_import_plan& plan)
	{
		if (const std::string why = identity_problem(plan); !why.empty()) throw std::runtime_error(why);
		std::ifstream in(plan.source, std::ios::binary);
		if (!in) throw std::runtime_error("the file cannot be opened");
		std::vector<std::byte> bytes(static_cast<std::size_t>(plan.size));
		in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		if (static_cast<std::uintmax_t>(in.gcount()) != plan.size || in.peek() != std::char_traits<char>::eof()) {
			throw std::runtime_error("the size of the file changed while it was read");
		}
		if (const std::string why = identity_problem(plan); !why.empty()) {
			throw std::runtime_error("the file changed while it was read: " + why);
		}
		return bytes;
	}

	fs::path temp_leaf_for(const fs::path& leaf)
	{
		std::random_device random;
		fs::path out{ "." };
		out += leaf;
		out += std::format(".{:08x}.tmp", random());
		return out;
	}

	std::string write_new(platform::beneath::pinned_root& pinned, const fs::path& top, const fs::path& folder,
		const fs::path& leaf, std::span<const std::byte> data)
	{
		platform::beneath::write_request request;
		request.folder = folder;
		request.leaf = leaf;
		request.tmp_leaf = temp_leaf_for(leaf);
		request.data = data;
		request.replace = false;
		request.tmp_path = top / folder / request.tmp_leaf;
		return platform::beneath::write_beneath(pinned, request);
	}

	// In the order they are written.
	std::vector<std::string> relatives_of(const edt::image_import_plan& plan)
	{
		return { std::string(plan.meta.relative()), std::string(plan.texture_meta.relative()),
			std::string(plan.image.relative()), std::string(plan.texture.relative()) };
	}

	struct job_inputs
	{
		edt::image_import_plan plan;
		std::vector<fs::path> layers;
		fs::path top;
		std::function<bool(std::string_view)> fail_step;
		bool agent = false;
		edt::source_volume_ports volumes;
	};

	json::value run_import(const job_inputs& in, ds::task_context& ctx)
	{
		ctx.throw_if_stopped();
		if (in.agent) {
			const std::string why = edt::agent_source_path_problem(fs_paths::utf8_of(in.plan.source), in.volumes);
			if (!why.empty()) throw std::runtime_error(why);
		}
		ctx.set_progress(0.f, "Reading the picture");
		const std::vector<std::byte> bytes = read_checked_source(in.plan);
		ctx.set_progress(0.3f, "Decoding the picture");
		const std::string why = edt::image_importer::image_problem(bytes, extension_of(in.plan.name));
		if (!why.empty()) {
			throw std::runtime_error(why);
		}
		ctx.throw_if_stopped();
		// A name may have been taken since the question: the writes refuse it anyway, this says which.
		for (const std::string& relative : relatives_of(in.plan)) {
			if (const auto held = name_held(in.layers, relative)) {
				throw std::runtime_error(held->second + ": nothing was written");
			}
		}
		ctx.set_progress(0.8f, "Writing the files");
		const edt::image_import_written written = edt::image_importer::write_files(in.top, in.plan, bytes, in.fail_step);
		if (!written.ok()) throw std::runtime_error(written.error);
		return json::object{ { "image", std::string(written.image.view()) },
			{ "resource", std::string(written.meta.view()) }, { "texture", std::string(written.texture.view()) },
			{ "texture_resource", std::string(written.texture_meta.view()) }, { "guid", res::to_text(written.guid) },
			{ "texture_guid", res::to_text(written.texture_guid) } };
	}

	bool is_separator(char c, bool windows)
	{
		return c == '/' || (windows && c == '\\');
	}

	// The components of `path` between separators, empty ones left out.
	std::vector<std::string_view> components_of(std::string_view path, bool windows)
	{
		std::vector<std::string_view> out;
		std::size_t begin = 0;
		for (std::size_t at = 0; at <= path.size(); ++at) {
			if (at < path.size() && !is_separator(path[at], windows)) continue;
			if (at > begin) out.push_back(path.substr(begin, at - begin));
			begin = at + 1;
		}
		return out;
	}

	std::string windows_volume_problem(std::string_view path, const edt::source_volume_ports& ports)
	{
		const char letter = path.size() >= 3 ? path[0] : '\0';
		const bool lettered = (letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z');
		if (!lettered || path[1] != ':' || !is_separator(path[2], true)) {
			return "the source must start with a drive letter (C:/...): an agent may import only from a local disk";
		}
		if (path.find(':', 2) != std::string_view::npos) return "the source path holds a ':' after its drive";
		// Only the drive's root, built from the letter: the drive table answers, the path is not looked at.
		const unsigned type = ports.drive_type ? ports.drive_type(letter) : 0;
		if (type != edt::k_drive_fixed && type != edt::k_drive_removable) {
			return std::format("the drive {}: is no local fixed or removable disk (a network drive, a CD, a RAM disk or "
				"none): an agent may import only from a local disk", letter);
		}
		return {};
	}

	std::string posix_volume_problem(std::span<const std::string_view> parts, const edt::source_volume_ports& ports)
	{
		const std::optional<std::string> table = ports.mount_table ? ports.mount_table() : std::nullopt;
		if (!table) return "the mount table cannot be read: an agent's source cannot be told to be on a local disk";
		std::string normal;
		for (const std::string_view part : parts) normal += "/" + std::string(part);
		if (normal.empty()) normal = "/";
		// The longest mount point on component boundaries holds the path; a later line over the same point wins.
		const edt::agent_log_mount* holder = nullptr;
		const std::vector<edt::agent_log_mount> mounts = edt::agent_log_mounts(*table);
		for (const edt::agent_log_mount& mount : mounts) {
			const std::string& point = mount.point;
			const bool under = point == "/" || normal == point
				|| (normal.size() > point.size() && normal.starts_with(point) && normal[point.size()] == '/');
			if (under && (holder == nullptr || point.size() >= holder->point.size())) holder = &mount;
		}
		if (holder == nullptr) return "no mount of the mount table holds the source";
		if (holder->remote) {
			return "the source is on a network or automount file system: an agent may import only from a local disk";
		}
		return {};
	}

	void replace_all(std::string& text, std::string_view from, std::string_view to)
	{
		if (from.empty()) return;
		for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
			text.replace(at, from.size(), to);
		}
	}

	// The spellings of `path` a message may hold, longest first, without a trailing separator.
	std::vector<std::string> spellings_of(const fs::path& path)
	{
		std::vector<std::string> bases{ fs_paths::utf8_of(path), fs_paths::generic_utf8_of(path) };
		std::error_code ec;
		const fs::path resolved = fs::weakly_canonical(path, ec);
		if (!ec) {
			bases.push_back(fs_paths::utf8_of(resolved));
			bases.push_back(fs_paths::generic_utf8_of(resolved));
		}
		std::vector<std::string> out;
		for (std::string base : bases) {
			while (base.size() > 1 && (base.back() == '/' || base.back() == '\\')) base.pop_back();
			if (base.empty()) continue;
			out.push_back("\\\\?\\" + base);
			out.push_back(std::move(base));
		}
		std::ranges::sort(out, [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
		return out;
	}
}

#if defined(_WIN32)
static_assert(static_cast<unsigned>(DRIVE_FIXED) == edt::k_drive_fixed);
static_assert(static_cast<unsigned>(DRIVE_REMOVABLE) == edt::k_drive_removable);
#endif

unsigned edt::system_drive_type(char letter)
{
#if defined(_WIN32)
	const wchar_t root[4]{ static_cast<wchar_t>(letter), L':', L'\\', L'\0' };
	return static_cast<unsigned>(GetDriveTypeW(root));
#else
	static_cast<void>(letter);
	return 0;
#endif
}

std::optional<std::string> edt::system_mount_table()
{
#if defined(_WIN32)
	return std::nullopt;
#else
	std::ifstream in("/proc/self/mounts", std::ios::binary);
	if (!in) return std::nullopt;
	std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	if (text.empty()) return std::nullopt;
	return text;
#endif
}

std::string edt::agent_source_path_problem(std::string_view path, const source_volume_ports& ports)
{
	if (path.empty()) return "the source path is empty";
	if (path.starts_with("\\??\\") || (path.size() >= 2 && is_separator(path[0], true) && is_separator(path[1], true))) {
		return "the source is a network share or a device path: an agent may import only from a local disk";
	}
	const std::vector<std::string_view> parts = components_of(path, ports.windows);
	if (std::ranges::any_of(parts, [](std::string_view part) { return part == "." || part == ".."; })) {
		return "the source path holds a '.' or '..' component: name the file by its plain absolute path";
	}
	if (ports.windows) return windows_volume_problem(path, ports);
	if (path.front() != '/') return "the source must be an absolute path to a picture";
	return posix_volume_problem(parts, ports);
}

std::string edt::agent_source_path_problem(std::string_view path)
{
	return agent_source_path_problem(path, source_volume_ports{});
}

std::string edt::image_importer::without_paths(std::string text, const std::vector<fs::path>& roots,
	const fs::path& source)
{
	if (!source.empty()) {
		const std::string name = fs_paths::utf8_of(source.filename());
		for (const std::string& spelling : spellings_of(source)) replace_all(text, spelling, name);
	}
	for (const fs::path& root : roots) {
		if (root.empty()) continue;
		for (const std::string& spelling : spellings_of(root)) {
			replace_all(text, spelling + "/", "res://");
			replace_all(text, spelling + "\\", "res://");
			replace_all(text, spelling, "res://");
		}
	}
	return text;
}

std::string_view edt::image_wrap_name(image_wrap wrap)
{
	return k_wrap_names[static_cast<std::size_t>(wrap)];
}

std::string_view edt::image_filter_name(image_filter filter)
{
	return k_filter_names[static_cast<std::size_t>(filter)];
}

edt::image_settings_parse edt::parse_image_settings(const json::value& value)
{
	image_settings_parse out;
	if (value.is_null()) {
		return out;
	}
	if (!value.is_object()) {
		out.error = "settings must be an object";
		return out;
	}
	const auto name_of = [](const json::value& text, auto names) -> std::optional<std::size_t> {
		if (!text.is_string()) return std::nullopt;
		const auto found = std::ranges::find(names, std::string_view(text.as_string()));
		if (found == names.end()) return std::nullopt;
		return static_cast<std::size_t>(found - names.begin());
	};
	const auto listed = [](auto names) {
		std::string text;
		for (const std::string_view name : names) text += (text.empty() ? "" : ", ") + std::string(name);
		return text;
	};
	for (const json::key_value_pair& entry : value.as_object()) {
		const std::string_view key = entry.key();
		const json::value& item = entry.value();
		if (key == "version") {
			const bool one = (item.is_int64() && item.as_int64() == k_image_settings_version)
				|| (item.is_uint64() && item.as_uint64() == static_cast<std::uint64_t>(k_image_settings_version));
			if (!one) {
				out.error = std::format("settings.version must be {}", k_image_settings_version);
				return out;
			}
		} else if (key == "wrap") {
			const std::optional<std::size_t> index = name_of(item, k_wrap_names);
			if (!index) {
				out.error = "settings.wrap must be one of " + listed(k_wrap_names);
				return out;
			}
			out.settings.wrap = static_cast<image_wrap>(*index);
		} else if (key == "filter") {
			const std::optional<std::size_t> index = name_of(item, k_filter_names);
			if (!index) {
				out.error = "settings.filter must be one of " + listed(k_filter_names);
				return out;
			}
			out.settings.filter = static_cast<image_filter>(*index);
		} else if (key == "mips") {
			if (!item.is_bool()) {
				out.error = "settings.mips must be true or false";
				return out;
			}
			out.settings.mips = item.as_bool();
		} else {
			out.error = std::format("settings.{} is not a setting of an image (version, wrap, filter, mips)",
				shown(key));
			return out;
		}
	}
	return out;
}

json::object edt::image_settings_json(const image_import_settings& settings)
{
	return json::object{ { "version", k_image_settings_version }, { "wrap", image_wrap_name(settings.wrap) },
		{ "filter", image_filter_name(settings.filter) }, { "mips", settings.mips } };
}

json::object edt::image_texture_desc_json(const res::tag& image, std::string_view name,
	const image_import_settings& settings)
{
	const std::string_view filter = image_filter_name(settings.filter);
	json::object data{ { "mip_levels", settings.mips ? k_image_full_mip_chain : std::int64_t{ 1 } } };
	json::object header{ { "wrap", image_wrap_name(settings.wrap) }, { "mag", filter },
		{ "min", settings.mips ? k_mipmap_min : filter }, { "data", std::move(data) } };
	return json::object{ { "__type", "texture_2d_desc" }, { "__parent", "res://base_texture.desc" }, { "name", name },
		{ "data", std::string(image.view()) }, { "header", std::move(header) } };
}

bool edt::image_import_plan::same_as(const image_import_plan& other) const
{
	return folder == other.folder && name == other.name && image == other.image && meta == other.meta
		&& texture == other.texture && texture_meta == other.texture_meta && settings == other.settings;
}

edt::image_importer::image_importer(job_service& jobs, image_import_options options)
	: m_jobs(jobs), m_options(std::move(options))
{
}

edt::image_importer::~image_importer() = default;

bool edt::image_importer::running() const
{
	return m_job != 0 && m_jobs.is_running(m_job);
}

std::chrono::steady_clock::time_point edt::image_importer::now() const
{
	return m_options.now ? m_options.now() : std::chrono::steady_clock::now();
}

edt::source_volume_ports edt::image_importer::volumes() const
{
	source_volume_ports ports;
	ports.drive_type = m_options.drive_type;
	ports.mount_table = m_options.mount_table;
	return ports;
}

void edt::image_importer::remember(std::string key, std::string session, image_import_plan plan)
{
	const auto at = now();
	std::erase_if(m_shown, [&](const shown_plan& item) { return item.key == key || at - item.at > k_shown_lifetime; });
	const auto of_session = [&](const shown_plan& item) { return item.session == session; };
	if (static_cast<std::size_t>(std::ranges::count_if(m_shown, of_session)) >= k_max_shown) {
		m_shown.erase(std::ranges::find_if(m_shown, of_session));
	}
	m_shown.push_back({ std::move(key), std::move(session), std::move(plan), at });
}

std::optional<edt::image_import_plan> edt::image_importer::take(const std::string& key)
{
	const auto at = now();
	std::erase_if(m_shown, [&](const shown_plan& item) { return at - item.at > k_shown_lifetime; });
	const auto found = std::ranges::find_if(m_shown, [&](const shown_plan& item) { return item.key == key; });
	if (found == m_shown.end()) return std::nullopt;
	image_import_plan plan = std::move(found->plan);
	m_shown.erase(found);
	return plan;
}

namespace
{
	std::string call_key(const edt::image_import_request& request, const cmd::origin& caller)
	{
		const std::string folder = request.folder ? "=" + *request.folder : std::string{ "-" };
		const std::string settings = json::serialize(request.settings);
		return std::format("{}:{}|{}:{}|{}:{}|{}:{}|{}:{}", caller.session.size(), caller.session, request.path.size(),
			request.path, folder.size(), folder, request.name.size(), request.name, settings.size(), settings);
	}
}

edt::image_import_plan edt::image_importer::resolve(const image_import_request& request, bool person) const
{
	image_import_plan plan;
	const auto refuse = [&plan](const char* code, std::string why) {
		plan.code = code;
		plan.error = std::move(why);
		return plan;
	};
	if (!m_options.project_open || !m_options.project_open()) return refuse(cmd::codes::disabled, "no project is open");
	if (request.path.empty()) return refuse(cmd::codes::invalid_args, "the source path is empty");
	// A person may pick a file on a share in the native dialog; an agent's source stays on a local disk.
	if (!person) {
		if (std::string why = agent_source_path_problem(request.path, volumes()); !why.empty()) {
			return refuse(cmd::codes::invalid_args, std::move(why));
		}
	}
	const fs::path given = from_utf8(request.path);
	if (!given.is_absolute()) {
		return refuse(cmd::codes::invalid_args, "the source must be an absolute path to a picture");
	}
	std::error_code ec;
	const fs::file_status status = fs::symlink_status(given, ec);
	if (ec || status.type() == fs::file_type::not_found) {
		return refuse(cmd::codes::not_found, "no such file: " + shown(request.path));
	}
	if (status.type() == fs::file_type::symlink) {
		return refuse(cmd::codes::invalid_args, "the source is a link: name the file itself");
	}
	if (status.type() != fs::file_type::regular) {
		return refuse(cmd::codes::invalid_args, "the source is not a regular file");
	}
	if (!known_extension(extension_of(fs_paths::utf8_of(given.filename())))) {
		return refuse(cmd::codes::invalid_args, "the source is not a picture this import takes (.png, .jpg or .jpeg)");
	}
	const fs::path resolved = fs::canonical(given, ec);
	if (ec) return refuse(cmd::codes::invalid_args, "the source path cannot be resolved");
	if (!person) {
		if (std::string why = agent_source_path_problem(fs_paths::utf8_of(resolved), volumes()); !why.empty()) {
			return refuse(cmd::codes::invalid_args, "the source resolves to a path an agent may not read: " + why);
		}
	}
	if (fs::symlink_status(resolved, ec).type() != fs::file_type::regular) {
		return refuse(cmd::codes::invalid_args, "the source is not a regular file");
	}
	plan.source = resolved;
	plan.on_disk = true;
	plan.size = fs::file_size(resolved, ec);
	if (ec) return refuse(cmd::codes::invalid_args, "the size of the source cannot be read");
	if (plan.size == 0) return refuse(cmd::codes::invalid_args, "the source file is empty");
	const std::uintmax_t limit = person ? m_options.max_person_bytes : m_options.max_agent_bytes;
	if (plan.size > limit) {
		return refuse(cmd::codes::invalid_args, std::format("the picture of {} exceeds {} for {} import ({})",
			size_text(plan.size), limit_text(limit), person ? "a person's" : "an agent's", k_step_source));
	}
	plan.modified = fs::last_write_time(resolved, ec);
	if (ec) return refuse(cmd::codes::invalid_args, "the time of the source cannot be read");
	plan.id = file_id_of(resolved);
	if (!plan.id.valid) return refuse(cmd::codes::invalid_args, "the identity of the source on the disk cannot be read");
	plan_targets(request, fs_paths::utf8_of(resolved.filename()), plan);
	return plan;
}

edt::image_import_plan edt::image_importer::text_plan(const image_import_request& request) const
{
	image_import_plan plan;
	const auto refuse = [&plan](const char* code, std::string why) {
		plan.code = code;
		plan.error = std::move(why);
		return plan;
	};
	if (!m_options.project_open || !m_options.project_open()) return refuse(cmd::codes::disabled, "no project is open");
	if (std::string why = agent_source_path_problem(request.path, volumes()); !why.empty()) {
		return refuse(cmd::codes::invalid_args, std::move(why));
	}
	const fs::path given = from_utf8(request.path);
	if (!given.is_absolute()) {
		return refuse(cmd::codes::invalid_args, "the source must be an absolute path to a picture");
	}
	const std::string source_name = fs_paths::utf8_of(given.filename());
	if (!known_extension(extension_of(source_name))) {
		return refuse(cmd::codes::invalid_args, "the source is not a picture this import takes (.png, .jpg or .jpeg)");
	}
	plan.source = given;
	plan_targets(request, source_name, plan);
	return plan;
}

void edt::image_importer::plan_targets(const image_import_request& request, std::string_view source_name,
	image_import_plan& plan) const
{
	const auto refuse = [&plan](const char* code, std::string why) {
		plan.code = code;
		plan.error = std::move(why);
	};
	const image_settings_parse parsed = parse_image_settings(request.settings);
	if (!parsed.ok()) return refuse(cmd::codes::invalid_args, parsed.error);
	plan.settings = parsed.settings;

	const std::string source_extension = extension_of(source_name);
	std::string name = request.name.empty() ? std::string(source_name) : request.name;
	if (!request.name.empty() && !known_extension(extension_of(name))) name += source_extension;
	if (is_png_extension(extension_of(name)) != is_png_extension(source_extension)) {
		return refuse(cmd::codes::invalid_args,
			std::format("the name '{}' has the extension of another format than the source", shown(name)));
	}
	if (const agent_level_path checked = check_agent_name(name); !checked.ok()) {
		return refuse(cmd::codes::invalid_args, "the name: " + checked.error);
	}
	if (res::is_skipped_asset_name(name, false)) {
		return refuse(cmd::codes::invalid_args,
			"the asset list hides a name like this (a leading dot, a temporary file)");
	}
	plan.name = name;
	if (!request.folder) {
		plan.folder = std::string(k_image_default_folder);
	} else if (!request.folder->empty()) {
		const agent_level_path folder = check_agent_folder(*request.folder);
		if (!folder.ok()) return refuse(cmd::codes::invalid_args, "the folder: " + folder.error);
		plan.folder = folder.path;
	}
	const std::string relative = joined(plan.folder, plan.name);
	const std::string texture = relative + std::string(k_image_texture_suffix);
	const agent_level_path whole = check_agent_level_path(texture);
	if (!whole.ok()) {
		return refuse(cmd::codes::invalid_args, "the path: " + whole.error);
	}
	plan.image = res::tag::make(relative);
	plan.meta = res::tag::make(relative + std::string(res::asset_meta_suffix));
	plan.texture = res::tag::make(texture);
	plan.texture_meta = res::tag::make(texture + std::string(res::asset_meta_suffix));

	// The project's own layers: looking at them is no look at the source.
	const std::vector<fs::path> layers =
		m_options.resource_layers ? m_options.resource_layers() : std::vector<fs::path>{};
	for (const std::string& target : relatives_of(plan)) {
		if (auto held = name_held(layers, target)) return refuse(held->first, std::move(held->second));
	}
	if (m_options.blocker) {
		if (std::optional<std::string> why = m_options.blocker()) return refuse(cmd::codes::busy, *why);
	}
	if (running()) return refuse(cmd::codes::busy, "an image import is already running");
}

std::optional<std::string> edt::image_importer::describe(const image_import_request& request, const cmd::origin& caller)
{
	const bool person = caller.kind == cmd::origin_kind::ui;
	image_import_plan plan = person ? resolve(request, true) : text_plan(request);
	if (!plan.ok()) return "This request will be refused: " + text_escape::escaped_message(plan.error, 1024);
	const std::string size = plan.on_disk ? size_text(plan.size) : std::string{ "(its size is read once approved)" };
	std::string text = std::format("Import the picture\n  {}\n  {}\ninto the project as\n  {}  (the bytes as they are)\n"
		"  {}  (importer texture: wrap {}, filter {}, mips {})\n  {}\n  {}  (the texture's ID)\nNothing is overwritten.",
		shown(fs_paths::generic_utf8_of(plan.source)), size, shown(plan.image.view()), shown(plan.meta.view()),
		image_wrap_name(plan.settings.wrap), image_filter_name(plan.settings.filter), plan.settings.mips ? "on" : "off",
		shown(plan.texture.view()), shown(plan.texture_meta.view()));
	remember(call_key(request, caller), caller.session, std::move(plan));
	return text;
}

edt::image_importer::started edt::image_importer::start(const image_import_request& request, const cmd::origin& caller)
{
	// A person's own call is the approval. Anyone else needs the plan describe remembered for exactly this call: the
	// registry invokes an agent's call the policy asks about only after a person approved its ticket, and describes a
	// call the policy allows right before it runs (describe_before_allowed_invoke). Without that plan nothing is looked
	// at; with it, the source is looked at on the disk here for the first time, after the text rule once more.
	const bool person = caller.kind == cmd::origin_kind::ui;
	std::optional<image_import_plan> shown_plan;
	if (!person) {
		shown_plan = take(call_key(request, caller));
		if (!shown_plan) {
			return { 0, cmd::codes::denied,
				"this import was not shown to a person or described before it ran, or that is too old: ask again" };
		}
		if (std::string why = agent_source_path_problem(request.path, volumes()); !why.empty()) {
			return { 0, cmd::codes::invalid_args, std::move(why) };
		}
	}
	image_import_plan plan = resolve(request, person);
	if (!plan.ok()) return { 0, plan.code, plan.error };
	if (shown_plan && !shown_plan->same_as(plan)) {
		return { 0, cmd::codes::denied, "the files to write are not the ones a person was shown: ask again" };
	}
	job_inputs inputs;
	inputs.layers = m_options.resource_layers ? m_options.resource_layers() : std::vector<fs::path>{};
	inputs.top = m_options.resources_path ? m_options.resources_path() : fs::path{};
	inputs.fail_step = m_options.fail_step;
	inputs.agent = !person;
	inputs.volumes = volumes();
	inputs.plan = std::move(plan);
	if (inputs.top.empty()) return { 0, cmd::codes::disabled, "no project is open" };
	std::vector<res::tag> tags{ inputs.plan.meta, inputs.plan.texture_meta, inputs.plan.image, inputs.plan.texture };
	std::string label = "Import " + inputs.plan.name;
	m_job = m_jobs.start_with_result(ds::lane::compute, { std::move(label), "import" }, m_lifetime,
		[inputs = std::move(inputs)](ds::task_context& ctx) -> json::value { return run_import(inputs, ctx); },
		[this, tags = std::move(tags)](ds::task_result<json::value>& result) {
			if (result.ok() && m_options.written) m_options.written(tags);
		});
	return { m_job, {}, {} };
}

std::string edt::image_importer::image_problem(std::span<const std::byte> bytes, std::string_view extension)
{
	static constexpr std::array<unsigned char, 8> png_signature = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
	static constexpr std::array<unsigned char, 3> jpeg_signature = { 0xFF, 0xD8, 0xFF };
	const auto starts_with = [&bytes](const auto& signature) {
		return bytes.size() >= signature.size() && std::equal(signature.begin(), signature.end(), bytes.begin(),
			[](unsigned char expected, std::byte got) { return static_cast<unsigned char>(got) == expected; });
	};
	const bool png = is_png_extension(extension);
	if (png ? !starts_with(png_signature) : !starts_with(jpeg_signature)) {
		return std::format("the file is not a {} picture", png ? "PNG" : "JPEG");
	}
	try {
		const std::vector<std::byte> copy(bytes.begin(), bytes.end());
		const res::tag tag{ std::format("memory://image_import/picture{}", extension) };
		if (!res::pct_adapter{}(tag, copy)) return "the picture cannot be decoded";
	} catch (const std::exception& failed) {
		return failed.what();
	}
	return {};
}

edt::image_import_written edt::image_importer::write_files(const fs::path& top, const image_import_plan& plan,
	std::span<const std::byte> bytes, const std::function<bool(std::string_view)>& fail_step)
{
	image_import_written out;
	out.image = plan.image;
	out.meta = plan.meta;
	out.texture = plan.texture;
	out.texture_meta = plan.texture_meta;
	out.guid = res::generate_asset_id();
	out.texture_guid = res::generate_asset_id();
	if (top.empty()) {
		out.error = "no project is open";
		return out;
	}

	res::asset_meta meta;
	meta.guid = out.guid;
	meta.importer = std::string(k_image_importer);
	meta.settings = image_settings_json(plan.settings);
	const std::string meta_text = res::write_asset_meta(meta);
	// The texture desc's own ID: an output of the import, not a recipe, so no importer and no settings.
	res::asset_meta texture_meta;
	texture_meta.guid = out.texture_guid;
	const std::string texture_meta_text = res::write_asset_meta(texture_meta);
	const std::vector<std::byte> texture_bytes =
		res::pretty_json_bytes(image_texture_desc_json(plan.image, plan.name, plan.settings));

	const std::vector<fs::path> roots{ top };
	const auto private_text = [&](std::string text) {
		return shown(without_paths(std::move(text), roots, plan.source));
	};
	const fs::path folder = fs_paths::path_from_utf8(plan.folder);
	platform::beneath::pinned_root pinned{ top };
	if (!plan.folder.empty()) {
		if (std::string error = platform::beneath::make_folders_beneath(pinned, folder); !error.empty()) {
			out.error = std::format("the folder res://{} cannot be made: {}", shown(plan.folder), private_text(error));
			return out;
		}
	}
	struct step
	{
		std::string_view name;
		const res::tag* tag;
		std::span<const std::byte> data;
	};
	// The sidecars first: the asset database, seeing the picture or the desc, finds its ID and makes none.
	const std::array<step, 4> steps = { step{ "meta", &plan.meta, std::as_bytes(std::span(meta_text)) },
		step{ "texture_meta", &plan.texture_meta, std::as_bytes(std::span(texture_meta_text)) },
		step{ "image", &plan.image, bytes }, step{ "texture", &plan.texture, texture_bytes } };
	std::vector<fs::path> written;
	for (const step& item : steps) {
		const fs::path leaf = fs_paths::path_from_utf8(std::string(item.tag->relative())).filename();
		std::string error = fail_step && fail_step(item.name) ? std::string{ "a test failed this write" }
			: write_new(pinned, top, folder, leaf, item.data);
		if (!error.empty()) {
			out.error = std::format("{} was not written: {}", shown(item.tag->view()), private_text(error));
			for (auto made = written.rbegin(); made != written.rend(); ++made) {
				if (std::string left = platform::beneath::remove_beneath(pinned, folder, *made, true); !left.empty()) {
					out.error += std::format("; {} could not be taken back: {}", shown(fs_paths::utf8_of(*made)),
						private_text(left));
				}
			}
			return out;
		}
		written.push_back(leaf);
	}
	return out;
}

void edt::image_import_form::open(std::string folder, fs::path source)
{
	m_folder = std::move(folder);
	m_source = std::move(source);
	m_settings = {};
	m_name = fs_paths::utf8_of(m_source.filename());
	m_error.clear();
	check();
}

void edt::image_import_form::set_name(std::string name)
{
	if (name == m_name) {
		return;
	}
	m_name = std::move(name);
	m_error.clear();
	check();
}

edt::image_import_request edt::image_import_form::request() const
{
	image_import_request out;
	out.path = fs_paths::utf8_of(m_source);
	out.folder = m_folder;
	out.name = m_name;
	out.settings = image_settings_json(m_settings);
	return out;
}

void edt::image_import_form::check()
{
	m_plan = m_importer.resolve(request(), true);
	m_problem = m_plan.error;
}

std::vector<std::string> edt::image_import_form::final_names() const
{
	if (!m_plan.image.is_valid()) {
		return {};
	}
	return { std::string(m_plan.image.view()), std::string(m_plan.meta.view()), std::string(m_plan.texture.view()),
		std::string(m_plan.texture_meta.view()) };
}

std::optional<std::uint64_t> edt::image_import_form::submit()
{
	if (m_source.empty()) {
		return std::nullopt;
	}
	const image_importer::started started = m_importer.start(request(), cmd::origin::ui());
	if (!started.ok()) {
		m_error = started.error;
		check();
		return std::nullopt;
	}
	m_error.clear();
	return started.job;
}
