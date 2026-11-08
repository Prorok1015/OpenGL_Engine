#include "edt_agent_model_import.h"
#include "edt_file_menu.h"
#include <snk/common/logger/engine_log.h>
#include <algorithm>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <cctype>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace json = boost::json;
namespace fs = std::filesystem;

struct edt::agent_import_scans
{
	struct found
	{
		std::string token;
		std::string key;
		std::string session;
		edt::agent_import_plan plan;
		std::chrono::steady_clock::time_point at;
	};

	std::mutex mutex;
	std::vector<found> items;
	std::uint64_t counter = 0;
};

namespace {
	fs::path from_utf8(const std::string& text)
	{
		return fs::path(std::u8string(text.begin(), text.end()));
	}

	std::string to_utf8(const fs::path& path)
	{
		const std::u8string text = path.u8string();
		return std::string(text.begin(), text.end());
	}

	std::string lowered(std::string text)
	{
		std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return text;
	}

	std::string size_text(std::uintmax_t bytes)
	{
		return std::format("{} bytes ({:.1f} MiB)", bytes, static_cast<double>(bytes) / (1024.0 * 1024.0));
	}

	// The state of a project file in every layer of res://: a link or a folder anywhere is unsafe, a file anywhere is a file.
	edt::level_file_state state_in_layers(const std::vector<fs::path>& layers, const std::string& relative)
	{
		bool found = false;
		for (const fs::path& root : layers) {
			switch (edt::inspect_level_file(root, relative)) {
			case edt::level_file_state::unsafe: return edt::level_file_state::unsafe;
			case edt::level_file_state::file: found = true; break;
			case edt::level_file_state::missing: break;
			}
		}
		return found ? edt::level_file_state::file : edt::level_file_state::missing;
	}

	// Empty when `plan.source` still is the file that was shown: a plain file, the same place, size and time.
	std::string identity_problem(const edt::agent_import_plan& plan)
	{
		std::error_code ec;
		const fs::file_status status = fs::symlink_status(plan.source, ec);
		if (ec || status.type() != fs::file_type::regular) return "the file is gone or is no plain file any more";
		const fs::path resolved = fs::canonical(plan.source, ec);
		if (ec || resolved != plan.source) return "the file is reached by another path now";
		if (fs::file_size(plan.source, ec) != plan.size || ec) return "the size of the file changed";
		if (fs::last_write_time(plan.source, ec) != plan.modified || ec) return "the file was modified";
		return {};
	}

	// The bytes of the model, read once: no more than the size that was shown, and the file must still be that file.
	std::vector<std::byte> read_checked_source(const edt::agent_import_plan& plan)
	{
		if (const std::string why = identity_problem(plan); !why.empty()) throw std::runtime_error(why);
		std::ifstream in(plan.source, std::ios::binary);
		if (!in) throw std::runtime_error("the file cannot be opened");
		std::vector<std::byte> bytes(static_cast<std::size_t>(plan.size));
		in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		if (static_cast<std::uintmax_t>(in.gcount()) != plan.size || in.peek() != std::char_traits<char>::eof()) {
			throw std::runtime_error("the size of the file changed while it was read");
		}
		if (const std::string why = identity_problem(plan); !why.empty()) throw std::runtime_error("the file changed while it was read: " + why);
		return bytes;
	}

	// Takes back the files a failed or cancelled export wrote: only those, only plain files.
	void take_back(const std::vector<res::tag>& written, const fs::path& top)
	{
		for (auto tag = written.rbegin(); tag != written.rend(); ++tag) {
			const fs::path file = top / from_utf8(std::string(tag->relative()));
			std::error_code ec;
			if (fs::symlink_status(file, ec).type() == fs::file_type::regular) fs::remove(file, ec);
		}
	}

	struct import_job_inputs
	{
		edt::asset_exporter* exporter = nullptr;
		res::resource_system* resources = nullptr;
		std::function<edt::import_intermediate(const edt::import_source&, ds::task_context*)> import;
		std::function<edt::model_file_scan(const edt::import_source&, ds::task_context*)> scan;
		std::function<std::chrono::steady_clock::time_point()> now;
		std::shared_ptr<edt::agent_import_scans> scans;
		std::string scan_key;
		std::string session;
		std::chrono::seconds time_limit{ 60 };
		// A person is asked a second time, about the files the model refers to, before any of them is read.
		bool ask_about_files = true;
		edt::model_file_limits limits;
		std::vector<fs::path> layers;
		fs::path top;
		edt::agent_import_plan plan;
	};

