#include <boost/test/unit_test.hpp>
#include "test_temp_dir.hpp"
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <snk/resource/res_free_name.h>
#ifndef _WIN32
#include <unistd.h>
#endif

// #1279: a new res:// name is free in every layer, and the next free numbered name.
namespace {

namespace fs = std::filesystem;

struct two_layers
{
	test_util::temp_dir dir{ "res_free_name" };
	fs::path lower = dir.path() / "engine";
	fs::path upper = dir.path() / "project";

	two_layers()
	{
		fs::create_directories(lower);
		fs::create_directories(upper);
	}

	std::vector<fs::path> layers() const { return { lower, upper }; }
};

void touch(const fs::path& file)
{
	fs::create_directories(file.parent_path());
	std::ofstream made(file, std::ios::binary);
	BOOST_REQUIRE_MESSAGE(made.is_open(), "cannot write " << file.string());
}

}

BOOST_FIXTURE_TEST_SUITE(ResFreeName, two_layers)

BOOST_AUTO_TEST_CASE(NameTaken_AFreeName_IsNotRefused)
{
	touch(upper / "a" / "other.desc");
	BOOST_TEST(!res::name_taken(layers(), "a/free.desc").has_value());
	BOOST_TEST(!res::name_taken(layers(), "free.desc").has_value());
}

BOOST_AUTO_TEST_CASE(NameTaken_InTheTopLayer_AndInALowerOne_NamesTheLayerAndThePath)
{
	touch(upper / "a" / "top.desc");
	touch(lower / "a" / "low.desc");
	touch(lower / "a" / "both.desc");
	touch(upper / "a" / "both.desc");

	const auto top = res::name_taken(layers(), "a/top.desc");
	BOOST_REQUIRE(top.has_value());
	BOOST_TEST(top->layer == 1u);
	BOOST_TEST(top->existing == upper / "a" / "top.desc");
	BOOST_TEST(!top->meta_left);
	BOOST_TEST(top->error.empty());

	const auto low = res::name_taken(layers(), "a/low.desc");
	BOOST_REQUIRE(low.has_value());
	BOOST_TEST(low->layer == 0u);
	BOOST_TEST(low->existing == lower / "a" / "low.desc");

	// Walked highest first: the top layer answers.
	const auto both = res::name_taken(layers(), "a/both.desc");
	BOOST_REQUIRE(both.has_value());
	BOOST_TEST(both->layer == 1u);
}

BOOST_AUTO_TEST_CASE(NameTaken_ByALeftoverResource)
{
	touch(lower / "gone.desc.resource");
	const auto taken = res::name_taken(layers(), "gone.desc");
	BOOST_REQUIRE(taken.has_value());
	BOOST_TEST(taken->layer == 0u);
	BOOST_TEST(taken->meta_left);
	BOOST_TEST(taken->existing == lower / "gone.desc.resource");
}

// Folded on every platform, so a project made on Linux never holds two names Windows takes for one.
BOOST_AUTO_TEST_CASE(NameTaken_ANameThatDiffersOnlyInCase_IsTakenOnEveryPlatform)
{
	touch(upper / "Rock.Geom.desc");
	const auto taken = res::name_taken(layers(), "rock.geom.DESC");
	BOOST_REQUIRE(taken.has_value());
	BOOST_TEST(taken->layer == 1u);
	BOOST_TEST(taken->existing.filename() == fs::path("Rock.Geom.desc"));

	touch(lower / "Old.DESC.RESOURCE");
	const auto meta = res::name_taken(layers(), "old.desc");
	BOOST_REQUIRE(meta.has_value());
	BOOST_TEST(meta->meta_left);
}

