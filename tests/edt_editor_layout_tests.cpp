#include <boost/test/unit_test.hpp>
#include <snk/editor/edt_editor_extension.h>
#include <snk/editor/edt_editor_layout_key.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

// #969: the editor layout key a game_editor module reports is evaluated from the editor headers it was built with; the
// editor compares it with its own before it creates the module (edt::check_editor_api_version).

namespace {

// The key of `Values` with the value at `Raised` one higher.
template<std::size_t Raised, std::uint64_t... Values, std::size_t... I>
constexpr std::uint64_t key_with_raised(gs::detail::layout_values<Values...>, std::index_sequence<I...>)
{
	return edt::detail::editor_layout_of<gs::detail::layout_values<(I == Raised ? Values + 1u : Values)...>>();
}

template<std::uint64_t... Values, std::size_t... I>
constexpr bool raising_each_changes_the_key(gs::detail::layout_values<Values...> values, std::index_sequence<I...> order)
{
	const std::uint64_t key = edt::detail::editor_layout_of<gs::detail::layout_values<Values...>>();
	return ((key_with_raised<I>(values, order) != key) && ...);
}

template<std::uint64_t... Values>
constexpr bool raising_each_changes_the_key(gs::detail::layout_values<Values...> values)
{
	return raising_each_changes_the_key(values, std::make_index_sequence<sizeof...(Values)>{});
}

std::uint64_t this_binarys_key() { return edt::editor_layout_key(); }

}

BOOST_AUTO_TEST_SUITE(edt_editor_layout_tests)

// Every CI configuration runs this: a module built with these headers and the editor of the same build agree.
BOOST_AUTO_TEST_CASE(a_binary_built_with_the_editor_headers_has_the_editor_key)
{
	static_assert(edt::editor_layout_key() != 0u);
	BOOST_TEST(edt::check_editor_api_version([]() -> std::uint32_t { return SNK_EDITOR_API_VERSION; }, &this_binarys_key).empty());
}

BOOST_AUTO_TEST_CASE(a_raised_editor_interface_revision_changes_the_key)
{
	using edt::detail::editor_layout_of;
	using gs::detail::layout_values;
	static_assert(raising_each_changes_the_key(edt::detail::editor_interface_abi_revisions{}));
	static_assert(editor_layout_of<std::uint16_t, layout_values<1u, 1u>>() != editor_layout_of<std::uint16_t, layout_values<1u, 2u>>());
	BOOST_TEST((editor_layout_of<layout_values<1u>>() != editor_layout_of<layout_values<2u>>()));
}

BOOST_AUTO_TEST_CASE(another_imgui_version_or_size_of_its_state_changes_the_key)
{
	using edt::detail::editor_layout_of;
	using gs::detail::layout_values;
	static_assert(raising_each_changes_the_key(edt::detail::imgui_version{}));
	struct grown_style { ImGuiStyle style; float extra = 0.f; };
	static_assert(editor_layout_of<ImGuiStyle, ImGuiIO>() != editor_layout_of<grown_style, ImGuiIO>());
	BOOST_TEST((editor_layout_of<layout_values<IMGUI_VERSION_NUM>>() != editor_layout_of<layout_values<IMGUI_VERSION_NUM + 1>>()));
}

BOOST_AUTO_TEST_CASE(a_size_change_of_a_listed_type_changes_the_key)
{
	struct grown { std::uint64_t a = 0; std::uint64_t b = 0; };
	BOOST_TEST(edt::detail::editor_layout_of<std::uint64_t>() != edt::detail::editor_layout_of<grown>());
	BOOST_TEST(edt::detail::editor_layout_of<std::uint64_t>() == edt::detail::editor_layout_of<std::uint64_t>());
}

BOOST_AUTO_TEST_SUITE_END()