	// The model refers to other files: nothing is imported. The list waits for the second call, whose question shows it.
	json::value park_files(const import_job_inputs& in, const edt::model_file_scan& found)
	{
		edt::agent_import_plan plan = in.plan;
		plan.files = found.files;
		plan.files_bytes = found.total_bytes;
		plan.scanned = true;
		std::string token;
		{
			const std::lock_guard lock(in.scans->mutex);
			const auto at = in.now();
			std::erase_if(in.scans->items, [&](const auto& item) {
				return item.key == in.scan_key || at - item.at > edt::agent_model_import::k_shown_lifetime;
			});
			const auto of_session = [&](const auto& item) { return item.session == in.session; };
			if (static_cast<std::size_t>(std::count_if(in.scans->items.begin(), in.scans->items.end(), of_session)) >= edt::agent_model_import::k_max_shown) {
				in.scans->items.erase(std::find_if(in.scans->items.begin(), in.scans->items.end(), of_session));
			}
			token = std::format("files-{}", ++in.scans->counter);
			in.scans->items.push_back({ token, in.scan_key, in.session, std::move(plan), at });
		}
		return json::object{
			{ "needs_files_approval", true },
			{ "files_token", token },
			{ "file_count", static_cast<std::int64_t>(found.files.size()) },
			{ "next", "Nothing was imported or written. Call file.import_model again with the same arguments and files_token: a person is then shown the list of files and decides." } };
	}

	json::value run_import(const import_job_inputs& in, ds::task_context& ctx)
	{
		ctx.throw_if_stopped();
		ctx.set_progress(0.f, "Reading the model");
		if (const std::string why = identity_problem(in.plan); !why.empty()) throw std::runtime_error(why);

		const std::vector<std::byte> source_bytes = read_checked_source(in.plan);
		edt::import_source source;
		source.path = in.plan.source;
		source.preloaded = &source_bytes;
		source.limits = in.limits;
		source.deadline = std::chrono::steady_clock::now() + in.time_limit;
		std::vector<edt::referenced_file> files = in.plan.files;
		if (!in.plan.scanned) {
			const edt::model_file_scan found = in.scan(source, &ctx);
			if (!found.ok()) throw std::runtime_error(found.error);
			if (!found.files.empty() && in.ask_about_files) return park_files(in, found);
			files = found.files;
		}
		source.allowed = &files;
		source.deadline = std::chrono::steady_clock::now() + in.time_limit;

		edt::import_intermediate imported = in.import(source, &ctx);
		const std::vector<res::tag> memory_copies = imported.created_tags;
		const auto drop_memory_copies = [&] {
			for (const res::tag& tag : memory_copies) in.resources->erase(tag);
		};

		std::vector<res::tag> written;
		std::unordered_map<res::tag, res::tag> remap;
		edt::import_result result;
		try {
			if (!imported.error.empty()) throw std::runtime_error(imported.error);
			if (const std::string why = identity_problem(in.plan); !why.empty()) {
				throw std::runtime_error("the file changed while it was imported: " + why);
			}
			ctx.throw_if_stopped();

			result.root_tag = imported.prefab_tag;
			result.all_tags = imported.all_tags;
			result.source_path = in.plan.source;
			remap = in.exporter->build_remap(result, in.plan.name, in.plan.folder);
			for (const auto& entry : remap) {
				const std::string relative(entry.second.relative());
				switch (state_in_layers(in.layers, relative)) {
				case edt::level_file_state::missing: break;
				case edt::level_file_state::file:
					throw std::runtime_error(std::format("'{}' exists: nothing was written", entry.second.view()));
				case edt::level_file_state::unsafe:
					throw std::runtime_error(std::format("'{}' is not a plain file: nothing was written", entry.second.view()));
				}
			}

			ctx.set_progress(0.5f, "Writing the assets");
			const bool exported = in.exporter->export_to_project(result, remap, nullptr, &ctx, &written, true,
				[&ctx](int done, int total) {
					ctx.set_progress(0.5f + 0.5f * static_cast<float>(done) / static_cast<float>(std::max(total, 1)), "Writing the assets");
				});
			if (!exported) throw std::runtime_error("the assets could not be written");
		} catch (...) {
			take_back(written, in.top);
			drop_memory_copies();
			throw;
		}
		drop_memory_copies();

		std::vector<std::string> assets;
		assets.reserve(remap.size());
		for (const auto& entry : remap) assets.emplace_back(entry.second.view());
		std::sort(assets.begin(), assets.end());
		json::array listed;
		for (const std::string& asset : assets) listed.emplace_back(json::string(asset));
		const auto prefab = remap.find(result.root_tag);
		return json::object{
			{ "prefab", prefab == remap.end() ? std::string{} : std::string(prefab->second.view()) },
			{ "assets", std::move(listed) } };
	}
}