BOOST_AUTO_TEST_CASE(NameTaken_ByALeftoverBinaryOrStagedBinaryOfTheDesc)
{
	touch(lower / "mesh.geom.bin");
	const auto binary = res::name_taken(layers(), "mesh.geom.desc");
	BOOST_REQUIRE(binary.has_value());
	BOOST_TEST(binary->layer == 0u);
	BOOST_TEST(!binary->meta_left);
	BOOST_TEST(binary->existing == lower / "mesh.geom.bin");

	touch(upper / "Staged.Geom.BIN.new");
	const auto staged = res::name_taken(layers(), "staged.geom.desc");
	BOOST_REQUIRE(staged.has_value());
	BOOST_TEST(staged->layer == 1u);
	BOOST_TEST(staged->existing == upper / "Staged.Geom.BIN.new");

	// "a.desc" has no binary file (res::desc_binary::binary_tag), and a name that is no desc has none either.
	touch(upper / "a.bin");
	BOOST_TEST(!res::name_taken(layers(), "a.desc").has_value());
	touch(upper / "tex.png.bin");
	BOOST_TEST(!res::name_taken(layers(), "tex.png").has_value());
}

BOOST_AUTO_TEST_CASE(NameTaken_AMissingFolder_OrAFileOnTheWay_IsSkipped)
{
	touch(lower / "deep" / "x.desc");
	BOOST_TEST(!res::name_taken(layers(), "nowhere/at/all/x.desc").has_value());
	touch(upper / "deep");
	const auto below = res::name_taken(layers(), "deep/x.desc");
	BOOST_REQUIRE(below.has_value());
	BOOST_TEST(below->layer == 0u);
	BOOST_TEST(below->error.empty());
	BOOST_TEST(!res::name_taken({ dir.path() / "no_layer" }, "x.desc").has_value());
}

#ifndef _WIN32
BOOST_AUTO_TEST_CASE(NameTaken_AnUnreadableFolder_IsARefusalWithItsError)
{
	if (::geteuid() == 0) {
		BOOST_TEST_MESSAGE("root reads every folder: the listing cannot be made to fail");
		return;
	}
	const fs::path locked = upper / "locked";
	fs::create_directories(locked);
	touch(lower / "locked" / "x.desc");
	fs::permissions(locked, fs::perms::none);
	struct restore_on_exit {
		fs::path path;
		~restore_on_exit()
		{
			std::error_code ec;
			fs::permissions(path, fs::perms::owner_all, ec);
		}
	} restore{ locked };

	const auto taken = res::name_taken(layers(), "locked/x.desc");
	BOOST_REQUIRE(taken.has_value());
	BOOST_TEST(taken->layer == 1u);
	BOOST_TEST(!taken->error.empty());
	BOOST_TEST(taken->existing == locked);

	const res::free_name_result found = res::free_name(layers(), "locked", "x", ".desc");
	BOOST_TEST(!found.ok());
	BOOST_TEST(found.name.empty());
	BOOST_TEST(!found.error.empty());
}
#endif

// A link is an entry like any other: its name is taken, and a folder that is a link is never entered.
BOOST_AUTO_TEST_CASE(NameTaken_ALinkIsNotFollowed)
{
	touch(lower / "real" / "inside.desc");
	std::error_code file_link, folder_link;
	fs::create_symlink(upper / "nothing.desc", upper / "dangling.desc", file_link);
	fs::create_directory_symlink(lower / "real", upper / "linked", folder_link);
	if (file_link || folder_link) {
		BOOST_TEST_MESSAGE("no links can be made here: " << (file_link ? file_link : folder_link).message());
		return;
	}

	const auto dangling = res::name_taken(layers(), "dangling.desc");
	BOOST_REQUIRE(dangling.has_value());
	BOOST_TEST(dangling->layer == 1u);
	BOOST_TEST(dangling->existing == upper / "dangling.desc");

	const auto linked = res::name_taken(layers(), "linked/inside.desc");
	BOOST_REQUIRE(linked.has_value());
	BOOST_TEST(linked->layer == 1u);
	BOOST_TEST(!linked->error.empty());
}

BOOST_AUTO_TEST_CASE(FreeName_TheBaseName_WhenItIsFree)
{
	const res::free_name_result found = res::free_name(layers(), "shaders", "rock", ".shader.desc");
	BOOST_REQUIRE_MESSAGE(found.ok(), found.error);
	BOOST_TEST(found.name == "rock.shader.desc");
}

