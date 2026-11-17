#pragma once
#include <snk/gui/gui_icons.h>
#include <imgui.h>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gui
{
	// Whether the current font has the icon glyphs (gui_icons.h); during a frame. Without them, an icon string would
	// draw as '?' boxes, so the caller shows a text placeholder instead. The classic theme loads no icons.
	inline bool ui_font_has_icons()
	{
		ImFont* font = ImGui::GetFont();
		return font && font->FindGlyphNoFallback(icon::first) != nullptr;
	}

	// The UI font: a TTF kept as bytes (the atlas does not own them) plus the glyph ranges built for it.
	// Both must outlive the atlas, and rebuild_fonts adds the font again, so the object lives as long as the backend.
	// Without usable bytes it adds ImGui's built-in font, so a missing font file changes nothing.
	class ui_font {
	public:
		// Latin + Cyrillic + Greek. The built-in range alone has no Cyrillic, which shows up as '?'.
		static void build_glyph_ranges(ImFontAtlas& atlas, ImVector<ImWchar>& out)
		{
			ImFontGlyphRangesBuilder builder;
			builder.AddRanges(atlas.GetGlyphRangesDefault());
			builder.AddRanges(atlas.GetGlyphRangesCyrillic());
			builder.AddRanges(atlas.GetGlyphRangesGreek());
			out.clear();
			builder.BuildRanges(&out);
		}

		// A TrueType / OpenType font (or the first font of a collection) whose table directory stays inside the bytes
		// and has the tables stb_truetype reads, long enough for the fields it takes from them. ImGui asserts on bytes
		// it cannot parse and stb_truetype trusts every offset, so a truncated or wrong file (a font may come from a
		// theme preset) must be turned away here, before it reaches the atlas.
		static bool looks_like_font(const std::vector<std::byte>& bytes)
		{
			if (bytes.size() >= 16 && tag_at(bytes, 0) == tag("ttcf")) {
				return looks_like_sfnt(bytes, u32_at(bytes, 12));
			}
			return looks_like_sfnt(bytes, 0);
		}

		// Call before the first add_to: the atlas keeps a pointer into the bytes.
		void set_bytes(std::vector<std::byte> bytes)
		{
			m_bytes = std::move(bytes);
			m_ranges.clear();
		}

		// The icon font (gui_icons.h), merged into the UI font so a label can mix text and icons.
		// Before the first add_to.
		void set_icon_bytes(std::vector<std::byte> bytes) { m_icon_bytes = std::move(bytes); }

		// A font merged after the UI font: ImGui keeps a glyph the first font has, so this one only fills what that one
		// lacks (the Cyrillic of a Latin-only font). Before the first add_to.
		void set_fallback_bytes(std::vector<std::byte> bytes) { m_fallback_bytes = std::move(bytes); }

		bool has_bytes() const { return !m_bytes.empty(); }
		bool usable() const { return looks_like_font(m_bytes); }
		bool has_icons() const { return looks_like_font(m_icon_bytes); }
		const ImVector<ImWchar>& ranges() const { return m_ranges; }

		// Adds the UI font (or the built-in one when there is no usable font) at `size_px`, with the fallback font and
		// the icons merged in.
		ImFont* add_to(ImFontAtlas& atlas, float size_px)
		{
			ImFont* font = add_text_font(atlas, size_px);
			if (font && usable() && looks_like_font(m_fallback_bytes)) {
				merge_fallback(atlas, size_px);
			}
			if (font && has_icons()) {
				merge_icons(atlas, size_px);
			}
			return font;
		}

	private:
		static constexpr std::uint32_t tag(const char (&name)[5])
		{
			return (std::uint32_t(std::uint8_t(name[0])) << 24) | (std::uint32_t(std::uint8_t(name[1])) << 16)
				| (std::uint32_t(std::uint8_t(name[2])) << 8) | std::uint32_t(std::uint8_t(name[3]));
		}

		static std::uint32_t u32_at(const std::vector<std::byte>& bytes, std::uint64_t at)
		{
			std::uint32_t value = 0;
			for (std::uint64_t i = 0; i < 4; ++i) {
				value = (value << 8) | static_cast<std::uint8_t>(bytes[static_cast<size_t>(at + i)]);
			}
			return value;
		}

		static std::uint32_t tag_at(const std::vector<std::byte>& bytes, std::uint64_t at) { return u32_at(bytes, at); }

		static std::uint16_t u16_at(const std::vector<std::byte>& bytes, std::uint64_t at)
		{
			return static_cast<std::uint16_t>((static_cast<std::uint8_t>(bytes[static_cast<size_t>(at)]) << 8)
				| static_cast<std::uint8_t>(bytes[static_cast<size_t>(at + 1)]));
		}

		static bool looks_like_sfnt(const std::vector<std::byte>& bytes, std::uint64_t base)
		{
			const std::uint64_t size = bytes.size();
			if (base + 12 > size) {
				return false;
			}
			const std::uint32_t version = tag_at(bytes, base);
			if (version != 0x00010000u && version != tag("true") && version != tag("OTTO")) {
				return false;
			}
			const std::uint64_t tables = u16_at(bytes, base + 4);
			if (tables == 0 || base + 12 + tables * 16 > size) {
				return false;
			}
			struct required {
				std::uint32_t name;
				std::uint64_t min_length;
				bool found = false;
			};
			// The lengths cover the fields stb_truetype reads at fixed offsets (head's indexToLocFormat at 50,
			// hhea's numberOfHMetrics at 34, maxp's numGlyphs at 4).
			required needed[] = { { tag("cmap"), 4 }, { tag("head"), 54 }, { tag("hhea"), 36 }, { tag("hmtx"), 0 },
				{ tag("maxp"), 6 } };
			bool glyf = false;
			bool loca = false;
			bool cff = false;
			for (std::uint64_t i = 0; i < tables; ++i) {
				const std::uint64_t record = base + 12 + i * 16;
				const std::uint32_t name = tag_at(bytes, record);
				const std::uint64_t offset = u32_at(bytes, record + 8);
				const std::uint64_t length = u32_at(bytes, record + 12);
				if (offset + length > size) {
					return false;
				}
				for (required& r : needed) {
					if (r.name == name) {
						r.found = length >= r.min_length;
					}
				}
				glyf = glyf || name == tag("glyf");
				loca = loca || name == tag("loca");
				cff = cff || name == tag("CFF ");
			}
			for (const required& r : needed) {
				if (!r.found) {
					return false;
				}
			}
			return (glyf && loca) || cff;
		}

		ImFont* add_text_font(ImFontAtlas& atlas, float size_px)
		{
			if (usable()) {
				if (m_ranges.empty()) {
					build_glyph_ranges(atlas, m_ranges);
				}
				ImFontConfig cfg;
				cfg.FontDataOwnedByAtlas = false;
				if (ImFont* font = atlas.AddFontFromMemoryTTF(m_bytes.data(), static_cast<int>(m_bytes.size()), size_px, &cfg, m_ranges.Data)) {
					return font;
				}
			}
			ImFontConfig cfg;
			cfg.SizePixels = size_px;
			return atlas.AddFontDefault(&cfg);
		}

		void merge_fallback(ImFontAtlas& atlas, float size_px)
		{
			if (m_ranges.empty()) {
				build_glyph_ranges(atlas, m_ranges);
			}
			ImFontConfig cfg;
			cfg.MergeMode = true;
			cfg.FontDataOwnedByAtlas = false;
			const int size = static_cast<int>(m_fallback_bytes.size());
			atlas.AddFontFromMemoryTTF(m_fallback_bytes.data(), size, size_px, &cfg, m_ranges.Data);
		}

		void merge_icons(ImFontAtlas& atlas, float size_px)
		{
			// The atlas keeps the pointer to the ranges.
			static constexpr ImWchar ranges[] = { icon::first, icon::last, 0 };
			ImFontConfig cfg;
			cfg.MergeMode = true;
			cfg.FontDataOwnedByAtlas = false;
			cfg.PixelSnapH = true;
			cfg.GlyphMinAdvanceX = size_px;
			const int size = static_cast<int>(m_icon_bytes.size());
			atlas.AddFontFromMemoryTTF(m_icon_bytes.data(), size, size_px, &cfg, ranges);
		}

		std::vector<std::byte> m_bytes;
		std::vector<std::byte> m_fallback_bytes;
		std::vector<std::byte> m_icon_bytes;
		ImVector<ImWchar> m_ranges;
	};
}