bool edt::agent_import_plan::same_file_as(const agent_import_plan& other) const
{
	return source == other.source && size == other.size && modified == other.modified && name == other.name && folder == other.folder;
}

edt::agent_model_import::agent_model_import(asset_exporter& exporter, job_service& jobs, res::resource_system& resources, agent_import_options options)
	: m_exporter(exporter), m_jobs(jobs), m_resources(resources), m_options(std::move(options)), m_scans(std::make_shared<agent_import_scans>())
{
}

edt::agent_model_import::~agent_model_import() = default;

bool edt::agent_model_import::running() const
{
	return m_job != 0 && m_jobs.is_running(m_job);
}

std::chrono::steady_clock::time_point edt::agent_model_import::now() const
{
	return m_options.now ? m_options.now() : std::chrono::steady_clock::now();
}

std::string edt::agent_model_import::scan_key(const agent_import_request& request, const cmd::origin& caller) const
{
	return std::format("{}:{}|{}:{}|{}:{}|{}:{}", caller.session.size(), caller.session, request.path.size(), request.path,
		request.name.size(), request.name, request.folder.size(), request.folder);
}

std::string edt::agent_model_import::call_key(const agent_import_request& request, const cmd::origin& caller) const
{
	return scan_key(request, caller) + "|" + request.files_token;
}

void edt::agent_model_import::remember(std::string key, std::string session, agent_import_plan plan)
{
	const auto at = now();
	std::erase_if(m_shown, [&](const shown_plan& shown) { return shown.key == key || at - shown.at > k_shown_lifetime; });
	const auto of_session = [&](const shown_plan& shown) { return shown.session == session; };
	if (static_cast<std::size_t>(std::count_if(m_shown.begin(), m_shown.end(), of_session)) >= k_max_shown) {
		m_shown.erase(std::find_if(m_shown.begin(), m_shown.end(), of_session));
	}
	m_shown.push_back({ std::move(key), session, std::move(plan), at });
}

std::optional<edt::agent_import_plan> edt::agent_model_import::take(const std::string& key)
{
	const auto at = now();
	std::erase_if(m_shown, [&](const shown_plan& shown) { return at - shown.at > k_shown_lifetime; });
	const auto found = std::find_if(m_shown.begin(), m_shown.end(), [&](const shown_plan& shown) { return shown.key == key; });
	if (found == m_shown.end()) return std::nullopt;
	agent_import_plan plan = std::move(found->plan);
	m_shown.erase(found);
	return plan;
}

