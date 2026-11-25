#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "autosave/edt_autosave_component.h"
#include "autosave/edt_autosave_files.h"
#include "autosave/edt_autosave_recovery.h"
#include "autosave/edt_autosave_restore.h"
#include "autosave/edt_autosave_source.h"
#include "edt_data_document.h"
#include "edt_level_controller.h"
#include "edt_mesh_document.h"
#include "edt_mesh_shape_asset.h"
#include "edt_prefab_document.h"
#include "edt_scene_document.h"
#include "edt_scene_editor.h"
#include "edt_shader_source_document.h"
#include "edt_world_controller.h"
#include <snk/common/hash/sha256.h>
#include <snk/desc/desc_schema.h>
#include <snk/desc/desc_system.h>
#include <snk/editor/edt_document_manager.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/editor/edt_problem_list.h>
#include <snk/platform/plt_file_lock.h>
#include <snk/resource/res_mount_table.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_hot_reload_manager.h>
#include <snk/scene/level/scn_level_desc.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <boost/json.hpp>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#if !defined(_WIN32)
#include <unistd.h>
#endif

// Crash recovery (#1187): the autosaves another editor run left behind are judged by their session's lock, checked
// record by record, asked about in one question and restored through each kind's own open path.
namespace {

namespace fs = std::filesystem;
namespace json = boost::json;
using namespace std::chrono_literals;

constexpr const char* k_settings_schema = R"({
	"name": "test.recovery_settings", "kind": "desc", "is_document": true,
	"fields": [ { "name": "speed", "kind": "f32", "default": 1.0 } ]
})";

constexpr const char* k_preview_template = R"({
	"__type": "world_desc", "__version": 5, "name": "__prefab_preview", "systems": [],
	"components": { "anchor": { "__type": "anchor_desc", "__version": 1 } },
	"children": []
})";

constexpr const char* k_prefab = R"({
	"__type": "prefab_desc", "__version": 2,
	"components": { "stat": { "__type": "stat_desc", "hp": 10 } },
	"children": [
		{ "__id": "00000000000000a1", "name": "arm", "components": { "stat": { "__type": "stat_desc", "hp": 3 } } }
	]
})";

std::string level_text(std::string_view node)
{
	return std::format(R"({{"__type":"level_desc","__version":2,"name":"lv","worlds":[
		{{"__type":"world_desc","__version":5,"name":"w0","systems":[],"children":[
			{{"__id":"00000000000000a2","name":"{}","components":{{"stat":{{"__type":"stat_desc","hp":3}}}}}}]}}]}})", node);
}

std::vector<std::byte> bytes_of(std::string_view text)
{
	const auto* first = reinterpret_cast<const std::byte*>(text.data());
	return { first, first + text.size() };
}

// A record name may be 160 characters below a deep build folder: past Windows' 260, a plain path cannot open it.
fs::path openable(const fs::path& path)
{
#ifdef _WIN32
	fs::path full = fs::absolute(path);
	full.make_preferred();
	if (full.native().starts_with(L"\\\\?\\")) return full;
	return fs::path(L"\\\\?\\" + full.native());
#else
	return path;
#endif
}

