#include <boost/test/unit_test.hpp>
#include <boost/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

// The layout of the shipped resources (#163): res:// holds game resources only, and every res:// / edt:// path
// the shipped descs mention resolves to a file.

#ifndef SNK_EDITOR_RES_DIR
#error "SNK_EDITOR_RES_DIR must point at Editor/res"
#endif
#ifndef SNK_CORE_RES_DIR
#error "SNK_CORE_RES_DIR must point at core/res"
#endif

namespace fs = std::filesystem;
namespace json = boost::json;

namespace {

const fs::path core_res{ SNK_CORE_RES_DIR };
const fs::path editor_res{ SNK_EDITOR_RES_DIR };
const fs::path editor_data = editor_res.parent_path() / "data";

bool has_file(const std::vector<fs::path>& layers, std::string_view rel)
{
	return std::ranges::any_of(layers, [&](const fs::path& layer) { return fs::is_regular_file(layer / rel); });
}

void collect_references(const json::value& value, std::vector<std::string>& out)
{
	if (value.is_string()) {
		const std::string_view text = value.as_string();
		if (text.find("://") != std::string_view::npos) out.emplace_back(text);
	} else if (value.is_object()) {
		for (const auto& member : value.as_object()) collect_references(member.value(), out);
	} else if (value.is_array()) {
		for (const auto& element : value.as_array()) collect_references(element, out);
	}
}

json::value read_json(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	const std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	return json::parse(text);
}

}

BOOST_AUTO_TEST_SUITE(ResRepoLayout)

BOOST_AUTO_TEST_CASE(GameResourceRoots_DoNotHoldLeftoverTestFiles)
{
	for (const char* name : { "test.jpg", "test2.png", "block.png", "milla.bmp", "window.png",
	                          "test_desc.desc", "test_field_desc.desc", "test_field_parent_desc.desc",
	                          "test_parent_desc.desc", "test_sub_field_desc.desc" }) {
		BOOST_TEST(!fs::exists(editor_res / name), editor_res / name);
		BOOST_TEST(!fs::exists(core_res / name), core_res / name);
	}
}

BOOST_AUTO_TEST_CASE(EveryResAndEdtPathInShippedDescs_ResolvesToAFile)
{
	const std::vector<fs::path> res_layers{ core_res, editor_res };
	const std::vector<fs::path> edt_layers{ editor_data };

	std::size_t checked = 0;
	for (const fs::path& root : { core_res, editor_res, editor_data }) {
		if (!fs::is_directory(root)) continue;
		for (const auto& entry : fs::recursive_directory_iterator(root)) {
			if (!entry.is_regular_file() || entry.path().extension() != ".desc") continue;
			// The samples submodule is optional (update = none) and has its own content.
			if (std::ranges::any_of(entry.path(), [](const fs::path& part) { return part == "samples"; })) continue;

			std::vector<std::string> references;
			collect_references(read_json(entry.path()), references);
			for (const std::string& reference : references) {
				const std::size_t split = reference.find("://");
				const std::string_view protocol = std::string_view{ reference }.substr(0, split);
				const std::string_view rel = std::string_view{ reference }.substr(split + 3);
				if (protocol == "res") {
					BOOST_TEST(has_file(res_layers, rel), entry.path().generic_string() << " -> " << reference);
				} else if (protocol == "edt") {
					BOOST_TEST(has_file(edt_layers, rel), entry.path().generic_string() << " -> " << reference);
				} else {
					BOOST_ERROR(entry.path().generic_string() << " uses the unexpected protocol in " << reference);
				}
				++checked;
			}
		}
	}
	BOOST_TEST(checked > 0u);
}

BOOST_AUTO_TEST_SUITE_END()
