#pragma once
#include <snk/command/cmd_registration.h>
#include <snk/common/snk_api.h>
#include <snk/driver/rnd_driver_interface.h>
#include <snk/driver/rnd_texture_interface.h>

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace rnd
{
	// A frame read back from a render target: 4 bytes (RGBA8) a pixel, the top row first.
	struct captured_image
	{
		glm::ivec2 size{ 0 };
		std::vector<std::byte> rgba;
	};

	// Empty `error`: the capture worked.
	struct capture_result
	{
		captured_image image;
		std::string error;

		bool ok() const { return error.empty(); }
	};

	// Reads the final render target of a view back to the CPU. The views are named by whoever owns them (the editor
	// registers its scene and game views, a game its own): the service knows ids and how to get the texture of
	// each, nothing about editors or cameras.
	// A capture is a read of the target as the last finished render left it, so it belongs on the frame boundary: the
	// `viewport.capture` command runs in the `tasks` stage, after the previous frame was presented and before the
	// next one renders. Main thread only (the thread of the driver).
	class SNK_API capture_service
	{
	public:
		// The texture a view renders to right now; null while it has none (nothing loaded, view hidden).
		using target_provider = std::function<driver::texture_interface*()>;

		// `drv` must outlive the service.
		explicit capture_service(driver::driver_interface& drv);
		capture_service(const capture_service&) = delete;
		capture_service& operator=(const capture_service&) = delete;

		// Names a view. An empty or taken id is refused (logged): the handle is then not active. The provider is
		// called on every capture. The handle removes the view; it stays harmless if the service dies first.
		[[nodiscard]] cmd::registration add_target(std::string id, target_provider provider);

		bool has_target(std::string_view id) const;
		// In registration order; the first one is what capture() reads for an empty id.
		std::vector<std::string> target_ids() const;

		// `size` > 0 in a component scales the image to it (nearest neighbour); with one component 0 the other
		// decides and the aspect stays. {0, 0}: the size of the target.
		capture_result capture(std::string_view id, glm::ivec2 size = { 0, 0 }) const;

		// Opaque RGB PNG; the alpha of the target is dropped. Empty for an empty image.
		static std::vector<std::byte> encode_png(const captured_image& image);

	private:
		struct target
		{
			std::uint64_t key = 0;
			std::string id;
			target_provider provider;
		};

		driver::driver_interface& m_driver;
		std::vector<target> m_targets;
		std::uint64_t m_next_key = 1;
		std::shared_ptr<int> m_alive = std::make_shared<int>(0);
	};
}
