#include "edt_editor_system.h"
#include "res_system.h"
#include "level/scn_level_desc.h"
#include "rnd_render_system.h"
#include "scn_camera_component.hpp"
#include "scn_camera_controller_component.hpp"
#include "ecs_event.hpp"
#include "inp_input_system.h"
#include "edt_input_manager.h"
#include "gui_system.h"
#include <imgui.h>
#include "scn_skinning_prototype_desc.h"

#include "level/scn_world.h"
#include "level/scn_level.h"
#include "level/scn_level_manager.h"
#include "wnd_window_system.h"

#include "edt_component_renderers.h"
#include <entt/entt.hpp>
#include <level/scn_prefab_desc.h>
#include "eng_transform_3d.hpp"
#include "resources/res_resource_text.h"
#include <fstream>
#include <algorithm>
#include <boost/json.hpp>


edt::editor_system::editor_system(desc::desc_system& desc_system_, res::resource_system& res_sys, rnd::render_system& rnd_sys, gui::gui_system& gui_sys)
	: desc_system(desc_system_)
	, m_res(res_sys)
	, m_rnd(rnd_sys)
	, m_gui(gui_sys)
{
	input = std::make_shared<edt::input_manager>();
}

edt::editor_system::~editor_system()
{
}

void edt::editor_system::init(ds::app_data_storage& data)
{
	file_dialog.set_current_path(res::resource_system::get_resources_path());
	m_rnd.get_texture_manager().generate_texture(res::tag(res::tag::memory, "__black"), {1,1}, rnd::driver::texture_header::TYPE::RGB8, {0, 0, 0});
	m_rnd.get_texture_manager().generate_texture(res::tag(res::tag::memory, "__red"),   {1,1}, rnd::driver::texture_header::TYPE::RGB8, {255, 0, 0});

	editor_layer = std::make_shared<edt::editor_layer>();

	m_gui.push_layer(editor_layer);

	editor_layer->register_tool("File/Import...", [this] { return show_file_dialog(); });

	m_lvl_manager = data.require_shared<scn::level_manager>();
	m_sfactory  = &data.require<ecs::system_factory>();
	m_assembler = &data.require<scn::ecs_assembler>();
	auto& inp_sys = data.require<inp::input_system>();
	inp_sys.push_input_layer(inp_sys.get_focused_window(), inp::input_layer{ input, true });
	ecs_input = data.require_shared<inp::ecs_input_manager>();

	// Панели
	m_hierarchy_panel    = std::make_shared<scene_hierarchy_panel>();
	m_inspector_panel    = std::make_shared<inspector_panel>();
	register_desc_component_renderers(*m_inspector_panel);
	m_viewport_panel     = std::make_shared<viewport_panel>(m_rnd, m_gui);
	m_viewport_panel->set_registry_getter([this]() -> entt::registry* {
		auto lvl_mgr = m_lvl_manager.lock();
		if (!lvl_mgr || m_active_world_idx >= (int)m_world_names.size()) return nullptr;
		const auto& name = m_world_names[m_active_world_idx];
		auto& level = lvl_mgr->get_level();
		if (!level.has_world(name)) return nullptr;
		return &level.get_world(name).state();
	});
	m_console_panel      = std::make_shared<console_panel>();
	m_asset_browser_panel = std::make_shared<asset_browser_panel>();

	m_asset_browser_panel->set_base_path(res::resource_system::get_resources_path());

	m_viewport_panel->set_input_manager(input);
	m_viewport_panel->set_ecs_input_manager(ecs_input);
	m_viewport_panel->set_on_asset_dropped([this](const std::string& abs_path) {
		if (!m_editor_tag.is_valid()) return;

		auto base = res::resource_system::get_resources_path();
		auto rel  = std::filesystem::path(abs_path).lexically_relative(base);
		res::tag tag = res::tag::make(rel.string());

		auto desc_handle = m_res.require_sync<desc::desc_base>(tag);
		if (!desc_handle.is_ready()) {
			egLOG("Editor/Drop", "Failed to load desc '{}'", tag.view());
			return;
		}

		const std::string stem = rel.stem().string();
		const std::string key  = make_unique_key(stem.empty() ? "Asset" : stem);

		scn::prefab_desc::prefab_comp_node comp;
		comp.type_name   = std::string(desc_handle->get_type());
		comp.parent_desc = desc_handle;

		scn::prefab_desc::prefab_node node;
		node.name = key;
		node.components[key] = std::move(comp);

		active_world_desc().get_root().children.push_back(std::move(node));
		m_is_dirty = true;
		serialize_and_push();
	});

	// Hierarchy callbacks
	m_hierarchy_panel->set_on_node_selected([this](scn::prefab_desc::prefab_node* node) {
		m_inspector_panel->set_selected_node(node);

		// Find matching ECS entity by name for gizmo support
		auto lvl_mgr = m_lvl_manager.lock();
		if (lvl_mgr && node && m_active_world_idx < (int)m_world_names.size()) {
			auto& reg = lvl_mgr->get_level().get_world(m_world_names[m_active_world_idx]).state();
			ecs::entity found = entt::null;
			reg.view<scn::name_component>().each([&](auto ent, const scn::name_component& nc) {
				if (nc.name == node->name) found = ent;
			});
			m_viewport_panel->set_selected_entity(found);
		} else {
			m_viewport_panel->set_selected_entity(entt::null);
		}
	});

	m_hierarchy_panel->set_on_world_changed([this](int idx) {
		switch_to_world(idx);
	});

	m_hierarchy_panel->set_on_create_world([this](const std::string& name) {
		create_world(name);
	});

	m_hierarchy_panel->set_on_create_node([this](const std::string& type_name) {
		create_entity(type_name);
	});
	m_hierarchy_panel->set_on_delete_node([this](const std::string& name) {
		delete_entity(name);
	});
	m_hierarchy_panel->set_on_rename_node([this](const std::string& /*old_name*/, const std::string& /*new_name*/) {
		// node.name already mutated in place via pointer; just persist
		m_is_dirty = true;
		serialize_and_push();
	});
	m_hierarchy_panel->set_on_duplicate_node([this](const std::string& name) {
		duplicate_entity(name);
	});

	// Inspector callback: node changed → serialize & push
	m_inspector_panel->set_on_node_changed([this]() {
		m_is_dirty = true;
		serialize_and_push();
	});

	m_viewport_panel->set_on_transform_committed(
		[this](const std::string& name, glm::vec3 pos, glm::vec3 rot, glm::vec3 scale) {
			on_transform_committed(name, pos, rot, scale);
		});

	auto& pm = editor_layer->get_panel_manager();
	pm.add_panel(m_hierarchy_panel);
	pm.add_panel(m_inspector_panel);
	pm.add_panel(m_viewport_panel);
	pm.add_panel(m_console_panel);
	pm.add_panel(m_asset_browser_panel);

	auto& wnd_sys = data.require<wnd::window_system>();
	m_exit_action = [&wnd_sys] {
		if (auto win = wnd_sys.get_active_window())
			win->shutdown();
	};

	auto& ds = editor_layer->get_dockspace();
	ds.set_on_new_level([this] {
		if (m_is_dirty) {
			m_confirm_action = [this] { new_level(); };
			editor_layer->register_implicit("edt/new_confirm", [this] {
				return show_exit_confirm();
			});
		} else {
			new_level();
		}
	});
	ds.set_on_open_level([this] {
		editor_layer->register_implicit("edt/load_level", [this] { return load_level(); });
	});
	ds.set_on_save_level([this] {
		editor_layer->register_implicit("edt/save_level", [this] { return save_level(); });
	});
	ds.set_on_quick_save([this] {
		if (m_level_tag.is_valid() && m_editor_tag.is_valid()) {
			quick_save_level();
		} else if (m_editor_tag.is_valid()) {
			editor_layer->register_implicit("edt/save_level", [this] { return save_level(); });
		}
	});
	ds.set_on_exit([this] {
		if (m_is_dirty) {
			m_confirm_action = m_exit_action;
			editor_layer->register_implicit("edt/exit_confirm", [this] {
				return show_exit_confirm();
			});
		} else {
			m_exit_action();
		}
	});
}