std::string read_file(const fs::path& path)
{
	std::ifstream in(openable(path), std::ios::binary);
	return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

void write_file(const fs::path& path, std::string_view text)
{
	std::error_code ec;
	fs::create_directories(path.parent_path(), ec);
	BOOST_REQUIRE_MESSAGE(!ec, "cannot make " << path.parent_path().string() << ": " << ec.message());
	std::ofstream out(openable(path), std::ios::binary);
	out << text;
	out.close();
	BOOST_REQUIRE_MESSAGE(!out.fail(), "cannot write " << path.string());
}

std::set<fs::path> folders_below(const fs::path& root)
{
	std::set<fs::path> out;
	for (const auto& entry : fs::recursive_directory_iterator(root)) {
		if (entry.is_directory()) out.insert(entry.path());
	}
	return out;
}

struct asked_question
{
	std::string title;
	std::string text;
	std::vector<std::string> options;
	std::function<void(std::optional<std::size_t>)> answer;
};

struct recovery_fixture : test_util::scene_env
{
	recovery_fixture()
	{
		fs::create_directories(content);
		fs::create_directories(cache);
		rs->unregistrate_resolver(res::tag::default_protocol());
		rs->registrate_resolver<res::vfs_resolver>(res::tag::default_protocol(), std::vector<fs::path>{ content },
			res::mount_access::read_write, res::write_policy::allow_all());
		BOOST_REQUIRE(rs->add_mount(res::mount{ res::tag::cache, { cache }, res::mount_access::read_write }));
		auto& edt_ram = rs->registrate_resolver<test_util::ram_resolver>("edt");
		edt_ram.store(res::tag("edt://templates/prefab_preview.desc"), test_util::to_bytes(k_preview_template));
		descs->register_schema(desc::parse_type_schema(json::parse(k_settings_schema).as_object()));

		hot = std::make_shared<scn::hot_reload_manager>(*assembler, *rs);
		mgr = std::make_shared<scn::context_manager>(*rs, *assembler, sf, hot);
		services.ctx_manager = mgr;
		services.base_descs = descs.get();
		services.base_resources = rs.get();
		services.base_assembler = assembler.get();
		services.base_sfactory = &sf;
		// The mesh document makes no preview world without a context manager.
		mesh_services.base_descs = descs.get();
		mesh_services.base_resources = rs.get();
		mesh_services.base_assembler = assembler.get();
		mesh_services.base_sfactory = &sf;
		state.set_autosave_capture([this] { return level_ctrl.capture_autosave(); });
		autosave = make_autosave();
		autosave->tick();
		BOOST_REQUIRE(fs::is_directory(cache / edt::autosave_session_folder(autosave->session())));
	}

	~recovery_fixture()
	{
		recovery.reset();
		prefab.reset();
		mesh.reset();
		autosave.reset();
		documents.shutdown();
		rs->unregistrate_resolver(res::tag::cache);
		rs->unregistrate_resolver(res::tag::default_protocol());
		// The temp folder's own removal takes plain paths: on Windows a record name past 260 would stay, and the
		// sessions of this test with it, for the next test to find.
		std::error_code ignored;
		fs::remove_all(openable(dir.path()), ignored);
	}

	std::unique_ptr<edt::autosave_component> make_autosave()
	{
		auto made = std::make_unique<edt::autosave_component>(edt::autosave_component::ports{ .documents = documents, .events = events,
			.resources = *rs, .jobs = nullptr, .gesture_active = {}, .problems = &problems });
		made->set_clock_for_tests([this] { return now; });
		return made;
	}

	edt::desc_file_layout layout()
	{
		edt::desc_file_layout out;
		out.top_layer = [this] { return content; };
		out.layers = [this] { return std::vector<fs::path>{ content }; };
		out.open_level_files = [] { return std::vector<std::string>{}; };
		return out;
	}

	// A session folder of another run: session.json and, unless `lock_file` is false, an unheld session.lock.
	std::string other_session(bool lock_file = true, std::string host = edt::this_host_name(), bool locked = true, std::string named = {})
	{
		const std::string id = named.empty() ? edt::autosave_new_session_id() : std::move(named);
		edt::autosave_session_info info;
		info.session = id;
		info.host = std::move(host);
		info.pid = 4242;
		info.started_at = "2026-11-24T09:00:00Z";
		info.locked = locked;
		const fs::path folder = cache / edt::autosave_session_folder(id);
		write_file(folder / "session.json", edt::autosave_session_text(info));
		if (lock_file) write_file(folder / "session.lock", "");
		return id;
	}

	static edt::autosave_meta meta_of(const std::string& session, std::string_view kind, std::string_view source,
		const std::vector<std::byte>& payload, std::string saved_at = "2026-11-24T10:00:00Z")
	{
		edt::autosave_meta meta;
		meta.session = session;
		meta.kind = std::string(kind);
		meta.source = std::string(source);
		meta.generation = 3;
		meta.saved_at = std::move(saved_at);
		meta.host = edt::this_host_name();
		meta.payload_size = payload.size();
		meta.payload_sha256 = cmn::sha256_hex(payload);
		return meta;
	}

	// The payload and the meta text as they are, at the name autosave gives them.
	std::string put_raw(const std::string& session, std::string_view kind_folder, std::string_view leaf, std::string_view payload,
		std::string_view meta_text)
	{
		const std::string file = edt::autosave_session_folder(session) + "/" + std::string(kind_folder) + "/" + std::string(leaf);
		write_file(cache / file, payload);
		write_file(cache / edt::autosave_meta_of(file), meta_text);
		return file;
	}

	std::string put_record(const std::string& session, std::string_view kind, std::string_view source, const std::vector<std::byte>& payload,
		std::string saved_at = "2026-11-24T10:00:00Z")
	{
		const edt::autosave_meta meta = meta_of(session, kind, source, payload, std::move(saved_at));
		const res::tag tag = source.empty() ? res::tag{} : res::tag::make(source);
		const std::string file = edt::autosave_file_of(session, kind, tag);
		const std::string text(reinterpret_cast<const char*>(payload.data()), payload.size());
		return put_raw(session, kind, edt::autosave_leaf_of(file), text, edt::autosave_meta_text(meta));
	}

	bool session_exists(const std::string& session) const { return fs::exists(cache / edt::autosave_session_folder(session)); }

	edt::recovery_scan_options scan_options() const
	{
		edt::recovery_scan_options options;
		options.cache_root = cache;
		options.own_session = autosave->session();
		options.host = edt::this_host_name();
		options.res_roots = { content };
		if (!lower_layer.empty()) options.res_roots.insert(options.res_roots.begin(), lower_layer);
		return options;
	}

	edt::autosave_recovery::ports recovery_ports()
	{
		edt::autosave_recovery::ports ports;
		ports.cache_root = [this] { return cache; };
		ports.res_roots = [this] {
			std::vector<fs::path> roots;
			if (!lower_layer.empty()) roots.push_back(lower_layer);
			roots.push_back(content);
			return roots;
		};
		ports.own_session = [this] { return autosave->session(); };
		ports.ensure_own_session = [this] { return autosave->ensure_session(); };
		ports.adopt = [this](edt::document_id id, std::uint64_t generation) { autosave->adopt(id, generation); };
		ports.headless = false;
		ports.choose = [this](std::string title, std::string text, std::vector<std::string> options, std::function<void(std::optional<std::size_t>)> answer) {
			asked.push_back({ std::move(title), std::move(text), std::move(options), std::move(answer) });
		};
		ports.message = [this](std::string text) { messages.push_back(std::move(text)); };
		ports.is_document_type = [this](std::string_view type) {
			for (const std::string& known : descs->document_types()) {
				if (known == type) return true;
			}
			return false;
		};
		ports.restore_scene = [this](const edt::autosave_recovery::restore_request& request, std::function<void(edt::autosave_recovery::restore_outcome)> done) {
			++scene_restores;
			const res::tag copy{ std::format("memory://editor/recovery/{}/{}", request.serial, request.working_name) };
			rs->store(copy, *request.bytes);
			auto handle = rs->require_sync<scn::level_desc>(copy);
			if (!level_ctrl.take_loaded_level(handle, request.source, copy)) {
				done({ "the level did not load", {}, 0 });
				return;
			}
			state.commit("Recovered autosave", edt::commit_mode::state_only);
			done({ {}, state.id(), state.generation() });
		};
		ports.restore_prefab = [this](const edt::autosave_recovery::restore_request& request, std::function<void(edt::autosave_recovery::restore_outcome)> done) {
			++other_restores;
			prefab = std::make_unique<edt::prefab_document>(services);
			prefab->assign_id(70);
			prefab->open(request.source);
			const res::tag copy{ std::format("memory://editor/recovery/{}/{}", request.serial, request.working_name) };
			const std::string error = edt::restore_prefab_bytes(*rs, *rs, *prefab, copy, *request.bytes);
			done({ error, {}, prefab->generation() });
		};
		ports.restore_mesh = [this](const edt::autosave_recovery::restore_request& request, std::function<void(edt::autosave_recovery::restore_outcome)> done) {
			++other_restores;
			mesh = std::make_unique<edt::mesh_document>(mesh_services);
			mesh->assign_id(90);
			mesh->set_files(layout());
			mesh->open(request.source);
			const std::string error = edt::restore_mesh_bytes(*mesh, *request.bytes);
			done({ error, {}, mesh->generation() });
		};
		ports.restore_data = [this](const edt::autosave_recovery::restore_request& request, std::function<void(edt::autosave_recovery::restore_outcome)> done) {
			++other_restores;
			auto made = std::make_unique<edt::data_document>(services, layout());
			const edt::open_result opened = made->open(request.source);
			if (!opened.opened) {
				done({ opened.error, {}, 0 });
				return;
			}
			auto& data = dynamic_cast<edt::data_document&>(documents.adopt(std::move(made)));
			const std::string error = edt::restore_data_bytes(data, *request.bytes);
			done({ error, data.id(), data.generation() });
		};
		ports.restore_shader_source = [this](const edt::autosave_recovery::restore_request& request, std::function<void(edt::autosave_recovery::restore_outcome)> done) {
			++other_restores;
			const edt::shader_restore_result restored = edt::restore_shader_source(documents, [this](const res::tag& tag) {
				for (edt::shader_source_document* open : shaders_open()) {
					if (open->source() == tag) return true;
				}
				auto made = std::make_unique<edt::shader_source_document>(services);
				made->set_files(shader_layout());
				made->open(tag);
				if (!made->is_open()) return false;
				documents.adopt(std::move(made));
				return true;
			}, request.source, *request.bytes, request.source_changed);
			done({ restored.error, restored.document, restored.generation });
		};
		return ports;
	}

	std::vector<edt::shader_source_document*> shaders_open()
	{
		std::vector<edt::shader_source_document*> out;
		documents.for_each([&out](edt::document& open) {
			if (auto* found = dynamic_cast<edt::shader_source_document*>(&open)) out.push_back(found);
		});
		return out;
	}

	edt::shader_source_document* shader_open()
	{
		const std::vector<edt::shader_source_document*> all = shaders_open();
		return all.empty() ? nullptr : all.front();
	}

	edt::desc_file_layout shader_layout()
	{
		edt::desc_file_layout out = layout();
		if (!lower_layer.empty()) out.layers = [this] { return std::vector<fs::path>{ lower_layer, content }; };
		return out;
	}

	void run(edt::autosave_recovery::ports ports)
	{
		recovery = std::make_unique<edt::autosave_recovery>(std::move(ports));
		recovery->start();
	}
	void run() { run(recovery_ports()); }

	void answer(std::string_view option)
	{
		BOOST_REQUIRE(!asked.empty());
		asked_question question = std::move(asked.front());
		asked.erase(asked.begin());
		for (std::size_t index = 0; index < question.options.size(); ++index) {
			if (question.options[index] == option) {
				question.answer(index);
				return;
			}
		}
		BOOST_FAIL("no such option");
	}

	// The probe another editor would make: true when the lock of `session` can be taken now.
	bool lock_is_free(const std::string& session) const
	{
		const fs::path relative = fs::path(edt::autosave_session_folder(session)) / "session.lock";
		const platform::lock_attempt attempt = platform::file_lock::acquire(cache, relative, platform::lock_open::existing_only);
		return attempt.outcome == platform::lock_outcome::acquired;
	}

	std::vector<std::byte> own_payload(std::string_view kind, std::string_view source) const
	{
		const std::string file = edt::autosave_file_of(autosave->session(), kind, source.empty() ? res::tag{} : res::tag::make(source));
		return bytes_of(read_file(cache / file));
	}

	std::optional<edt::autosave_meta> own_meta(std::string_view kind, std::string_view source) const
	{
		const std::string file = edt::autosave_file_of(autosave->session(), kind, source.empty() ? res::tag{} : res::tag::make(source));
		return edt::parse_autosave_meta(read_file(cache / edt::autosave_meta_of(file)));
	}

	test_util::temp_dir dir{ "edt_autosave_recovery" };
	const fs::path content = dir.path() / "content";
	const fs::path cache = dir.path() / "cache";
	std::shared_ptr<scn::hot_reload_manager> hot;
	ecs::system_factory sf;
	std::shared_ptr<scn::context_manager> mgr;
	edt::editor_services services;
	edt::editor_services mesh_services;
	edt::editor_events events;
	edt::scene_document state;
	edt::scene_editor scene_ed{ state, services };
	edt::world_controller world_ctrl{ state, services };
	edt::level_controller level_ctrl{ state, services, scene_ed, world_ctrl };
	edt::document_manager documents{ state, events };
	edt::problem_list problems;
	edt::autosave_component::clock::time_point now{ std::chrono::hours(1) };
	std::unique_ptr<edt::autosave_component> autosave;
	std::unique_ptr<edt::prefab_document> prefab;
	std::unique_ptr<edt::mesh_document> mesh;
	fs::path lower_layer;
	std::unique_ptr<edt::autosave_recovery> recovery;
	std::vector<asked_question> asked;
	std::vector<std::string> messages;
	int scene_restores = 0;
	int other_restores = 0;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtAutosaveRecovery, recovery_fixture)

BOOST_AUTO_TEST_CASE(TheOwnSession_IsMadeBeforeTheFirstRecord_AndACleanExitLeavesNoFolder)
{
	const fs::path folder = cache / edt::autosave_session_folder(autosave->session());
	BOOST_TEST(fs::exists(folder / "session.lock"));
	const std::optional<edt::autosave_session_info> info = edt::parse_autosave_session(read_file(folder / "session.json"));
	BOOST_REQUIRE(info.has_value());
	BOOST_TEST(info->session == autosave->session());
	BOOST_TEST(info->host == edt::this_host_name());
	BOOST_TEST(info->locked == autosave->session_locked());
	BOOST_TEST(!fs::exists(folder / "scene"));

	// A level has nothing to capture without its working copy's tag.
	state.editor_tag = tag_of("level.desc");
	state.commit("edit", edt::commit_mode::state_only);
	now += 1s;
	autosave->tick();
	now += 61s;
	autosave->tick();
	BOOST_TEST(fs::exists(folder / "scene" / "untitled"));

	autosave->shutdown();
	BOOST_TEST(!fs::exists(folder));
}

BOOST_AUTO_TEST_CASE(FormatOneKindFolders_AreRemovedAtStartup_OtherNamesStay)
{
	write_file(cache / "autosave" / "scene" / "untitled", "{}");
	write_file(cache / "autosave" / "data" / "x.desc", "{}");
	write_file(cache / "autosave" / "notes" / "keep.txt", "keep");
	autosave = make_autosave();
	autosave->tick();
	BOOST_TEST(!fs::exists(cache / "autosave" / "scene"));
	BOOST_TEST(!fs::exists(cache / "autosave" / "data"));
	BOOST_TEST(fs::exists(cache / "autosave" / "notes" / "keep.txt"));
}

BOOST_AUTO_TEST_CASE(AHeldLock_IsLive_NothingIsOfferedOrTouched)
{
	const std::string other = other_session();
	const std::string file = put_record(other, "data", "settings.desc", bytes_of("{}"));
	platform::lock_attempt held = platform::file_lock::acquire(cache, fs::path(edt::autosave_session_folder(other)) / "session.lock");
	BOOST_REQUIRE((held.outcome == platform::lock_outcome::acquired));

	run();
	BOOST_TEST(asked.empty());
	BOOST_TEST(fs::exists(cache / file));
	BOOST_TEST(fs::exists(cache / edt::autosave_meta_of(file)));
}

BOOST_AUTO_TEST_CASE(AnUnheldLock_IsDead_AndOffered)
{
	const std::string other = other_session();
	put_record(other, "data", "settings.desc", bytes_of("{}"));
	run();
	BOOST_REQUIRE(asked.size() == 1u);
	BOOST_TEST(asked[0].title == std::string(edt::autosave_recovery::k_title));
	BOOST_TEST(asked[0].options[0] == std::string(edt::autosave_recovery::k_restore));
	// Held by the scanner while the question is open.
	BOOST_TEST(!lock_is_free(other));
}

BOOST_AUTO_TEST_CASE(AMissingLock_InAFreshFolder_IsAnEditorStarting_InAnOldOneItIsOffered)
{
	const std::string other = other_session(false);
	put_record(other, "data", "settings.desc", bytes_of("{}"));
	run();
	BOOST_TEST(asked.empty());
	BOOST_TEST(!fs::exists(cache / edt::autosave_session_folder(other) / "session.lock"));

	fs::last_write_time(cache / edt::autosave_session_folder(other), fs::file_time_type::clock::now() - 10min);
	run();
	BOOST_TEST(asked.size() == 1u);
}

// A lock file and no session.json yet in a young folder: an editor between its lock and its session.json.
BOOST_AUTO_TEST_CASE(ALockFileWithoutSessionJson_InAFreshFolder_IsAnEditorStarting_InAnOldOneItIsDead)
{
	const std::string other = other_session();
	const fs::path folder = cache / edt::autosave_session_folder(other);
	fs::remove(folder / "session.json");
	put_record(other, "data", "settings.desc", bytes_of("{}"));
	run();
	BOOST_TEST(asked.empty());
	BOOST_TEST(fs::exists(folder / "session.lock"));
	BOOST_TEST(lock_is_free(other));

	fs::last_write_time(folder, fs::file_time_type::clock::now() - 10min);
	run();
	BOOST_REQUIRE(asked.size() == 1u);
	BOOST_TEST(asked[0].title == std::string(edt::autosave_recovery::k_title));
}

// A folder not listed whole may hold records the scan never saw: Discard all leaves it, with its lock file.
BOOST_AUTO_TEST_CASE(ASessionNotReadWhole_IsNeverRemoved_DiscardTakesOnlyWhatItRead)
{
	const std::string other = other_session();
	const fs::path folder = cache / edt::autosave_session_folder(other);
	for (int index = 0; index < 70; ++index) write_file(folder / std::format("extra{}.txt", index), "x");
	run();
	BOOST_REQUIRE(asked.size() == 1u);
	BOOST_TEST(asked[0].text.find("was not fully read") != std::string::npos);
	answer(edt::autosave_recovery::k_discard);
	BOOST_TEST(fs::exists(folder));
	BOOST_TEST(fs::exists(folder / "session.lock"));
	BOOST_TEST(fs::exists(folder / "session.json"));
	BOOST_TEST(lock_is_free(other));
}

#if !defined(_WIN32)
BOOST_AUTO_TEST_CASE(AKindFolderThatCannotBeListed_KeepsTheSession)
{
	if (::geteuid() == 0) {
		BOOST_TEST_MESSAGE("running as root: a folder without permissions is still listed");
		return;
	}
	const std::string other = other_session();
	const fs::path folder = cache / edt::autosave_session_folder(other);
	put_record(other, "data", "gone.desc", bytes_of("{}"));
	put_record(other, "prefab", "p.prefab.desc", bytes_of("{}"));
	fs::permissions(folder / "prefab", fs::perms::none);
	run();
	BOOST_REQUIRE(asked.size() == 1u);
	answer(edt::autosave_recovery::k_discard);
	fs::permissions(folder / "prefab", fs::perms::owner_all);
	BOOST_TEST(fs::exists(folder / "prefab" / "p.prefab.desc"));
	BOOST_TEST(fs::exists(folder / "session.lock"));
	BOOST_TEST(!fs::exists(cache / edt::autosave_file_of(other, "data", res::tag::make("gone.desc"))));
}
#endif

BOOST_AUTO_TEST_CASE(ALockFileSystemWithoutLocks_IsUnverifiable_LaterFirst_AndLaterDeletesNothing)
{
	const std::string other = other_session();
	const std::string file = put_record(other, "data", "settings.desc", bytes_of("{}"));
	edt::autosave_recovery::ports ports = recovery_ports();
	ports.lock = [](const fs::path&, const fs::path&, platform::lock_open) {
		platform::lock_attempt attempt;
		attempt.outcome = platform::lock_outcome::unsupported;
		attempt.error = "no locks here";
		return attempt;
	};
	run(std::move(ports));
	BOOST_REQUIRE(asked.size() == 1u);
	BOOST_TEST(asked[0].title == std::string(edt::autosave_recovery::k_title_unverifiable));
	BOOST_TEST(asked[0].options[0] == std::string(edt::autosave_recovery::k_later));
	BOOST_TEST(asked[0].text.find("cannot tell whether that editor still runs") != std::string::npos);
	asked[0].answer(0);
	BOOST_TEST(fs::exists(cache / file));
	BOOST_TEST(fs::exists(cache / edt::autosave_session_folder(other) / "session.json"));
}

BOOST_AUTO_TEST_CASE(AnotherHost_OrASessionWithoutALock_IsUnverifiable)
{
	const std::string remote_session = other_session(true, "build-machine-7");
	put_record(remote_session, "data", "settings.desc", bytes_of("{}"));
	run();
	BOOST_REQUIRE(asked.size() == 1u);
	BOOST_TEST(asked[0].options[0] == std::string(edt::autosave_recovery::k_later));
	BOOST_TEST(asked[0].text.find("may still be running on build-machine-7") != std::string::npos);
	asked[0].answer(std::nullopt);
	asked.clear();
	fs::remove_all(cache / edt::autosave_session_folder(remote_session));

	const std::string unlocked = other_session(true, edt::this_host_name(), false);
	put_record(unlocked, "data", "settings.desc", bytes_of("{}"));
	run();
	BOOST_REQUIRE(asked.size() == 1u);
	BOOST_TEST(asked[0].title == std::string(edt::autosave_recovery::k_title_unverifiable));
}

BOOST_AUTO_TEST_CASE(TwoScanners_OfOneDeadSession_TheSecondSeesItLive)
{
	const std::string other = other_session();
	put_record(other, "data", "settings.desc", bytes_of("{}"));
	run();
	BOOST_REQUIRE(asked.size() == 1u);

	std::vector<asked_question> first = std::move(asked);
	asked.clear();
	edt::autosave_recovery second(recovery_ports());
	second.start();
	BOOST_TEST(asked.empty());
	first[0].answer(std::nullopt);
}

BOOST_AUTO_TEST_CASE(Later_KeepsEverything_AndLetsGoOfTheLock)
{
	const std::string other = other_session();
	const std::string file = put_record(other, "data", "settings.desc", bytes_of("{}"));
	run();
	answer(edt::autosave_recovery::k_later);
	BOOST_TEST(fs::exists(cache / file));
	BOOST_TEST(lock_is_free(other));
	BOOST_TEST(!recovery->busy());
}

BOOST_AUTO_TEST_CASE(DiscardAll_RemovesTheDeadSessionsFolder)
{
	const std::string other = other_session();
	put_record(other, "data", "missing.desc", bytes_of("{}"));
	run();
	BOOST_REQUIRE(asked.size() == 1u);
	BOOST_TEST(asked[0].text.find("source missing: can only be discarded") != std::string::npos);
	answer(edt::autosave_recovery::k_discard);
	BOOST_TEST(!session_exists(other));
	BOOST_TEST(session_exists(autosave->session()));
}

BOOST_AUTO_TEST_CASE(Headless_AsksNothing_KeepsTheFiles_AndLetsGoOfTheLock)
{
	const std::string other = other_session();
	const std::string file = put_record(other, "data", "settings.desc", bytes_of("{}"));
	edt::autosave_recovery::ports ports = recovery_ports();
	ports.headless = true;
	run(std::move(ports));
	BOOST_TEST(asked.empty());
	BOOST_TEST(fs::exists(cache / file));
	BOOST_TEST(lock_is_free(other));
}

BOOST_AUTO_TEST_CASE(TheRecordsThatFailACheck_AreRejected_WithTheirReason)
{
	const std::string other = other_session();
	const std::vector<std::byte> payload = bytes_of("{\"a\":1}");
	const std::string text = "{\"a\":1}";
	// Wrong payload name.
	put_raw(other, "data", "other.desc", text, edt::autosave_meta_text(meta_of(other, "data", "named.desc", payload)));
	// Another session in the meta.
	put_raw(other, "data", "b.desc", text, edt::autosave_meta_text(meta_of(edt::autosave_new_session_id(), "data", "b.desc", payload)));
	// Another kind in the meta than the folder.
	put_raw(other, "data", "c.prefab.desc", text, edt::autosave_meta_text(meta_of(other, "prefab", "c.prefab.desc", payload)));
	// A meta over 64 KiB.
	put_raw(other, "data", "d.desc", text, edt::autosave_meta_text(meta_of(other, "data", "d.desc", payload)) + std::string(70 << 10, ' '));
	// A size and a hash that are not the payload's.
	edt::autosave_meta sized = meta_of(other, "data", "e.desc", payload);
	sized.payload_size += 1;
	put_raw(other, "data", "e.desc", text, edt::autosave_meta_text(sized));
	edt::autosave_meta hashed = meta_of(other, "data", "f.desc", payload);
	hashed.payload_sha256 = std::string(64, '0');
	put_raw(other, "data", "f.desc", text, edt::autosave_meta_text(hashed));
	// A folder of no known kind.
	put_raw(other, "texture", "g.desc", text, edt::autosave_meta_text(meta_of(other, "texture", "g.desc", payload)));
	// A source outside res://.
	put_raw(other, "data", "%2E%2E%2Fh.desc", text, edt::autosave_meta_text(meta_of(other, "data", "../h.desc", payload)));
	// A folder whose name is no session.
	put_raw("not-a-session", "data", "i.desc", text, edt::autosave_meta_text(meta_of("not-a-session", "data", "i.desc", payload)));
	write_file(cache / "autosave" / "not-a-session" / "session.lock", "");
#if !defined(_WIN32)
	const std::string linked = put_record(other, "data", "j.desc", payload);
	fs::remove(cache / edt::autosave_meta_of(linked));
	write_file(dir.path() / "outside.json", edt::autosave_meta_text(meta_of(other, "data", "j.desc", payload)));
	fs::create_symlink(dir.path() / "outside.json", cache / edt::autosave_meta_of(linked));
	const std::string linked_payload = put_record(other, "data", "k.desc", payload);
	fs::remove(cache / linked_payload);
	write_file(dir.path() / "outside.desc", text);
	fs::create_symlink(dir.path() / "outside.desc", cache / linked_payload);
#endif

	const edt::recovery_scan_result scan = edt::scan_autosave_sessions(scan_options());
	BOOST_REQUIRE(scan.sessions.size() == 1u);
	const edt::recovery_session& session = scan.sessions[0];
	BOOST_TEST(session.id == other);
	BOOST_TEST(session.records.empty());
	std::vector<std::string> reasons;
	for (const edt::recovery_rejected& rejected : session.rejected) reasons.push_back(rejected.reason);
	const auto has = [&reasons](std::string_view part) {
		for (const std::string& reason : reasons) {
			if (reason.find(part) != std::string::npos) return true;
		}
		return false;
	};
	BOOST_TEST(has("file name is not the one of its source"));
	BOOST_TEST(has("is not the folder's"));
	BOOST_TEST(has("larger than 64 KiB"));
	BOOST_TEST(has("size is not the meta's"));
	BOOST_TEST(has("SHA-256 is not the meta's"));
	BOOST_TEST(has("not a folder of a known kind"));
	BOOST_TEST(has("not a path inside res://"));
#if !defined(_WIN32)
	BOOST_TEST(has("the meta is not a regular file"));
	BOOST_TEST(has("the autosave is not a regular file"));
	BOOST_TEST(reasons.size() == 10u);
#else
	BOOST_TEST(reasons.size() == 8u);
#endif
}

BOOST_AUTO_TEST_CASE(MoreThan256Records_TheRestAreCountedNotRead)
{
	const std::string other = other_session();
	for (int index = 0; index < 260; ++index) put_record(other, "data", std::format("d/{}.desc", index), bytes_of("{}"));
	const edt::recovery_scan_result scan = edt::scan_autosave_sessions(scan_options());
	BOOST_REQUIRE(scan.sessions.size() == 1u);
	BOOST_TEST(scan.sessions[0].records.size() + scan.sessions[0].rejected.size() == 256u);
	BOOST_TEST(scan.sessions[0].over_limit == 4u);
}

// Review of #1342: Discard all takes only the records it read; those past a limit stay for the next start.
BOOST_AUTO_TEST_CASE(DiscardAll_OfASessionWithRecordsPastTheLimit_KeepsTheUnreadOnes)
{
	const std::string other = other_session();
	for (int index = 0; index < 260; ++index) put_record(other, "data", std::format("d/{}.desc", index), bytes_of("{}"));
	run();
	BOOST_REQUIRE(asked.size() == 1u);
	answer(edt::autosave_recovery::k_discard);
	BOOST_TEST(session_exists(other));
	std::size_t metas = 0;
	for (const auto& entry : fs::directory_iterator(cache / "autosave" / other / "data")) {
		if (entry.path().filename().string().ends_with(".autosave.json")) ++metas;
	}
	BOOST_TEST(metas == 4u);
	BOOST_TEST(lock_is_free(other));
}

BOOST_AUTO_TEST_CASE(TheQuestion_ShowsTextFromFilesAsData_AndAChangedSource)
{
	write_file(content / "settings.desc", R"({"__type":"test.recovery_settings","speed":2.0})");
	const std::string other = other_session();
	const std::string long_source = std::string(220, 'x') + ".desc";
	put_record(other, "data", long_source, bytes_of("{}"), "2026-11-24T10:00:00Z");
	const std::vector<std::byte> payload = bytes_of("{}");
	edt::autosave_meta changed = meta_of(other, "data", "settings.desc", payload, "2026\x01-11");
	changed.source_size = 3;
	changed.source_sha256 = std::string(64, '0');
	const std::string file = edt::autosave_file_of(other, "data", res::tag::make("settings.desc"));
	put_raw(other, "data", edt::autosave_leaf_of(file), "{}", edt::autosave_meta_text(changed));
	run();
	BOOST_REQUIRE(asked.size() == 1u);
	const std::string& text = asked[0].text;
	BOOST_TEST(text.find(std::string(200, 'x') + "...") != std::string::npos);
	BOOST_TEST(text.find(std::string(201, 'x')) == std::string::npos);
	BOOST_TEST(text.find("2026\\x01-11") != std::string::npos);
	BOOST_TEST(text.find('\x01') == std::string::npos);
	BOOST_TEST(text.find("file changed on disk since the autosave") != std::string::npos);

	BOOST_TEST(edt::recovery_shown_text(std::string(300, 'y')) == std::string(200, 'y') + "...");
	BOOST_TEST(edt::recovery_shown_text("a\nb\x7f\xff") == "a\\x0Ab\\x7F\\xFF");
}

BOOST_AUTO_TEST_CASE(AnUntitledLevel_IsRestoredDirty_AndItsAutosaveMovesIntoThisSession)
{
	const std::vector<std::byte> payload = bytes_of(level_text("Barrel"));
	const std::string other = other_session();
	put_record(other, "scene", "", payload);
	const std::set<fs::path> before = folders_below(content);
	run();
	answer(edt::autosave_recovery::k_restore);
	BOOST_TEST(messages.empty());
	BOOST_TEST(state.is_dirty());
	BOOST_TEST(!state.level_tag.is_valid());
	BOOST_REQUIRE(!state.world_descs.empty());
	BOOST_TEST(state.world_descs[0].get_root().children[0].name == "Barrel");
	BOOST_TEST((folders_below(content) == before));
	BOOST_TEST((own_payload("scene", "") == payload));
	const std::optional<edt::autosave_meta> meta = own_meta("scene", "");
	BOOST_REQUIRE(meta.has_value());
	BOOST_TEST(meta->session == autosave->session());
	BOOST_TEST(meta->host == edt::this_host_name());
	BOOST_TEST(!session_exists(other));
	BOOST_TEST(autosave->written_file(state.id()) == edt::autosave_file_of(autosave->session(), "scene", res::tag{}));
}

BOOST_AUTO_TEST_CASE(ALevelWithASource_IsRestoredDirty_ItsFileUnchanged)
{
	write_file(content / "levels" / "a.desc", level_text("Crate"));
	const std::string on_disk = read_file(content / "levels" / "a.desc");
	const std::vector<std::byte> payload = bytes_of(level_text("Barrel"));
	const std::string other = other_session();
	put_record(other, "scene", "levels/a.desc", payload);
	run();
	answer(edt::autosave_recovery::k_restore);
	BOOST_TEST(messages.empty());
	BOOST_TEST(state.is_dirty());
	BOOST_TEST((state.level_tag == res::tag::make("levels/a.desc")));
	BOOST_REQUIRE(!state.world_descs.empty());
	BOOST_TEST(state.world_descs[0].get_root().children[0].name == "Barrel");
	BOOST_TEST(read_file(content / "levels" / "a.desc") == on_disk);
	BOOST_TEST((own_payload("scene", "levels/a.desc") == payload));
	BOOST_TEST(!session_exists(other));
}

BOOST_AUTO_TEST_CASE(OnlyTheNewestLevel_IsRestored_TheOthersAreKept)
{
	write_file(content / "levels" / "a.desc", level_text("Crate"));
	write_file(content / "levels" / "b.desc", level_text("Crate"));
	const std::string older = other_session();
	const std::string kept = put_record(older, "scene", "levels/a.desc", bytes_of(level_text("Old")), "2026-11-24T09:00:00Z");
	const std::string newer = other_session();
	put_record(newer, "scene", "levels/b.desc", bytes_of(level_text("New")), "2026-11-24T11:00:00Z");
	run();
	BOOST_REQUIRE(asked.size() == 1u);
	BOOST_TEST(asked[0].text.find("only one level can be open") != std::string::npos);
	answer(edt::autosave_recovery::k_restore);
	BOOST_TEST(scene_restores == 1);
	BOOST_TEST(state.world_descs[0].get_root().children[0].name == "New");
	BOOST_TEST(fs::exists(cache / kept));
	BOOST_TEST(session_exists(older));
	BOOST_TEST(!session_exists(newer));
	BOOST_TEST(lock_is_free(older));
}

BOOST_AUTO_TEST_CASE(AKindThatDoesNotFitItsSource_IsNotRestored)
{
	write_file(content / "shaders" / "a.glsl", "void main() {}");
	write_file(content / "prefabs" / "p.prefab.desc", k_prefab);
	const std::string other = other_session();
	const std::string glsl = put_record(other, "scene", "shaders/a.glsl", bytes_of(level_text("X")));
	const std::string prefab_file = put_record(other, "scene", "prefabs/p.prefab.desc", bytes_of(level_text("Y")));
	run();
	BOOST_REQUIRE(asked.size() == 1u);
	BOOST_TEST(asked[0].text.find("its source is not a level file") != std::string::npos);
	answer(edt::autosave_recovery::k_restore);
	BOOST_TEST(scene_restores == 0);
	BOOST_TEST(fs::exists(cache / glsl));
	BOOST_TEST(fs::exists(cache / prefab_file));
	BOOST_TEST(session_exists(other));
}

BOOST_AUTO_TEST_CASE(AMissingSource_IsNotRestored_DiscardRemovesIt)
{
	const std::string other = other_session();
	const std::string file = put_record(other, "data", "gone.desc", bytes_of(R"({"__type":"test.recovery_settings"})"));
	run();
	answer(edt::autosave_recovery::k_restore);
	BOOST_TEST(fs::exists(cache / file));

	run();
	answer(edt::autosave_recovery::k_discard);
	BOOST_TEST(!session_exists(other));
}

BOOST_AUTO_TEST_CASE(APrefab_IsRestoredDirty_ItsFileUnchanged)
{
	write_file(content / "prefabs" / "a.prefab.desc", k_prefab);
	const std::string on_disk = read_file(content / "prefabs" / "a.prefab.desc");
	std::vector<std::byte> payload;
	{
		edt::prefab_document edited(services);
		edited.assign_id(71);
		edited.open(res::tag::make("prefabs/a.prefab.desc"));
		BOOST_REQUIRE(edited.is_open());
		BOOST_REQUIRE(edited.edit([](scn::prefab_desc& working) {
			working.get_root().children[0].name = "leg";
			return true;
		}));
		payload = edited.autosave_capture().bytes;
		edited.close();
	}
	BOOST_REQUIRE(!payload.empty());
	const std::string other = other_session();
	put_record(other, "prefab", "prefabs/a.prefab.desc", payload);
	run();
	answer(edt::autosave_recovery::k_restore);
	BOOST_TEST(messages.empty());
	BOOST_REQUIRE(prefab != nullptr);
	BOOST_TEST(prefab->is_dirty());
	BOOST_TEST(prefab->working()->get_root().children[0].name == "leg");
	BOOST_TEST((prefab->autosave_capture().bytes == payload));
	BOOST_TEST(read_file(content / "prefabs" / "a.prefab.desc") == on_disk);
	BOOST_TEST((own_payload("prefab", "prefabs/a.prefab.desc") == payload));
	BOOST_TEST(own_meta("prefab", "prefabs/a.prefab.desc").value_or(edt::autosave_meta{}).session == autosave->session());
	BOOST_TEST(!session_exists(other));
}

BOOST_AUTO_TEST_CASE(AMesh_IsRestoredDirty_ItsFileUnchanged)
{
	const edt::mesh::shape_asset_result box = edt::mesh::build_shape_asset(edt::mesh::box_params{}, res::tag{ "res://shapes/crate.geom.desc" });
	BOOST_REQUIRE_MESSAGE(box.files.has_value(), box.error);
	write_file(content / "shapes" / "crate.poly.desc", box.files->poly);
	const std::string on_disk = read_file(content / "shapes" / "crate.poly.desc");
	std::vector<std::byte> payload;
	{
		edt::mesh_document edited(mesh_services);
		edited.assign_id(91);
		edited.set_files(layout());
		edited.open(res::tag::make("shapes/crate.poly.desc"));
		BOOST_REQUIRE(edited.is_open());
		BOOST_REQUIRE(edited.edit([](edt::mesh::poly_mesh& working) {
			working.set_position(working.vertices().begin()->first, glm::vec3{ 5.f, 5.f, 5.f });
			return true;
		}));
		payload = edited.autosave_capture().bytes;
		edited.close();
	}
	BOOST_REQUIRE(!payload.empty());
	const std::string other = other_session();
	put_record(other, "mesh", "shapes/crate.poly.desc", payload);
	run();
	answer(edt::autosave_recovery::k_restore);
	BOOST_TEST(messages.empty());
	BOOST_REQUIRE(mesh != nullptr);
	BOOST_TEST(mesh->is_dirty());
	BOOST_TEST((mesh->autosave_capture().bytes == payload));
	BOOST_TEST(read_file(content / "shapes" / "crate.poly.desc") == on_disk);
	BOOST_TEST((own_payload("mesh", "shapes/crate.poly.desc") == payload));
	BOOST_TEST(!session_exists(other));
}

namespace {
	constexpr const char* k_shader_on_disk = "\xEF\xBB\xBF#version 450 core\r\nvoid main()\r\n{\r\n}\r\n";
	constexpr const char* k_shader_autosaved = "\xEF\xBB\xBF#version 450 core\r\nvoid main()\r\n{\r\n\tgl_Position = vec4(0.0);\r\n}\r\n";
}

BOOST_AUTO_TEST_CASE(AShaderSource_IsRestoredDirty_ItsFileUnchanged_AndKeepsItsBomAndCrlfOnSave)
{
	write_file(content / "shaders" / "a.vert", k_shader_on_disk);
	const std::string other = other_session();
	const std::vector<std::byte> payload = bytes_of(k_shader_autosaved);
	edt::autosave_meta meta = meta_of(other, "shader_source", "shaders/a.vert", payload);
	meta.source_size = std::string_view(k_shader_on_disk).size();
	meta.source_sha256 = cmn::sha256_hex(bytes_of(k_shader_on_disk));
	const std::string file = edt::autosave_file_of(other, "shader_source", res::tag::make("shaders/a.vert"));
	put_raw(other, "shader_source", edt::autosave_leaf_of(file), k_shader_autosaved, edt::autosave_meta_text(meta));
	run();
	answer(edt::autosave_recovery::k_restore);
	BOOST_TEST(messages.empty());
	edt::shader_source_document* shader = shader_open();
	BOOST_REQUIRE(shader != nullptr);
	BOOST_TEST(shader->is_dirty());
	BOOST_TEST(!shader->changed_on_disk());
	BOOST_TEST(shader->text() == "#version 450 core\nvoid main()\n{\n\tgl_Position = vec4(0.0);\n}\n");
	BOOST_TEST(read_file(content / "shaders" / "a.vert") == k_shader_on_disk);
	BOOST_TEST((shader->autosave_capture().bytes == bytes_of(k_shader_autosaved)));
	BOOST_TEST((own_payload("shader_source", "shaders/a.vert") == bytes_of(k_shader_autosaved)));
	BOOST_TEST(!session_exists(other));
	BOOST_REQUIRE(shader->save().saved);
	BOOST_TEST(read_file(content / "shaders" / "a.vert") == k_shader_autosaved);
}

BOOST_AUTO_TEST_CASE(AShaderSource_WhoseFileChangedSinceTheAutosave_IsRestoredAsADiskConflict)
{
	write_file(content / "shaders" / "a.frag", "void main() {}\n");
	const std::string other = other_session();
	const std::vector<std::byte> payload = bytes_of("void main() { discard; }\n");
	edt::autosave_meta changed = meta_of(other, "shader_source", "shaders/a.frag", payload);
	changed.source_size = 3;
	changed.source_sha256 = std::string(64, '0');
	const std::string file = edt::autosave_file_of(other, "shader_source", res::tag::make("shaders/a.frag"));
	put_raw(other, "shader_source", edt::autosave_leaf_of(file), "void main() { discard; }\n", edt::autosave_meta_text(changed));
	run();
	answer(edt::autosave_recovery::k_restore);
	BOOST_TEST(messages.empty());
	edt::shader_source_document* shader = shader_open();
	BOOST_REQUIRE(shader != nullptr);
	BOOST_TEST(shader->is_dirty());
	BOOST_TEST(shader->changed_on_disk());
	BOOST_TEST(read_file(content / "shaders" / "a.frag") == "void main() {}\n");
	BOOST_TEST(shader->save().saved == false);
	BOOST_TEST(shader->revert().empty());
	BOOST_TEST(!shader->is_dirty());
	BOOST_TEST(shader->text() == "void main() {}\n");
}

BOOST_AUTO_TEST_CASE(AShaderSource_WhoseConflictIsOverwritten_WritesTheRestoredText)
{
	write_file(content / "shaders" / "a.frag", "void main() {}\n");
	const std::string other = other_session();
	const std::vector<std::byte> payload = bytes_of("void main() { discard; }\n");
	edt::autosave_meta changed = meta_of(other, "shader_source", "shaders/a.frag", payload);
	changed.source_size = 3;
	changed.source_sha256 = std::string(64, '0');
	const std::string file = edt::autosave_file_of(other, "shader_source", res::tag::make("shaders/a.frag"));
	put_raw(other, "shader_source", edt::autosave_leaf_of(file), "void main() { discard; }\n", edt::autosave_meta_text(changed));
	run();
	answer(edt::autosave_recovery::k_restore);
	edt::shader_source_document* shader = shader_open();
	BOOST_REQUIRE(shader != nullptr);
	BOOST_REQUIRE(shader->changed_on_disk());
	BOOST_REQUIRE(shader->overwrite().saved);
	BOOST_TEST(read_file(content / "shaders" / "a.frag") == "void main() { discard; }\n");
	BOOST_TEST(!shader->changed_on_disk());
}

BOOST_AUTO_TEST_CASE(AShaderSourceAutosave_OfARunBeforeTheFix_WithoutASourceHash_IsRestoredAsADiskConflict)
{
	write_file(content / "shaders" / "a.frag", "void main() {}\n");
	const std::string other = other_session();
	put_record(other, "shader_source", "shaders/a.frag", bytes_of("void main() { discard; }\n"));
	run();
	answer(edt::autosave_recovery::k_restore);
	BOOST_TEST(messages.empty());
	edt::shader_source_document* shader = shader_open();
	BOOST_REQUIRE(shader != nullptr);
	BOOST_TEST(shader->is_dirty());
	BOOST_TEST(shader->changed_on_disk());
	BOOST_TEST(!session_exists(other));
}

BOOST_AUTO_TEST_CASE(AShaderSource_AlreadyOpenWithUnsavedChanges_IsNotRestoredOver_ItsRecordIsKept)
{
	write_file(content / "shaders" / "a.frag", "void main() {}\n");
	auto made = std::make_unique<edt::shader_source_document>(services);
	made->set_files(shader_layout());
	made->open(res::tag::make("shaders/a.frag"));
	BOOST_REQUIRE(made->is_open());
	auto& open = dynamic_cast<edt::shader_source_document&>(documents.adopt(std::move(made)));
	BOOST_REQUIRE(open.set_text("// the person's edit\n"));
	const std::string other = other_session();
	const std::string file = put_record(other, "shader_source", "shaders/a.frag", bytes_of("void main() { discard; }\n"));
	run();
	answer(edt::autosave_recovery::k_restore);
	BOOST_REQUIRE(messages.size() == 1u);
	BOOST_TEST(messages[0].find("unsaved changes") != std::string::npos);
	BOOST_TEST(open.text() == "// the person's edit\n");
	BOOST_TEST(fs::exists(cache / file));
}

BOOST_AUTO_TEST_CASE(RestoreShaderSource_WhenTheShaderDoesNotOpen_SaysSo)
{
	const edt::shader_restore_result result = edt::restore_shader_source(documents, [](const res::tag&) { return false; },
		res::tag::make("shaders/a.frag"), bytes_of("x"), false);
	BOOST_TEST(result.error == "the shader did not open");
	const edt::shader_restore_result unfound = edt::restore_shader_source(documents, [](const res::tag&) { return true; },
		res::tag::make("shaders/a.frag"), bytes_of("x"), false);
	BOOST_TEST(unfound.error == "the shader did not open");
}

BOOST_AUTO_TEST_CASE(AShaderSourceWithTypingNotHandedOver_IsAutosavedWithTheTypedText_WithoutFlushing)
{
	write_file(content / "shaders" / "a.frag", "void main() {}\n");
	edt::shader_source_document document(services);
	document.set_files(shader_layout());
	document.open(res::tag::make("shaders/a.frag"));
	BOOST_REQUIRE(document.is_open());
	int flushes = 0;
	document.set_edit_source([] { return true; }, [&flushes] { ++flushes; }, [] { return std::string("void main() { typed(); }\r\n"); });
	BOOST_TEST((document.autosave_capture().bytes == bytes_of("void main() { typed(); }\n")));
	BOOST_TEST(flushes == 0);
	document.set_edit_source({}, {});
}

BOOST_AUTO_TEST_CASE(AShaderSource_OfALowerLayer_IsNotRestored_ItsRecordIsKept)
{
	lower_layer = dir.path() / "lower";
	write_file(lower_layer / "shaders" / "a.frag", "void main() {}\n");
	const std::string other = other_session();
	const std::string file = put_record(other, "shader_source", "shaders/a.frag", bytes_of("void main() { discard; }\n"));
	run();
	answer(edt::autosave_recovery::k_restore);
	BOOST_REQUIRE(messages.size() == 1u);
	BOOST_TEST(messages[0].find("belongs to a layer below the project") != std::string::npos);
	BOOST_TEST(fs::exists(cache / file));
	BOOST_TEST(session_exists(other));
}

BOOST_AUTO_TEST_CASE(AShaderSourceRecord_OfAnotherKindsSource_OrOfAMissingFile_IsNotRestored)
{
	write_file(content / "a.desc", R"({"__type":"test.recovery_settings"})");
	const std::string other = other_session();
	const std::string wrong = put_record(other, "shader_source", "a.desc", bytes_of("void main() {}\n"));
	const std::string missing = put_record(other, "shader_source", "shaders/gone.frag", bytes_of("void main() {}\n"));
	run();
	BOOST_REQUIRE(asked.size() == 1u);
	BOOST_TEST(asked[0].text.find("its source is not a shader source file") != std::string::npos);
	BOOST_TEST(asked[0].text.find("source missing: can only be discarded") != std::string::npos);
	answer(edt::autosave_recovery::k_restore);
	BOOST_TEST(other_restores == 0);
	BOOST_TEST(fs::exists(cache / wrong));
	BOOST_TEST(fs::exists(cache / missing));
}

BOOST_AUTO_TEST_CASE(ADirtyShaderSource_IsWrittenByTheAutosave_AndIsKnownToTheRecovery)
{
	write_file(content / "shaders" / "a.frag", "void main() {}\n");
	auto made = std::make_unique<edt::shader_source_document>(services);
	made->set_files(shader_layout());
	made->open(res::tag::make("shaders/a.frag"));
	BOOST_REQUIRE(made->is_open());
	auto& open = dynamic_cast<edt::shader_source_document&>(documents.adopt(std::move(made)));
	BOOST_REQUIRE(open.set_text("void main() { discard; }\n"));
	now += 1s;
	autosave->tick();
	now += 61s;
	autosave->tick();
	BOOST_TEST((own_payload("shader_source", "shaders/a.frag") == bytes_of("void main() { discard; }\n")));
	BOOST_TEST(edt::is_autosave_kind("shader_source"));
}

BOOST_AUTO_TEST_CASE(ADataDocument_IsRestoredDirty_ItsFileUnchanged)
{
	write_file(content / "settings.desc", R"({"__type":"test.recovery_settings","speed":2.0})");
	const std::string on_disk = read_file(content / "settings.desc");
	std::vector<std::byte> payload;
	{
		edt::data_document edited(services, layout());
		BOOST_REQUIRE(edited.open(res::tag::make("settings.desc")).opened);
		BOOST_REQUIRE(edited.edit([](json::value& shown) {
			shown.as_object()["speed"] = 5.0;
			return true;
		}));
		payload = edited.autosave_capture().bytes;
	}
	BOOST_REQUIRE(!payload.empty());
	const std::string other = other_session();
	put_record(other, "data", "settings.desc", payload);
	run();
	answer(edt::autosave_recovery::k_restore);
	BOOST_TEST(messages.empty());
	edt::data_document* restored = nullptr;
	documents.for_each([&restored](edt::document& open) {
		if (auto* data = dynamic_cast<edt::data_document*>(&open)) restored = data;
	});
	BOOST_REQUIRE(restored != nullptr);
	BOOST_TEST(restored->is_dirty());
	BOOST_TEST((restored->autosave_capture().bytes == payload));
	BOOST_TEST(read_file(content / "settings.desc") == on_disk);
	BOOST_TEST((own_payload("data", "settings.desc") == payload));
	BOOST_TEST(!session_exists(other));
	BOOST_TEST(autosave->written_file(restored->id()) == edt::autosave_file_of(autosave->session(), "data", res::tag::make("settings.desc")));
}

BOOST_AUTO_TEST_CASE(AnOwnLockTakenByAnotherEditor_MakesItUseAnotherFolder_AndLeavesThatOneAlone)
{
	autosave.reset();
	const std::string first = edt::autosave_session_id();
	std::optional<platform::lock_attempt> theirs;
	autosave = make_autosave();
	autosave->set_lock_for_tests([&theirs](const fs::path& root, const fs::path& relative, platform::lock_open open) {
		if (!theirs) theirs = platform::file_lock::acquire(root, relative, platform::lock_open::create);
		return platform::file_lock::acquire(root, relative, open);
	});
	autosave->tick();
	BOOST_REQUIRE(theirs.has_value());
	BOOST_REQUIRE((theirs->outcome == platform::lock_outcome::acquired));
	BOOST_TEST(autosave->session() != first);
	BOOST_TEST(autosave->session_locked());
	const fs::path taken = cache / edt::autosave_session_folder(first);
	BOOST_TEST(fs::exists(taken / "session.lock"));
	BOOST_TEST(!fs::exists(taken / "session.json"));
	BOOST_TEST(!lock_is_free(first));
	BOOST_TEST(fs::exists(cache / edt::autosave_session_folder(autosave->session()) / "session.json"));
}

// The empty dead session is listed again under its lock before it goes: a record written after the scan keeps it.
BOOST_AUTO_TEST_CASE(AnEmptyDeadSession_ThatGainsARecordAfterTheScan_IsKept)
{
	write_file(content / "settings.desc", R"({"__type":"test.recovery_settings","speed":2.0})");
	const std::string first = other_session(true, edt::this_host_name(), true, std::string(31, '0') + "1");
	put_record(first, "data", "settings.desc", bytes_of(R"({"__type":"test.recovery_settings"})"));
	const std::string empty = other_session(true, edt::this_host_name(), true, std::string(32, 'f'));
	edt::autosave_recovery::ports ports = recovery_ports();
	const auto known = ports.is_document_type;
	bool planted = false;
	ports.is_document_type = [&](std::string_view type) {
		if (!planted) {
			planted = true;
			put_record(empty, "data", "late.desc", bytes_of("{}"));
		}
		return known(type);
	};
	run(std::move(ports));
	BOOST_TEST(planted);
	BOOST_TEST(session_exists(empty));
	BOOST_TEST(fs::exists(cache / edt::autosave_file_of(empty, "data", res::tag::make("late.desc"))));
	BOOST_REQUIRE(asked.size() == 1u);
	answer(edt::autosave_recovery::k_later);
}

BOOST_AUTO_TEST_CASE(AnEmptyDeadSession_IsRemoved)
{
	const std::string empty = other_session();
	run();
	BOOST_TEST(asked.empty());
	BOOST_TEST(!session_exists(empty));
}

BOOST_AUTO_TEST_CASE(ManyStartingOrFutureFolders_DoNotPushOutADeadSession)
{
	for (int index = 0; index < 70; ++index) fs::create_directories(cache / edt::autosave_session_folder(edt::autosave_new_session_id()));
	for (int index = 0; index < 3; ++index) {
		const fs::path future = cache / edt::autosave_session_folder(edt::autosave_new_session_id());
		fs::create_directories(future);
		fs::last_write_time(future, fs::file_time_type::clock::now() + 1h);
	}
	const std::string dead = other_session(true, edt::this_host_name(), true, std::string(32, 'f'));
	put_record(dead, "data", "real.desc", bytes_of("{}"));
	const edt::recovery_scan_result scan = edt::scan_autosave_sessions(scan_options());
	BOOST_TEST(scan.starting == 70u);
	BOOST_TEST(scan.unchecked_sessions == 0u);
	bool found = false;
	for (const edt::recovery_session& session : scan.sessions) {
		if (session.id == dead) found = session.records.size() == 1u;
		// A folder dated in the future is no editor starting.
		BOOST_TEST((session.state == edt::recovery_state::dead));
	}
	BOOST_TEST(found);
	BOOST_TEST(scan.sessions.size() == 4u);
}

BOOST_AUTO_TEST_CASE(PrefabMeshAndDataRecords_OfAnotherKindsSource_AreNotRestored)
{
	write_file(content / "a.desc", R"({"__type":"test.recovery_settings"})");
	write_file(content / "m.desc", R"({"__type":"test.recovery_settings"})");
	write_file(content / "p.prefab.desc", k_prefab);
	const std::string other = other_session();
	const std::string prefab_file = put_record(other, "prefab", "a.desc", bytes_of(k_prefab));
	const std::string mesh_file = put_record(other, "mesh", "m.desc", bytes_of("{}"));
	const std::string data_file = put_record(other, "data", "p.prefab.desc", bytes_of(k_prefab));
	run();
	BOOST_REQUIRE(asked.size() == 1u);
	BOOST_TEST(asked[0].text.find("its source is not a prefab file") != std::string::npos);
	BOOST_TEST(asked[0].text.find("its source is not a mesh source file") != std::string::npos);
	BOOST_TEST(asked[0].text.find("its source is not a data document file") != std::string::npos);
	answer(edt::autosave_recovery::k_restore);
	BOOST_TEST(other_restores == 0);
	BOOST_TEST(fs::exists(cache / prefab_file));
	BOOST_TEST(fs::exists(cache / mesh_file));
	BOOST_TEST(fs::exists(cache / data_file));
	BOOST_TEST(session_exists(other));
}

BOOST_AUTO_TEST_CASE(ALevelRecord_WhoseSourceIsAnotherDescType_IsNotRestored)
{
	write_file(content / "materials" / "m.desc", R"({"__type":"test.recovery_settings","speed":2.0})");
	const std::string on_disk = read_file(content / "materials" / "m.desc");
	const std::string other = other_session();
	const std::string file = put_record(other, "scene", "materials/m.desc", bytes_of(level_text("X")));
	const std::string untyped = put_record(other, "scene", "", bytes_of(R"({"__type":"test.recovery_settings"})"));
	run();
	BOOST_REQUIRE(asked.size() == 1u);
	BOOST_TEST(asked[0].text.find("its source is not a level file") != std::string::npos);
	BOOST_TEST(asked[0].text.find("the autosave is not a level") != std::string::npos);
	answer(edt::autosave_recovery::k_restore);
	BOOST_TEST(scene_restores == 0);
	BOOST_TEST(read_file(content / "materials" / "m.desc") == on_disk);
	BOOST_TEST(fs::exists(cache / file));
	BOOST_TEST(fs::exists(cache / untyped));
}

BOOST_AUTO_TEST_CASE(TheWorkingCopy_EndsWithTheKindsExtension)
{
	BOOST_TEST(edt::recovery_working_name("scene", "untitled") == "untitled.desc");
	BOOST_TEST(edt::recovery_working_name("scene", "levels%2Fa.desc") == "levels%2Fa.desc");
	BOOST_TEST(edt::recovery_working_name("prefab", "x~0123456789abcdef") == "x~0123456789abcdef.prefab.desc");
	BOOST_TEST(edt::recovery_working_name("prefab", "p%2Fa.prefab.desc") == "p%2Fa.prefab.desc");
	BOOST_TEST(edt::recovery_working_name("mesh", "m~0123456789abcdef") == "m~0123456789abcdef.poly.desc");
	BOOST_TEST(edt::recovery_working_name("data", "s~0123456789abcdef") == "s~0123456789abcdef.desc");
}

BOOST_AUTO_TEST_CASE(FilesNoMetaAccountsFor_KeepAnEmptyLookingSession)
{
	const std::string other = other_session();
	write_file(cache / edt::autosave_session_folder(other) / "scene" / ".untitled.0123456789abcdef.tmp", "{");
	const edt::recovery_scan_result scan = edt::scan_autosave_sessions(scan_options());
	BOOST_REQUIRE(scan.sessions.size() == 1u);
	BOOST_TEST(scan.sessions[0].unread == 1u);
	BOOST_TEST(!scan.sessions[0].fully_read());
}

#if !defined(_WIN32)
BOOST_AUTO_TEST_CASE(ALinkAtASessionFolderOrAtItsLock_IsNeverFollowed)
{
	const fs::path outside = dir.path() / "outside_session";
	write_file(outside / "session.lock", "");
	write_file(outside / "data" / "x.desc", "{}");
	fs::create_directories(cache / "autosave");
	const std::string linked = edt::autosave_new_session_id();
	fs::create_directory_symlink(outside, cache / edt::autosave_session_folder(linked));

	const std::string other = other_session(false);
	write_file(dir.path() / "outside.lock", "keep");
	fs::create_symlink(dir.path() / "outside.lock", cache / edt::autosave_session_folder(other) / "session.lock");
	put_record(other, "data", "settings.desc", bytes_of("{}"));

	const edt::recovery_scan_result scan = edt::scan_autosave_sessions(scan_options());
	BOOST_REQUIRE(scan.sessions.size() == 1u);
	BOOST_TEST(scan.sessions[0].id == other);
	BOOST_TEST((scan.sessions[0].state == edt::recovery_state::unverifiable));
	BOOST_TEST(read_file(dir.path() / "outside.lock") == "keep");
	BOOST_TEST(fs::exists(outside / "data" / "x.desc"));
}
#endif

BOOST_AUTO_TEST_SUITE_END()
