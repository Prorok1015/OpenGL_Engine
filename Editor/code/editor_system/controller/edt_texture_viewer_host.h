#pragma once
#include "edt_desc_file_commands.h"
#include "edt_document_creator.h"
#include <snk/editor/edt_registration.h>
#include <snk/model_codec/res_tag.h>
#include <boost/json/object.hpp>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace inp { class input_router; }

namespace edt
{
	class editor_context;
	class texture_viewer_panel;
	struct editor_services;

	// The texture desc "Create Texture" writes for a picture (#1205): `<folder>/<stem>.txm.desc` beside it, holding only its
	// parent and the picture.
	struct picture_texture
	{
		std::string folder;
		std::string name;
		boost::json::object fields;
	};
	picture_texture picture_texture_of(const res::tag& picture);

	// The first own fields of a new document of `type` made by Create Document (document_creator::ports::initial_fields): a
	// texture is a child of the engine's base texture, as Create Texture makes it; any other type gets none.
	boost::json::object initial_document_fields(std::string_view type);

	// Why "Create Texture" is refused for `picture`: the picture is not the project's own (`in_top_layer`), the project may
	// not be written now (the creator's blocker), or the creator refuses the name (taken in any layer). Empty: it may be
	// made. Walks the folder: asked when a viewer opens and after a Create, not every frame.
	std::string picture_texture_refusal(const document_creator& creator, const res::tag& picture, bool in_top_layer);
	// The file Create Texture makes for `picture` (the type's suffix as the creator gives it).
	res::tag picture_texture_tag(const document_creator& creator, const res::tag& picture);
	// Writes the desc through the creator (never over a file).
	document_creation create_picture_texture(const document_creator& creator, const res::tag& picture);
	// The texture desc Create Texture made (or would make) for `picture` when it is there and its `data` names this picture;
	// empty: none. Read fresh from the disk (read_desc_file), any layer.
	res::tag existing_picture_texture(const document_creator& creator, res::resource_system& resources, const desc::desc_system& descs,
		const desc_file_layout& layout, const res::tag& picture);

	// The read-only texture viewers (#1205), one per source, each a floating window registered with the editor's panels
	// while it is open: a picture double-clicked in the Asset Browser, and a texture desc of a lower layer, which is no data
	// document. Closing the window lets it go.
	class texture_viewer_host
	{
	public:
		struct ports
		{
			editor_context& context;
			editor_services& services;
			std::function<desc_file_layout()> layout;
			// The creator of the asset browser's Create Document (null: no Create Texture).
			std::function<const document_creator*()> creator;
			// Opens a texture desc that was just made (in the texture window).
			std::function<void(const res::tag&)> open_texture;
		};

		explicit texture_viewer_host(ports wired);
		~texture_viewer_host();
		texture_viewer_host(const texture_viewer_host&) = delete;
		texture_viewer_host& operator=(const texture_viewer_host&) = delete;

		void open_picture(const res::tag& picture);
		// A texture desc of a lower layer: what the runtime reads of it, read-only.
		void open_desc(const res::tag& desc);
		// Whether `tag` is a desc a lower layer holds (read_desc_file: read, and not of the top layer): no data document.
		bool is_lower_layer_desc(const res::tag& tag) const;

		bool is_open(const res::tag& source) const;
		texture_viewer_panel* find(const res::tag& source) const;
		void close(const res::tag& source);
		// A picture viewer's Create Texture: the desc, then the texture window. False (said in the viewer) when it was refused.
		bool create_for(const res::tag& picture);
		// The texture desc Create Texture made (or would make) for `picture` when it is there and names this picture; empty:
		// none. Read fresh from the disk.
		res::tag texture_of_picture(const res::tag& picture) const;

		// Where the viewers' preview cameras take the mouse from; for the viewers opened after.
		void set_input_router(inp::input_router* router) { m_input_router = router; }
		// The game layer goes / came: the viewers' preview stages are dropped and made again.
		void release_previews();
		void rebuild_previews();
		void shutdown();

	private:
		struct viewer
		{
			std::shared_ptr<texture_viewer_panel> panel;
			registration registered;
		};

		bool focus(const res::tag& source);
		void add(const res::tag& source, std::shared_ptr<texture_viewer_panel> panel);
		bool picture_in_top_layer(const res::tag& picture) const;
		// A picture viewer's button: "Open Texture" when its texture is there, else "Create Texture" with why it is refused.
		void refresh_picture(texture_viewer_panel& viewer, const res::tag& picture);

		ports m_ports;
		inp::input_router* m_input_router = nullptr;
		std::map<std::string, viewer, std::less<>> m_viewers;
		std::uint64_t m_serial = 0;
	};
}