bool edt::editor_system::show_file_dialog()
{
	bool is_open = true;
	file_dialog.clear_extension_filters();
	file_dialog.add_extension_filter(".glb");
	file_dialog.add_extension_filter(".obj");
	file_dialog.add_extension_filter(".fbx");
	file_dialog.add_extension_filter(".gltf");

	if (file_dialog.show("Import", &is_open))
	{
		auto relateve = file_dialog.get_selected_path().lexically_relative(file_dialog.get_base_path());
		res::tag tag = res::tag::make(relateve.string());
		if (std::find(imported_models_list.begin(), imported_models_list.end(), tag) == imported_models_list.end()) {
			m_res.warmup<scn::skinning_prototype_desc>(tag);
			auto handle = m_res.require<scn::skinning_prototype_desc>(tag);
			handle.then([this, tag](auto& desc) {
				imported_models_list.push_back(desc.get_tag());
				imported_models_list.push_back(tag);
				});
		}
	}
    return is_open;
}

bool edt::editor_system::load_level()
{
	bool is_open = false;
	if (auto level_manager = m_lvl_manager.lock())
	{
		is_open = true;
		file_dialog.clear_extension_filters();
		file_dialog.add_extension_filter(".desc");

		if (file_dialog.show("Load Level", &is_open)) {
			auto relateve = file_dialog.get_selected_path().lexically_relative(file_dialog.get_base_path());
			res::tag original_tag = res::tag::make(relateve.string());

			if (m_editor_tag.is_valid()) {
				m_res.unwatch(m_editor_tag, this);
			}

			std::string stem = std::filesystem::path(std::string(original_tag.relative())).stem().string();
			m_editor_tag = res::tag{ "memory://editor/" + stem + ".desc" };
			m_level_tag = original_tag;

			auto abs_path = res::resource_system::get_resources_path() / std::string(original_tag.relative());
			std::ifstream in(abs_path, std::ios::binary | std::ios::ate);
			if (in) {
				auto size = (std::streamsize)in.tellg();
				in.seekg(0);
				std::vector<std::byte> bytes(static_cast<size_t>(size));
				in.read(reinterpret_cast<char*>(bytes.data()), size);
				m_res.store(m_editor_tag, bytes);
			}

			if (level_manager->load(m_editor_tag)) {
				m_res.watch(m_editor_tag, this, [this](const res::tag&) {
					on_editor_world_reloaded();
				});

				auto level_res = m_res.require_sync<scn::level_desc>(m_editor_tag);
				populate_worlds_from_level(level_res);
				m_level_name = std::string(m_level_tag.relative());
				if (auto p = m_level_name.rfind('.'); p != std::string::npos) m_level_name.resize(p);
				if (auto p = m_level_name.rfind('/');  p != std::string::npos) m_level_name = m_level_name.substr(p + 1);

				scn::level& lvl = level_manager->get_level();
				m_world_names.clear();
				for (uint32_t i = 0; i < (uint32_t)lvl.get_world_count(); ++i)
					m_world_names.push_back(std::string(lvl.get_world(i).get_name()));

				m_active_world_idx = 0;
				m_is_dirty = false;
				switch_to_world(0);
			}
		}
	}
	return is_open;
}

