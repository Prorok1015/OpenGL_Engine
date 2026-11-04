#include <boost/test/unit_test.hpp>
#include <snk/mcp/mcp_reparse_rule.h>
#include <snk/platform/plt_reparse_tag.h>
#include <cstdint>
#include <random>
#include <vector>

// The MCP module keeps its own copy of the platform folder rule (#529); this keeps the two from drifting apart.

namespace {

std::vector<std::uint32_t> tags_to_compare()
{
	std::vector<std::uint32_t> tags;
	for (std::uint32_t variant = 0; variant <= 0xFu; ++variant) {
		tags.push_back(0x9000001Au | (variant << 12)); // CLOUD, CLOUD_1..CLOUD_F
	}
	tags.insert(tags.end(), {
		0x80000013u, // DEDUP
		0x80000017u, // WOF
		0x80000018u, // WCI
		0x90001018u, // WCI_1
		0x9000101Bu, // not a cloud tag: another low word under the cloud mask bits
		0xA0000003u, // MOUNT_POINT (junction)
		0xA000000Cu, // SYMLINK
		0xA000001Du, // LX_SYMLINK
		0x80000023u, // AF_UNIX
		0x8000001Bu, // APPEXECLINK
		0x00000000u,
		0xFFFFFFFFu,
	});
	std::mt19937 random(529);
	for (int i = 0; i < 1000; ++i) {
		tags.push_back(static_cast<std::uint32_t>(random()));
	}
	return tags;
}

}

BOOST_AUTO_TEST_SUITE(McpReparseRuleTests)

BOOST_AUTO_TEST_CASE(McpReparseRule_MatchesThePlatformRule)
{
	const std::uint32_t directory = platform::beneath::k_file_attribute_directory;
	const std::uint32_t reparse = platform::beneath::k_file_attribute_reparse_point;
	const std::uint32_t attribute_sets[] = { 0u, directory, reparse, directory | reparse, directory | reparse | 0x1u };
	for (const std::uint32_t tag : tags_to_compare()) {
		for (const std::uint32_t attributes : attribute_sets) {
			for (const bool tag_known : { true, false }) {
				const auto platform_entry = platform::beneath::folder_entry_of(attributes, tag_known, tag);
				const auto mcp_entry = mcp::detail::folder_entry_of(attributes, tag_known, tag);
				BOOST_TEST(static_cast<int>(platform_entry) == static_cast<int>(mcp_entry),
					"tag " << std::hex << tag << " attributes " << attributes << " known " << tag_known);
			}
		}
	}
}

BOOST_AUTO_TEST_CASE(McpReparseRule_EntersOneDriveAndDedup_RefusesLinksAndOtherFilters)
{
	using mcp::detail::folder_entry;
	using mcp::detail::folder_entry_of;
	constexpr std::uint32_t reparse_folder = mcp::detail::k_attribute_directory | mcp::detail::k_attribute_reparse_point;
	BOOST_TEST((folder_entry_of(mcp::detail::k_attribute_directory, true, 0) == folder_entry::enter));
	BOOST_TEST((folder_entry_of(reparse_folder, true, 0x9000001Au) == folder_entry::through_filter)); // CLOUD
	BOOST_TEST((folder_entry_of(reparse_folder, true, 0x9000301Au) == folder_entry::through_filter)); // CLOUD_3
	BOOST_TEST((folder_entry_of(reparse_folder, true, 0x80000013u) == folder_entry::through_filter)); // DEDUP
	BOOST_TEST((folder_entry_of(reparse_folder, false, 0x9000001Au) == folder_entry::refused));      // tag unreadable
	BOOST_TEST((folder_entry_of(reparse_folder, true, 0xA0000003u) == folder_entry::refused));       // junction
	BOOST_TEST((folder_entry_of(reparse_folder, true, 0xA000000Cu) == folder_entry::refused));       // symlink
	BOOST_TEST((folder_entry_of(reparse_folder, true, 0x80000017u) == folder_entry::refused));       // WOF
	BOOST_TEST((folder_entry_of(reparse_folder, true, 0x90001018u) == folder_entry::refused));       // WCI_1
	BOOST_TEST((folder_entry_of(reparse_folder, true, 0x9000101Bu) == folder_entry::refused));
	BOOST_TEST((folder_entry_of(mcp::detail::k_attribute_reparse_point, true, 0x9000001Au) == folder_entry::not_a_folder));
}

BOOST_AUTO_TEST_SUITE_END()
