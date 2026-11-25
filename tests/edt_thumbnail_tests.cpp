#include <boost/test/unit_test.hpp>
#include "edt_thumbnail.h"
#include "edt_thumbnail_service.h"
#include "test_temp_dir.hpp"
#include <snk/image/img_image.h>
#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

// #1192: thumbnails of the asset browser: the downscale, the decode of a file below a root, and the service that asks for
// them, queues, uploads, caches and gives back, all without a GPU or a worker.

namespace {

std::vector<unsigned char> picture(int width, int height, int channels, unsigned char fill)
{
	return std::vector<unsigned char>(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * static_cast<std::size_t>(channels), fill);
}

const unsigned char* pixel_at(const edt::thumbnail_pixels& pixels, int x, int y)
{
	return pixels.rgba.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(pixels.width) + static_cast<std::size_t>(x)) * 4;
}

}

BOOST_AUTO_TEST_SUITE(EdtThumbnail)

BOOST_AUTO_TEST_CASE(ASmallPicture_IsNotEnlarged_AndAnyChannelCountBecomesRgba)
{
	const std::vector<unsigned char> grey{ 10, 20, 30, 40 };
	const auto from_grey = edt::downscale_to_thumbnail(grey.data(), 2, 2, 1, false);
	BOOST_REQUIRE(from_grey.has_value());
	BOOST_TEST(from_grey->width == 2);
	BOOST_TEST(from_grey->height == 2);
	BOOST_TEST(pixel_at(*from_grey, 1, 0)[0] == 20);
	BOOST_TEST(pixel_at(*from_grey, 1, 0)[2] == 20);
	BOOST_TEST(pixel_at(*from_grey, 1, 0)[3] == 255);

	const std::vector<unsigned char> grey_alpha{ 7, 99 };
	const auto from_grey_alpha = edt::downscale_to_thumbnail(grey_alpha.data(), 1, 1, 2, false);
	BOOST_REQUIRE(from_grey_alpha.has_value());
	BOOST_TEST(pixel_at(*from_grey_alpha, 0, 0)[1] == 7);
	BOOST_TEST(pixel_at(*from_grey_alpha, 0, 0)[3] == 99);

	const std::vector<unsigned char> rgb{ 1, 2, 3 };
	const auto from_rgb = edt::downscale_to_thumbnail(rgb.data(), 1, 1, 3, false);
	BOOST_REQUIRE(from_rgb.has_value());
	BOOST_TEST(pixel_at(*from_rgb, 0, 0)[2] == 3);
	BOOST_TEST(pixel_at(*from_rgb, 0, 0)[3] == 255);

	const std::vector<unsigned char> rgba{ 4, 5, 6, 7 };
	const auto from_rgba = edt::downscale_to_thumbnail(rgba.data(), 1, 1, 4, false);
	BOOST_REQUIRE(from_rgba.has_value());
	BOOST_TEST(pixel_at(*from_rgba, 0, 0)[3] == 7);
}

BOOST_AUTO_TEST_CASE(ALargePicture_FitsTheSide_KeepsItsProportions_AndAveragesWhatItCovers)
{
	const auto wide = picture(200, 100, 3, 80);
	const auto out = edt::downscale_to_thumbnail(wide.data(), 200, 100, 3, false);
	BOOST_REQUIRE(out.has_value());
	BOOST_TEST(out->width == edt::k_thumbnail_side);
	BOOST_TEST(out->height == edt::k_thumbnail_side / 2);
	BOOST_TEST(pixel_at(*out, 10, 10)[0] == 80);

	const auto tall = picture(10, 1000, 4, 1);
	const auto thin = edt::downscale_to_thumbnail(tall.data(), 10, 1000, 4, false);
	BOOST_REQUIRE(thin.has_value());
	BOOST_TEST(thin->height == edt::k_thumbnail_side);
	BOOST_TEST(thin->width >= 1);

	std::vector<unsigned char> half(2 * 2 * 1, 0);
	half[0] = 200;
	half[1] = 100;
	const auto mixed = edt::downscale_to_thumbnail(half.data(), 2, 2, 1, false, 1);
	BOOST_REQUIRE(mixed.has_value());
	BOOST_TEST(mixed->width == 1);
	BOOST_TEST(pixel_at(*mixed, 0, 0)[0] == 75);
}