BOOST_AUTO_TEST_CASE(FreeName_ANumberBeforeTheWholeSuffix_SkippingTakenNamesInAnyLayer)
{
	touch(upper / "shaders" / "rock.shader.desc");
	const res::free_name_result second = res::free_name(layers(), "shaders", "rock", ".shader.desc");
	BOOST_REQUIRE_MESSAGE(second.ok(), second.error);
	BOOST_TEST(second.name == "rock-2.shader.desc");

	touch(lower / "shaders" / "ROCK-2.shader.desc.resource");
	const res::free_name_result third = res::free_name(layers(), "shaders", "rock", ".shader.desc");
	BOOST_REQUIRE_MESSAGE(third.ok(), third.error);
	BOOST_TEST(third.name == "rock-3.shader.desc");
}

BOOST_AUTO_TEST_CASE(FreeName_ACustomSeparator)
{
	touch(upper / "hero_variant.prefab.desc");
	const res::free_name_result none = res::free_name(layers(), "", "hero_variant", ".prefab.desc", "");
	BOOST_REQUIRE_MESSAGE(none.ok(), none.error);
	BOOST_TEST(none.name == "hero_variant2.prefab.desc");

	const res::free_name_result underscore = res::free_name(layers(), "", "hero_variant", ".prefab.desc", "_");
	BOOST_REQUIRE_MESSAGE(underscore.ok(), underscore.error);
	BOOST_TEST(underscore.name == "hero_variant_2.prefab.desc");
}

BOOST_AUTO_TEST_CASE(FreeName_EveryNumberUpToTheMaxTaken_IsAnError)
{
	touch(upper / "a.desc");
	touch(upper / "a-2.desc");
	touch(lower / "a-3.desc");
	const res::free_name_result full = res::free_name(layers(), "", "a", ".desc", "-", {}, 3);
	BOOST_TEST(!full.ok());
	BOOST_TEST(full.name.empty());
	BOOST_TEST(full.error.find("taken") != std::string::npos, full.error);

	const res::free_name_result one_more = res::free_name(layers(), "", "a", ".desc", "-", {}, 4);
	BOOST_REQUIRE_MESSAGE(one_more.ok(), one_more.error);
	BOOST_TEST(one_more.name == "a-4.desc");
}

BOOST_AUTO_TEST_CASE(FreeName_AlsoTaken_IsAskedWithThePathBelowRes)
{
	std::vector<std::string> asked;
	const auto also_taken = [&asked](std::string_view relative) {
		asked.emplace_back(relative);
		return relative == "m/rock.shader.desc" || relative == "m/rock-2.shader.desc";
	};
	const res::free_name_result found = res::free_name(layers(), "m", "rock", ".shader.desc", "-", also_taken);
	BOOST_REQUIRE_MESSAGE(found.ok(), found.error);
	BOOST_TEST(found.name == "rock-3.shader.desc");
	BOOST_TEST((asked == std::vector<std::string>{ "m/rock.shader.desc", "m/rock-2.shader.desc", "m/rock-3.shader.desc" }));
}

BOOST_AUTO_TEST_CASE(FreeName_ACandidateThatIsNoPortableName_IsAnError_NotSkipped)
{
	const res::free_name_result device = res::free_name(layers(), "", "CON", ".desc");
	BOOST_TEST(!device.ok());
	BOOST_TEST(device.error.find("portable") != std::string::npos, device.error);

	// "a.desc" is taken and the next candidate holds ':': an error, never a skip to "a:3.desc".
	touch(upper / "a.desc");
	const res::free_name_result colon = res::free_name(layers(), "", "a", ".desc", ":");
	BOOST_TEST(!colon.ok());
	BOOST_TEST(colon.name.empty());
	BOOST_TEST(colon.error.find("a:2.desc") != std::string::npos, colon.error);

	const res::free_name_result trailing = res::free_name(layers(), "", "y", ".");
	BOOST_TEST(!trailing.ok());

	const res::free_name_result too_long = res::free_name(layers(), "", std::string(260, 'a'), ".desc");
	BOOST_TEST(!too_long.ok());
}

BOOST_AUTO_TEST_SUITE_END()