void edt::editor_system::new_level()
{
	auto lvl_mgr = m_lvl_manager.lock();
	if (!lvl_mgr) return;

	namespace json = boost::json;

	json::object level_json = load_desc_template("level.desc");
	if (level_json.empty()) {
		egLOG("Editor/NewLevel", "level.desc template missing — aborting");
		return;
	}

	res::tag mem_tag{ "memory://editor/new_level.desc" };

	if (m_editor_tag.is_valid())
		m_res.unwatch(m_editor_tag, this);
	m_editor_tag = mem_tag;

	m_res.store(mem_tag, desc_system.serialize_to_bytes(json::value{ level_json }));
	m_res.signal_changed(mem_tag);

	if (!lvl_mgr->load(mem_tag)) {
		egLOG("Editor/NewLevel", "Failed to create new level via desc pipeline");
		return;
	}

	m_res.watch(m_editor_tag, this, [this](const res::tag&) {
		on_editor_world_reloaded();
	});

	auto level_res = m_res.require_sync<scn::level_desc>(mem_tag);
	populate_worlds_from_level(level_res);
	m_level_name = "new_level";

	m_level_tag = res::tag{};
	scn::level& lvl = lvl_mgr->get_level();
	m_world_names.clear();
	for (uint32_t i = 0; i < (uint32_t)lvl.get_world_count(); ++i)
		m_world_names.push_back(std::string(lvl.get_world(i).get_name()));

	m_active_world_idx = 0;
	m_is_dirty = true;
	switch_to_world(0);
}

