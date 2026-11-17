#include <boost/test/unit_test.hpp>
#include <snk/gui/gui_ui_font.h>
#include <snk/resource/adapters/res_binary_adapter.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <utility>
#include <string_view>
#include <vector>

// gui::ui_font picks between the shipped TTF and ImGui's built-in font, and builds the glyph ranges the TTF is
// loaded with. Everything runs over a bare ImFontAtlas (no window, no GL context); the shipped
// core/res/fonts/NotoSans-Regular.ttf is checked for the Cyrillic glyphs the editor UI needs.

#ifndef SNK_CORE_RES_DIR
#error "SNK_CORE_RES_DIR must point at core/res"
#endif

namespace {

bool range_has(const ImVector<ImWchar>& ranges, unsigned code_point)
{
	// Pairs of [first, last], ended by 0.
	for (const ImWchar* r = ranges.Data; r && r[0] != 0; r += 2) {
		if (code_point >= static_cast<unsigned>(r[0]) && code_point <= static_cast<unsigned>(r[1])) {
			return true;
		}
	}
	return false;
}

std::vector<std::byte> bytes_of(std::initializer_list<unsigned> values)
{
	std::vector<std::byte> out;
	for (unsigned v : values) {
		out.push_back(static_cast<std::byte>(v));
	}
	return out;
}

struct table {
	std::string_view name;
	std::uint32_t length;
};

// A font file with these tables (zero filled) in a valid directory; `version` is the first four bytes.
std::vector<std::byte> sfnt(std::initializer_list<unsigned> version, const std::vector<table>& tables)
{
	std::vector<std::byte> out = bytes_of(version);
	auto u16 = [&](std::uint32_t v) { out.push_back(std::byte(v >> 8)); out.push_back(std::byte(v & 0xff)); };
	auto u32 = [&](std::uint32_t v) { u16(v >> 16); u16(v & 0xffff); };
	u16(static_cast<std::uint32_t>(tables.size()));
	u16(0);
	u16(0);
	u16(0);
	std::uint32_t offset = static_cast<std::uint32_t>(12 + 16 * tables.size());
	for (const table& t : tables) {
		for (char c : t.name) out.push_back(static_cast<std::byte>(c));
		u32(0);
		u32(offset);
		u32(t.length);
		offset += t.length;
	}
	out.resize(offset);
	return out;
}

// A TrueType collection holding `font` as its only font.
std::vector<std::byte> collection_of(const std::vector<std::byte>& font)
{
	std::vector<std::byte> out = bytes_of({ 't', 't', 'c', 'f', 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 16 });
	const std::size_t base = out.size();
	out.insert(out.end(), font.begin(), font.end());
	// The table offsets of a collection's font count from the start of the file.
	const std::uint32_t tables = (std::to_integer<std::uint32_t>(out[base + 4]) << 8)
		| std::to_integer<std::uint32_t>(out[base + 5]);
	for (std::uint32_t i = 0; i < tables; ++i) {
		const std::size_t at = base + 12 + i * 16 + 8;
		std::uint32_t offset = 0;
		for (std::size_t b = 0; b < 4; ++b) offset = (offset << 8) | std::to_integer<std::uint32_t>(out[at + b]);
		offset += static_cast<std::uint32_t>(base);
		for (std::size_t b = 0; b < 4; ++b) out[at + b] = std::byte((offset >> (24 - 8 * b)) & 0xff);
	}
	return out;
}

std::vector<std::byte> read_bytes(const std::filesystem::path& path)
{
	std::ifstream in(path, std::ios::binary);
	const std::vector<char> raw{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	std::vector<std::byte> out(raw.size());
	std::memcpy(out.data(), raw.data(), raw.size());
	return out;
}

}

BOOST_AUTO_TEST_SUITE(GuiUiFontTests)

BOOST_AUTO_TEST_CASE(WithoutBytesTheBuiltInFontIsAdded)
{
	ImFontAtlas atlas;
	gui::ui_font font;

	BOOST_TEST(!font.has_bytes());
	BOOST_TEST(!font.usable());
	ImFont* added = font.add_to(atlas, 13.f);

	BOOST_REQUIRE(added != nullptr);
	BOOST_TEST(atlas.Fonts.Size == 1);
	BOOST_TEST(atlas.ConfigData.Size == 1);
	BOOST_TEST(atlas.ConfigData[0].SizePixels == 13.f);
}

BOOST_AUTO_TEST_CASE(BytesThatAreNotAFontFallBackToTheBuiltInFont)
{
	ImFontAtlas atlas;
	gui::ui_font font;
	font.set_bytes(std::vector<std::byte>(64, std::byte{ 0x42 }));

	BOOST_TEST(font.has_bytes());
	BOOST_TEST(!font.usable());
	// ImGui would assert on these bytes; the built-in font is what got added, at the requested size.
	BOOST_REQUIRE(font.add_to(atlas, 26.f) != nullptr);
	BOOST_TEST(atlas.Fonts.Size == 1);
	BOOST_TEST(atlas.ConfigData[0].SizePixels == 26.f);
}

BOOST_AUTO_TEST_CASE(TheFontCheckReadsTheTableDirectory)
{
	using gui::ui_font;
	const std::vector<table> truetype_tables = { { "cmap", 4 }, { "head", 54 }, { "hhea", 36 }, { "hmtx", 4 },
		{ "maxp", 6 }, { "loca", 4 }, { "glyf", 4 } };
	BOOST_TEST(ui_font::looks_like_font(sfnt({ 0, 1, 0, 0 }, truetype_tables)));
	BOOST_TEST(ui_font::looks_like_font(sfnt({ 't', 'r', 'u', 'e' }, truetype_tables)));
	BOOST_TEST(ui_font::looks_like_font(sfnt({ 'O', 'T', 'T', 'O' }, { { "cmap", 4 }, { "head", 54 },
		{ "hhea", 36 }, { "hmtx", 4 }, { "maxp", 6 }, { "CFF ", 4 } })));
	BOOST_TEST(ui_font::looks_like_font(collection_of(sfnt({ 0, 1, 0, 0 }, truetype_tables))));

	// The magic alone is not a font any more: it is what a truncated file still has.
	BOOST_TEST(!ui_font::looks_like_font(bytes_of({ 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 })));
	BOOST_TEST(!ui_font::looks_like_font(bytes_of({ 't', 't', 'c', 'f', 0, 0, 0, 0, 0, 0, 0, 0 })));
	BOOST_TEST(!ui_font::looks_like_font({}));
	BOOST_TEST(!ui_font::looks_like_font(bytes_of({ '<', 'h', 't', 'm', 'l', '>', 0, 0, 0, 0, 0, 0 })));

	auto without = [&](std::string_view name) {
		std::vector<table> tables;
		for (const table& t : truetype_tables) {
			if (t.name != name) tables.push_back(t);
		}
		return sfnt({ 0, 1, 0, 0 }, tables);
	};
	BOOST_TEST(!ui_font::looks_like_font(without("cmap")));
	BOOST_TEST(!ui_font::looks_like_font(without("glyf")));
	BOOST_TEST(!ui_font::looks_like_font(sfnt({ 0, 1, 0, 0 }, { { "cmap", 4 }, { "head", 20 }, { "hhea", 36 },
		{ "hmtx", 4 }, { "maxp", 6 }, { "loca", 4 }, { "glyf", 4 } })));

	std::vector<std::byte> past_the_end = sfnt({ 0, 1, 0, 0 }, truetype_tables);
	past_the_end.resize(past_the_end.size() - 1);
	BOOST_TEST(!ui_font::looks_like_font(past_the_end));
}

BOOST_AUTO_TEST_CASE(ATruncatedRealFont_IsNoFont_AndTheBuiltInFontIsAdded)
{
	const std::filesystem::path fonts = std::filesystem::path{ SNK_CORE_RES_DIR } / "fonts";
	for (const char* name : { "NotoSans-Regular.ttf", "lucide-icons.ttf" }) {
		BOOST_TEST_CONTEXT(name) {
			std::vector<std::byte> bytes = read_bytes(fonts / name);
			BOOST_REQUIRE(gui::ui_font::looks_like_font(bytes));
			bytes.resize(bytes.size() / 2);
			BOOST_TEST(!gui::ui_font::looks_like_font(bytes));

			gui::ui_font font;
			font.set_bytes(bytes);
			font.set_icon_bytes(bytes);
			ImFontAtlas atlas;
			BOOST_REQUIRE(font.add_to(atlas, 13.f) != nullptr);
			BOOST_TEST(atlas.Fonts.Size == 1);
			BOOST_TEST(atlas.ConfigData.Size == 1);
			BOOST_TEST(atlas.Build());
		}
	}
}

BOOST_AUTO_TEST_CASE(GlyphRangesCoverLatinCyrillicAndGreek)
{
	ImFontAtlas atlas;
	ImVector<ImWchar> ranges;
	gui::ui_font::build_glyph_ranges(atlas, ranges);

	BOOST_TEST(range_has(ranges, 'A'));
	BOOST_TEST(range_has(ranges, 0x00E9));
	BOOST_TEST(range_has(ranges, 0x0400));
	BOOST_TEST(range_has(ranges, 0x0416));
	BOOST_TEST(range_has(ranges, 0x044F));
	BOOST_TEST(range_has(ranges, 0x04FF));
	BOOST_TEST(range_has(ranges, 0x03A9));
	BOOST_TEST(!range_has(ranges, 0x4E2D));
}

BOOST_AUTO_TEST_CASE(TheBinaryAdapterKeepsTheBytesAsTheyAre)
{
	const res::binary_adapter adapter;
	const res::tag tag{ "res://fonts/x.ttf" };
	const auto raw = bytes_of({ 0, 1, 0, 0, 0xFF, 0x80, 0x00, 0x7F });

	auto entry = adapter.deserialize(tag, raw);
	BOOST_REQUIRE(entry != nullptr);
	auto binary = std::dynamic_pointer_cast<res::binary_resource>(entry);
	BOOST_REQUIRE(binary != nullptr);
	BOOST_TEST((binary->bytes() == raw));
	BOOST_TEST((adapter.serialize(tag, entry) == raw));
}

BOOST_AUTO_TEST_CASE(TheShippedFontHasCyrillicGlyphs)
{
	gui::ui_font font;
	font.set_bytes(read_bytes(std::filesystem::path{ SNK_CORE_RES_DIR } / "fonts" / "NotoSans-Regular.ttf"));
	BOOST_REQUIRE(font.usable());

	ImFontAtlas atlas;
	ImFont* added = font.add_to(atlas, 13.f);
	BOOST_REQUIRE(added != nullptr);
	BOOST_REQUIRE(atlas.Build());

	BOOST_TEST(added->FindGlyphNoFallback(0x0416) != nullptr); // Ж
	BOOST_TEST(added->FindGlyphNoFallback(0x044F) != nullptr); // я
	BOOST_TEST(added->FindGlyphNoFallback(0x03A9) != nullptr); // Ω
	BOOST_TEST(added->FindGlyphNoFallback('A') != nullptr);
}

BOOST_AUTO_TEST_CASE(TheIconsMergeIntoTheUiFont_WhichKeepsItsCyrillic)
{
	const std::filesystem::path fonts = std::filesystem::path{ SNK_CORE_RES_DIR } / "fonts";
	gui::ui_font font;
	font.set_bytes(read_bytes(fonts / "NotoSans-Regular.ttf"));
	font.set_icon_bytes(read_bytes(fonts / "lucide-icons.ttf"));
	BOOST_REQUIRE(font.usable());
	BOOST_REQUIRE(font.has_icons());

	ImFontAtlas atlas;
	ImFont* added = font.add_to(atlas, 14.f);
	BOOST_REQUIRE(added != nullptr);
	BOOST_REQUIRE(atlas.Build());

	BOOST_TEST(atlas.Fonts.Size == 1);
	BOOST_TEST(added->FindGlyphNoFallback(0x0416) != nullptr); // Ж
	BOOST_TEST(added->FindGlyphNoFallback('A') != nullptr);
	BOOST_TEST(added->FindGlyphNoFallback(gui::icon::first) != nullptr);
	BOOST_TEST(added->FindGlyphNoFallback(gui::icon::last) != nullptr);
}

// #1136: the editor picks a face by its index in the atlas.
BOOST_AUTO_TEST_CASE(TheFaces_AreTextBoldMono_InThatOrder_EachWithCyrillicAndIcons)
{
	const std::filesystem::path fonts = std::filesystem::path{ SNK_CORE_RES_DIR } / "fonts";
	gui::ui_font font;
	const std::vector<std::byte> text = read_bytes(fonts / "NotoSans-Regular.ttf");
	const std::vector<std::byte> bold = read_bytes(fonts / "NotoSans-Bold.ttf");
	const std::vector<std::byte> mono = read_bytes(fonts / "NotoSansMono-Regular.ttf");
	font.set_bytes(text);
	font.set_bold_bytes(bold);
	font.set_mono_bytes(mono);
	font.set_icon_bytes(read_bytes(fonts / "lucide-icons.ttf"));

	ImFontAtlas atlas;
	ImFont* added = font.add_faces_to(atlas, 14.f);
	BOOST_REQUIRE(added != nullptr);
	BOOST_REQUIRE(atlas.Build());

	BOOST_REQUIRE(atlas.Fonts.Size == gui::ui_face_count);
	BOOST_TEST(atlas.Fonts[int(gui::ui_face::text)] == added);
	const std::pair<gui::ui_face, const std::vector<std::byte>*> faces[] = {
		{ gui::ui_face::text, &text }, { gui::ui_face::bold, &bold }, { gui::ui_face::mono, &mono } };
	for (const auto& [face, bytes] : faces) {
		BOOST_TEST_CONTEXT(int(face)) {
			const ImFont* f = atlas.Fonts[int(face)];
			BOOST_TEST(f->ConfigData->FontDataSize == static_cast<int>(bytes->size()));
			BOOST_TEST(f->FindGlyphNoFallback(0x0416) != nullptr); // Ж
			BOOST_TEST(f->FindGlyphNoFallback(gui::icon::first) != nullptr);
		}
	}
}

BOOST_AUTO_TEST_CASE(WithoutBoldOrMonoBytes_TheTextFontTakesTheirIndices)
{
	const std::filesystem::path fonts = std::filesystem::path{ SNK_CORE_RES_DIR } / "fonts";
	gui::ui_font font;
	const std::vector<std::byte> text = read_bytes(fonts / "NotoSans-Regular.ttf");
	font.set_bytes(text);
	font.set_mono_bytes(bytes_of({ 1, 2, 3, 4 }));

	ImFontAtlas atlas;
	BOOST_REQUIRE(font.add_faces_to(atlas, 13.f) != nullptr);
	BOOST_REQUIRE(atlas.Build());

	BOOST_REQUIRE(atlas.Fonts.Size == gui::ui_face_count);
	for (const ImFont* f : atlas.Fonts) {
		BOOST_TEST(f->ConfigData->FontDataSize == static_cast<int>(text.size()));
		BOOST_TEST(f->FindGlyphNoFallback(0x044F) != nullptr); // я
	}
}

BOOST_AUTO_TEST_SUITE_END()
