#pragma once
#include "edt_preview_stage.h"
#include <snk/model_codec/res_tag.h>
#include <boost/json/value.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace edt
{
	struct editor_services;

	// What a texture preview shows: the texture desc's own fields as they are now and a number that moves when they change.
	// The texture window gives a data document's own fields (unsaved edits included); the read-only viewer a picture or a file
	// of a lower layer.
	struct texture_preview_source
	{
		// The fields of the desc as its file holds them: its `__parent` is kept, other keys starting with "__" are not taken.
		// Null: nothing to show.
		std::function<boost::json::value()> body;
		std::function<std::uint64_t()> generation;
		// Written as the copy's __parent over the body's ("res://base_texture.desc" under a picture, the file itself for a
		// lower layer's desc); empty: the body's own.
		std::string parent;
	};

	// The preview of a texture (#1205): a plane seen from above, drawn with a memory:// material whose diffuse sampler is a
	// memory:// copy of the texture desc, written again when the source's generation moves (unsaved edits show, the file is
	// untouched). Main thread only.
	class texture_preview_content final : public preview_content
	{
	public:
		texture_preview_content(texture_preview_source source, editor_services& services);
		~texture_preview_content() override;
		texture_preview_content(const texture_preview_content&) = delete;
		texture_preview_content& operator=(const texture_preview_content&) = delete;

		state update() override;
		void build(scn::prefab_desc::prefab_node& root) override;
		void refresh(scn::prefab_desc::prefab_node& root) override;
		void release() override;
		preview_framing framing() const override;

		// The memory:// copy of the texture desc the material samples.
		const res::tag& texture_tag() const { return m_texture_tag; }
		// The memory:// material of the plane.
		const res::tag& material_tag() const { return m_material_tag; }

	private:
		bool write_texture();
		bool write_material();
		bool store(const res::tag& tag, const boost::json::value& content);
		void report_write_failure(std::string_view why);

		texture_preview_source m_source;
		editor_services& m_services;
		bool m_texture_written = false;
		bool m_material_written = false;
		std::uint64_t m_written_generation = 0;
		bool m_write_failure_logged = false;
		std::uint64_t m_failure_generation = 0;
		res::tag m_texture_tag;
		res::tag m_material_tag;
	};
}
