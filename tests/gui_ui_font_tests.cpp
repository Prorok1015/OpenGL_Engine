#include <boost/test/unit_test.hpp>
#include <snk/gui/gui_ui_font.h>
#include <snk/resource/adapters/res_binary_adapter.h>

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <vector>

// gui::ui_font picks between the shipped TTF and ImGui's built-in font, and builds the glyph ranges the TTF is
// loaded with. No real TTF is shipped with the tests, so what is covered is the selection and the ranges, over a
// bare ImFontAtlas (no window, no GL context): the TTF decoding itself is ImGui's.

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

BOOST_AUTO_TEST_CASE(TheFontHeaderCheckKnowsTheSfntMagics)
{
	using gui::ui_font;
	BOOST_TEST(ui_font::looks_like_font(bytes_of({ 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 })));
	BOOST_TEST(ui_font::looks_like_font(bytes_of({ 'O', 'T', 'T', 'O', 0, 0, 0, 0, 0, 0, 0, 0 })));
	BOOST_TEST(ui_font::looks_like_font(bytes_of({ 't', 't', 'c', 'f', 0, 0, 0, 0, 0, 0, 0, 0 })));
	BOOST_TEST(!ui_font::looks_like_font({}));
	BOOST_TEST(!ui_font::looks_like_font(bytes_of({ 0, 1, 0, 0 })));
	BOOST_TEST(!ui_font::looks_like_font(bytes_of({ '<', 'h', 't', 'm', 'l', '>', 0, 0, 0, 0, 0, 0 })));
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

BOOST_AUTO_TEST_SUITE_END()