void edt::editor_system::create_world(const std::string& name)
{
	auto lvl_mgr = m_lvl_manager.lock();
	if (!lvl_mgr || !m_sfactory || !m_assembler) return;

	namespace json = boost::json;

	json::object world_json = load_desc_template("world.desc");
	if (world_json.empty()) {
		egLOG("Editor/CreateWorld", "world.desc template missing — aborting");
		return;
	}
	world_json["name"] = name;

	res::tag mem_tag{ "memory://editor/world_" + name + ".desc" };
	m_res.store(mem_tag, desc_system.serialize_to_bytes(json::value{ world_json }));
	m_res.signal_changed(mem_tag);

	auto world_res = m_res.require_sync<scn::world_desc>(mem_tag);
	if (!world_res.is_ready()) {
		egLOG("Editor/CreateWorld", "Failed to create world desc for '{}'", name);
		return;
	}

	scn::level& lvl = lvl_mgr->get_level();
	uint32_t new_id = (uint32_t)lvl.get_world_count();
	scn::world& world = lvl.create_world(name, new_id);

	for (const auto& sys : world_res->get_systems())
		m_sfactory->create_system(sys, world.state(), lvl.organizer());

	entt::entity e = world.state().create();
	m_assembler->spawn_from_desc(world.state(), e, *world_res, name);

	lvl.mark_systems_graphs_dirty();

	m_world_names.push_back(name);
	m_world_descs.push_back(*world_res);
	m_worlds_systems_list.push_back(world_res->get_systems());
	m_is_dirty = true;
	switch_to_world((int)m_world_names.size() - 1);
}

void edt::editor_system::switch_to_world(int idx)
{
	auto lvl_mgr = m_lvl_manager.lock();
	if (!lvl_mgr || idx < 0 || idx >= (int)m_world_names.size())
		return;

	// Save camera state from the current world before switching
	if (!m_world_names.empty() && m_active_world_idx < (int)m_world_names.size()) {
		save_editor_camera_state(lvl_mgr->get_level().get_world(m_world_names[m_active_world_idx]).state());
	}

	m_active_world_idx = idx;
	scn::world& world = lvl_mgr->get_level().get_world(m_world_names[idx]);

	inject_editor_camera(world.state());

	m_hierarchy_panel->set_world_desc(&active_world_desc());
	m_hierarchy_panel->set_world_names(m_world_names, idx);

	m_inspector_panel->set_selected_node(nullptr);
	m_hierarchy_panel->set_selected_node(nullptr);
	m_viewport_panel->set_selected_entity(entt::null);
}

