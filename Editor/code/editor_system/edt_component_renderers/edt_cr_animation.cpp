#include "edt_cr_internal.h"
#include "level/scn_prefab_desc.h"
#include "scn_animation_controller_desc.h"
#include "scn_animation_collection_desc.h"
#include <imgui.h>

void edt::edt_cr_animation_register(edt::component_ui_registry& registry)
{
	// --- animation_controller_desc ---
	registry.register_renderer<scn::animation_controller_desc>([](scn::prefab_desc::prefab_comp_node& comp) -> bool {
		bool changed = false;

		// default_animation
		std::string anim;
		if (auto it = comp.overrides.find("default_animation"); it != comp.overrides.end() && it->value().is_string())
			anim = std::string(it->value().as_string());
		char buf[256];
		std::copy_n(anim.c_str(), std::min(anim.size() + 1, sizeof(buf)), buf);
		buf[sizeof(buf) - 1] = '\0';
		if (ImGui::InputText("Default Animation", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
			comp.overrides["default_animation"] = std::string(buf);
			changed = true;
		}

		// speed
		float speed = 1.f;
		if (auto it = comp.overrides.find("speed"); it != comp.overrides.end() && it->value().is_number())
			speed = edt::json_number_to_float(it->value(), 1.f);
		if (ImGui::DragFloat("Speed", &speed, 0.01f, 0.f, 10.f)) {
			comp.overrides["speed"] = static_cast<double>(speed);
			changed = true;
		}

		// autoplay
		bool autoplay = true;
		if (auto it = comp.overrides.find("autoplay"); it != comp.overrides.end() && it->value().is_bool())
			autoplay = it->value().as_bool();
		if (ImGui::Checkbox("Autoplay", &autoplay)) {
			comp.overrides["autoplay"] = autoplay;
			changed = true;
		}

		// loop
		ImGui::SameLine();
		bool loop = true;
		if (auto it = comp.overrides.find("loop"); it != comp.overrides.end() && it->value().is_bool())
			loop = it->value().as_bool();
		if (ImGui::Checkbox("Loop", &loop)) {
			comp.overrides["loop"] = loop;
			changed = true;
		}

		return changed;
	});

	// --- animation_collection_desc (read-only) ---
	registry.register_renderer<scn::animation_collection_desc>([](scn::prefab_desc::prefab_comp_node& comp) -> bool {
		auto it = comp.overrides.find("clips");
		if (it == comp.overrides.end() || !it->value().is_array()) {
			ImGui::TextDisabled("No clips");
			return false;
		}

		const auto& clips = it->value().as_array();
		ImGui::Text("Clips: %zu", clips.size());
		ImGui::Separator();

		for (std::size_t i = 0; i < clips.size(); ++i) {
			if (!clips[i].is_object()) continue;
			const auto& clip = clips[i].as_object();

			std::string name = "unnamed";
			if (auto* p = clip.if_contains("name"); p && p->is_string())
				name = std::string(p->as_string());

			float duration = 0.f;
			if (auto* p = clip.if_contains("duration"); p && p->is_number())
				duration = edt::json_number_to_float(*p, 0.f);

			float tps = 25.f;
			if (auto* p = clip.if_contains("ticks_per_second"); p && p->is_number())
				tps = edt::json_number_to_float(*p, 25.f);

			std::size_t channel_count = 0;
			if (auto* p = clip.if_contains("channels"); p && p->is_object())
				channel_count = p->as_object().size();

			float duration_sec = (tps > 0.f) ? duration / tps : 0.f;

			std::string header = std::format("{}  ({:.2f}s, {} ch)##{}", name, duration_sec, channel_count, i);
			if (ImGui::TreeNode(header.c_str())) {
				ImGui::TextDisabled("Duration: %.1f ticks (%.2f s)", duration, duration_sec);
				ImGui::TextDisabled("Ticks/sec: %.1f", tps);
				ImGui::TextDisabled("Channels: %zu", channel_count);
				ImGui::TreePop();
			}
		}

		return false;
	});
}
