#pragma once
#include <string>
#include <vector>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <snk/resource/res_tag.h>

// GL- and ImGui-free part of the Game view: which game cameras exist, which one is picked, and the
// rule that only the picked camera gets a viewport (the others keep a zero viewport and are skipped
// by the render passes).
namespace edt::game_view
{
	struct camera_entry
	{
		entt::entity entity = entt::null;
		std::string name;
	};

	// Game cameras = cameras that edt::camera_redirector gave their own texture.
	std::vector<camera_entry> collect_cameras(entt::registry& reg);

	class selection
	{
	public:
		// Call every frame with the current registry. The pick is kept while its camera still exists in
		// the same registry, otherwise it falls back to the first camera (or none).
		void refresh(const entt::registry* reg, std::vector<camera_entry> cameras);
		void select(int index);

		[[nodiscard]] const std::vector<camera_entry>& cameras() const { return m_cameras; }
		[[nodiscard]] int selected_index() const;
		[[nodiscard]] entt::entity selected() const { return m_selected; }

	private:
		const entt::registry* m_registry = nullptr;
		std::vector<camera_entry> m_cameras;
		entt::entity m_selected = entt::null;
	};

	// The cameras that draw into `target` (the game cameras of one set share a texture, see camera_redirector) get
	// `size` as viewport, every other game camera of `reg` is set back to a zero viewport. An invalid `target` or a
	// non-positive size turns all game cameras off. Call it for the registry of every world.
	// `only` (the picked camera, for the picked world's registry): when not null, no other camera of `reg` gets a
	// viewport even when it shares the target, so the picker decides what that world shows. The cameras of the other
	// worlds are visited with a null `only`: they composite onto the picked camera's texture by layer.
	void apply_target_viewports(entt::registry& reg, const res::tag& target, glm::ivec2 size, entt::entity only = entt::null);

	// Largest size with the aspect of `content` that fits into `avail` (zero when either is empty).
	glm::vec2 fit_letterbox(glm::vec2 avail, glm::vec2 content);
}