bool edt::editor_system::save_level()
{
	if (!m_editor_tag.is_valid()) {
		egLOG("Editor/Save", "No scene loaded — nothing to save.");
		return false;
	}

	if (!m_save_dialog_open) {
		m_save_dialog_open = true;
		std::string current = m_level_tag.is_valid() ? std::string(m_level_tag.relative()) : "levels/my_level.desc";
		strncpy(m_save_path_buf, current.c_str(), sizeof(m_save_path_buf) - 1);
		m_save_path_buf[sizeof(m_save_path_buf) - 1] = '\0';
		ImGui::OpenPopup("Save Level##edt_save");
	}

	bool is_open = true;
	if (ImGui::BeginPopupModal("Save Level##edt_save", nullptr,
		ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar))
	{
		ImGui::Text("Path (relative to res://):");
		ImGui::SetNextItemWidth(480.f);
		ImGui::InputText("##sp", m_save_path_buf, sizeof(m_save_path_buf));

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		const float bw = 110.f;
		const float avail = ImGui::GetContentRegionAvail().x;
		ImGui::SetCursorPosX((avail - bw * 2.f - ImGui::GetStyle().ItemSpacing.x) * 0.5f);

		if (ImGui::Button("Save", {bw, 0.f})) {
			const std::string rel_path_str(m_save_path_buf);
			if (write_level_to_disk(rel_path_str)) {
				m_level_tag = res::tag::make(rel_path_str);
			}
			m_save_dialog_open = false;
			is_open = false;
			ImGui::CloseCurrentPopup();
		}

		ImGui::SameLine();

		if (ImGui::Button("Cancel", {bw, 0.f})) {
			m_save_dialog_open = false;
			is_open = false;
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	} else if (m_save_dialog_open) {
		m_save_dialog_open = false;
		is_open = false;
	}

	return is_open;
}

void edt::editor_system::quick_save_level()
{
	if (!m_level_tag.is_valid())
		return;

	write_level_to_disk(std::string(m_level_tag.relative()));
}

bool edt::editor_system::show_exit_confirm()
{
	ImGui::OpenPopup("Unsaved Changes##edt_exit");

	bool is_open = true;
	if (ImGui::BeginPopupModal("Unsaved Changes##edt_exit", nullptr,
		ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar))
	{
		ImGui::Text("You have unsaved changes. What would you like to do?");
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		const float bw = 120.f;
		const float total = bw * 3.f + ImGui::GetStyle().ItemSpacing.x * 2.f;
		ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - total) * 0.5f);

		if (ImGui::Button("Save & Continue", {bw, 0.f})) {
			if (m_level_tag.is_valid()) {
				quick_save_level();
				if (m_confirm_action) m_confirm_action();
			} else {
				editor_layer->register_implicit("edt/save_level", [this] { return save_level(); });
			}
			is_open = false;
			ImGui::CloseCurrentPopup();
		}

		ImGui::SameLine();

		if (ImGui::Button("Discard", {bw, 0.f})) {
			m_is_dirty = false;
			if (m_confirm_action) m_confirm_action();
			is_open = false;
			ImGui::CloseCurrentPopup();
		}

		ImGui::SameLine();

		if (ImGui::Button("Cancel", {bw, 0.f})) {
			is_open = false;
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	} else {
		is_open = false;
	}

	return is_open;
}

void edt::editor_system::on_editor_world_reloaded()
{
	egLOG("Editor", "Editor world reloaded");
}

// ─── EPIC-11: Editor camera ───────────────────────────────────────────────────

void edt::editor_system::save_editor_camera_state(entt::registry& reg)
{
	auto view = reg.view<edt::editor_camera_tag, scn::mouse_controller_component>();
	for (auto ent : view) {
		auto& ctrl = reg.get<scn::mouse_controller_component>(ent);
		m_camera_state.position = ctrl.position;
		m_camera_state.rotation = ctrl.rotation;
		m_camera_state.distance = ctrl.distance;
		break;
	}
}

void edt::editor_system::inject_editor_camera(entt::registry& reg)
{
	// Remove any existing editor camera
	for (auto ent : reg.view<edt::editor_camera_tag>())
		reg.destroy(ent);

	entt::entity cam = reg.create();
	reg.emplace<edt::editor_camera_tag>(cam);

	scn::camera_component cam_comp;
	cam_comp.fov           = m_camera_state.fov;
	cam_comp.near_distance = m_camera_state.near_distance;
	cam_comp.far_distance  = m_camera_state.far_distance;
	cam_comp.texture       = res::tag{ edt::editor_camera_state::RT_TAG };
	reg.emplace<scn::camera_component>(cam, cam_comp);

	scn::mouse_controller_component ctrl;
	ctrl.position       = m_camera_state.position;
	ctrl.rotation       = m_camera_state.rotation;
	ctrl.distance       = m_camera_state.distance;
	ctrl.movement_speed = m_camera_state.movement_speed;
	ctrl.rotating_speed = m_camera_state.rotating_speed;
	reg.emplace<scn::mouse_controller_component>(cam, ctrl);

	reg.emplace<scn::local_transform>(cam);
	reg.emplace<scn::world_transform>(cam);
	reg.emplace<scn::name_component>(cam, "Editor Camera");
	reg.emplace<scn::depth_level>(cam);
}

// ─── Template & multi-world helpers ──────────────────────────────────────────

boost::json::object edt::editor_system::load_desc_template(const std::string& filename)
{
	namespace json = boost::json;
	auto tag = res::tag::make("templates/" + filename);
	auto text = m_res.require_sync<res::text_resource>(tag);
	if (text.is_ready()) {
		try {
			return json::parse(text->str()).as_object();
		} catch (...) {
			egLOG("Editor/Template", "Failed to parse template '{}'", filename);
		}
	}
	egLOG("Editor/Template", "Template '{}' not found", filename);
	return {};
}

