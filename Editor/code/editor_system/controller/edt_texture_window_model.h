#pragma once
#include <snk/model_codec/res_tag.h>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace edt
{
	class data_document;

	// Where a texture_2d_desc takes its pixels from (#1205).
	struct texture_source
	{
		enum class kind
		{
			none,
			// `data` names a picture file.
			file,
			// `pixels` holds them (#1204).
			inline_pixels,
		};
		kind from = kind::none;
		// The path of the picture (file).
		std::string file;
		// The size and format of the inline pixels.
		std::int64_t width = 0;
		std::int64_t height = 0;
		std::string format;
	};

	// The header as the runtime reads it from `body` (a texture desc's JSON, its parents merged in): the header there over
	// the defaults of rnd::driver::texture_header, objects merged key by key.
	boost::json::object effective_texture_header(const boost::json::value& body);
	texture_source texture_source_of(const boost::json::value& body);
	// "file.png", "inline 4x4 RGBA8" or "none".
	std::string texture_source_text(const texture_source& source);

	// The picture files a texture may name, as a picker filter (the schema's `data` field has the same list).
	inline constexpr std::string_view k_picture_extensions = "png,jpg,jpeg,tga,bmp,pct,raw";

	// The sampling a 2D texture takes from its header (docs/epics/editor/EPIC-27.md): the rest of the header is the picture's.
	std::span<const std::string_view> texture_wrap_options();
	std::span<const std::string_view> texture_filter_options();

	// The texture window without ImGui (#1205): the sampling of a texture_2d_desc data document and the change of its source.
	// Every change writes the whole effective header into the file's own `header` (the runtime merges it over the parents'
	// anyway) and is one record of the document's history.
	class texture_window_model
	{
	public:
		explicit texture_window_model(data_document& document);

		boost::json::object effective_header() const;
		std::string wrap() const;
		std::string min_filter() const;
		std::string mag_filter() const;
		int mip_levels() const;

		// False when nothing changed (the same value, or one that is not an option).
		bool set_wrap(std::string_view wrap);
		bool set_min(std::string_view filter);
		bool set_mag(std::string_view filter);
		// At least 1.
		bool set_mip_levels(int levels);

		texture_source source() const;
		// `data` names `picture` and the inline pixels go, in one record. False when nothing changed.
		bool use_file(const res::tag& picture);

	private:
		bool write_header(const std::function<void(boost::json::object& header)>& change);

		data_document& m_document;
	};
}
