#include "edt_level_load_component.h"
#include "edt_desc_file_commands.h"
#include "edt_desc_location.h"
#include "edt_editor_layer.h"
#include "edt_level_controller.h"
#include "edt_scene_document.h"
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/editor/edt_editor_context.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/resource/res_system.h>
#include <imgui.h>
#include <boost/json.hpp>
#include <filesystem>
#include <utility>

namespace
{
	// A desc file that says it is something else than a level (`"__type": "material_desc"`, a prefab...). Reading it as a level
	// asserts in the desc adapter, so it is refused before. A file that is no JSON, or says nothing, fails the load as before.
	bool names_another_desc_type(const std::vector<std::byte>& bytes)
	{
		boost::json::error_code ec;
		boost::json::parse_options options;
		options.max_depth = 64;
		const boost::json::value parsed = boost::json::parse(
			std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), ec, {}, options);
		if (ec || !parsed.is_object()) return false;
		const boost::json::value* type = parsed.as_object().if_contains("__type");
		return type != nullptr && type->is_string() && std::string_view(type->as_string()) != scn::level_desc::__type;
	}
}

edt::desc_problem_publisher::locator edt::level_locator(const res::res_handle<scn::level_desc>& handle, const res::tag& copy,
	const res::tag& file)
{
	if (!handle.is_ready()) return {};
	struct located
	{
		// Kept with its worlds: their roots point into it.
		std::shared_ptr<scn::level_desc> desc;
		level_worlds worlds;
	};
	auto held = std::make_shared<located>();
	held->desc = handle.get_sync();
	if (!held->desc) return {};
	held->worlds = level_worlds_of(*held->desc, file.string(), { copy.string(), file.string() });
	return [held](const res::diagnostic& item) { return locate_desc(held->worlds, item.resource, item.json_path); };
}

edt::level_load_component::level_load_component(ports from)
	: m_ports(std::move(from))
{
}

edt::level_load_component::~level_load_component() = default;

void edt::level_load_component::install(std::vector<registration>& out)
{
	out.push_back(m_ports.context.actions().add({ .id = "level.cancel_load", .label = "Cancel Level Load",
		.menu_path = "File", .separator_before = true,
		// While nothing loads the registry answers `disabled` with this reason: a cancel that comes after the load ended changes nothing.
		.disabled_reason = [] { return std::string{ "no level is loading" }; },
		.execute_call = [this](const cmd::call& call) { return cancel(call.from); },
		.result_schema = boost::json::parse(R"json({
			"type": "object",
			"properties": { "cancelled": { "type": "boolean" }, "level": { "type": "string" } },
			"required": ["cancelled", "level"],
			"additionalProperties": false
		})json"),
		.enabled = [this] { return m_loading; },
		.category = cmd::agent_category::scene_edit }));
}

bool edt::level_load_component::begin(const res::tag& original_tag)
{
	// Through the resolver: a level in any res:// layer (a lower content root, the engine) is found.
	auto& game_resources = *m_ports.services.resources();
	std::optional<std::vector<std::byte>> level_bytes;
	if (const auto raw = game_resources.fetch_data(original_tag)) {
		raw->wait();
		if (raw->is_ready()) level_bytes = raw->data;
	}
	if (level_bytes && names_another_desc_type(*level_bytes)) {
		egLOG_ERROR("Editor/Load", "'{}' is not a level file (its __type is not '{}')", original_tag.view(), scn::level_desc::__type);
		return false;
	}

	// Another level replaces the one Play was built from.
	m_ports.stop_play();
	m_ports.level_ctrl.unwatch_level();

	std::string stem = std::filesystem::path(std::string(original_tag.relative())).stem().string();
	// A load that was never finished (begin called again): its copy goes.
	if (m_loading) abandon();
	cancel_settle();
	m_editor_tag = res::tag{ "memory://editor/loading/" + std::to_string(++m_serial) + "/" + stem + ".desc" };
	m_tag = original_tag;
	m_file_version = level_bytes ? desc_root_version(*level_bytes) : 0;

	if (level_bytes) m_ports.services.writable_resources()->store(m_editor_tag, *level_bytes);

	m_handle = game_resources.require<scn::level_desc>(m_editor_tag);
	m_loading = true;
	m_slow_load.start(slow_load_notice::clock::now());
	return true;
}

bool edt::level_load_component::begin_and_watch(const res::tag& original_tag, std::function<void(bool)> ended)
{
	if (!begin(original_tag)) return false;
	m_ports.layer.register_implicit("edt/load_level", [this, ended = std::move(ended)] {
		bool done = false;
		const bool loading = poll(&done);
		if (!loading && ended) ended(done);
		return loading;
	});
	return true;
}

void edt::level_load_component::release()
{
	m_editor_tag = res::tag{};
	m_tag = res::tag{};
	m_loading = false;
	m_handle = {};
}