BOOST_AUTO_TEST_CASE(TheDownscaleKeepsTheOrientation_AnAsymmetricPictureStaysTheSameWayUp)
{
	// 8x4, rows from the top: only the top-left quarter is white.
	std::vector<unsigned char> data(8 * 4, 0);
	for (int y = 0; y < 2; ++y) {
		for (int x = 0; x < 4; ++x) data[static_cast<std::size_t>(y * 8 + x)] = 255;
	}
	const auto top_down = edt::downscale_to_thumbnail(data.data(), 8, 4, 1, false, 4);
	BOOST_REQUIRE(top_down.has_value());
	BOOST_REQUIRE(top_down->width == 4);
	BOOST_REQUIRE(top_down->height == 2);
	BOOST_TEST(pixel_at(*top_down, 0, 0)[0] == 255);
	BOOST_TEST(pixel_at(*top_down, 1, 0)[0] == 255);
	BOOST_TEST(pixel_at(*top_down, 2, 0)[0] == 0);
	BOOST_TEST(pixel_at(*top_down, 3, 0)[0] == 0);
	BOOST_TEST(pixel_at(*top_down, 0, 1)[0] == 0);
	BOOST_TEST(pixel_at(*top_down, 3, 1)[0] == 0);

	// The same bytes as a picture whose first row is the bottom one: its white quarter is at the bottom, so it comes out at
	// the bottom, and the output is still read from the top.
	const auto bottom_up = edt::downscale_to_thumbnail(data.data(), 8, 4, 1, true, 4);
	BOOST_REQUIRE(bottom_up.has_value());
	BOOST_TEST(pixel_at(*bottom_up, 0, 1)[0] == 255);
	BOOST_TEST(pixel_at(*bottom_up, 1, 1)[0] == 255);
	BOOST_TEST(pixel_at(*bottom_up, 0, 0)[0] == 0);
	BOOST_TEST(pixel_at(*bottom_up, 3, 1)[0] == 0);
}

BOOST_AUTO_TEST_CASE(ARefusedPicture_HasNoThumbnail)
{
	const auto data = picture(4, 4, 3, 1);
	BOOST_TEST(!edt::downscale_to_thumbnail(nullptr, 4, 4, 3, false).has_value());
	BOOST_TEST(!edt::downscale_to_thumbnail(data.data(), 0, 4, 3, false).has_value());
	BOOST_TEST(!edt::downscale_to_thumbnail(data.data(), 4, -1, 3, false).has_value());
	BOOST_TEST(!edt::downscale_to_thumbnail(data.data(), 4, 4, 0, false).has_value());
	BOOST_TEST(!edt::downscale_to_thumbnail(data.data(), 4, 4, 5, false).has_value());
	BOOST_TEST(!edt::downscale_to_thumbnail(data.data(), 4, 4, 3, false, 0).has_value());
}

BOOST_AUTO_TEST_CASE(OnlyThePicturesTheDecoderReadsAreDecodable)
{
	BOOST_TEST(edt::thumbnail_decodable("a/b.png"));
	BOOST_TEST(edt::thumbnail_decodable("a/B.JPG"));
	BOOST_TEST(edt::thumbnail_decodable("a/b.tga"));
	BOOST_TEST(!edt::thumbnail_decodable("a/b.dds"));
	BOOST_TEST(!edt::thumbnail_decodable("a/b.ktx2"));
	BOOST_TEST(!edt::thumbnail_decodable("a/b.glb"));
}

BOOST_AUTO_TEST_CASE(AFileIsDecodedBelowTheRoot_AMissingOrBrokenOneGivesAReason)
{
	test_util::temp_dir dir("thumbnail_decode");
	const std::filesystem::path root = std::filesystem::weakly_canonical(dir.path());
	const auto pixels = picture(100, 50, 3, 120);
	std::vector<std::byte> png;
	img::image::write_to_memory(img::image_type::png, pixels.data(), 100, 50, 3, png);
	BOOST_REQUIRE(!png.empty());
	dir.write("tex/ok.png", std::string_view(reinterpret_cast<const char*>(png.data()), png.size()));
	dir.write("tex/not_a_picture.png", "this is not a png");

	const edt::thumbnail_decoded ok = edt::decode_thumbnail_file(root, "tex/ok.png");
	BOOST_REQUIRE(ok.pixels.has_value());
	BOOST_TEST(ok.pixels->width == 64);
	BOOST_TEST(ok.pixels->height == 32);

	const edt::thumbnail_decoded broken = edt::decode_thumbnail_file(root, "tex/not_a_picture.png");
	BOOST_TEST(!broken.pixels.has_value());
	BOOST_TEST(!broken.error.empty());
	BOOST_TEST(!edt::decode_thumbnail_file(root, "tex/missing.png").pixels.has_value());
}