boost::json::object edt::editor_system::build_level_json() const
{
	const std::string name = m_level_name.empty() ? "level" : m_level_name;
	return scn::level_desc::build_json(name, m_world_descs);
}

bool edt::editor_system::write_level_to_disk(const std::string& rel_path)
{
	sync_ecs_transforms_to_desc();

	std::filesystem::path frel(rel_path);
	const std::string stem = frel.stem().string();
	const std::string level_name = stem.empty() ? m_level_name : stem;

	const auto level_json = scn::level_desc::build_json(level_name, m_world_descs);

	std::filesystem::path abs_path = res::resource_system::get_resources_path() / frel;
	std::error_code ec;
	std::filesystem::create_directories(abs_path.parent_path(), ec);
	std::ofstream out(abs_path);
	if (!out) {
		egLOG("Editor/Save", "Failed to open file for writing: {}", abs_path.string());
		return false;
	}

	out << boost::json::serialize(level_json);
	m_level_name = level_name;
	m_is_dirty   = false;
	egLOG("Editor/Save", "Level saved to {}", abs_path.string());
	return true;
}

void edt::editor_system::populate_worlds_from_level(const res::res_handle<scn::level_desc>& level_res)
{
	m_world_descs.clear();
	m_worlds_systems_list.clear();
	if (!level_res.is_ready()) return;
	for (const auto& wh : level_res->get_worlds()) {
		m_world_descs.push_back(*wh);
		m_worlds_systems_list.push_back(wh->get_systems());
	}
}

// ─── EPIC-09: Desc-driven scene editing ──────────────────────────────────────

void edt::editor_system::serialize_and_push()
{
	if (!m_editor_tag.is_valid()) return;
	auto lvl_mgr = m_lvl_manager.lock();
	if (!lvl_mgr || !m_sfactory || !m_assembler) return;

	// Build level JSON with ALL worlds (active world is already up to date via active_world_desc())
	const auto level_obj = build_level_json();
	m_res.store(m_editor_tag,
		desc_system.serialize_to_bytes(boost::json::value{ level_obj }));

	const std::string world_name = m_active_world_idx < (int)m_world_names.size()
		? m_world_names[m_active_world_idx] : std::string(active_world_desc().get_name());

	scn::level& lvl = lvl_mgr->get_level();
	bool world_exists = false;
	for (uint32_t i = 0; i < (uint32_t)lvl.get_world_count(); ++i) {
		if (lvl.get_world(i).get_name() == world_name) { world_exists = true; break; }
	}
	if (!world_exists) {
		egLOG("Editor/Push", "World '{}' not found — skipping reload", world_name);
		return;
	}

	// Save editor camera state before world rebuild
	{
		scn::world& world = lvl.get_world(world_name);
		save_editor_camera_state(world.state());
	}

	lvl.reload_world(world_name, active_world_desc(), *m_sfactory, *m_assembler);

	// Inject editor camera into the fresh registry
	scn::world& world = lvl.get_world(world_name);
	inject_editor_camera(world.state());

	// Re-sync viewport entity selection by name
	auto* selected = m_hierarchy_panel->get_selected_node();
	if (selected) {
		ecs::entity found = entt::null;
		world.state().view<scn::name_component>().each([&](auto ent, const scn::name_component& nc) {
			if (nc.name == selected->name) found = ent;
		});
		m_viewport_panel->set_selected_entity(found);
	} else {
		m_viewport_panel->set_selected_entity(entt::null);
	}
}

std::string edt::editor_system::make_unique_key(const std::string& base_name) const
{
	const auto& children = active_world_desc().get_root().children;
	auto has_name = [&](const std::string& n) {
		for (const auto& c : children)
			if (c.name == n) return true;
		return false;
	};
	if (!has_name(base_name))
		return base_name;
	for (int i = 1; ; ++i) {
		std::string candidate = base_name + "_" + std::to_string(i);
		if (!has_name(candidate))
			return candidate;
	}
}

