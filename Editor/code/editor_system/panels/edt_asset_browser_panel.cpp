#include <snk/common/fs_paths.h>
#include "edt_asset_browser_panel.h"
#include "edt_prefab_variant_creator.h"
#include "edt_asset_filter.h"
#include "edt_code_section.h"
#include "edt_thumbnail_service.h"
#include "edt_document_creator.h"
#include "edt_image_import.h"
#include "edt_material_creator.h"
#include "edt_ui_probe_imgui.h"
#include "edt_ui_metrics.h"
#include <snk/gui/gui_ui_font.h>
#include <snk/gui/gui_theme.h>
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <iterator>
#include <optional>
#include <system_error>
#include <vector>

namespace edt
{
	// ─── helpers ──────────────────────────────────────────────────────────────

	static std::string to_lower(std::string s)
	{
		for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return s;
	}

	struct file_icon {
		const char* glyph;
		const char* text;
	};

	static file_icon icon_of_extension(const std::string& ext)
	{
		if (ext == ".obj" || ext == ".gltf" || ext == ".glb" || ext == ".fbx") return {gui::icon::model, "[3D] "};
		if (ext == ".desc")                                                      return {gui::icon::desc, "[D]  "};
		if (ext == ".png"  || ext == ".jpg"  || ext == ".jpeg" ||
		    ext == ".tga"  || ext == ".hdr"  || ext == ".bmp"  ||
		    ext == ".pct"  || ext == ".raw")                                     return {gui::icon::image, "[IMG]"};
		if (ext == ".vert" || ext == ".frag" || ext == ".geom" || ext == ".glsl") return {gui::icon::file_code, "[SHD]"};
		if (ext == ".wav"  || ext == ".ogg"  || ext == ".mp3")                  return {gui::icon::sound, "[SND]"};
		return {gui::icon::file, "[F]  "};
	}

	static const char* folder_icon()
	{
		return gui::ui_font_has_icons() ? gui::icon::folder : "[/]";
	}

	const char* asset_browser_panel::get_file_icon(const std::filesystem::path& p) const
	{
		// A texture desc is a texture, not any desc (#1205).
		const bool texture_desc = to_lower(fs_paths::utf8_of(p.filename())).ends_with(".txm.desc");
		const file_icon icon = texture_desc ? file_icon{ gui::icon::image, "[IMG]" } : icon_of_extension(to_lower(fs_paths::utf8_of(p.extension())));
		return gui::ui_font_has_icons() ? icon.glyph : icon.text;
	}

	// ─── lifecycle ────────────────────────────────────────────────────────────

	asset_browser_panel::asset_browser_panel()
		: panel_base("asset_browser", "Asset Browser")
	{
	}

	void asset_browser_panel::on_hidden()
	{
		const ui_probe::scoped_pass probe_pass(m_ui_probe.get(), {"asset_browser", "asset_browser_search", "asset_browser_kinds"});
	}

	void asset_browser_panel::set_layers(std::vector<std::filesystem::path> layers)
	{
		m_layers = std::move(layers);
		m_current_rel.clear();
		m_desc_types.clear();
		if (m_thumbnails)
			m_thumbnails->release_all();
		m_needs_refresh = true;
	}

	asset_browser_panel::~asset_browser_panel() = default;

	void asset_browser_panel::set_document_creator(std::shared_ptr<document_creator> creator, std::function<void(const res::tag&)> on_created)
	{
		m_material_form.reset();
		m_material_creator.reset();
		m_creator = std::move(creator);
		m_create_form = m_creator ? std::make_unique<document_create_form>(*m_creator) : nullptr;
		if (m_creator) {
			m_material_creator = std::make_unique<material_creator>(*m_creator);
			m_material_form = std::make_unique<material_create_form>(*m_material_creator);
		}
		m_on_document_created = std::move(on_created);
	}

	void asset_browser_panel::set_image_importer(image_importer* importer, image_picker pick_file)
	{
		m_image_form = importer && pick_file ? std::make_unique<image_import_form>(*importer) : nullptr;
		m_pick_image = std::move(pick_file);
	}

	void asset_browser_panel::draw_create_menu_item(const std::filesystem::path& folder)
	{
		if (m_create_form) {
			const bool no_types = m_creator->types().empty();
			ImGui::BeginDisabled(no_types);
			if (ImGui::MenuItem("Create Document...")) {
				m_create_form->open(fs_paths::generic_utf8_of(folder));
				m_create_requested = true;
			}
			ImGui::EndDisabled();
			if (no_types && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
				ImGui::SetTooltip("no desc type is registered as a document");
			}
		}
		if (m_material_form && ImGui::MenuItem("New Material...")) {
			m_material_form->open(fs_paths::generic_utf8_of(folder));
			m_material_requested = true;
		}
		if (m_image_form && ImGui::MenuItem("Import Image...")) {
			const std::string target = fs_paths::generic_utf8_of(folder);
			m_pick_image([this, alive = std::weak_ptr<int>(m_alive), target](const std::filesystem::path& picked) {
				if (!alive.lock()) {
					return;
				}
				m_image_form->open(target, picked);
				m_image_requested = true;
			});
		}
	}