void edt::level_load_component::erase_loading_copy(const res::tag& copy)
{
	constexpr std::string_view prefix = "memory://editor/loading/";
	if (!copy.is_valid() || !copy.view().starts_with(prefix)) return;
	m_ports.services.writable_resources()->erase(copy);
}

void edt::level_load_component::abandon()
{
	const res::tag copy = m_editor_tag;
	release();
	erase_loading_copy(copy);
}

cmd::result edt::level_load_component::cancel(const cmd::origin& from)
{
	// A load that finished or failed is no longer one: nothing to undo, and the level stays as that load left it.
	if (!m_loading) return cmd::result::failure(cmd::codes::failed, "no level is loading");
	const res::tag level = m_tag;
	abandon();
	m_ports.level_ctrl.resume_watch();
	const char* who = from.kind == cmd::origin_kind::agent ? "an agent" : from.kind == cmd::origin_kind::ui ? "a person"
		: from.kind == cmd::origin_kind::test ? "a test" : "the game";
	egLOG_INFO("editor", "the load of level '{}' was cancelled by {}{}", level.view(), who,
		from.session.empty() ? std::string{} : " (session " + from.session + ")");
	boost::json::object out;
	out["cancelled"] = true;
	out["level"] = boost::json::string_view(level.view().data(), level.view().size());
	return cmd::result::success(std::move(out));
}

bool edt::level_load_component::poll(bool* loaded_out)
{
	// The load was refused (not a level) or is gone: nothing to wait for.
	if (!m_loading) {
		if (loaded_out) *loaded_out = false;
		return false;
	}
	if (m_handle.has_error()) {
		scn::report_level_failed(m_tag, m_handle);
		m_ports.desc_problems.report(m_ports.context.problems(), "level", diagnostics_of(m_handle), m_editor_tag, m_tag);
		if (loaded_out) *loaded_out = false;
		abandon();
		m_ports.level_ctrl.resume_watch();
		return false;
	}

	if (!m_handle.is_ready()) {
		if (const auto waited = m_slow_load.due(slow_load_notice::clock::now())) {
			egLOG_WARN("editor", "level '{}' is still loading after {} s; level.cancel_load (File > Cancel Level Load) cancels it",
				m_tag.view(), waited->count());
		}
		ImGui::OpenPopup("Loading Level##edt_load");
		if (ImGui::BeginPopupModal("Loading Level##edt_load", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar))
		{
			if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
				ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
				if (loaded_out) *loaded_out = false;
				static_cast<void>(cancel(cmd::origin::ui()));
				ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
				return false;
			}
			ImGui::Text("Loading level...");
			ImGui::EndPopup();
		}
		return true;
	}

	ImGui::OpenPopup("Loading Level##edt_load");
	if (ImGui::BeginPopupModal("Loading Level##edt_load", nullptr,
		ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar))
	{
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	// Play cannot start during a load (start_blocker); one that runs anyway is of the level being replaced.
	m_ports.stop_play();
	const res::tag editor_tag_before = m_ports.state.editor_tag;
	const bool loaded = m_ports.level_ctrl.take_loaded_level(m_handle, m_tag, m_editor_tag, m_file_version);
	if (loaded_out) *loaded_out = loaded;
	// Warnings of a level that loaded too (an unknown component); none: the scope goes.
	m_ports.desc_problems.report(m_ports.context.problems(), "level", diagnostics_of(m_handle), m_editor_tag, m_tag,
		level_locator(m_handle, m_editor_tag, m_tag));
	if (loaded) {
		// The previous level's working copy has no user left: the level set was rebuilt from the new one.
		release();
		if (editor_tag_before != m_ports.state.editor_tag) erase_loading_copy(editor_tag_before);
	} else {
		abandon();
	}
	if (loaded) {
		m_ports.refresh_world_problems();
		start_settle(m_ports.state.level_tag);
	}
	return false;
}

void edt::level_load_component::start_settle(const res::tag& level)
{
	m_settle_contexts = m_ports.services.ctx_manager.lock();
	ASSERT_MSG(m_settle_contexts && m_ports.tasks, "a level loaded before editor_system::init");
	m_settle.emplace(*m_settle_contexts, m_ports.resources, *m_ports.tasks);
	m_settle->start(level);
}

void edt::level_load_component::cancel_settle()
{
	m_settle.reset();
	m_settle_contexts.reset();
}

void edt::level_load_component::tick_settle()
{
	if (!m_settle || !m_settle->poll()) return;
	cancel_settle();
	if (m_ports.stop_after_level_loaded) {
		egLOG_INFO("app", "exit after the level loaded (editor/exit_after_level_loaded)");
		m_ports.stop_after_level_loaded();
	}
}
