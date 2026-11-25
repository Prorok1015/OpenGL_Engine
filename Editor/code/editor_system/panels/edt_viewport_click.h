#pragma once
#include "edt_mesh_gesture.h"
#include <glm/glm.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace edt
{
	struct viewport_click_frame
	{
		bool takes_input = false;
		bool locked = false;
		bool pressed = false;
		bool down = false;
		bool released = false;
		// The window lost focus or the world was replaced under the gesture: a release seen now is not a click.
		bool interrupted = false;
		glm::vec2 pixel{ 0.f };
		mesh::key_modifiers modifiers;
	};

	// What a pick does to the selection (#1327): Shift adds, Ctrl removes (Ctrl wins over Shift), neither replaces. The modeller's
	// `mesh::select_op` toggles on Ctrl; the viewport removes, as the owner decided.
	enum class pick_op : std::uint8_t
	{
		replace,
		add,
		remove,
	};

	inline pick_op pick_op_of(mesh::select_op op)
	{
		switch (op) {
		case mesh::select_op::add: return pick_op::add;
		case mesh::select_op::toggle: return pick_op::remove;
		case mesh::select_op::replace: break;
		}
		return pick_op::replace;
	}

	// The nodes a click or a box under the pointer picked, as their `node_id_component::path`s, and what to do with them. A replace with no
	// path clears the selection (a click on empty space); an add or a remove with none changes nothing.
	struct viewport_pick
	{
		std::vector<std::vector<std::uint64_t>> paths;
		pick_op op = pick_op::replace;
		// From a box: a node inside a prefab instance stands for the instance, as with several nodes (a click may take the inside one).
		bool box = false;
	};

	// Left button on the viewport image: a click selects, a drag past the threshold is a box (#1327), Alt+left pans the camera.
	class viewport_click
	{
	public:
		explicit viewport_click(float drag_threshold) : m_gesture(drag_threshold) {}

		mesh::gesture_result update(const viewport_click_frame& frame);
		bool camera_input() const { return m_gesture.camera_input(); }
		void cancel() { m_gesture.cancel(); }
		// The threshold follows the DPI: set it every frame.
		void set_drag_threshold(float pixels) { m_gesture.set_drag_threshold(pixels); }
		const mesh::select_gesture& gesture() const { return m_gesture; }

	private:
		mesh::select_gesture m_gesture;
	};

	// The longest part of `path` that is the desc's own, then the ids inside an instance: `try_select(path, target)` is asked with
	// the shortest target first and the first that takes it wins. False: none did, the selection must be left as it is.
	template <typename path_t, typename try_select_t>
	bool select_longest_own_prefix(const path_t& path, try_select_t&& try_select)
	{
		for (std::size_t tail = 0; tail < path.size(); ++tail) {
			const path_t target(path.end() - static_cast<std::ptrdiff_t>(tail), path.end());
			if (try_select(path, target))
				return true;
		}
		return false;
	}
}
