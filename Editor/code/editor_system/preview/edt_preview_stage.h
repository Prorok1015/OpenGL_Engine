#pragma once
#include <snk/model_codec/res_tag.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <entt/entt.hpp>

namespace scn { struct camera_component; }
namespace rnd { class render_system; }
namespace gui { class gui_system; }
namespace inp { class input_router; }

namespace edt
{
	struct editor_services;

	// What a preview stage draws its image with; a null one: no image (tests), no orbit input.
	struct preview_view
	{
		rnd::render_system* render = nullptr;
		gui::gui_system* gui = nullptr;
		inp::input_router* input = nullptr;
	};

	// Where the stage's orbit camera starts, around the origin of the content.
	struct preview_framing
	{
		float distance = 2.2f;
		// Negative: from above.
		float pitch_degrees = -30.f;
	};

	// What a preview stage shows (a material on a shape, later a texture on a plane, a mesh): the nodes under the stage's
	// root and the resources they read. Main thread only.
	class preview_content
	{
	public:
		enum class state
		{
			// Nothing to show: the stage gives up its world and targets, and the content's resources (release).
			none,
			// Not ready this frame (a write failed): the stage does nothing and asks again next frame.
			waiting,
			unchanged,
			// The nodes or what they read changed: the stage reconciles its world.
			changed,
		};

		virtual ~preview_content() = default;
		// Once per shown frame, before the stage makes or reconciles its world.
		virtual state update() = 0;
		// The world is made: names the root and adds the nodes under it.
		virtual void build(scn::prefab_desc::prefab_node& root) = 0;
		// Before a reconcile: brings the nodes built earlier up to date.
		virtual void refresh(scn::prefab_desc::prefab_node& root) = 0;
		// The stage closes: gives up what the content wrote (memory:// copies). Called again on a closed content.
		virtual void release() = 0;
		// Read when the camera is made and again at the stage's sync(); the default suits the unit shapes.
		virtual preview_framing framing() const { return {}; }
	};

	// A reusable live preview (#1196): an isolated set of the context manager made from edt://templates/prefab_preview.desc
	// (a light and the sky, no entity of the level), an orbit camera drawing into memory:// targets at the size it is given,
	// and the nodes of a preview_content. Released with its owner window: the destructor closes it. The content must
	// outlive the stage. Main thread only.
	class preview_stage
	{
	public:
		preview_stage(editor_services& services, preview_content& content, preview_view view = {});
		~preview_stage();
		preview_stage(const preview_stage&) = delete;
		preview_stage& operator=(const preview_stage&) = delete;

		// Once per frame while the owner is shown: asks the content, makes the world when there is none, reconciles it
		// when the content changed. A content with nothing to show gives everything up.
		void update();
		// The content changed outside update (a choice in the owner's UI): reconciles now and frames the camera again.
		void sync();

		// The template's sky (true) or none: the camera's clear shows behind the content.
		bool sky() const { return m_sky; }
		void set_sky(bool shown);

		// The size the camera draws at, in framebuffer pixels; zero: nothing.
		void set_viewport_size(int width, int height);
		// The owner is hidden or collapsed: the camera draws nothing until it is shown again.
		void park();
		// Fills the available region with the color target and takes orbit/zoom input on it; parks when there is no room.
		void draw_image();

		// The game layer goes: the world (a set of its context manager) is dropped, the content and the targets stay;
		// rebuild_world makes it again in the new layer.
		void release_world();
		void rebuild_world();
		// Gives up the world, the render targets, the world template and the content's resources (also done by the destructor).
		// A layer change in progress stays (rebuild_world ends it).
		void close();

		// A failed open (the template not loaded yet, no context manager) is tried again after this many update() calls;
		// sync(), rebuild_world() and close() try at the next chance.
		static constexpr int open_retry_frames = 30;

		// The render targets are the renderer's: this gives their tags up when the stage closes. Set from the view's renderer.
		using targets_release = std::function<void(const std::vector<res::tag>&)>;
		void set_targets_release(targets_release release) { m_release_targets = std::move(release); }

		static constexpr std::string_view context_name() { return "__preview_stage"; }
		bool has_world() const { return m_has_world; }
		// Null without a world.
		entt::registry* registry();
		scn::camera_component* camera();
		const std::string& set_name() const { return m_set_name; }
		const std::string& render_target() const { return m_rt_color; }
		const res::tag& world_template() const { return m_world_tag; }

	private:
		// Empty when the world is made, else why it is not.
		std::string_view open_world();
		void try_open_world();
		void reconcile();
		void apply_framing(entt::registry& reg);
		void drop_world();
		void apply_sky();
		void release_resources();

		editor_services& m_services;
		preview_content& m_content;
		preview_view m_view;
		targets_release m_release_targets;

		std::uint64_t m_serial = 0;
		bool m_sky = true;
		// update() calls left before a failed open is tried again; zero: try at the next one.
		int m_retry_frames = 0;
		// The failure was logged: retries stay quiet until a world is made or the stage closes.
		bool m_failure_logged = false;
		// Between release_world and rebuild_world (a game layer change).
		bool m_layer_parked = false;
		bool m_has_world = false;
		// The content had something to show since the last close: rebuild_world makes the world again.
		bool m_content_live = false;

		res::tag m_world_tag;
		std::string m_key;
		std::string m_set_name;
		std::string m_rt_color;
		std::string m_rt_depth;
		std::string m_rt_accum;
		std::string m_rt_reveal;
		std::string m_working_tag;
		std::unique_ptr<scn::prefab_desc> m_prefab;
		entt::entity m_root = entt::null;
		entt::entity m_camera = entt::null;
	};
}