BOOST_AUTO_TEST_CASE(AnAbsurdPicture_IsRefusedFromItsHeader_BeforeAnythingIsAllocated)
{
	test_util::temp_dir dir("thumbnail_huge");
	const std::filesystem::path root = std::filesystem::weakly_canonical(dir.path());
	const auto pixels = picture(8, 8, 3, 1);
	std::vector<std::byte> png;
	img::image::write_to_memory(img::image_type::png, pixels.data(), 8, 8, 3, png);
	BOOST_REQUIRE(png.size() > 24);
	// The IHDR width is the four bytes at 16: 20000 pixels, more than a side may have.
	png[16] = std::byte{ 0 };
	png[17] = std::byte{ 0 };
	png[18] = std::byte{ 0x4E };
	png[19] = std::byte{ 0x20 };
	dir.write("huge.png", std::string_view(reinterpret_cast<const char*>(png.data()), png.size()));
	const edt::thumbnail_decoded refused = edt::decode_thumbnail_file(root, "huge.png");
	BOOST_TEST(!refused.pixels.has_value());
	BOOST_TEST(!refused.error.empty());
}

BOOST_AUTO_TEST_SUITE_END()

namespace {

struct service_env
{
	using task = std::pair<std::function<edt::thumbnail_decoded()>, std::function<void(edt::thumbnail_decoded)>>;

	edt::thumbnail_service::ports make_ports()
	{
		edt::thumbnail_service::ports ports;
		ports.dispatch = [this](std::function<edt::thumbnail_decoded()> work, std::function<void(edt::thumbnail_decoded)> done) {
			tasks.emplace_back(std::move(work), std::move(done));
		};
		ports.stat = [this](const std::filesystem::path&, const std::filesystem::path& relative) -> std::optional<edt::desc_type_cache::stamp> {
			const auto found = stamps.find(relative.generic_string());
			if (found == stamps.end()) return std::nullopt;
			return found->second;
		};
		ports.decode = [this](const std::filesystem::path&, const std::filesystem::path& relative) {
			++decodes;
			const auto failure = failures.find(relative.generic_string());
			if (failure != failures.end()) return edt::thumbnail_decoded{ std::nullopt, failure->second };
			edt::thumbnail_pixels pixels;
			pixels.width = 2;
			pixels.height = 2;
			pixels.rgba.assign(16, 255);
			return edt::thumbnail_decoded{ pixels, {} };
		};
		ports.upload = [this](const std::string& key, const edt::thumbnail_pixels&) -> std::uint64_t {
			++uploads;
			if (fail_upload) return 0;
			live_textures.push_back(key);
			return 1000 + live_textures.size();
		};
		ports.release = [this](const std::string& key) {
			++releases;
			std::erase(live_textures, key);
		};
		ports.now = [this] { return clock; };
		return ports;
	}

	void run_tasks()
	{
		auto pending = std::move(tasks);
		tasks.clear();
		for (auto& [work, done] : pending) done(work());
	}

	void add(const std::string& name, std::uint64_t size = 1)
	{
		stamps[name] = { size, 1, 1 };
	}

	std::vector<task> tasks;
	std::map<std::string, edt::desc_type_cache::stamp> stamps;
	std::map<std::string, std::string> failures;
	std::vector<std::string> live_textures;
	int decodes = 0;
	int uploads = 0;
	int releases = 0;
	bool fail_upload = false;
	double clock = 0.0;
};

using view_state = edt::thumbnail_service::view::state_t;

}

BOOST_AUTO_TEST_SUITE(EdtThumbnailService)