	void asset_browser_panel::ask_reimport(std::string target)
	{
		const std::weak_ptr<int> alive = m_alive;
		m_pick_image([this, alive, target = std::move(target)](const std::filesystem::path& picked) {
			if (!alive.lock()) {
				return;
			}
			m_image_form->open_reimport(target, picked);
			m_image_requested = true;
		});
	}

	void asset_browser_panel::draw_image_window()
	{
		if (!m_image_form) {
			return;
		}
		constexpr const char* title = "Import Image";
		if (m_image_requested) {
			ImGui::OpenPopup(title);
			m_image_requested = false;
		}
		if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			return;
		}
		image_import_form& form = *m_image_form;
		const image_import_settings settings = form.settings();
		if (form.reimport()) {
			ImGui::Text("Re-import %s", form.target().c_str());
		}
		ImGui::TextDisabled("Source: %s", fs_paths::utf8_of(form.source()).c_str());
		if (!form.reimport()) {
			ImGui::TextDisabled("Folder: res://%s", form.folder().c_str());
		}
		const std::string wrap_preview{ image_wrap_name(settings.wrap) };
		if (ImGui::BeginCombo("Wrap", wrap_preview.c_str())) {
			for (const image_wrap wrap : { image_wrap::clamp_to_edge, image_wrap::repeat, image_wrap::repeat_mirror,
					image_wrap::clamp_to_border }) {
				const std::string label{ image_wrap_name(wrap) };
				if (ImGui::Selectable(label.c_str(), wrap == settings.wrap)) {
					form.set_wrap(wrap);
				}
			}
			ImGui::EndCombo();
		}
		const std::string filter_preview{ image_filter_name(settings.filter) };
		if (ImGui::BeginCombo("Filter", filter_preview.c_str())) {
			for (const image_filter filter : { image_filter::nearest, image_filter::linear }) {
				const std::string label{ image_filter_name(filter) };
				if (ImGui::Selectable(label.c_str(), filter == settings.filter)) {
					form.set_filter(filter);
				}
			}
			ImGui::EndCombo();
		}
		bool mips = settings.mips;
		if (ImGui::Checkbox("Mipmaps", &mips)) {
			form.set_mips(mips);
		}
		bool entered = false;
		if (form.reimport()) {
			ImGui::TextDisabled("Files rewritten:");
			for (const std::string& file : form.rewritten_names()) {
				ImGui::BulletText("%s", file.c_str());
			}
		} else {
			std::vector<char> buffer(std::max<std::size_t>(form.name().size() + 64, 256), '\0');
			std::copy(form.name().begin(), form.name().end(), buffer.begin());
			if (ImGui::IsWindowAppearing()) {
				ImGui::SetKeyboardFocusHere();
			}
			entered = ImGui::InputText("Name", buffer.data(), buffer.size(), ImGuiInputTextFlags_EnterReturnsTrue);
			if (buffer.data() != form.name()) {
				form.set_name(buffer.data());
			}
			ImGui::TextDisabled("Files written:");
			for (const std::string& file : form.final_names()) {
				ImGui::BulletText("%s", file.c_str());
			}
		}
		for (const std::string* message : { &form.problem(), &form.error() }) {
			if (!message->empty()) {
				ImGui::PushStyleColor(ImGuiCol_Text, colors().error);
				ImGui::TextWrapped("%s", message->c_str());
				ImGui::PopStyleColor();
			}
		}
		ImGui::BeginDisabled(!form.can_submit());
		const bool clicked = ImGui::Button(form.reimport() ? "Re-import" : "Import");
		ImGui::EndDisabled();
		if ((clicked || entered) && form.can_submit() && form.submit()) {
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel")) {
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	void asset_browser_panel::draw_create_window()
	{
		if (!m_create_form) {
			return;
		}
		if (m_create_requested) {
			ImGui::OpenPopup("Create Document");
			m_create_requested = false;
		}
		if (!ImGui::BeginPopupModal("Create Document", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			return;
		}
		document_create_form& form = *m_create_form;
		ImGui::TextDisabled("Folder: res://%s", form.folder().c_str());
		const auto& types = form.types();
		const char* preview = types.empty() ? "(no document types)" : types[form.type_index()].c_str();
		if (ImGui::BeginCombo("Type", preview)) {
			for (std::size_t i = 0; i < types.size(); ++i) {
				if (ImGui::Selectable(types[i].c_str(), i == form.type_index())) {
					form.set_type_index(i);
				}
			}
			ImGui::EndCombo();
		}
		std::vector<char> buffer(std::max<std::size_t>(form.name().size() + 64, 256), '\0');
		std::copy(form.name().begin(), form.name().end(), buffer.begin());
		if (ImGui::IsWindowAppearing()) {
			ImGui::SetKeyboardFocusHere();
		}
		const bool entered = ImGui::InputText("Name", buffer.data(), buffer.size(), ImGuiInputTextFlags_EnterReturnsTrue);
		if (buffer.data() != form.name()) {
			form.set_name(buffer.data());
		}
		if (!form.file_name().empty()) {
			const std::string target = form.folder().empty() ? form.file_name() : form.folder() + "/" + form.file_name();
			if (form.suffix_replaced()) {
				ImGui::TextColored(colors().warning, "'%s' is created as res://%s", form.name().c_str(), target.c_str());
			} else {
				ImGui::TextDisabled("Creates res://%s", target.c_str());
			}
		} else {
			ImGui::TextDisabled("The type adds its extension (.desc, or its asset kind's such as .shader.desc)");
		}
		if (const std::string problem = form.problem(); !problem.empty()) {
			ImGui::PushStyleColor(ImGuiCol_Text, colors().error);
			ImGui::TextWrapped("%s", problem.c_str());
			ImGui::PopStyleColor();
		}
		if (!form.error().empty()) {
			ImGui::PushStyleColor(ImGuiCol_Text, colors().error);
			ImGui::TextWrapped("%s", form.error().c_str());
			ImGui::PopStyleColor();
		}
		ImGui::BeginDisabled(!form.can_submit());
		const bool clicked = ImGui::Button("Create");
		ImGui::EndDisabled();
		if ((clicked || (entered && form.can_submit())) && form.can_submit()) {
			if (const std::optional<res::tag> made = form.submit()) {
				ImGui::CloseCurrentPopup();
				if (m_on_document_created) {
					m_on_document_created(*made);
				}
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel")) {
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	void asset_browser_panel::draw_material_window()
	{
		if (!m_material_form) {
			return;
		}
		material_create_form& form = *m_material_form;
		// One popup ID whatever the title says (New Material, from a shader, a variant).
		constexpr const char* id = "###new_material";
		if (m_material_requested) {
			ImGui::OpenPopup(id);
			m_material_requested = false;
		}
		const std::string title = form.title() + id;
		if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			return;
		}
		ImGui::TextDisabled("Folder: res://%s", form.folder().c_str());
		std::vector<char> buffer(std::max<std::size_t>(form.name().size() + 64, 256), '\0');
		std::copy(form.name().begin(), form.name().end(), buffer.begin());
		if (ImGui::IsWindowAppearing()) {
			ImGui::SetKeyboardFocusHere();
		}
		ImGui::SetNextItemWidth(em(240.f));
		const bool entered = ImGui::InputText("Name", buffer.data(), buffer.size(), ImGuiInputTextFlags_EnterReturnsTrue);
		if (buffer.data() != form.name()) {
			form.set_name(buffer.data());
		}
		if (!form.file_name().empty()) {
			const std::string target = form.folder().empty() ? form.file_name() : form.folder() + "/" + form.file_name();
			ImGui::TextDisabled("Creates res://%s", target.c_str());
		} else {
			const std::string parent{ form.options().parent.view().empty() ? k_base_material : form.options().parent.view() };
			ImGui::TextDisabled("Give it a name; it is a child of %s", parent.c_str());
		}
		if (!form.options().shader.view().empty()) {
			ImGui::TextDisabled("Shader: %s", std::string(form.options().shader.view()).c_str());
		}
		for (const std::string* text : { &form.problem(), &form.error() }) {
			if (text->empty()) continue;
			ImGui::PushStyleColor(ImGuiCol_Text, colors().error);
			ImGui::TextWrapped("%s", text->c_str());
			ImGui::PopStyleColor();
		}
		ImGui::BeginDisabled(!form.can_submit());
		const bool clicked = ImGui::Button("Create");
		ImGui::EndDisabled();
		if ((clicked || entered) && form.can_submit()) {
			if (const std::optional<res::tag> made = form.submit()) {
				ImGui::CloseCurrentPopup();
				if (m_on_document_created) {
					m_on_document_created(*made);
				}
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel")) {
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	material_menu_item asset_browser_panel::material_item_of(const layer_entry& file)
	{
		const std::string relative = fs_paths::utf8_of(file.relative);
		std::string desc_type;
		if (asset_kind_needs_desc_type(m_kinds, relative) && file.layer < m_layers.size()) {
			int budget = 1;
			desc_type = m_desc_types.type_of(m_layers[file.layer], file.relative, budget).value_or(std::string{});
		}
		// The asset kinds know a plain .desc only as a level, world or prefab by its `__type`: a material or shader is read here.
		material_menu_item item = material_menu_item_of_type(desc_type);
		if (item == material_menu_item::none) {
			item = material_menu_item_of(asset_kind_of(m_kinds, relative, desc_type));
		}
		// An item whose click would open nothing (a name the creator cannot split) is not offered.
		const std::string_view type = item == material_menu_item::from_shader ? k_shader_type : k_material_type;
		if (item != material_menu_item::none && !m_material_creator->source_of(res::tag::make(file.relative.generic_string()), type)) {
			return material_menu_item::none;
		}
		return item;
	}

	void asset_browser_panel::set_variant_creator(std::shared_ptr<prefab_variant_creator> creator, std::function<void(const res::tag&)> on_created)
	{
		m_variant_creator = std::move(creator);
		m_variant_form = m_variant_creator ? std::make_unique<prefab_variant_form>(*m_variant_creator) : nullptr;
		m_on_variant_created = std::move(on_created);
	}

	void asset_browser_panel::draw_variant_window()
	{
		if (!m_variant_form) {
			return;
		}
		constexpr const char* title = "Create Prefab Variant";
		if (m_variant_requested) {
			ImGui::OpenPopup(title);
			m_variant_requested = false;
		}
		if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			return;
		}
		prefab_variant_form& form = *m_variant_form;
		ImGui::TextDisabled("Base: %s", std::string(form.base().view()).c_str());
		std::vector<char> buffer(std::max<std::size_t>(form.name().size() + 64, 256), '\0');
		std::copy(form.name().begin(), form.name().end(), buffer.begin());
		if (ImGui::IsWindowAppearing()) {
			ImGui::SetKeyboardFocusHere();
		}
		const bool entered = ImGui::InputText("Name", buffer.data(), buffer.size(), ImGuiInputTextFlags_EnterReturnsTrue);
		if (buffer.data() != form.name()) {
			form.set_name(buffer.data());
		}
		ImGui::TextDisabled(".prefab.desc is added; the file lands next to the base");
		for (const std::string* text : { &form.problem(), &form.error() }) {
			if (text->empty()) continue;
			ImGui::PushStyleColor(ImGuiCol_Text, colors().error);
			ImGui::TextWrapped("%s", text->c_str());
			ImGui::PopStyleColor();
		}
		ImGui::BeginDisabled(!form.can_submit());
		const bool clicked = ImGui::Button("Create");
		ImGui::EndDisabled();
		if ((clicked || entered) && form.can_submit()) {
			if (const std::optional<res::tag> made = form.submit()) {
				ImGui::CloseCurrentPopup();
				if (m_on_variant_created) {
					m_on_variant_created(*made);
				}
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	void asset_browser_panel::set_code_section(std::shared_ptr<code_section> section)
	{
		m_code_section = std::move(section);
	}

	void asset_browser_panel::navigate_to(const std::filesystem::path& rel_dir)
	{
		m_current_rel   = rel_dir;
		m_needs_refresh = true;
		if (m_thumbnails)
			m_thumbnails->release_all();
	}

	// ─── refresh ──────────────────────────────────────────────────────────────

	void asset_browser_panel::refresh_entries()
	{
		m_entries.clear();
		m_needs_refresh = false;
		m_labels_have_icons = gui::ui_font_has_icons();

		m_filter.set_name(m_search);
		m_filter.keep_only(asset_filter_kinds(m_kinds));

		// A desc's `__type` is read for a few files per pass; the rest of a big folder appears over the next frames.
		constexpr int k_type_reads_per_pass = 32;
		int type_reads_left = k_type_reads_per_pass;
		bool types_pending = false;

		for (auto& src : list_merged_directory(m_layers, m_current_rel)) {
			if (!src.is_dir) {
				if (!m_filter.accepts_name(src.name))
					continue;
				std::string kind;
				if (m_filter.needs_kind()) {
					const std::string relative = fs_paths::utf8_of(src.relative);
					std::string desc_type;
					if (asset_kind_needs_desc_type(m_kinds, relative) && src.layer < m_layers.size()) {
						const auto known = m_desc_types.type_of(m_layers[src.layer], src.relative, type_reads_left);
						if (!known) {
							types_pending = true;
							continue;
						}
						desc_type = *known;
					}
					kind = asset_kind_of(m_kinds, relative, desc_type);
				}
				if (!m_filter.accepts(false, src.name, kind))
					continue;
			}
			std::string label = src.is_dir
				? std::string(folder_icon()) + " " + src.name
				: std::string(get_file_icon(src.path)) + " " + src.name;
			m_entries.push_back({std::move(src), std::move(label)});
		}
		if (types_pending)
			m_needs_refresh = true;
	}

	void asset_browser_panel::draw_picture_row(const layer_entry& src, const std::string& label, float x, float y, float height, std::string& failure)
	{
		const float pad = ImGui::GetStyle().FramePadding.y;
		const float slot = height - 2.f * pad;
		thumbnail_service::view preview;
		if (ImGui::IsItemVisible() && src.layer < m_layers.size())
			preview = m_thumbnails->lookup(m_layers[src.layer], src.relative);
		const bool ready = preview.state == thumbnail_service::view::state_t::ready && preview.texture != 0 && preview.width > 0 && preview.height > 0;
		if (preview.state == thumbnail_service::view::state_t::failed)
			failure = preview.error;

		ImDrawList* list = ImGui::GetWindowDrawList();
		if (ready) {
			const float scale = slot / static_cast<float>(std::max(preview.width, preview.height));
			const float w = static_cast<float>(preview.width) * scale;
			const float h = static_cast<float>(preview.height) * scale;
			const ImVec2 a{ x + pad + (slot - w) * 0.5f, y + pad + (slot - h) * 0.5f };
			list->AddImage(static_cast<ImTextureID>(preview.texture), a, ImVec2(a.x + w, a.y + h));
		}
		// Without a thumbnail (waiting, failed) the row reads as every other row: the kind's icon in front of the name.
		const std::string& text = ready ? src.name : label;
		list->AddText(ImVec2(x + slot + 3.f * pad, y + (height - ImGui::GetTextLineHeight()) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), text.c_str());
	}

	// ─── left panel: directory tree ───────────────────────────────────────────

	void asset_browser_panel::draw_dir_tree(const std::filesystem::path& rel_dir, int depth)
	{
		if (depth > 6)
			return;

		for (const auto& sub : list_merged_directory(m_layers, rel_dir, true)) {
			const bool selected  = (sub.relative == m_current_rel);
			// Auto-expand if m_current_rel is inside this subtree
			const bool is_ancestor = [&] {
				auto p = m_current_rel;
				while (p != p.parent_path()) {
					if (p == sub.relative) return true;
					p = p.parent_path();
				}
				return false;
			}();

			ImGuiTreeNodeFlags flags =
				ImGuiTreeNodeFlags_OpenOnArrow |
				ImGuiTreeNodeFlags_SpanFullWidth;
			if (selected)   flags |= ImGuiTreeNodeFlags_Selected;
			if (is_ancestor) ImGui::SetNextItemOpen(true, ImGuiCond_Once);

			const bool opened = ImGui::TreeNodeEx(sub.name.c_str(), flags);

			if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
				navigate_to(sub.relative);
			if (has_folder_menu() && ImGui::BeginPopupContextItem()) {
				draw_create_menu_item(sub.relative);
				ImGui::EndPopup();
			}

			if (opened) {
				draw_dir_tree(sub.relative, depth + 1);
				ImGui::TreePop();
			}
		}
	}

	// ─── render ───────────────────────────────────────────────────────────────

	void asset_browser_panel::on_render()
	{
		const ui_probe::scoped_pass search_pass(m_ui_probe.get(), {"asset_browser_search", "asset_browser_kinds"});
		if (m_layers.empty()) {
			ImGui::TextDisabled("(base path not set)");
			return;
		}
		if (m_thumbnails)
			m_thumbnails->tick();

		// ── breadcrumb ───────────────────────────────────────────────────────
		{
			// Root button
			if (ImGui::SmallButton("res://"))
				navigate_to({});

			// One button per path segment between res:// and current
			std::filesystem::path cursor;
			for (const auto& part : m_current_rel) {
				if (part == ".") continue;
				cursor /= part;
				ImGui::SameLine(0.f, em(2.f));
				ImGui::TextDisabled(">");
				ImGui::SameLine(0.f, em(2.f));
				const std::string seg = fs_paths::utf8_of(part);
				if (ImGui::SmallButton(seg.c_str()))
					navigate_to(cursor);
			}
		}

		// ── search ───────────────────────────────────────────────────────────
		ImGui::SetNextItemWidth(-em(10.f));
		if (ImGui::InputTextWithHint("##search", "Search files...", m_search, sizeof(m_search)))
			m_needs_refresh = true;
		edt::probe_last_item(m_ui_probe.get(), "asset_browser_search", "search");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-1.f);
		if (ImGui::BeginCombo("##kinds", m_filter.summary().c_str())) {
			if (ImGui::Selectable("All", !m_filter.needs_kind(), ImGuiSelectableFlags_NoAutoClosePopups)) {
				m_filter.clear_kinds();
				m_needs_refresh = true;
			}
			ImGui::Separator();
			for (const asset_filter_kind& kind : asset_filter_kinds(m_kinds)) {
				const bool picked = m_filter.kind_picked(kind.id);
				const std::string label = kind.label + "###kind_" + kind.id;
				if (ImGui::Selectable(label.c_str(), picked, ImGuiSelectableFlags_NoAutoClosePopups)) {
					m_filter.set_kind(kind.id, !picked);
					m_needs_refresh = true;
				}
			}
			ImGui::EndCombo();
		}
		edt::probe_last_item(m_ui_probe.get(), "asset_browser_kinds", "kinds");

		ImGui::Separator();

		// ── two columns: tree | files ─────────────────────────────────────────
		ImGui::Columns(2, "ab_cols", true);
		ImGui::SetColumnWidth(0, em(160.f));

		// Left: directory tree
		ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, em(12.f));
		if (ImGui::TreeNodeEx("res://##directorytreeroot", ImGuiTreeNodeFlags_DefaultOpen |
		                                ImGuiTreeNodeFlags_SpanFullWidth))
		{
			draw_dir_tree({});
			ImGui::TreePop();
		}
		ImGui::PopStyleVar();

		if (m_code_section)
			m_code_section->render(colors());

		ImGui::NextColumn();

		// Right: current-directory contents
		if (m_needs_refresh || m_labels_have_icons != gui::ui_font_has_icons())
			refresh_entries();

		// Room under the list for "Used by" of the picked file.
		const bool shows_usages = !m_selected_rel.empty() && m_usages.has_source();
		const float usages_height = shows_usages ? ImGui::GetTextLineHeightWithSpacing() * 10.f : 0.f;
		if (ImGui::BeginChild("##asset_files", ImVec2(0.f, -usages_height), ImGuiChildFlags_None)) {
			// ".." back button (disabled at root)
			{
				bool at_root = m_current_rel.empty();
				if (at_root) ImGui::BeginDisabled();
				if (ImGui::Selectable((std::string(folder_icon()) + " ..").c_str()))
					navigate_to(m_current_rel.parent_path());
				if (at_root) ImGui::EndDisabled();
			}

			if (m_ui_probe)
				m_ui_probe->begin_pass("asset_browser");

			const size_t top_layer = m_layers.size() - 1;
			for (const auto& e : m_entries) {
				// Entries that come from a lower layer are dimmed; the tooltip names the layer root.
				const bool from_lower_layer = e.src.layer != top_layer;
				if (from_lower_layer)
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
				const bool picture_row = m_thumbnails && !e.src.is_dir && thumbnail_decodable(e.src.name);
				const bool selected_row = !e.src.is_dir && e.src.relative == m_selected_rel;
				std::string no_preview;
				bool picked = false;
				if (picture_row) {
					const ImVec2 row_min = ImGui::GetCursorScreenPos();
					const float row_height = em(2.5f);
					// The name is drawn over the row below, next to the thumbnail: the selectable only holds the place and the id.
					picked = ImGui::Selectable(("##" + e.label).c_str(), selected_row, ImGuiSelectableFlags_None, ImVec2(0.f, row_height));
					draw_picture_row(e.src, e.label, row_min.x, row_min.y, row_height, no_preview);
				} else {
					picked = ImGui::Selectable(e.label.c_str(), selected_row);
				}
				if (m_ui_probe) {
					// A row the list cuts off is "visible" to ImGui; only its part inside the list counts, so a
					// driver that aims at the centre does not land below the list.
					const ImDrawList* list = ImGui::GetWindowDrawList();
					const ImVec2 item_min{ std::max(ImGui::GetItemRectMin().x, list->GetClipRectMin().x),
						std::max(ImGui::GetItemRectMin().y, list->GetClipRectMin().y) };
					const ImVec2 item_max{ std::min(ImGui::GetItemRectMax().x, list->GetClipRectMax().x),
						std::min(ImGui::GetItemRectMax().y, list->GetClipRectMax().y) };
					if (ImGui::IsItemVisible() && item_max.x - item_min.x >= 2.f && item_max.y - item_min.y >= 2.f) {
						m_ui_probe->report("asset_browser", e.src.name, item_min.x, item_min.y, item_max.x, item_max.y);
					} else {
						m_ui_probe->report_hidden("asset_browser", e.src.name);
					}
				}
				if (from_lower_layer)
					ImGui::PopStyleColor();
				if (ImGui::IsItemHovered()) {
					std::string tip;
					if (m_layers.size() > 1)
						tip = "layer " + std::to_string(e.src.layer) + ": " + fs_paths::utf8_of(m_layers[e.src.layer]);
					if (!no_preview.empty())
						tip += (tip.empty() ? "" : "\n") + std::string("No preview: ") + no_preview;
					if (!tip.empty())
						ImGui::SetTooltip("%s", tip.c_str());
				}

				if (e.src.is_dir) {
					// Navigate into folder on click
					if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
						navigate_to(e.src.relative);
					if (has_folder_menu() && ImGui::BeginPopupContextItem()) {
						draw_create_menu_item(e.src.relative);
						ImGui::EndPopup();
					}
				} else {
					if (picked)
						m_selected_rel = e.src.relative;
					// Double-click on file
					if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
						if (m_on_file_double_clicked)
							m_on_file_double_clicked(e.src.relative);
					}

					// Drag source: path relative to res:// (the physical path would be wrong for lower layers)
					if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
						const std::string full = fs_paths::generic_utf8_of(e.src.relative);
						ImGui::SetDragDropPayload("ASSET_PATH", full.c_str(), full.size() + 1);
						ImGui::TextUnformatted(e.label.c_str());
						ImGui::EndDragDropSource();
					}

					// Context menu
					if (ImGui::BeginPopupContextItem()) {
						if (ImGui::IsWindowAppearing()) {
							m_offers_open_as_data = m_open_as_data_offered && m_open_as_data
								&& m_open_as_data_offered(res::tag::make(e.src.relative.generic_string()));
							// Only a look for the sidecar: the re-import itself refuses what it cannot take.
							const std::string extension = to_lower(fs_paths::utf8_of(e.src.path.extension()));
							const auto& pictures = image_importer::k_extensions;
							std::error_code ec;
							std::filesystem::path sidecar = e.src.path;
							sidecar += std::string(res::asset_meta_suffix);
							m_offers_reimport = m_image_form
								&& std::find(std::begin(pictures), std::end(pictures), extension) != std::end(pictures)
								&& std::filesystem::is_regular_file(std::filesystem::symlink_status(sidecar, ec));
							m_material_item = m_material_form ? material_item_of(e.src) : material_menu_item::none;
						}
						if (m_offers_open_as_data && ImGui::MenuItem("Open as data")) {
							m_open_as_data(res::tag::make(e.src.relative.generic_string()));
						}
						if (m_operation_form) {
							if (ImGui::MenuItem("Rename...")) ask_operation(asset_operation::rename, e.src.relative);
							if (ImGui::MenuItem("Move...")) ask_operation(asset_operation::move, e.src.relative);
							if (ImGui::MenuItem("Delete...")) ask_operation(asset_operation::remove, e.src.relative);
							ImGui::Separator();
						}
						if (ImGui::MenuItem("Copy path"))
							ImGui::SetClipboardText(fs_paths::utf8_of(e.src.path).c_str());
						if (ImGui::MenuItem("Copy relative path")) {
							ImGui::SetClipboardText(fs_paths::generic_utf8_of(e.src.relative).c_str());
						}
						if (m_offers_reimport && ImGui::MenuItem("Re-import...")) {
							ask_reimport(std::string(res::tag::make(fs_paths::generic_utf8_of(e.src.relative)).view()));
						}
						if (m_variant_form && e.src.relative.generic_string().ends_with(".prefab.desc") && ImGui::MenuItem("Create Variant")) {
							m_variant_form->open(res::tag::make(e.src.relative.generic_string()));
							m_variant_requested = true;
						}
						if (m_material_form && m_material_item == material_menu_item::from_shader && ImGui::MenuItem("Create Material")) {
							m_material_requested = m_material_form->open_from_shader(res::tag::make(e.src.relative.generic_string()));
						}
						if (m_material_form && m_material_item == material_menu_item::variant && ImGui::MenuItem("Create Material Variant")) {
							m_material_requested = m_material_form->open_variant_of(res::tag::make(e.src.relative.generic_string()));
						}
						ImGui::EndPopup();
					}
				}
			}

			if (m_ui_probe)
				m_ui_probe->end_pass("asset_browser");

			if (m_entries.empty())
				ImGui::TextDisabled("(empty)");

			// The blank of the open folder.
			if (has_folder_menu() && ImGui::BeginPopupContextWindow("##asset_browser_blank",
					ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
				draw_create_menu_item(m_current_rel);
				ImGui::EndPopup();
			}
		}
		ImGui::EndChild();

		draw_usages();

		ImGui::Columns(1);

		draw_create_window();
		draw_material_window();
		draw_image_window();
		draw_variant_window();
		draw_operation_window();
	}
}

void edt::asset_browser_panel::draw_usages()
{
	if (m_selected_rel.empty() || !m_usages.has_source())
		return;
	ImGui::Separator();
	if (ImGui::BeginChild("##asset_usages", ImVec2(0.f, 0.f), ImGuiChildFlags_None))
		draw_usage_rows();
	ImGui::EndChild();
}

void edt::asset_browser_panel::draw_usage_rows()
{
	const std::string selected = fs_paths::generic_utf8_of(m_selected_rel);
	ImGui::Text("Used by: %s", selected.c_str());
	ImGui::SameLine();
	ImGui::TextDisabled("(saved files)");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Found in the files as they are on the disk: an unsaved change is not counted.");

	const usage_report* report = m_usages.report_for(res::tag::make(selected));
	if (!report) {
		ImGui::TextDisabled("No asset database: no project is open.");
		return;
	}
	if (report->complete && !report->found) {
		ImGui::SameLine();
		ImGui::TextDisabled("(missing)");
	}
	if (!report->complete)
		ImGui::TextDisabled("Still reading files: the list may grow.");
	if (report->items.empty())
		ImGui::TextDisabled("Nothing refers to it.");
	const std::vector<std::string>& rows = m_usages.rows();
	ImGuiListClipper clipper;
	clipper.Begin(static_cast<int>(rows.size()));
	while (clipper.Step()) {
		for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
			const asset_usage& usage = report->items[static_cast<std::size_t>(index)];
			// The row is drawn as plain text over an unnamed selectable: a "##" in a file name or a JSON key stays text.
			const ImVec2 row_start = ImGui::GetCursorPos();
			ImGui::PushID(index);
			ImGui::Selectable("##usage");
			ImGui::PopID();
			const bool hovered = ImGui::IsItemHovered();
			const bool opened = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
			ImGui::SetCursorPos(row_start);
			// Found by its text, not through a field of a schema: it may be no reference at all.
			const bool loose = usage.kind == usage_kind::loose;
			if (loose)
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			ImGui::TextUnformatted(rows[static_cast<std::size_t>(index)].c_str());
			if (loose)
				ImGui::PopStyleColor();
			if (hovered)
				ImGui::SetTooltip("%s", loose ? "Not a field of a schema (a custom field, the record of a prefab instance): found by its text."
					: usage.by_guid ? "Refers to it by its ID." : "Refers to it by its path.");
			constexpr std::string_view protocol = "res://";
			if (opened && m_on_file_double_clicked && usage.file.starts_with(protocol))
				m_on_file_double_clicked(fs_paths::path_from_utf8(std::string_view(usage.file).substr(protocol.size())));
		}
	}
	if (!report->skipped.empty()) {
		ImGui::TextDisabled("%zu file(s) too large to read for references", report->skipped.size());
		if (ImGui::IsItemHovered()) {
			std::string names;
			for (const std::string& file : report->skipped)
				names += file + '\n';
			ImGui::SetTooltip("%s", names.c_str());
		}
	}
}

void edt::asset_browser_panel::set_asset_operations(asset_operation_ports ports)
{
	m_operation_form = std::make_unique<asset_operation_form>(std::move(ports));
}

void edt::asset_browser_panel::ask_operation(asset_operation what, const std::filesystem::path& relative)
{
	m_operation_form->open(what, res::tag::make(fs_paths::generic_utf8_of(relative)));
	m_operation_requested = true;
}

namespace
{
	std::vector<std::string> lines_of(const std::string& text)
	{
		std::vector<std::string> out;
		std::size_t start = 0;
		while (start <= text.size()) {
			const std::size_t end = text.find('\n', start);
			out.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
			if (end == std::string::npos) break;
			start = end + 1;
		}
		return out;
	}

	const char* verb_of(edt::asset_operation what)
	{
		switch (what) {
		case edt::asset_operation::rename: return "Rename";
		case edt::asset_operation::move: return "Move";
		case edt::asset_operation::remove: return "Delete";
		}
		return "Change";
	}
}

void edt::asset_browser_panel::draw_operation_lines(const char* id, const std::vector<std::string>& lines, bool error)
{
	if (ImGui::BeginChild(id, ImVec2(em(480.f), em(lines.size() > 8 ? 220.f : 120.f)), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
		if (error) ImGui::PushStyleColor(ImGuiCol_Text, colors().error);
		for (const std::string& line : lines) ImGui::TextUnformatted(line.c_str());
		if (error) ImGui::PopStyleColor();
	}
	ImGui::EndChild();
}

void edt::asset_browser_panel::draw_operation_window()
{
	if (!m_operation_form) return;
	asset_operation_form& form = *m_operation_form;
	constexpr const char* title = "Asset Operation";
	if (m_operation_requested) {
		ImGui::OpenPopup(title);
		m_operation_requested = false;
	}
	if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
	switch (form.current()) {
	case asset_operation_form::stage::editing:
		draw_operation_editing(form);
		break;
	case asset_operation_form::stage::confirming_force:
		draw_operation_force(form);
		break;
	case asset_operation_form::stage::reported:
		draw_operation_report(form);
		break;
	case asset_operation_form::stage::closed:
		ImGui::CloseCurrentPopup();
		break;
	}
	ImGui::EndPopup();
}

void edt::asset_browser_panel::draw_operation_editing(asset_operation_form& form)
{
	const char* verb = verb_of(form.operation());
	form.settle(std::chrono::steady_clock::now());
	ImGui::Text("%s %s", verb, form.asset_text().c_str());
	bool entered = false;
	if (form.operation() != asset_operation::remove) {
		std::vector<char> buffer(std::max<std::size_t>(form.input().size() + 64, 256), '\0');
		std::copy(form.input().begin(), form.input().end(), buffer.begin());
		if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
		ImGui::SetNextItemWidth(em(480.f));
		const char* label = form.operation() == asset_operation::rename ? "New name" : "Folder below res://";
		entered = ImGui::InputText(label, buffer.data(), buffer.size(), ImGuiInputTextFlags_EnterReturnsTrue);
		if (buffer.data() != form.input()) {
			if (entered) form.set_input(buffer.data());
			else form.type_input(buffer.data(), std::chrono::steady_clock::now());
		}
	}
	if (form.planning_pending()) {
		ImGui::TextDisabled("Checking...");
	} else if (form.refused()) {
		draw_operation_lines("##operation_refusal", lines_of(form.refusal_text()), true);
	} else {
		draw_operation_lines("##operation_preview", form.preview_lines(), false);
	}
	if (form.needs_force() && !form.refused()) {
		ImGui::TextUnformatted("Other files refer to it, so Delete stays off.");
	}
	if (!form.error().empty()) draw_operation_lines("##operation_error", lines_of(form.error()), true);

	ImGui::BeginDisabled(!form.can_submit());
	const bool clicked = ImGui::Button(verb);
	ImGui::EndDisabled();
	const bool done = (clicked || (entered && form.can_submit())) && form.submit();
	if (form.take_disk_changed()) m_needs_refresh = true;
	if (done) m_selected_rel.clear();
	if (form.can_ask_force()) {
		ImGui::SameLine();
		if (ImGui::Button("Delete anyway (force)...")) form.ask_force();
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
		form.close();
		ImGui::CloseCurrentPopup();
	}
}

void edt::asset_browser_panel::draw_operation_force(asset_operation_form& form)
{
	ImGui::Text("Delete %s although other files refer to it?", form.asset_text().c_str());
	draw_operation_lines("##operation_force", lines_of(form.force_text()), false);
	if (!form.error().empty()) draw_operation_lines("##operation_error", lines_of(form.error()), true);
	ImGui::PushStyleColor(ImGuiCol_Text, colors().warning);
	const bool clicked = ImGui::Button("Delete and break the references");
	ImGui::PopStyleColor();
	const bool done = clicked && form.submit_force();
	if (form.take_disk_changed()) m_needs_refresh = true;
	if (done) m_selected_rel.clear();
	ImGui::SameLine();
	if (ImGui::Button("Back") || ImGui::IsKeyPressed(ImGuiKey_Escape)) form.back();
}

void edt::asset_browser_panel::draw_operation_report(asset_operation_form& form)
{
	ImGui::TextUnformatted(form.report_title().c_str());
	draw_operation_lines("##operation_report", form.report_lines(), false);
	if (form.undone()) ImGui::TextDisabled("Undone: the files are back as they were.");
	if (!form.undo_error().empty()) draw_operation_lines("##operation_undo_error", lines_of(form.undo_error()), true);
	ImGui::BeginDisabled(!form.can_undo());
	const bool undo = ImGui::Button("Undo");
	ImGui::EndDisabled();
	if (undo) form.undo();
	if (form.take_disk_changed()) m_needs_refresh = true;
	ImGui::SameLine();
	if (ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
		form.close();
		ImGui::CloseCurrentPopup();
	}
}