scn::prefab_desc::prefab_node* edt::editor_system::find_node_by_name(
	std::vector<scn::prefab_desc::prefab_node>& nodes, const std::string& name)
{
	for (auto& node : nodes) {
		if (node.name == name)
			return &node;
		if (auto* found = find_node_by_name(node.children, name))
			return found;
	}
	return nullptr;
}

bool edt::editor_system::remove_node_by_name(
	std::vector<scn::prefab_desc::prefab_node>& nodes, const std::string& name)
{
	for (auto it = nodes.begin(); it != nodes.end(); ++it) {
		if (it->name == name) {
			nodes.erase(it);
			return true;
		}
		if (remove_node_by_name(it->children, name))
			return true;
	}
	return false;
}

std::vector<scn::prefab_desc::prefab_node>* edt::editor_system::find_parent_children(
	std::vector<scn::prefab_desc::prefab_node>& nodes, const std::string& name)
{
	for (auto& node : nodes) {
		if (node.name == name)
			return &nodes;
		if (auto* p = find_parent_children(node.children, name))
			return p;
	}
	return nullptr;
}

void edt::editor_system::create_entity(const std::string& type_name)
{
	std::string base = (type_name == "Empty" || type_name.empty()) ? "Entity" : type_name;
	std::string key  = make_unique_key(base);

	scn::prefab_desc::prefab_node node;
	node.name = key;

	if (type_name != "Empty" && !type_name.empty()) {
		scn::prefab_desc::prefab_comp_node comp;
		comp.type_name = type_name;
		node.components[type_name] = std::move(comp);
	}

	// push_back may reallocate the vector, invalidating stored prefab_node pointers.
	m_hierarchy_panel->set_selected_node(nullptr);
	m_inspector_panel->set_selected_node(nullptr);
	active_world_desc().get_root().children.push_back(std::move(node));
	m_is_dirty = true;
	serialize_and_push();
}

void edt::editor_system::delete_entity(const std::string& name)
{
	// Clear selection before erasing: erase() invalidates all iterators/pointers
	// into the vector, and serialize_and_push reads m_selected_node afterwards.
	m_hierarchy_panel->set_selected_node(nullptr);
	m_inspector_panel->set_selected_node(nullptr);
	remove_node_by_name(active_world_desc().get_root().children, name);
	m_is_dirty = true;
	serialize_and_push();
}

void edt::editor_system::duplicate_entity(const std::string& name)
{
	auto* siblings = find_parent_children(active_world_desc().get_root().children, name);
	if (!siblings) return;

	auto it = std::find_if(siblings->begin(), siblings->end(),
		[&](const scn::prefab_desc::prefab_node& n) { return n.name == name; });
	if (it == siblings->end()) return;

	scn::prefab_desc::prefab_node copy = *it;
	copy.name = make_unique_key(name + "_copy");

	// Clear selection before insert: reallocation invalidates all pointers into
	// the vector, including m_selected_node stored in the hierarchy panel.
	m_hierarchy_panel->set_selected_node(nullptr);
	m_inspector_panel->set_selected_node(nullptr);
	siblings->insert(it + 1, std::move(copy));
	m_is_dirty = true;
	serialize_and_push();
}

void edt::editor_system::on_transform_committed(
	const std::string& name, glm::vec3 pos, glm::vec3 rot, glm::vec3 scale)
{
	auto* node = find_node_by_name(active_world_desc().get_root().children, name);
	if (!node) return;
	node->position = pos;
	node->rotation = rot;
	node->scale    = scale;
	m_is_dirty = true;
	serialize_and_push();
}

void edt::editor_system::sync_ecs_transforms_to_desc()
{
	auto lvl_mgr = m_lvl_manager.lock();
	if (!lvl_mgr || m_active_world_idx >= (int)m_world_names.size()) return;
	auto& reg = lvl_mgr->get_level().get_world(m_world_names[m_active_world_idx]).state();
	reg.view<scn::name_component, scn::local_transform>().each(
		[this](const scn::name_component& nc, const scn::local_transform& lt) {
			auto* node = find_node_by_name(active_world_desc().get_root().children, nc.name);
			if (!node) return;
			eng::transform3d tr{ lt.local };
			node->position = tr.get_pos();
			node->rotation = tr.get_angles_degrees();
			node->scale    = tr.get_scale();
		});
}
