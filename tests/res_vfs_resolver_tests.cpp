#include <boost/test/unit_test.hpp>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <snk/resource/res_vfs_test_hook.h>
#include "test_temp_dir.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#include <cstring>
#endif

namespace {

std::vector<std::byte> to_bytes(std::string_view s)
{
	std::vector<std::byte> out(s.size());
	for (size_t i = 0; i < s.size(); ++i) out[i] = static_cast<std::byte>(s[i]);
	return out;
}

std::string to_string(const std::vector<std::byte>& b)
{
	std::string s(b.size(), '\0');
	for (size_t i = 0; i < b.size(); ++i) s[i] = static_cast<char>(b[i]);
	return s;
}

// Blocks on the async result; returns the payload text, or "ERR:<msg>" on failure.
std::string load(const res::vfs_resolver& r, const char* tag)
{
	auto block = r.resolve(res::tag{ tag });
	block->wait();
	if (block->has_error()) return "ERR:" + block->error_msg;
	return to_string(block->data);
}

bool wait_for_file(const std::filesystem::path& p)
{
	for (int i = 0; i < 500; ++i) {
		if (std::filesystem::exists(p)) return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return false;
}

std::string read_text(const std::filesystem::path& p)
{
	std::ifstream in(p, std::ios::binary);
	return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

std::size_t temp_files_in(const std::filesystem::path& folder)
{
	std::size_t count = 0;
	std::error_code ec;
	for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
		if (entry.path().extension() == ".tmp") ++count;
	}
	return count;
}

// Mount order matters: the LAST point is the top of the stack and wins.
struct mounts_fixture
{
	test_util::temp_dir dir{ "vfs" };
	std::filesystem::path low = dir.path() / "low";
	std::filesystem::path high = dir.path() / "high";

	mounts_fixture()
	{
		dir.write("low/a.txt", "low-a");
		dir.write("low/only_low.txt", "low-only");
		dir.write("low/sub/deep.txt", "low-deep");
		dir.write("high/a.txt", "high-a");
		dir.write("high/only_high.txt", "high-only");
	}
};

}

BOOST_AUTO_TEST_SUITE(ResVfsResolverTests)

BOOST_FIXTURE_TEST_CASE(Resolve_TopMountShadowsLowerMount, mounts_fixture)
{
	res::vfs_resolver r("res", { low, high });
	BOOST_TEST(load(r, "res://a.txt") == "high-a");
}

BOOST_FIXTURE_TEST_CASE(Resolve_FallsThroughToLowerMount, mounts_fixture)
{
	res::vfs_resolver r("res", { low, high });
	BOOST_TEST(load(r, "res://only_low.txt") == "low-only");
	BOOST_TEST(load(r, "res://only_high.txt") == "high-only");
	BOOST_TEST(load(r, "res://sub/deep.txt") == "low-deep");
}

BOOST_FIXTURE_TEST_CASE(Resolve_MountOrderDecidesPriority, mounts_fixture)
{
	res::vfs_resolver r("res", { high, low });
	BOOST_TEST(load(r, "res://a.txt") == "low-a");
}

BOOST_FIXTURE_TEST_CASE(Resolve_MissingFileReportsErrorInsteadOfThrowing, mounts_fixture)
{
	res::vfs_resolver r("res", { low, high });
	auto result = load(r, "res://nope.txt");
	BOOST_TEST(result.rfind("ERR:", 0) == 0u);
	BOOST_TEST(result.find("does not exist") != std::string::npos);
}

BOOST_FIXTURE_TEST_CASE(Resolve_EmptyFileIsReadyWithNoBytes, mounts_fixture)
{
	dir.write("high/empty.bin", "");
	res::vfs_resolver r("res", { low, high });
	auto block = r.resolve(res::tag{ "res://empty.bin" });
	block->wait();
	BOOST_TEST(block->is_ready());
	BOOST_TEST(block->data.empty());
}

BOOST_AUTO_TEST_CASE(Resolve_WithoutMountsReportsNoEntryPoints)
{
	res::vfs_resolver r("res", std::vector<std::filesystem::path>{});
	auto result = load(r, "res://a.txt");
	BOOST_TEST(result.find("No entry points") != std::string::npos);
}

// A tag that names a folder (an asset browser path, a failed level save that left one) is an error, never a read.
BOOST_FIXTURE_TEST_CASE(Resolve_AFolderIsAnError, mounts_fixture)
{
	res::vfs_resolver r("res", { low, high });
	BOOST_TEST(load(r, "res://sub").find("ERR:Not a regular file") == 0u);
}

BOOST_FIXTURE_TEST_CASE(Exists_SearchesEveryMount, mounts_fixture)
{
	res::vfs_resolver r("res", { low, high });
	BOOST_TEST(r.exists(res::tag{ "res://a.txt" }));
	BOOST_TEST(r.exists(res::tag{ "res://only_low.txt" }));
	BOOST_TEST(r.exists(res::tag{ "res://only_high.txt" }));
	BOOST_TEST(!r.exists(res::tag{ "res://missing.txt" }));
}

BOOST_FIXTURE_TEST_CASE(Store_WritesToTopMountOnlyAndCreatesFolders, mounts_fixture)
{
	res::vfs_resolver r("res", { low, high });
	BOOST_TEST(r.store(res::tag{ "res://gen/out/new.bin" }, to_bytes("payload")));

	// readable right away (served from the pending-write table or from disk)
	BOOST_TEST(load(r, "res://gen/out/new.bin") == "payload");

	BOOST_TEST(wait_for_file(high / "gen/out/new.bin"));
	BOOST_TEST(!std::filesystem::exists(low / "gen/out/new.bin"));
	// the atomic-rename temp file is gone
	BOOST_TEST(temp_files_in(high / "gen/out") == 0u);
}

// #394: the temp file of a store has a name nobody can guess, so links planted at the old names are never followed.
BOOST_FIXTURE_TEST_CASE(StoreSync_IgnoresTempFileLinksAtGuessableNames, mounts_fixture)
{
	dir.write("outside.txt", "keep");
	std::filesystem::create_directories(high / "gen");
	std::error_code ec;
	for (const char* planted : { "gen/x.tmp", "gen/x.txt.tmp" }) {
		std::filesystem::create_symlink(dir.path() / "outside.txt", high / planted, ec);
		if (ec) {
			BOOST_TEST_MESSAGE("symlinks unavailable: " << ec.message());
			return;
		}
	}

	res::vfs_resolver r("res", { low, high });
	BOOST_TEST(r.store_sync(res::tag{ "res://gen/x.txt" }, to_bytes("payload")));

	BOOST_TEST(read_text(high / "gen/x.txt") == "payload");
	BOOST_TEST(read_text(dir.path() / "outside.txt") == "keep");
	BOOST_TEST(std::filesystem::is_symlink(high / "gen/x.tmp"));
}

// #394: a write that fails leaves what was there, removes its temp file and does not serve the lost bytes.
BOOST_FIXTURE_TEST_CASE(StoreSync_AFailedReplaceLeavesTheTargetAndNoTempFile, mounts_fixture)
{
	dir.write("high/gen/busy.bin/keep.txt", "keep"); // a folder where the file should go: the rename fails

	res::vfs_resolver r("res", { low, high });
	BOOST_TEST(!r.store_sync(res::tag{ "res://gen/busy.bin" }, to_bytes("payload")));

	BOOST_TEST(read_text(high / "gen/busy.bin/keep.txt") == "keep");
	BOOST_TEST(temp_files_in(high / "gen") == 0u);
	BOOST_TEST(load(r, "res://gen/busy.bin") != "payload");
}

BOOST_FIXTURE_TEST_CASE(StoreSync_AFailedOpenLeavesTheOldFile, mounts_fixture)
{
	dir.write("high/ro/old.txt", "old");
	namespace fs = std::filesystem;
	fs::permissions(high / "ro", fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);
	{
		std::ofstream probe(high / "ro" / "probe");
		if (probe.is_open()) {
			// Root, or a file system without permissions (Windows): the folder is still writable.
			probe.close();
			fs::permissions(high / "ro", fs::perms::owner_all, fs::perm_options::replace);
			BOOST_TEST_MESSAGE("a read-only folder cannot be made here, skipping");
			return;
		}
	}

	res::vfs_resolver r("res", { low, high });
	const bool stored = r.store_sync(res::tag{ "res://ro/old.txt" }, to_bytes("new"));
	fs::permissions(high / "ro", fs::perms::owner_all, fs::perm_options::replace);

	BOOST_TEST(!stored);
	BOOST_TEST(read_text(high / "ro/old.txt") == "old");
	BOOST_TEST(temp_files_in(high / "ro") == 0u);
	BOOST_TEST(load(r, "res://ro/old.txt") == "old");
}

// #394: a symlinked parent folder gets no folders made through it.
BOOST_FIXTURE_TEST_CASE(StoreSync_RefusesASymlinkedParentFolder, mounts_fixture)
{
	std::filesystem::create_directories(dir.path() / "outside");
	std::error_code ec;
	std::filesystem::create_directory_symlink(dir.path() / "outside", high / "linkdir", ec);
	if (ec) {
		BOOST_TEST_MESSAGE("symlinks unavailable: " << ec.message());
		return;
	}

	res::vfs_resolver r("res", { low, high });
	BOOST_TEST(!r.store_sync(res::tag{ "res://linkdir/sub/x.bin" }, to_bytes("payload")));

	BOOST_TEST(!std::filesystem::exists(dir.path() / "outside" / "sub"));
}

// A store right before shutdown (a level saved, then the editor closed) still reaches the disk.
BOOST_FIXTURE_TEST_CASE(Store_IsWrittenBeforeTheResolverIsDestroyed, mounts_fixture)
{
	constexpr int count = 50;
	{
		res::vfs_resolver r("res", { low, high });
		for (int i = 0; i < count; ++i) {
			BOOST_REQUIRE(r.store(res::tag{ "res://flush/" + std::to_string(i) + ".bin" }, to_bytes("payload")));
		}
	}
	for (int i = 0; i < count; ++i) {
		BOOST_TEST_INFO("file " << i);
		BOOST_TEST(std::filesystem::exists(high / "flush" / (std::to_string(i) + ".bin")));
	}
}

BOOST_FIXTURE_TEST_CASE(Store_OverwritesShadowedLowerMountFile, mounts_fixture)
{
	res::vfs_resolver r("res", { low, high });
	BOOST_TEST(r.store(res::tag{ "res://only_low.txt" }, to_bytes("v2")));
	BOOST_TEST(wait_for_file(high / "only_low.txt"));
	BOOST_TEST(load(r, "res://only_low.txt") == "v2");
	// the lower mount is not modified
	BOOST_TEST(load(res::vfs_resolver("res", { low }), "res://only_low.txt") == "low-only");
}

BOOST_FIXTURE_TEST_CASE(Store_ThenExistsIsTrueImmediately, mounts_fixture)
{
	res::vfs_resolver r("res", { low, high });
	BOOST_TEST(!r.exists(res::tag{ "res://later.txt" }));
	r.store(res::tag{ "res://later.txt" }, to_bytes("x"));
	BOOST_TEST(r.exists(res::tag{ "res://later.txt" }));
}

BOOST_AUTO_TEST_CASE(Store_WithoutMountsFailsButNotifiesChange)
{
	res::vfs_resolver r("res", std::vector<std::filesystem::path>{});
	std::atomic<int> notified{ 0 };
	std::string last;
	r.set_resource_changed_callback([&](const res::tag& t) {
		++notified;
		last = std::string(t.view());
	});
	BOOST_TEST(!r.store(res::tag{ "res://x.txt" }, to_bytes("abc")));
	BOOST_TEST(notified.load() == 1);
	BOOST_TEST(last == "res://x.txt");
	// the data is still served from memory even though nothing was persisted
	BOOST_TEST(load(r, "res://x.txt") == "abc");
}

BOOST_FIXTURE_TEST_CASE(PathMapper_MapsFilesUnderAnyMountToTags, mounts_fixture)
{
	res::vfs_resolver r("res", { low, high });
	auto t1 = r.path_mapper(high / "a.txt");
	BOOST_REQUIRE(t1.has_value());
	BOOST_TEST(t1->protocol() == "res");
	BOOST_TEST(t1->relative() == "a.txt");

	auto t2 = r.path_mapper(low / "sub" / "deep.txt");
	BOOST_REQUIRE(t2.has_value());
	BOOST_TEST(t2->relative() == "sub/deep.txt");
}

BOOST_FIXTURE_TEST_CASE(PathMapper_NormalizesDotSegments, mounts_fixture)
{
	res::vfs_resolver r("res", { low, high });
	auto t = r.path_mapper(high / "x" / ".." / "a.txt");
	BOOST_REQUIRE(t.has_value());
	BOOST_TEST(t->relative() == "a.txt");
}

BOOST_FIXTURE_TEST_CASE(PathMapper_RejectsPathsOutsideAllMounts, mounts_fixture)
{
	res::vfs_resolver r("res", { low, high });
	BOOST_TEST(!r.path_mapper(dir.path() / "elsewhere" / "a.txt").has_value());
	BOOST_TEST(!r.path_mapper(dir.path() / "low_sibling" / "a.txt").has_value());
}

BOOST_FIXTURE_TEST_CASE(PathMapper_NestedMountsPreferTopOfStack, mounts_fixture)
{
	// "inner" lives inside "outer"; inner is mounted last, so it owns its subtree.
	auto outer = dir.path() / "outer";
	auto inner = outer / "inner";
	std::filesystem::create_directories(inner);
	res::vfs_resolver r("res", { outer, inner });

	auto in_inner = r.path_mapper(inner / "f.txt");
	BOOST_REQUIRE(in_inner.has_value());
	BOOST_TEST(in_inner->relative() == "f.txt");

	auto in_outer = r.path_mapper(outer / "g.txt");
	BOOST_REQUIRE(in_outer.has_value());
	BOOST_TEST(in_outer->relative() == "g.txt");
}

BOOST_FIXTURE_TEST_CASE(ExternalFileChange_FiresResourceChangedCallback, mounts_fixture)
{
	std::atomic<bool> fired{ false };
	std::string seen;
	std::mutex m;
	res::vfs_resolver r("res", { low, high });
	r.set_resource_changed_callback([&](const res::tag& t) {
		std::lock_guard lock(m);
		seen = std::string(t.relative());
		fired = true;
	});
	// give the watcher a chance to take its initial snapshot, then add a file externally
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	dir.write("high/created_externally.txt", "hi");
	for (int i = 0; i < 300 && !fired; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
	BOOST_TEST(fired.load());
	std::lock_guard lock(m);
	BOOST_TEST(seen == "created_externally.txt");
}

namespace {

struct change_recorder
{
	std::mutex m;
	std::vector<std::string> seen;

	void attach(res::vfs_resolver& r)
	{
		r.set_resource_changed_callback([this](const res::tag& t) {
			std::lock_guard lock(m);
			seen.emplace_back(t.relative());
		});
	}

	bool wait_for(const std::string& rel)
	{
		for (int i = 0; i < 300; ++i) {
			if (contains(rel)) return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		return false;
	}

	bool contains(const std::string& rel)
	{
		std::lock_guard lock(m);
		return std::find(seen.begin(), seen.end(), rel) != seen.end();
	}
};

}

BOOST_FIXTURE_TEST_CASE(Layers_ChangeInBaseLayerFiresWhenNotOverridden, mounts_fixture)
{
	change_recorder rec;
	res::vfs_resolver r("res", { low, high });
	rec.attach(r);
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	dir.write("low/fresh_base.txt", "x");
	BOOST_TEST(rec.wait_for("fresh_base.txt"));
}

BOOST_FIXTURE_TEST_CASE(Layers_ChangeInBaseLayerIgnoredWhenTopOverrides, mounts_fixture)
{
	change_recorder rec;
	res::vfs_resolver r("res", { low, high });
	rec.attach(r);
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	// a.txt exists in both layers: editing the shadowed base copy changes nothing visible
	dir.write("low/a.txt", "low-a-edited");
	// sentinel proves the watcher had time to report the shadowed change
	dir.write("low/sentinel.txt", "x");
	BOOST_TEST(rec.wait_for("sentinel.txt"));
	BOOST_TEST(!rec.contains("a.txt"));
	BOOST_TEST(load(r, "res://a.txt") == "high-a");
}

BOOST_FIXTURE_TEST_CASE(Layers_ChangeInTopLayerFiresForOverriddenPath, mounts_fixture)
{
	change_recorder rec;
	res::vfs_resolver r("res", { low, high });
	rec.attach(r);
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	dir.write("high/a.txt", "high-a-edited");
	BOOST_TEST(rec.wait_for("a.txt"));
}

BOOST_FIXTURE_TEST_CASE(Layers_RemovingTopOverrideRevealsBaseOnNextResolve, mounts_fixture)
{
	res::vfs_resolver r("res", { low, high });
	BOOST_TEST(load(r, "res://a.txt") == "high-a");
	std::filesystem::remove(high / "a.txt");
	// the file watcher does not report deletions, but lookups see the base file again
	BOOST_TEST(load(r, "res://a.txt") == "low-a");
}

BOOST_AUTO_TEST_SUITE_END()

// A tag comes from untrusted data (model files, scene descs): it must never reach a file outside the mount.
BOOST_AUTO_TEST_SUITE(ResVfsResolverContainmentTests)

namespace {

struct sandbox_fixture
{
	test_util::temp_dir dir{ "vfs_contain" };
	std::filesystem::path root = dir.path() / "sandbox";
	// Shares the "sandbox" string prefix on purpose: a string-prefix containment check would accept it.
	std::filesystem::path sibling = dir.path() / "sandbox2";

	sandbox_fixture()
	{
		dir.write("sandbox/a/b/c.txt", "inside");
		dir.write("sandbox2/secret.txt", "secret");
		dir.write("outside.txt", "outside");
	}

	std::unique_ptr<res::vfs_resolver> make() const
	{
		return std::make_unique<res::vfs_resolver>("memory", std::vector<std::filesystem::path>{ root });
	}
};

std::string slurp(const std::filesystem::path& p)
{
	std::ifstream in(p, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}

BOOST_FIXTURE_TEST_CASE(EscapingTags_AreRejectedByAllOperations, sandbox_fixture)
{
	auto rp = make();
	res::vfs_resolver& r = *rp;
	for (const char* t : {
		"memory://../x",
		"memory://a/../../x",
		"memory://../outside.txt",
		"memory://../sandbox2/secret.txt",
		"memory://..\\x",
		"memory:///etc/x",
		"memory://C:/x",
		"memory://C:\\x" }) {
		BOOST_TEST_CONTEXT("tag " << t) {
			const res::tag tg{ t };
			BOOST_TEST(!r.exists(tg));
			const auto text = load(r, t);
			BOOST_TEST(text.rfind("ERR:", 0) == 0u);
			BOOST_TEST(text.find("escapes") != std::string::npos);
			BOOST_TEST(!r.store(tg, to_bytes("pwned")));
			// a rejected store must not leave pending data behind either
			BOOST_TEST(load(r, t).rfind("ERR:", 0) == 0u);
		}
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	BOOST_TEST(!std::filesystem::exists(dir.path() / "x"));
	BOOST_TEST(slurp(dir.path() / "outside.txt") == "outside");
	BOOST_TEST(slurp(sibling / "secret.txt") == "secret");
}

BOOST_FIXTURE_TEST_CASE(SymlinkedSubdirPointingOutside_IsRejected, sandbox_fixture)
{
	std::error_code ec;
	std::filesystem::create_directory_symlink(sibling, root / "link", ec);
	if (ec) {
		BOOST_TEST_MESSAGE("cannot create a directory symlink here (" << ec.message() << "), skipping");
		return;
	}
	auto rp = make();
	res::vfs_resolver& r = *rp;
	const res::tag tg{ "memory://link/secret.txt" };
	BOOST_TEST(!r.exists(tg));
	BOOST_TEST(load(r, "memory://link/secret.txt").rfind("ERR:", 0) == 0u);
	BOOST_TEST(!r.store(tg, to_bytes("pwned")));
	BOOST_TEST(!r.store(res::tag{ "memory://link/new.txt" }, to_bytes("pwned")));
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	BOOST_TEST(slurp(sibling / "secret.txt") == "secret");
	BOOST_TEST(!std::filesystem::exists(sibling / "new.txt"));
}

BOOST_FIXTURE_TEST_CASE(NulAndEmptyTags_AreRejected, sandbox_fixture)
{
	auto rp = make();
	res::vfs_resolver& r = *rp;
	const res::tag nul_tag{ std::string("memory://a\0b.txt", 16) };
	const res::tag empty_tag{ "memory://" };
	const res::tag ctrl_tag{ "memory://a\nb.txt" };

	for (const res::tag* tg : { &nul_tag, &empty_tag, &ctrl_tag }) {
		BOOST_TEST_CONTEXT("tag size " << tg->string().size()) {
			BOOST_TEST(!r.exists(*tg));
			BOOST_TEST(!r.store(*tg, to_bytes("x")));
			auto block = r.resolve(*tg);
			block->wait();
			BOOST_TEST(block->has_error());
		}
	}
}

BOOST_FIXTURE_TEST_CASE(InsideTags_StillWork, sandbox_fixture)
{
	auto rp = make();
	res::vfs_resolver& r = *rp;
	BOOST_TEST(r.exists(res::tag{ "memory://a/b/c.txt" }));
	BOOST_TEST(load(r, "memory://a/b/c.txt") == "inside");
	BOOST_TEST(!r.exists(res::tag{ "memory://a/b/missing.txt" }));

	BOOST_TEST(r.store(res::tag{ "memory://a/b/new.txt" }, to_bytes("fresh")));
	BOOST_TEST(wait_for_file(root / "a" / "b" / "new.txt"));
	BOOST_TEST(load(r, "memory://a/b/new.txt") == "fresh");
}

// ---- #414: no race between the path check and the write -------------------------------------------------------------
// A hook pauses the write on the io thread right after the path was checked again (after_check) or with the temp file
// written (before_rename), and swaps a folder or a file for a link there. Nothing outside the root is ever created or
// changed. Linux runs every case; Windows skips when it cannot make links (no developer mode / privilege).

namespace {

namespace fs = std::filesystem;

struct swap_fixture : mounts_fixture
{
	fs::path outside = dir.path() / "outside";

	swap_fixture()
	{
		fs::create_directories(outside);
		dir.write("outside/keep.txt", "keep");
	}

	// Links can be made here (always on Linux; on Windows only with the privilege).
	bool links_available()
	{
		std::error_code ec;
		fs::create_symlink(outside / "keep.txt", dir.path() / "probe.link", ec);
		if (ec) {
			BOOST_TEST_MESSAGE("symlinks unavailable, skipping: " << ec.message());
			return false;
		}
		fs::remove(dir.path() / "probe.link", ec);
		return true;
	}

	bool outside_untouched() const
	{
		std::error_code ec;
		std::size_t entries = 0;
		for (const auto& entry : fs::directory_iterator(outside, ec)) {
			static_cast<void>(entry);
			++entries;
		}
		return entries == 1u && read_text(outside / "keep.txt") == "keep";
	}

	// Stores with `hook` set for that one store.
	bool store_with(res::vfs_resolver& r, const char* tag, std::string_view text,
		std::function<void(res::detail::write_stage, const fs::path&)> hook)
	{
		res::detail::write_hook_access::set(r, std::move(hook));
		const bool stored = r.store_sync(res::tag{ tag }, to_bytes(text));
		res::detail::write_hook_access::set(r, nullptr);
		return stored;
	}
};

}

BOOST_FIXTURE_TEST_CASE(Write_AParentSwappedForALinkAfterTheCheckIsRefused, swap_fixture)
{
	if (!links_available()) return;
	dir.write("high/levels/old.txt", "old");
	res::vfs_resolver r("res", { low, high });
	std::error_code swapped;
	const bool stored = store_with(r, "res://levels/a.txt", "payload", [&](res::detail::write_stage stage, const fs::path&) {
		if (stage == res::detail::write_stage::after_check) {
			fs::rename(high / "levels", high / "levels_moved", swapped);
			if (!swapped) {
				fs::create_directory_symlink(outside, high / "levels", swapped);
			}
		}
	});
	BOOST_REQUIRE(!swapped);
	BOOST_TEST(!stored);
	BOOST_TEST(outside_untouched());
	BOOST_TEST(read_text(high / "levels_moved/old.txt") == "old");
	BOOST_TEST(temp_files_in(high / "levels_moved") == 0u);
}

BOOST_FIXTURE_TEST_CASE(Write_AFolderToBeCreatedThatAppearsAsALinkIsRefused, swap_fixture)
{
	if (!links_available()) return;
	res::vfs_resolver r("res", { low, high });
	std::error_code swapped;
	const bool stored = store_with(r, "res://fresh/deeper/a.txt", "payload", [&](res::detail::write_stage stage, const fs::path&) {
		if (stage == res::detail::write_stage::after_check) {
			fs::create_directory_symlink(outside, high / "fresh", swapped);
		}
	});
	BOOST_REQUIRE(!swapped);
	BOOST_TEST(!stored);
	BOOST_TEST(outside_untouched());
	BOOST_TEST(!fs::exists(outside / "deeper"));
}

BOOST_FIXTURE_TEST_CASE(Write_ALinkPlantedAtTheTempNameIsNotOpened, swap_fixture)
{
	if (!links_available()) return;
	fs::create_directories(high / "gen");
	res::vfs_resolver r("res", { low, high });
	std::error_code planted;
	const bool stored = store_with(r, "res://gen/x.txt", "payload", [&](res::detail::write_stage stage, const fs::path& tmp) {
		if (stage == res::detail::write_stage::after_check) {
			fs::create_symlink(outside / "keep.txt", tmp, planted);
		}
	});
	BOOST_REQUIRE(!planted);
	BOOST_TEST(!stored);
	BOOST_TEST(outside_untouched());
	BOOST_TEST(!fs::exists(high / "gen/x.txt"));
}

BOOST_FIXTURE_TEST_CASE(Write_ATargetSwappedForALinkBeforeTheRenameReplacesTheLinkItself, swap_fixture)
{
	if (!links_available()) return;
	dir.write("high/gen/x.txt", "old");
	res::vfs_resolver r("res", { low, high });
	std::error_code swapped;
	const bool stored = store_with(r, "res://gen/x.txt", "payload", [&](res::detail::write_stage stage, const fs::path&) {
		if (stage == res::detail::write_stage::before_rename) {
			if (fs::remove(high / "gen/x.txt", swapped) && !swapped) {
				fs::create_symlink(outside / "keep.txt", high / "gen/x.txt", swapped);
			}
		}
	});
	BOOST_REQUIRE(!swapped);
	BOOST_TEST(stored);
	BOOST_TEST(outside_untouched());
	BOOST_TEST(!fs::is_symlink(high / "gen/x.txt"));
	BOOST_TEST(read_text(high / "gen/x.txt") == "payload");
}

BOOST_FIXTURE_TEST_CASE(Write_AParentSwappedBeforeTheRenameKeepsTheFileInTheOpenedFolder, swap_fixture)
{
	if (!links_available()) return;
	dir.write("high/gen/old.txt", "old");
	res::vfs_resolver r("res", { low, high });
	std::error_code moved;
	std::error_code linked;
	const bool stored = store_with(r, "res://gen/x.txt", "payload", [&](res::detail::write_stage stage, const fs::path&) {
		if (stage == res::detail::write_stage::before_rename) {
			fs::rename(high / "gen", high / "gen_moved", moved);
			if (!moved) {
				fs::create_directory_symlink(outside, high / "gen", linked);
			}
		}
	});
	BOOST_TEST(outside_untouched());
	if (moved) {
		// Windows: the folder is held without FILE_SHARE_DELETE for the write, so it cannot be moved at all.
		BOOST_TEST_MESSAGE("the folder could not be moved during the write: " << moved.message());
		BOOST_TEST(stored);
		BOOST_TEST(read_text(high / "gen/x.txt") == "payload");
		return;
	}
	BOOST_REQUIRE(!linked);
	// POSIX: the rename happens inside the folder that was opened, which is now gen_moved: still inside the root.
	BOOST_TEST(stored);
	BOOST_TEST(read_text(high / "gen_moved/x.txt") == "payload");
	BOOST_TEST(temp_files_in(high / "gen_moved") == 0u);
}

// The root may not exist when it is mounted (a user folder on first run, a project made later): it is opened on the
// first write, and made then if still missing.
BOOST_FIXTURE_TEST_CASE(Write_ARootMountedBeforeItExistsIsWrittenOnceItDoes, mounts_fixture)
{
	const fs::path later = dir.path() / "later";
	const fs::path never = dir.path() / "never";
	{
		res::vfs_resolver r("res", { low, later });
		fs::create_directories(later);
		BOOST_TEST(r.store_sync(res::tag{ "res://sub/a.txt" }, to_bytes("made later")));
		BOOST_TEST(read_text(later / "sub/a.txt") == "made later");
	}
	{
		res::vfs_resolver r("res", { low, never });
		BOOST_TEST(r.store_sync(res::tag{ "res://sub/b.txt" }, to_bytes("made by the store")));
		BOOST_TEST(read_text(never / "sub/b.txt") == "made by the store");
	}
}

#if defined(_WIN32)
namespace {

// Turns the empty folder `folder` into a junction to `target`, as FSCTL_SET_REPARSE_POINT does while another handle
// holds the folder. Empty when it worked, else why not.
std::string make_junction(const fs::path& folder, const fs::path& target)
{
	const std::wstring print = fs::absolute(target).native();
	const std::wstring substitute = L"\\??\\" + print;
	const std::size_t path_bytes = (substitute.size() + 1 + print.size() + 1) * sizeof(wchar_t);
	// REPARSE_DATA_BUFFER with its MountPointReparseBuffer (ntifs.h, not in the user-mode headers).
	struct mount_point_header {
		DWORD tag;
		WORD data_length;
		WORD reserved;
		WORD substitute_offset;
		WORD substitute_length;
		WORD print_offset;
		WORD print_length;
	};
	const mount_point_header header{ IO_REPARSE_TAG_MOUNT_POINT, static_cast<WORD>(4 * sizeof(WORD) + path_bytes), 0, 0,
		static_cast<WORD>(substitute.size() * sizeof(wchar_t)), static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t)),
		static_cast<WORD>(print.size() * sizeof(wchar_t)) };
	std::vector<std::byte> buffer(sizeof(header) + path_bytes);
	std::memcpy(buffer.data(), &header, sizeof(header));
	std::memcpy(buffer.data() + sizeof(header), substitute.c_str(), (substitute.size() + 1) * sizeof(wchar_t));
	std::memcpy(buffer.data() + sizeof(header) + (substitute.size() + 1) * sizeof(wchar_t), print.c_str(), (print.size() + 1) * sizeof(wchar_t));

	const HANDLE handle = ::CreateFileW(folder.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (handle == INVALID_HANDLE_VALUE) {
		return std::system_category().message(static_cast<int>(::GetLastError()));
	}
	DWORD returned = 0;
	const BOOL made = ::DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr, 0, &returned, nullptr);
	const DWORD code = ::GetLastError();
	::CloseHandle(handle);
	return made ? std::string{} : std::system_category().message(static_cast<int>(code));
}

}
#endif

// The folders are opened one from the other and the temp file from the last one: a held folder made a junction (Windows)
// or moved and replaced by a link (POSIX) between the opens and the create is never followed out of the root.
BOOST_FIXTURE_TEST_CASE(Write_AHeldFolderTurnedIntoALinkBeforeTheCreateIsNotFollowed, swap_fixture)
{
#if !defined(_WIN32)
	if (!links_available()) return;
#endif
	res::vfs_resolver r("res", { low, high });
	std::string swap_error;
	const bool stored = store_with(r, "res://fresh/deeper/a.txt", "payload", [&](res::detail::write_stage stage, const fs::path&) {
		if (stage != res::detail::write_stage::folders_held) {
			return;
		}
#if defined(_WIN32)
		swap_error = make_junction(high / "fresh" / "deeper", outside);
#else
		std::error_code ec;
		fs::rename(high / "fresh" / "deeper", high / "fresh" / "deeper_moved", ec);
		if (!ec) {
			fs::create_directory_symlink(outside, high / "fresh" / "deeper", ec);
		}
		swap_error = ec ? ec.message() : std::string{};
#endif
	});
	if (!swap_error.empty()) {
		BOOST_TEST_MESSAGE("the folder could not be swapped, skipping: " << swap_error);
		return;
	}
	BOOST_TEST(outside_untouched());
#if !defined(_WIN32)
	BOOST_TEST(stored);
	BOOST_TEST(read_text(high / "fresh/deeper_moved/a.txt") == "payload");
	BOOST_TEST(temp_files_in(high / "fresh/deeper_moved") == 0u);
#else
	static_cast<void>(stored); // refused, or written into the junction's own folder: either way not outside
#endif
}

// The root's folder renamed away and a new one made at its path (a project recreated): the next write goes to the new one.
BOOST_FIXTURE_TEST_CASE(Write_ARootReplacedAtItsPathIsOpenedAgain, mounts_fixture)
{
	res::vfs_resolver r("res", { low, high });
	BOOST_REQUIRE(r.store_sync(res::tag{ "res://first.txt" }, to_bytes("first")));
	fs::rename(high, dir.path() / "high_old");
	fs::create_directories(high);
	BOOST_TEST(r.store_sync(res::tag{ "res://second.txt" }, to_bytes("second")));
	BOOST_TEST(read_text(high / "second.txt") == "second");
	BOOST_TEST(!fs::exists(dir.path() / "high_old" / "second.txt"));

	fs::remove_all(high);
	BOOST_TEST(r.store_sync(res::tag{ "res://third.txt" }, to_bytes("third")));
	BOOST_TEST(read_text(high / "third.txt") == "third");
}

// An erase walks the folders like a store; a file in a nested folder and an empty folder both go.
BOOST_FIXTURE_TEST_CASE(Erase_RemovesThroughTheOpenedFoldersAndAnEmptyFolderToo, mounts_fixture)
{
	const fs::path memory = dir.path() / "memory";
	res::vfs_resolver r("memory", { memory });
	BOOST_REQUIRE(r.store_sync(res::tag{ "memory://a/b/c.txt" }, to_bytes("scratch")));
	fs::create_directories(memory / "a" / "empty");

	BOOST_TEST(r.erase(res::tag{ "memory://a/b/c.txt" }));
	BOOST_TEST(r.erase(res::tag{ "memory://a/empty" }));
	// The io thread runs in order: once this store is done, so are both erases.
	BOOST_REQUIRE(r.store_sync(res::tag{ "memory://flush.txt" }, to_bytes("flush")));
	BOOST_TEST(!fs::exists(memory / "a" / "b" / "c.txt"));
	BOOST_TEST(fs::is_directory(memory / "a" / "b"));
	BOOST_TEST(!fs::exists(memory / "a" / "empty"));
}

BOOST_AUTO_TEST_SUITE_END()
