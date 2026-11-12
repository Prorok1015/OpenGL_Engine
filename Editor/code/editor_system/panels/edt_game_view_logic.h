#pragma once
#include <cstdint>
#include <string>
#include <string_view>
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
		// scn::node_id_component of the camera's node; empty for a camera no desc spawned.
		std::vector<std::uint64_t> path{};
	};

	// Game cameras = cameras that edt::camera_redirector gave their own texture.
	std::vector<camera_entry> collect_cameras(entt::registry& reg);

	// Which registry this is, not where it lives: a stamp the editor keeps in the registry's ctx, taken the first time it is
	// asked for. A registry made later gets another one even at the address of a registry that is gone (a world is destroyed
	// and made again by every rebuild, and the allocator hands the same block back). registry::clear() leaves the ctx
	// alone, but gives every entity back with a new version, so a pick made before it no longer matches any entity.
	std::uint64_t registry_token(entt::registry& reg);

	class selection
	{
	public:
		// Call every frame with the registry of the world on show (null: none) and the name of that world. The pick is kept
		// while it is the same registry and its camera still exists. A registry that is not the one seen before (the world
		// was rebuilt) never keeps an entity: the camera of the same node (`camera_entry::path`) is picked again, when there
		// is one. Another world (`world` changed) starts over. Otherwise the first camera (or none).
		void refresh(entt::registry* reg, std::vector<camera_entry> cameras, std::string_view world = {});
		void select(int index);
		// The registry was rebuilt (a module reload in Play): the next refresh picks the camera of the same node again,
		// when it is there.
		void forget();

		[[nodiscard]] const std::vector<camera_entry>& cameras() const { return m_cameras; }
		[[nodiscard]] int selected_index() const;
		[[nodiscard]] entt::entity selected() const { return m_selected; }

	private:
		void remember_path();

		std::uint64_t m_token = 0;
		std::string m_world;
		std::vector<camera_entry> m_cameras;
		entt::entity m_selected = entt::null;
		// The node of the picked camera, kept when the registry goes so a rebuilt world can find it again.
		std::vector<std::uint64_t> m_pick_path;
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