BOOST_AUTO_TEST_CASE(AFileIsAskedFor_DecodedOnAWorker_UploadedOnTheMainThread_AndThenShown)
{
	service_env env;
	env.add("a.png");
	edt::thumbnail_service service(env.make_ports());
	BOOST_TEST((service.lookup("root", "a.png").state == view_state::pending));
	BOOST_TEST(env.decodes == 0);
	service.tick();
	BOOST_TEST(service.running() == 1);
	BOOST_TEST(env.decodes == 0);
	env.run_tasks();
	BOOST_TEST(env.decodes == 1);
	BOOST_TEST(service.running() == 0);
	BOOST_TEST((service.lookup("root", "a.png").state == view_state::pending));
	service.tick();
	const auto shown = service.lookup("root", "a.png");
	BOOST_TEST((shown.state == view_state::ready));
	BOOST_TEST(shown.texture != 0u);
	BOOST_TEST(shown.width == 2);
	BOOST_TEST(env.uploads == 1);
	BOOST_TEST(service.textures() == 1u);
}

BOOST_AUTO_TEST_CASE(NotMoreThanTheLimitDecodeAtOnce_AndNotMoreThanTheLimitUploadAFrame)
{
	service_env env;
	edt::thumbnail_service::limits limits;
	limits.max_running = 2;
	limits.uploads_per_frame = 1;
	edt::thumbnail_service service(env.make_ports(), limits);
	for (const char* name : { "a.png", "b.png", "c.png" }) {
		env.add(name);
		service.lookup("root", name);
	}
	service.tick();
	BOOST_TEST(service.running() == 2);
	BOOST_TEST(env.tasks.size() == 2u);
	env.run_tasks();
	for (const char* name : { "a.png", "b.png", "c.png" }) service.lookup("root", name);
	service.tick();
	BOOST_TEST(env.uploads == 1);
	BOOST_TEST(service.running() == 1);
	service.tick();
	BOOST_TEST(env.uploads == 2);
}

BOOST_AUTO_TEST_CASE(AShownFileIsNotDecodedAgain_UntilItsStampChanges)
{
	service_env env;
	env.add("a.png");
	edt::thumbnail_service service(env.make_ports());
	service.lookup("root", "a.png");
	service.tick();
	env.run_tasks();
	service.tick();
	BOOST_REQUIRE((service.lookup("root", "a.png").state == view_state::ready));
	BOOST_TEST(env.decodes == 1);

	env.clock = 5.0;
	service.lookup("root", "a.png");
	service.tick();
	BOOST_TEST(env.tasks.empty());
	BOOST_TEST(env.decodes == 1);
	BOOST_TEST(env.releases == 0);

	env.add("a.png", 2);
	env.clock = 10.0;
	BOOST_TEST((service.lookup("root", "a.png").state == view_state::pending));
	BOOST_TEST(env.releases == 1);
	BOOST_TEST(service.textures() == 0u);
	service.tick();
	env.run_tasks();
	service.tick();
	BOOST_TEST((service.lookup("root", "a.png").state == view_state::ready));
	BOOST_TEST(env.decodes == 2);
}

BOOST_AUTO_TEST_CASE(AFailedDecode_ShowsItsReason_AndIsNotTriedAgainEveryFrame)
{
	service_env env;
	env.add("bad.png");
	env.failures["bad.png"] = "the picture cannot be decoded";
	edt::thumbnail_service service(env.make_ports());
	service.lookup("root", "bad.png");
	service.tick();
	env.run_tasks();
	for (int frame = 0; frame < 20; ++frame) {
		env.clock += 0.2;
		const auto shown = service.lookup("root", "bad.png");
		service.tick();
		BOOST_TEST((shown.state == view_state::failed || frame == 0));
	}
	const auto failed = service.lookup("root", "bad.png");
	BOOST_TEST((failed.state == view_state::failed));
	BOOST_TEST(failed.error == "the picture cannot be decoded");
	BOOST_TEST(env.decodes == 1);
	BOOST_TEST(env.tasks.empty());

	env.failures.erase("bad.png");
	env.add("bad.png", 9);
	env.clock += 5.0;
	service.lookup("root", "bad.png");
	service.tick();
	env.run_tasks();
	service.tick();
	BOOST_TEST((service.lookup("root", "bad.png").state == view_state::ready));
	BOOST_TEST(env.decodes == 2);
}