edt::agent_import_plan edt::agent_model_import::resolve(const agent_import_request& request) const
{
	agent_import_plan plan;
	const auto refuse = [&plan](const char* code, std::string why) {
		plan.code = code;
		plan.error = std::move(why);
		return plan;
	};
	if (!available()) {
		return refuse(cmd::codes::disabled, "model import is not available: this editor was built without assimp (SNK_WITH_ASSIMP=OFF)");
	}
	if (request.path.empty()) return refuse(cmd::codes::invalid_args, "the source path is empty");

	const fs::path given = from_utf8(request.path);
	if (!given.is_absolute()) return refuse(cmd::codes::invalid_args, "the source must be an absolute path to a model file");
	std::error_code ec;
	const fs::file_status status = fs::symlink_status(given, ec);
	if (ec || status.type() == fs::file_type::not_found) return refuse(cmd::codes::not_found, "no such file: " + request.path);
	if (status.type() == fs::file_type::symlink) return refuse(cmd::codes::invalid_args, "the source is a link: name the file itself");
	if (status.type() != fs::file_type::regular) return refuse(cmd::codes::invalid_args, "the source is not a regular file");

	const std::string extension = lowered(to_utf8(given.extension()));
	if (std::none_of(std::begin(k_model_extensions), std::end(k_model_extensions), [&](std::string_view known) { return known == extension; })) {
		return refuse(cmd::codes::invalid_args, "the source is not a model file (.glb, .gltf, .obj or .fbx)");
	}

	const fs::path resolved = fs::canonical(given, ec);
	if (ec) return refuse(cmd::codes::invalid_args, "the source path cannot be resolved");
	if (fs::symlink_status(resolved, ec).type() != fs::file_type::regular) {
		return refuse(cmd::codes::invalid_args, "the source is not a regular file");
	}
	plan.source = resolved;
	plan.size = fs::file_size(resolved, ec);
	if (ec) return refuse(cmd::codes::invalid_args, "the size of the source cannot be read");
	if (plan.size == 0) return refuse(cmd::codes::invalid_args, "the source file is empty");
	if (plan.size > m_options.max_source_bytes) {
		return refuse(cmd::codes::invalid_args, std::format("the source is too big: {}, the limit is {}", size_text(plan.size), size_text(m_options.max_source_bytes)));
	}
	plan.modified = fs::last_write_time(resolved, ec);
	if (ec) return refuse(cmd::codes::invalid_args, "the time of the source cannot be read");

	const std::string wanted_name = request.name.empty() ? to_utf8(resolved.stem()) : request.name;
	const agent_level_path name = check_agent_name(wanted_name);
	if (!name.ok()) return refuse(cmd::codes::invalid_args, "the asset name: " + name.error + (request.name.empty() ? " (give a name)" : ""));
	plan.name = name.path;
	const agent_level_path folder = check_agent_folder(request.folder.empty() ? "assets/" + plan.name : request.folder);
	if (!folder.ok()) return refuse(cmd::codes::invalid_args, "the folder: " + folder.error);
	plan.folder = folder.path;
	plan.prefab = "res://" + plan.folder + "/" + plan.name + ".prefab.desc";

	const std::vector<fs::path> layers = m_options.resource_layers ? m_options.resource_layers() : std::vector<fs::path>{};
	switch (state_in_layers(layers, plan.folder + "/" + plan.name + ".prefab.desc")) {
	case level_file_state::missing: break;
	case level_file_state::file:
		return refuse("needs_input", std::format("'{}' exists: choose another name or folder, or ask a person to overwrite it in the editor", plan.prefab));
	case level_file_state::unsafe:
		return refuse(cmd::codes::invalid_args, std::format("'{}' is not a plain file", plan.prefab));
	}

	if (m_options.blocker) {
		if (std::optional<std::string> why = m_options.blocker()) return refuse(cmd::codes::busy, *why);
	}
	if (running()) return refuse(cmd::codes::busy, "a model import is already running");
	return plan;
}

std::optional<std::string> edt::agent_model_import::describe(const agent_import_request& request, const cmd::origin& caller)
{
	agent_import_plan plan = resolve(request);
	if (!plan.ok()) return "This request will be refused: " + plan.error;
	std::string text = std::format("Import the model\n  {}\n  {}\ninto res://{}/ as '{}'\n  prefab: {}\nIts geometry, materials and textures are written next to the prefab.\n",
		to_utf8(plan.source), size_text(plan.size), plan.folder, plan.name, plan.prefab);
	if (request.files_token.empty()) {
		text += "Allowing this reads the model, and the .bin/.mtl files it needs to be parsed, to list the files it refers to. Nothing is copied into the project until you approve that list in a second question (none, when the model is self-contained).";
	} else {
		const std::lock_guard lock(m_scans->mutex);
		const std::string key = scan_key(request, caller);
		const auto found = std::find_if(m_scans->items.begin(), m_scans->items.end(), [&](const auto& item) {
			return item.token == request.files_token && item.key == key && now() - item.at <= k_shown_lifetime;
		});
		if (found == m_scans->items.end()) {
			return "This request will be refused: no list of files belongs to this token (too old, or from another call): import without files_token first";
		}
		if (!found->plan.same_file_as(plan)) return "This request will be refused: the model changed since its files were listed";
		plan.files = found->plan.files;
		plan.files_bytes = found->plan.files_bytes;
		plan.scanned = true;
		text += std::format("The model makes the editor read these {} files from its folder, {} in all, and copy them into the project:", plan.files.size(), size_text(plan.files_bytes));
		for (const referenced_file& file : plan.files) text += std::format("\n  {}  {}", to_utf8(file.path), size_text(file.size));
	}
	remember(call_key(request, caller), caller.session, std::move(plan));
	return text;
}

