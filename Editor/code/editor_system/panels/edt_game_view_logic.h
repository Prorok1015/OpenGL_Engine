#pragma once
#include <string>
#include <vector>
#include <entt/entt.hpp>
#include <glm/glm.hpp>

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

	// `selected` gets `size` as viewport; every other game camera is set back to a zero viewport.
	// A null `selected` or a non-positive size turns all game cameras off.
	void apply_viewports(entt::registry& reg, entt::entity selected, glm::ivec2 size);

	// Largest size with the aspect of `content` that fits into `avail` (zero when either is empty).
	glm::vec2 fit_letterbox(glm::vec2 avail, glm::vec2 content);
}