BOOST_AUTO_TEST_CASE(ALostTexture_IsAFailureNotAnEndlessRetry)
{
	service_env env;
	env.add("a.png");
	env.fail_upload = true;
	edt::thumbnail_service service(env.make_ports());
	service.lookup("root", "a.png");
	service.tick();
	env.run_tasks();
	service.tick();
	BOOST_TEST((service.lookup("root", "a.png").state == view_state::failed));
	service.tick();
	BOOST_TEST(env.uploads == 1);
	BOOST_TEST(env.decodes == 1);
}

BOOST_AUTO_TEST_CASE(ANoRegularFile_IsAFailureWithoutAnyDecode)
{
	service_env env;
	edt::thumbnail_service service(env.make_ports());
	BOOST_TEST((service.lookup("root", "link.png").state == view_state::failed));
	service.tick();
	BOOST_TEST(env.tasks.empty());
	BOOST_TEST(env.decodes == 0);
}

BOOST_AUTO_TEST_CASE(LeavingAFolder_GivesTheTexturesBack_AndAnAnswerThatArrivesLateIsDropped)
{
	service_env env;
	env.add("a.png");
	env.add("b.png");
	edt::thumbnail_service service(env.make_ports());
	service.lookup("root", "a.png");
	service.lookup("root", "b.png");
	service.tick();
	BOOST_REQUIRE(env.tasks.size() == 2u);
	auto in_flight = std::move(env.tasks);
	env.tasks.clear();
	// a.png finishes before the folder is left, b.png after.
	in_flight[0].second(in_flight[0].first());
	service.tick();
	BOOST_REQUIRE((service.lookup("root", "a.png").state == view_state::ready));
	BOOST_TEST(env.live_textures.size() == 1u);

	service.release_all();
	BOOST_TEST(service.entries() == 0u);
	BOOST_TEST(env.live_textures.empty());
	BOOST_TEST(service.textures() == 0u);

	in_flight[1].second(in_flight[1].first());
	service.tick();
	BOOST_TEST(env.uploads == 1);
	BOOST_TEST(service.running() == 0);
	BOOST_TEST(env.live_textures.empty());
}

BOOST_AUTO_TEST_CASE(TheCacheIsBounded_TheLeastRecentlyAskedForGoesAndGivesItsTextureBack)
{
	service_env env;
	edt::thumbnail_service::limits limits;
	limits.max_entries = 2;
	edt::thumbnail_service service(env.make_ports(), limits);
	for (const char* name : { "a.png", "b.png" }) {
		env.add(name);
		service.lookup("root", name);
	}
	service.tick();
	env.run_tasks();
	service.tick();
	service.tick();
	BOOST_REQUIRE(service.textures() == 2u);
	service.lookup("root", "b.png");
	env.add("c.png");
	service.lookup("root", "c.png");
	BOOST_TEST(service.entries() == 2u);
	BOOST_TEST(env.releases == 1);
	BOOST_TEST(env.live_textures.size() == 1u);
	BOOST_TEST((service.lookup("root", "b.png").state == view_state::ready));
}

BOOST_AUTO_TEST_CASE(ARowThatIsNoLongerAskedFor_LeavesTheQueue_SoTheScrolledPastFilesCostNothing)
{
	service_env env;
	edt::thumbnail_service::limits limits;
	limits.max_running = 1;
	edt::thumbnail_service service(env.make_ports(), limits);
	for (const char* name : { "a.png", "b.png", "c.png", "d.png" }) {
		env.add(name);
		service.lookup("root", name);
	}
	service.tick();
	BOOST_REQUIRE(env.tasks.size() == 1u);
	// The next frame only d.png is on the screen.
	service.lookup("root", "d.png");
	service.tick();
	service.lookup("root", "d.png");
	service.tick();
	env.run_tasks();
	service.lookup("root", "d.png");
	service.tick();
	service.lookup("root", "d.png");
	service.tick();
	env.run_tasks();
	BOOST_TEST(env.decodes == 2);
	BOOST_TEST(service.entries() <= 2u);
}

BOOST_AUTO_TEST_SUITE_END()