edt::agent_model_import::started edt::agent_model_import::start(const agent_import_request& request, const cmd::origin& caller, bool confirmed)
{
	// A person's own call (a script of theirs) is the approval; anyone else needs one, for exactly this file. Nothing about
	// the disk is looked at before that, so an unapproved call cannot probe paths.
	std::optional<agent_import_plan> shown;
	if (caller.kind != cmd::origin_kind::ui) {
		if (!confirmed) return { 0, cmd::codes::denied, "an import needs a person's approval" };
		shown = take(call_key(request, caller));
		if (!shown) return { 0, cmd::codes::denied, "this import was not shown to a person, or the approval is too old: ask again" };
	}
	agent_import_plan plan = resolve(request);
	if (!plan.ok()) return { 0, plan.code, plan.error };
	if (shown && !shown->same_file_as(plan)) return { 0, cmd::codes::denied, "the file changed since a person was shown it: ask again" };
	if (!request.files_token.empty()) {
		std::optional<agent_import_plan> listed;
		{
			const std::lock_guard lock(m_scans->mutex);
			const std::string key = scan_key(request, caller);
			const auto found = std::find_if(m_scans->items.begin(), m_scans->items.end(), [&](const auto& item) {
				return item.token == request.files_token && item.key == key && now() - item.at <= k_shown_lifetime;
			});
			if (found != m_scans->items.end()) {
				listed = std::move(found->plan);
				m_scans->items.erase(found);
			}
		}
		if (!listed || !listed->same_file_as(plan)) return { 0, cmd::codes::denied, "no list of files belongs to this token, or the model changed since it was made: ask again without files_token" };
		const auto same_list = [](const std::vector<referenced_file>& a, const std::vector<referenced_file>& b) {
			return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const referenced_file& x, const referenced_file& y) {
				return x.path == y.path && x.size == y.size && x.modified == y.modified;
			});
		};
		if (shown && !same_list(shown->files, listed->files)) return { 0, cmd::codes::denied, "the files changed since a person was shown them: ask again" };
		plan.files = std::move(listed->files);
		plan.files_bytes = listed->files_bytes;
		plan.scanned = true;
	}

	import_job_inputs inputs;
	inputs.exporter = &m_exporter;
	inputs.resources = &m_resources;
	inputs.import = m_options.import;
	inputs.scan = m_options.scan;
	inputs.now = [clock = m_options.now] { return clock ? clock() : std::chrono::steady_clock::now(); };
	inputs.scans = m_scans;
	inputs.scan_key = scan_key(request, caller);
	inputs.session = caller.session;
	inputs.time_limit = m_options.parse_time_limit;
	inputs.ask_about_files = caller.kind != cmd::origin_kind::ui;
	inputs.limits = m_options.file_limits;
	inputs.layers = m_options.resource_layers ? m_options.resource_layers() : std::vector<fs::path>{};
	inputs.top = m_options.resources_path ? m_options.resources_path() : fs::path{};
	inputs.plan = std::move(plan);
	std::string label = "Import " + inputs.plan.name;
	m_job = m_jobs.start_with_result(ds::lane::compute, { std::move(label), "import" }, m_lifetime,
		[inputs = std::move(inputs)](ds::task_context& ctx) -> json::value { return run_import(inputs, ctx); });
	return { m_job, {}, {} };
}
