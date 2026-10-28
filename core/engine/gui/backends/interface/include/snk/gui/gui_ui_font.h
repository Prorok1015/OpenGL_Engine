#pragma once
#include <imgui.h>
#include <cstddef>
#include <vector>

namespace gui
{
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

		// sfnt magic of TrueType / OpenType / TrueType collection. ImGui asserts on bytes that are not a font,
		// so a truncated or wrong file must be turned away before it reaches the atlas.
		static bool looks_like_font(const std::vector<std::byte>& bytes)
		{
			if (bytes.size() < 12) {
				return false;
			}
			auto at = [&](size_t i) { return static_cast<unsigned char>(bytes[i]); };
			const bool truetype = at(0) == 0x00 && at(1) == 0x01 && at(2) == 0x00 && at(3) == 0x00;
			const bool tagged = (at(0) == 't' && at(1) == 'r' && at(2) == 'u' && at(3) == 'e')
				|| (at(0) == 'O' && at(1) == 'T' && at(2) == 'T' && at(3) == 'O')
				|| (at(0) == 't' && at(1) == 't' && at(2) == 'c' && at(3) == 'f');
			return truetype || tagged;
		}

		// Call before the first add_to: the atlas keeps a pointer into the bytes.
		void set_bytes(std::vector<std::byte> bytes)
		{
			m_bytes = std::move(bytes);
			m_ranges.clear();
		}

		bool has_bytes() const { return !m_bytes.empty(); }
		bool usable() const { return looks_like_font(m_bytes); }
		const ImVector<ImWchar>& ranges() const { return m_ranges; }

		// Adds the UI font (or the built-in one when there is no usable font) at `size_px`.
		ImFont* add_to(ImFontAtlas& atlas, float size_px)
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

	private:
		std::vector<std::byte> m_bytes;
		ImVector<ImWchar> m_ranges;
	};
}
