#include "edt_image_import.h"
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
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <cctype>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace json = boost::json;
namespace fs = std::filesystem;

namespace
{
	constexpr std::array<std::string_view, 4> k_wrap_names = {
		"clamp_to_edge", "repeat", "repeat_mirror", "clamp_to_border" };
	constexpr std::array<std::string_view, 2> k_filter_names = { "nearest", "linear" };
	// The only mipmap minification the texture desc knows: the driver filters it trilinearly.
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

	// The texture desc beside a picture: one rule for the import and the re-import.
	std::string texture_relative_of(const std::string& picture)
	{
		return picture + std::string(edt::k_image_texture_suffix);
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
			const std::string why = edt::without_paths(taken->error, layers, {});
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

	// `agent` (an agent's import): no link on the way to the file either, looked at before anything follows one.
	std::string identity_problem(const edt::image_import_plan& plan, const edt::source_volume_ports* agent)
	{
		// An agent's file is judged by the handle a read opens, never by the path.
		if (agent) return edt::agent_file_changed(plan.source, plan.stamp, *agent);
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
	std::vector<std::byte> read_checked_source(const edt::image_import_plan& plan, const edt::source_volume_ports* agent)
	{
		if (const std::string why = identity_problem(plan, agent); !why.empty()) throw std::runtime_error(why);
		if (agent) {
			// From the handle that was checked: nothing is opened again by the path once the walk is done (#1412).
			edt::agent_file_read read = edt::read_agent_file(plan.source, plan.size, *agent);
			if (!read.problem.empty()) throw std::runtime_error(read.problem);
			if (!plan.stamp || read.bytes.size() != plan.size || !edt::same_file_stamp(*read.stamp, *plan.stamp)) {
				throw std::runtime_error("the file changed since it was shown: another file, or other content");
			}
			return std::move(read.bytes);
		}
		std::ifstream in(plan.source, std::ios::binary);
		if (!in) throw std::runtime_error("the file cannot be opened");
		std::vector<std::byte> bytes(static_cast<std::size_t>(plan.size));
		in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		if (static_cast<std::uintmax_t>(in.gcount()) != plan.size || in.peek() != std::char_traits<char>::eof()) {
			throw std::runtime_error("the size of the file changed while it was read");
		}
		if (const std::string why = identity_problem(plan, agent); !why.empty()) {
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
		const std::vector<std::byte> bytes = read_checked_source(in.plan, in.agent ? &in.volumes : nullptr);
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
}

std::string_view edt::image_wrap_name(image_wrap wrap)
{
	return k_wrap_names[static_cast<std::size_t>(wrap)];
}

std::string_view edt::image_filter_name(image_filter filter)
{
	return k_filter_names[static_cast<std::size_t>(filter)];
}

edt::image_settings_parse edt::parse_image_settings(const json::value& value, const image_import_settings& base)
{
	image_settings_parse out;
	out.settings = base;
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

void edt::image_importer::remember(std::string key, std::string session, std::string ticket, image_import_plan plan)
{
	const auto at = now();
	std::erase_if(m_shown, [&](const shown_plan& item) { return item.key == key || at - item.at > k_shown_lifetime; });
	const auto of_session = [&](const shown_plan& item) { return item.session == session; };
	if (static_cast<std::size_t>(std::ranges::count_if(m_shown, of_session)) >= k_max_shown) {
		m_shown.erase(std::ranges::find_if(m_shown, of_session));
	}
	m_shown.push_back({ std::move(key), std::move(session), std::move(ticket), std::move(plan), at });
}

std::optional<edt::image_import_plan> edt::image_importer::take(const std::string& key, std::string_view ticket)
{
	const auto at = now();
	std::erase_if(m_shown, [&](const shown_plan& item) { return at - item.at > k_shown_lifetime; });
	const auto found = std::ranges::find_if(m_shown, [&](const shown_plan& item) { return item.key == key; });
	if (found == m_shown.end()) return std::nullopt;
	const bool same_ticket = found->ticket == ticket;
	image_import_plan plan = std::move(found->plan);
	m_shown.erase(found);
	if (!same_ticket) return std::nullopt;
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
	// Nothing on the way to an agent's source is followed: a link is refused where it is, before any stat goes through.
	if (!person) {
		std::string why;
		const fs::path spelled = agent_source_spelled(given, volumes(), why);
		if (!why.empty()) return refuse(cmd::codes::invalid_args, std::move(why));
		return resolve_for_agent(request, spelled, plan);
	}
	resolve_person_source(request.path, plan);
	if (!plan.ok()) return plan;
	plan_targets(request, fs_paths::utf8_of(plan.source.filename()), plan);
	return plan;
}

void edt::image_importer::resolve_person_source(const std::string& path, image_import_plan& plan) const
{
	const auto refuse = [&plan](const char* code, std::string why) {
		plan.code = code;
		plan.error = std::move(why);
	};
	const fs::path given = from_utf8(path);
	std::error_code ec;
	const fs::file_status status = fs::symlink_status(given, ec);
	if (ec || status.type() == fs::file_type::not_found) {
		return refuse(cmd::codes::not_found, "no such file: " + shown(path));
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
	if (fs::symlink_status(resolved, ec).type() != fs::file_type::regular) {
		return refuse(cmd::codes::invalid_args, "the source is not a regular file");
	}
	plan.source = resolved;
	plan.on_disk = true;
	plan.size = fs::file_size(resolved, ec);
	if (ec) return refuse(cmd::codes::invalid_args, "the size of the source cannot be read");
	if (plan.size == 0) return refuse(cmd::codes::invalid_args, "the source file is empty");
	const std::uintmax_t limit = m_options.max_person_bytes;
	if (plan.size > limit) {
		return refuse(cmd::codes::invalid_args, std::format("the picture of {} exceeds {} for a person's import ({})",
			size_text(plan.size), limit_text(limit), k_step_source));
	}
	plan.modified = fs::last_write_time(resolved, ec);
	if (ec) return refuse(cmd::codes::invalid_args, "the time of the source cannot be read");
	plan.id = file_id_of(resolved);
	if (!plan.id.valid) return refuse(cmd::codes::invalid_args, "the identity of the source on the disk cannot be read");
}

edt::image_import_plan edt::image_importer::resolve_for_agent(const image_import_request& request,
	const fs::path& given, image_import_plan& plan) const
{
	const auto refuse = [&plan](const char* code, std::string why) {
		plan.code = code;
		plan.error = std::move(why);
		return plan;
	};
	if (!known_extension(extension_of(fs_paths::utf8_of(given.filename())))) {
		return refuse(cmd::codes::invalid_args, "the source is not a picture this import takes (.png, .jpg or .jpeg)");
	}
	// Judged by the handle the walk opens (#1412): nothing is looked at by the path, which a swap could redirect.
	const fs::path source = given.lexically_normal();
	const agent_file_read stamped = read_agent_file(source, UINTMAX_MAX, volumes(), true);
	if (stamped.missing) return refuse(cmd::codes::not_found, "no such file: " + shown(request.path));
	if (!stamped.problem.empty()) return refuse(cmd::codes::invalid_args, stamped.problem);
	plan.source = source;
	plan.on_disk = true;
	plan.size = stamped.stamp->size;
	plan.stamp = stamped.stamp;
	if (plan.size == 0) return refuse(cmd::codes::invalid_args, "the source file is empty");
	if (plan.size > m_options.max_agent_bytes) {
		return refuse(cmd::codes::invalid_args, std::format("the picture of {} exceeds {} for an agent's import ({})",
			size_text(plan.size), limit_text(m_options.max_agent_bytes), k_step_source));
	}
	plan_targets(request, fs_paths::utf8_of(source.filename()), plan);
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
	const std::string texture = texture_relative_of(relative);
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

std::optional<std::string> edt::image_importer::describe(const image_import_request& request, const cmd::origin& caller,
	std::string_view ticket)
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
	remember(call_key(request, caller), caller.session, std::string(ticket), std::move(plan));
	return text;
}

edt::image_importer::started edt::image_importer::start(const image_import_request& request, const cmd::origin& caller,
	std::string_view ticket)
{
	// A person's own call is the approval. Anyone else needs the plan describe remembered for exactly this call: the
	// registry invokes an agent's call the policy asks about only after a person approved its ticket, and describes a
	// call the policy allows right before it runs (describe_before_allowed_invoke). Without that plan nothing is looked
	// at; with it, the source is looked at on the disk here for the first time, after the text rule once more. The
	// registry describes an asked call with its ticket's id and hands the command that id only once a person approved
	// it; an allowed call is described and run without one: either way the two must match.
	const bool person = caller.kind == cmd::origin_kind::ui;
	std::optional<image_import_plan> shown_plan;
	if (!person) {
		shown_plan = take(call_key(request, caller), ticket);
		if (!shown_plan) {
			return { 0, cmd::codes::denied, "this import was not shown to a person or described before it ran, "
				"its approval was given for another question, or that is too old: ask again" };
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

namespace
{
	// A sidecar is a few hundred bytes, a texture desc a few more; a bigger file at the name is none the import wrote.
	constexpr std::uint64_t k_max_meta_bytes = 64 * 1024;
	constexpr std::uint64_t k_max_texture_desc_bytes = 1024 * 1024;

	struct below_read
	{
		platform::beneath::read_result result{};
		std::vector<std::byte> bytes{};
	};

	below_read read_below(const fs::path& top, const res::tag& tag, std::uint64_t max_bytes, bool stamp_only = false)
	{
		const fs::path path = fs_paths::path_from_utf8(std::string(tag.relative()));
		platform::beneath::read_request request;
		request.root = top;
		request.folder = path.parent_path();
		request.leaf = path.filename();
		request.max_bytes = max_bytes;
		request.stamp_only = stamp_only;
		below_read out;
		out.result = platform::beneath::read_beneath(request, out.bytes);
		return out;
	}

	std::string replace_below(const fs::path& top, const res::tag& tag, std::span<const std::byte> data)
	{
		const fs::path path = fs_paths::path_from_utf8(std::string(tag.relative()));
		platform::beneath::pinned_root pinned{ top };
		platform::beneath::write_request request;
		request.folder = path.parent_path();
		request.leaf = path.filename();
		request.tmp_leaf = temp_leaf_for(request.leaf);
		request.data = data;
		request.replace = true;
		request.tmp_path = top / request.folder / request.tmp_leaf;
		return platform::beneath::write_beneath(pinned, request);
	}

	std::string text_of(const std::vector<std::byte>& bytes)
	{
		return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
	}

	// Replaces the file only while it holds `before`, checked once the temp file is complete: the last point
	// write_beneath hands back before its flush and rename. A write during the flush is not seen (accepted, #1459).
	std::string replace_if_unchanged(const fs::path& top, const res::tag& tag, std::span<const std::byte> before,
		std::span<const std::byte> data, bool& changed)
	{
		const fs::path path = fs_paths::path_from_utf8(std::string(tag.relative()));
		platform::beneath::pinned_root pinned{ top };
		platform::beneath::write_request request;
		request.folder = path.parent_path();
		request.leaf = path.filename();
		request.tmp_leaf = temp_leaf_for(request.leaf);
		request.replace = true;
		request.tmp_path = top / request.folder / request.tmp_leaf;
		changed = false;
		const platform::beneath::chunk_source source = [&](const platform::beneath::chunk_sink& sink) {
			if (!sink(data)) return false;
			const below_read now = read_below(top, tag, std::max<std::uint64_t>(before.size(), 1));
			changed = now.result.status != platform::beneath::read_status::ok || !std::ranges::equal(now.bytes, before);
			return !changed;
		};
		request.stream = &source;
		return platform::beneath::write_beneath(pinned, request);
	}

	struct reimport_inputs
	{
		edt::image_reimport_plan plan;
		fs::path top;
		std::uint64_t max_image_bytes = 0;
		std::function<bool(std::string_view)> fail_step;
	};

	// What a failed re-import could not put back: still replaced, so the cache must learn of it. Written by the job,
	// read on the main thread once it ended.
	struct reimport_leftovers
	{
		std::mutex lock;
		std::vector<res::tag> stays_changed;
	};

	json::value run_reimport(const reimport_inputs& in, reimport_leftovers& leftovers, ds::task_context& ctx)
	{
		const edt::image_reimport_plan& plan = in.plan;
		const std::vector<fs::path> roots{ in.top };
		const auto private_text = [&](std::string text) {
			return shown(edt::without_paths(std::move(text), roots, plan.source));
		};
		const auto changed_text = [](const res::tag& tag) {
			return std::format("{} changed since it was read", shown(tag.view()));
		};
		const auto failed = [&in](std::string_view step) { return in.fail_step && in.fail_step(step); };
		ctx.throw_if_stopped();
		ctx.set_progress(0.f, "Reading the picture");
		edt::image_import_plan source;
		source.source = plan.source;
		source.size = plan.size;
		source.modified = plan.modified;
		source.id = plan.id;
		const std::vector<std::byte> bytes = read_checked_source(source, nullptr);
		ctx.set_progress(0.3f, "Decoding the picture");
		if (const std::string why = edt::image_importer::image_problem(bytes, extension_of(plan.name)); !why.empty()) {
			throw std::runtime_error(why);
		}
		ctx.throw_if_stopped();
		// The picture in the project, read whole now: the plan kept only its stamp.
		const below_read old_image = read_below(in.top, plan.image, in.max_image_bytes);
		if (old_image.result.status != platform::beneath::read_status::ok || !old_image.result.stamp
			|| !plan.image_stamp || !edt::same_file_stamp(*old_image.result.stamp, *plan.image_stamp)) {
			throw std::runtime_error(changed_text(plan.image) + ": nothing was re-imported");
		}

		std::string meta_text;
		if (plan.settings_changed) {
			res::asset_meta meta = plan.recorded;
			meta.settings = edt::image_settings_json(plan.settings);
			meta_text = res::write_asset_meta(meta);
		}
		struct step
		{
			std::string_view name;
			std::string_view put_back;
			const res::tag* tag;
			std::span<const std::byte> before;
			std::span<const std::byte> data;
		};
		std::vector<step> steps;
		steps.reserve(3);
		steps.push_back({ "reimport_image", "put_back:reimport_image", &plan.image, old_image.bytes, bytes });
		if (!plan.texture_patch.empty()) {
			steps.push_back({ "reimport_texture", "put_back:reimport_texture", &plan.texture,
				std::as_bytes(std::span(plan.texture_bytes)), plan.texture_patch });
		}
		// The recipe last: until the outputs are in place it still describes them.
		if (plan.settings_changed) {
			steps.push_back({ "reimport_meta", "put_back:reimport_meta", &plan.meta,
				std::as_bytes(std::span(plan.meta_bytes)), std::as_bytes(std::span(meta_text)) });
		}

		ctx.set_progress(0.8f, "Writing the files");
		std::vector<const step*> done;
		for (const step& item : steps) {
			bool moved = false;
			std::string error = failed(item.name) ? std::string{ "a test failed this write" }
				: replace_if_unchanged(in.top, *item.tag, item.before, item.data, moved);
			if (error.empty()) {
				done.push_back(&item);
				continue;
			}
			std::string message = moved ? changed_text(*item.tag)
				: std::format("{} was not written: {}", shown(item.tag->view()), private_text(std::move(error)));
			std::vector<res::tag> stays;
			for (auto put = done.rbegin(); put != done.rend(); ++put) {
				const step& back = **put;
				std::string left = failed(back.put_back) ? std::string{ "a test failed this write" }
					: replace_below(in.top, *back.tag, back.before);
				if (!left.empty()) {
					message += std::format("; {} could not be put back: {}; {} stays changed", shown(back.tag->view()),
						private_text(std::move(left)), shown(back.tag->view()));
					stays.push_back(*back.tag);
				}
			}
			if (stays.empty()) {
				message += moved ? ": nothing was re-imported" : "; nothing was re-imported";
			} else {
				const std::scoped_lock hold(leftovers.lock);
				leftovers.stays_changed = std::move(stays);
			}
			throw std::runtime_error(message);
		}
		return json::object{ { "image", std::string(plan.image.view()) },
			{ "resource", std::string(plan.meta.view()) }, { "texture", std::string(plan.texture.view()) },
			{ "guid", res::to_text(plan.recorded.guid) }, { "texture_guid", res::to_text(plan.texture_guid) },
			{ "settings_changed", plan.settings_changed }, { "texture_rewritten", !plan.texture_patch.empty() } };
	}
}

edt::image_reimport_plan edt::image_importer::resolve_reimport(const image_reimport_request& request,
	const cmd::origin& caller) const
{
	image_reimport_plan plan;
	const auto refuse = [&plan](const char* code, std::string why) {
		plan.code = code;
		plan.error = std::move(why);
		return plan;
	};
	if (caller.kind != cmd::origin_kind::ui) {
		return refuse(cmd::codes::denied, "re-import is a person's action for now (#1457)");
	}
	if (!m_options.project_open || !m_options.project_open()) return refuse(cmd::codes::disabled, "no project is open");
	const fs::path top = m_options.resources_path ? m_options.resources_path() : fs::path{};
	if (top.empty()) return refuse(cmd::codes::disabled, "no project is open");
	if (m_options.blocker) {
		if (std::optional<std::string> why = m_options.blocker()) return refuse(cmd::codes::busy, *why);
	}
	if (running()) return refuse(cmd::codes::busy, "an image import is already running");

	// The target: a res:// path whose texture desc name passes the import's own rule.
	constexpr std::string_view scheme = "res://";
	const std::string_view target = request.target;
	if (target.empty()) return refuse(cmd::codes::invalid_args, "the target is empty");
	if (target.find("://") != std::string_view::npos && !target.starts_with(scheme)) {
		return refuse(cmd::codes::invalid_args, "the target must be a res:// tag of a picture in the project");
	}
	const agent_level_path whole = check_agent_level_path(texture_relative_of(std::string(target)));
	if (!whole.ok()) return refuse(cmd::codes::invalid_args, "the target: " + whole.error);
	const std::string relative = whole.path.substr(0, whole.path.size() - k_image_texture_suffix.size());
	const std::size_t slash = relative.rfind('/');
	plan.folder = slash == std::string::npos ? std::string{} : relative.substr(0, slash);
	plan.name = slash == std::string::npos ? relative : relative.substr(slash + 1);
	plan.image = res::tag::make(relative);
	plan.meta = res::tag::make(relative + std::string(res::asset_meta_suffix));
	plan.texture = res::tag::make(texture_relative_of(relative));
	plan.texture_meta = res::tag::make(texture_relative_of(relative) + std::string(res::asset_meta_suffix));
	const std::string image_tag = shown(plan.image.view());
	const std::string extension = extension_of(plan.name);
	if (!known_extension(extension)) {
		return refuse(cmd::codes::invalid_args,
			std::format("{} is not a picture this re-import takes (.png, .jpg or .jpeg)", image_tag));
	}
	if (m_options.open_with_unsaved_changes) {
		for (const res::tag* open : { &plan.image, &plan.texture }) {
			if (m_options.open_with_unsaved_changes(*open)) {
				return refuse(cmd::codes::busy, std::format("{} is open with unsaved changes: save or close it first; "
					"nothing was re-imported", shown(open->view())));
			}
		}
	}
	const std::vector<fs::path> roots{ top };
	const auto private_text = [&](std::string text) { return shown(without_paths(std::move(text), roots, {})); };
	const auto not_canonical = [](const std::string& named) {
		return std::format("{} is not in the form the editor writes (edited by hand?): it is not rewritten, nothing "
			"was re-imported", named);
	};
	const auto unreadable = [&](const res::tag& tag, const platform::beneath::read_result& read) {
		using platform::beneath::read_status;
		const std::string named = shown(tag.view());
		if (read.status == read_status::not_regular) return std::format("{} is not a plain file", named);
		if (read.status == read_status::too_large) return std::format("{} is larger than it may be", named);
		return std::format("{} cannot be read: {}", named, private_text(read.error));
	};

	// The picture: in the project's own layer only.
	const below_read image = read_below(top, plan.image, m_options.max_person_bytes, true);
	if (image.result.status == platform::beneath::read_status::missing) {
		const std::vector<fs::path> layers =
			m_options.resource_layers ? m_options.resource_layers() : std::vector<fs::path>{};
		for (std::size_t index = 0; index < layers.size(); ++index) {
			if (layers[index] == top) continue;
			std::error_code ec;
			if (fs::exists(fs::symlink_status(layers[index] / fs_paths::path_from_utf8(relative), ec))) {
				return refuse(cmd::codes::invalid_args, std::format("{} is in the {} layer, not the project's own: only "
					"a picture of the project is re-imported", image_tag, picker::layer_label(index, layers.size())));
			}
		}
		return refuse(cmd::codes::not_found, std::format("{} is not in the project", image_tag));
	}
	if (image.result.status != platform::beneath::read_status::ok || !image.result.stamp) {
		return refuse(cmd::codes::invalid_args, unreadable(plan.image, image.result) + ": nothing was re-imported");
	}
	plan.image_stamp = image.result.stamp;

	// Its `.resource`: kept as it is but for the settings, so it must be one this editor writes.
	const std::string meta_tag = shown(plan.meta.view());
	const below_read meta = read_below(top, plan.meta, k_max_meta_bytes);
	if (meta.result.status == platform::beneath::read_status::missing) {
		return refuse(cmd::codes::not_found, std::format("{} has no {}: only an imported picture is re-imported",
			image_tag, meta_tag));
	}
	if (meta.result.status != platform::beneath::read_status::ok) {
		return refuse(cmd::codes::invalid_args, unreadable(plan.meta, meta.result) + ": nothing was re-imported");
	}
	plan.meta_bytes = text_of(meta.bytes);
	const res::asset_meta_result read = res::read_asset_meta(plan.meta_bytes);
	if (!read) {
		if (read.error.kind == res::asset_meta_error_kind::newer_than_engine) {
			return refuse(cmd::codes::invalid_args, std::format("{} was written by a newer editor: it is not "
				"rewritten, nothing was re-imported", meta_tag));
		}
		return refuse(cmd::codes::invalid_args, std::format("{} cannot be read ({}): nothing was re-imported; fix or "
			"remove it", meta_tag, shown(read.error.message)));
	}
	if (res::write_asset_meta(*read.meta) != plan.meta_bytes) {
		return refuse(cmd::codes::invalid_args, not_canonical(meta_tag));
	}
	if (read.meta->importer != k_image_importer) {
		const std::string importer = read.meta->importer.empty() ? std::string{ "no importer" }
			: std::format("the importer '{}'", shown(read.meta->importer));
		return refuse(cmd::codes::invalid_args, std::format("{} records {}, not {}: nothing was re-imported",
			meta_tag, importer, k_image_importer));
	}
	const image_settings_parse recorded = parse_image_settings(json::value(read.meta->settings));
	if (!recorded.ok()) {
		return refuse(cmd::codes::invalid_args, std::format("{}: {}; nothing was re-imported", meta_tag,
			shown(recorded.error)));
	}
	plan.recorded = *read.meta;
	plan.old_settings = recorded.settings;

	const image_settings_parse parsed = parse_image_settings(request.settings, plan.old_settings);
	if (!parsed.ok()) return refuse(cmd::codes::invalid_args, parsed.error);
	plan.settings = parsed.settings;
	plan.settings_changed = plan.settings != plan.old_settings;

	// The texture desc beside it, and its own `.resource`, which is never written but must be the editor's.
	const std::string texture_tag = shown(plan.texture.view());
	const below_read texture = read_below(top, plan.texture, k_max_texture_desc_bytes);
	if (texture.result.status == platform::beneath::read_status::missing) {
		return refuse(cmd::codes::not_found, std::format("{} has no {}: only an imported picture is re-imported",
			image_tag, texture_tag));
	}
	if (texture.result.status != platform::beneath::read_status::ok) {
		return refuse(cmd::codes::invalid_args, unreadable(plan.texture, texture.result) + ": nothing was re-imported");
	}
	plan.texture_bytes = text_of(texture.bytes);
	const std::string texture_meta_tag = shown(plan.texture_meta.view());
	const below_read texture_meta = read_below(top, plan.texture_meta, k_max_meta_bytes);
	if (texture_meta.result.status == platform::beneath::read_status::missing) {
		return refuse(cmd::codes::not_found, std::format("{} has no {}: only an imported picture is re-imported",
			image_tag, texture_meta_tag));
	}
	if (texture_meta.result.status != platform::beneath::read_status::ok) {
		return refuse(cmd::codes::invalid_args,
			unreadable(plan.texture_meta, texture_meta.result) + ": nothing was re-imported");
	}
	const std::string texture_meta_text = text_of(texture_meta.bytes);
	const res::asset_meta_result texture_id = res::read_asset_meta(texture_meta_text);
	if (!texture_id) {
		return refuse(cmd::codes::invalid_args, std::format("{} cannot be read ({}): nothing was re-imported; fix or "
			"remove it", texture_meta_tag, shown(texture_id.error.message)));
	}
	if (res::write_asset_meta(*texture_id.meta) != texture_meta_text) {
		return refuse(cmd::codes::invalid_args, not_canonical(texture_meta_tag));
	}
	plan.texture_guid = texture_id.meta->guid;

	// Only the fields the settings make are replaced: whatever else was edited in the desc stays.
	boost::system::error_code parse_error;
	json::value desc = json::parse(plan.texture_bytes, parse_error);
	if (parse_error || !desc.is_object()) {
		return refuse(cmd::codes::invalid_args, std::format("{} is no JSON object: nothing was re-imported",
			texture_tag));
	}
	json::object& desc_object = desc.as_object();
	const json::value* picture = desc_object.if_contains("data");
	if (picture == nullptr || !picture->is_string() || picture->as_string() != plan.image.view()) {
		return refuse(cmd::codes::invalid_args, std::format("{} refers to another picture: nothing was re-imported",
			texture_tag));
	}
	if (plan.settings_changed) {
		json::value* header = desc_object.if_contains("header");
		json::value* header_data = header && header->is_object() ? header->as_object().if_contains("data") : nullptr;
		if (header_data == nullptr || !header_data->is_object()) {
			return refuse(cmd::codes::invalid_args, std::format("{} has no header.data object to change: nothing was "
				"re-imported", texture_tag));
		}
		const json::object made = image_texture_desc_json(plan.image, plan.name, plan.settings);
		const json::object& made_header = made.at("header").as_object();
		json::object& kept_header = header->as_object();
		for (const std::string_view key : { "wrap", "mag", "min" }) kept_header[key] = made_header.at(key);
		header_data->as_object()["mip_levels"] = made_header.at("data").as_object().at("mip_levels");
		std::vector<std::byte> patched = res::pretty_json_bytes(desc);
		if (text_of(patched) != plan.texture_bytes) plan.texture_patch = std::move(patched);
	}

	// The source, as a person's import takes it, in the picture's own format.
	if (request.path.empty()) return refuse(cmd::codes::invalid_args, "the source path is empty");
	if (!from_utf8(request.path).is_absolute()) {
		return refuse(cmd::codes::invalid_args, "the source must be an absolute path to a picture");
	}
	image_import_plan source;
	resolve_person_source(request.path, source);
	if (!source.ok()) return refuse(source.code.c_str(), source.error);
	const fs::path own = top / fs_paths::path_from_utf8(relative);
	std::error_code same_ec;
	if (fs::equivalent(source.source, own, same_ec) || file_id_of(own).same_as(source.id)) {
		return refuse(cmd::codes::invalid_args, "the source is the picture itself");
	}
	if (is_png_extension(extension_of(fs_paths::utf8_of(source.source.filename()))) != is_png_extension(extension)) {
		return refuse(cmd::codes::invalid_args, is_png_extension(extension)
			? std::format("{} is a PNG picture: re-import it from a .png file", image_tag)
			: std::format("{} is a JPEG picture: re-import it from a .jpg or .jpeg file", image_tag));
	}
	plan.source = source.source;
	plan.size = source.size;
	plan.modified = source.modified;
	plan.id = source.id;
	return plan;
}

edt::image_importer::started edt::image_importer::start_reimport(const image_reimport_request& request,
	const cmd::origin& caller)
{
	image_reimport_plan plan = resolve_reimport(request, caller);
	if (!plan.ok()) return { 0, plan.code, plan.error };
	reimport_inputs inputs;
	inputs.top = m_options.resources_path ? m_options.resources_path() : fs::path{};
	inputs.max_image_bytes = m_options.max_person_bytes;
	inputs.fail_step = m_options.fail_step;
	inputs.plan = std::move(plan);
	std::string label = "Re-import " + inputs.plan.name;
	std::array<res::tag, 3> tags{ inputs.plan.image, inputs.plan.texture, inputs.plan.meta };
	auto leftovers = std::make_shared<reimport_leftovers>();
	m_job = m_jobs.start_with_result(ds::lane::compute, { std::move(label), "import" }, m_lifetime,
		[inputs = std::move(inputs), leftovers](ds::task_context& ctx) -> json::value {
			return run_reimport(inputs, *leftovers, ctx);
		},
		[this, tags, leftovers](ds::task_result<json::value>& result) {
			if (!m_options.rewritten) return;
			std::vector<res::tag> written;
			if (result.ok() && result.value && result.value->is_object()) {
				const json::object& done = result.value->as_object();
				const auto flag = [&done](std::string_view key) {
					const json::value* value = done.if_contains(key);
					return value != nullptr && value->is_bool() && value->as_bool();
				};
				written.push_back(tags[0]);
				if (flag("texture_rewritten")) written.push_back(tags[1]);
				if (flag("settings_changed")) written.push_back(tags[2]);
			} else {
				const std::scoped_lock hold(leftovers->lock);
				written = leftovers->stays_changed;
			}
			if (!written.empty()) m_options.rewritten(written);
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
	m_target.clear();
	m_folder = std::move(folder);
	m_source = std::move(source);
	m_settings = {};
	m_name = fs_paths::utf8_of(m_source.filename());
	m_error.clear();
	check();
}

void edt::image_import_form::open_reimport(std::string target, fs::path source)
{
	m_target = std::move(target);
	m_folder.clear();
	m_source = std::move(source);
	m_settings = {};
	m_name.clear();
	m_plan = {};
	m_error.clear();
	image_reimport_request asked = reimport_request();
	asked.settings = nullptr;
	const image_reimport_plan plan = m_importer.resolve_reimport(asked, cmd::origin::ui());
	m_recorded = plan.ok() ? plan.old_settings : image_import_settings{};
	m_settings = m_recorded;
	m_problem = plan.error;
}

std::vector<std::string> edt::image_import_form::rewritten_names() const
{
	if (!reimport()) return {};
	std::vector<std::string> names{ m_target };
	if (m_settings != m_recorded) {
		names.push_back(m_target + std::string(k_image_texture_suffix));
		names.push_back(m_target + std::string(res::asset_meta_suffix));
	}
	return names;
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

edt::image_reimport_request edt::image_import_form::reimport_request() const
{
	image_reimport_request out;
	out.target = m_target;
	out.path = fs_paths::utf8_of(m_source);
	out.settings = image_settings_json(m_settings);
	return out;
}

void edt::image_import_form::check()
{
	if (reimport()) {
		m_problem = m_importer.resolve_reimport(reimport_request(), cmd::origin::ui()).error;
		return;
	}
	m_plan = m_importer.resolve(request(), true);
	m_problem = m_plan.error;
}

std::vector<std::string> edt::image_import_form::final_names() const
{
	if (reimport() || !m_plan.ok() || !m_plan.image.is_valid()) {
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
	const image_importer::started started = reimport() ? m_importer.start_reimport(reimport_request(), cmd::origin::ui())
		: m_importer.start(request(), cmd::origin::ui());
	if (!started.ok()) {
		m_error = started.error;
		check();
		return std::nullopt;
	}
	m_error.clear();
	return started.job;
}
