#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"
#include "test_manual_tasks.hpp"
#include "rnd_null_driver.h"
#include <snk/command/cmd_base64.h>
#include <snk/command/cmd_main_thread_queue.h>
#include <snk/command/cmd_registry.h>
#include <snk/render/rnd_capture_commands.h>
#include <snk/render/rnd_capture_service.h>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <vector>

// rnd::capture_service and the viewport.capture command on the null backend. The null driver answers with a
// placeholder (pixel (x, y) is {x, y, 0, 255}), so the pictures here are known without a GPU; the real frame
// is compared with the headless reference by the capture step of the editor visual smoke.

namespace {

namespace json = boost::json;
using namespace rnd::driver;

constexpr int tex_width = 4;
constexpr int tex_height = 2;

texture_header target_header(uint32_t width = tex_width, uint32_t height = tex_height)
{
	texture_header header;
	header.data.extent.width = width;
	header.data.extent.height = height;
	header.usage = texture_usage::color_target | texture_usage::sampled;
	return header;
}

int at(const std::vector<std::byte>& bytes, std::size_t index)
{
	return std::to_integer<int>(bytes.at(index));
}

// Channel `c` of pixel (x, y) counted from the top of the picture.
int channel(const rnd::captured_image& image, int x, int y, int c)
{
	return at(image.rgba, (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.size.x) + static_cast<std::size_t>(x)) * 4 + static_cast<std::size_t>(c));
}

std::uint32_t be32(const std::vector<std::byte>& bytes, std::size_t offset)
{
	return (static_cast<std::uint32_t>(at(bytes, offset)) << 24) | (static_cast<std::uint32_t>(at(bytes, offset + 1)) << 16)
		| (static_cast<std::uint32_t>(at(bytes, offset + 2)) << 8) | static_cast<std::uint32_t>(at(bytes, offset + 3));
}

bool is_png(const std::vector<std::byte>& bytes)
{
	return bytes.size() > 26 && at(bytes, 0) == 0x89 && at(bytes, 1) == 'P' && at(bytes, 2) == 'N' && at(bytes, 3) == 'G';
}

struct capture_fixture
{
	null::driver drv;
	std::unique_ptr<texture_interface> texture = drv.create_texture(target_header());
	rnd::capture_service service{ drv };
	cmd::registration scene = service.add_target("scene", [this] { return texture.get(); });
};

}

BOOST_FIXTURE_TEST_SUITE(RndCaptureServiceTests, capture_fixture)

BOOST_AUTO_TEST_CASE(AddTarget_RefusesAnEmptyOrTakenId)
{
	BOOST_CHECK(scene.active());
	auto empty = service.add_target("", [this] { return texture.get(); });
	auto taken = service.add_target("scene", [this] { return texture.get(); });
	auto no_provider = service.add_target("other", nullptr);
	BOOST_CHECK(!empty.active());
	BOOST_CHECK(!taken.active());
	BOOST_CHECK(!no_provider.active());
	BOOST_CHECK(service.target_ids() == std::vector<std::string>{ "scene" });
}

BOOST_AUTO_TEST_CASE(AddTarget_RegistrationRemovesTheView)
{
	auto game = service.add_target("game", [this] { return texture.get(); });
	BOOST_CHECK(service.has_target("game"));
	BOOST_CHECK(service.target_ids() == (std::vector<std::string>{ "scene", "game" }));
	game.reset();
	BOOST_CHECK(!service.has_target("game"));
	BOOST_CHECK(service.has_target("scene"));
}

BOOST_AUTO_TEST_CASE(Registration_OutlivingTheServiceIsHarmless)
{
	cmd::registration kept;
	{
		null::driver other_drv;
		rnd::capture_service other(other_drv);
		kept = other.add_target("x", []() -> texture_interface* { return nullptr; });
		BOOST_CHECK(kept.active());
	}
	BOOST_CHECK_NO_THROW(kept.reset());
}

BOOST_AUTO_TEST_CASE(Capture_PutsTheTopRowFirst)
{
	const auto result = service.capture("scene");

	BOOST_REQUIRE(result.ok());
	BOOST_CHECK_EQUAL(result.image.size.x, tex_width);
	BOOST_CHECK_EQUAL(result.image.size.y, tex_height);
	// The driver's row 0 is the bottom of the picture: the top row of the image is texture row 1.
	BOOST_CHECK_EQUAL(channel(result.image, 0, 0, 0), 0);
	BOOST_CHECK_EQUAL(channel(result.image, 0, 0, 1), 1);
	BOOST_CHECK_EQUAL(channel(result.image, 3, 0, 0), 3);
	BOOST_CHECK_EQUAL(channel(result.image, 3, 0, 1), 1);
	BOOST_CHECK_EQUAL(channel(result.image, 2, 1, 0), 2);
	BOOST_CHECK_EQUAL(channel(result.image, 2, 1, 1), 0);
	BOOST_CHECK_EQUAL(channel(result.image, 2, 1, 3), 255);
}

BOOST_AUTO_TEST_CASE(Capture_EmptyIdTakesTheFirstView)
{
	auto game = service.add_target("game", []() -> texture_interface* { return nullptr; });
	const auto result = service.capture("");
	BOOST_CHECK(result.ok());
}

BOOST_AUTO_TEST_CASE(Capture_Scales)
{
	const auto doubled = service.capture("scene", { 8, 0 });
	BOOST_REQUIRE(doubled.ok());
	BOOST_CHECK_EQUAL(doubled.image.size.x, 8);
	BOOST_CHECK_EQUAL(doubled.image.size.y, 4);
	// Nearest neighbour: pixel (7, 3) of 8x4 comes from (3, 1) of 4x2, whose texture row is 0.
	BOOST_CHECK_EQUAL(channel(doubled.image, 7, 3, 0), 3);
	BOOST_CHECK_EQUAL(channel(doubled.image, 7, 3, 1), 0);

	const auto narrow = service.capture("scene", { 0, 1 });
	BOOST_REQUIRE(narrow.ok());
	BOOST_CHECK_EQUAL(narrow.image.size.x, 2);
	BOOST_CHECK_EQUAL(narrow.image.size.y, 1);

	const auto exact = service.capture("scene", { 3, 3 });
	BOOST_REQUIRE(exact.ok());
	BOOST_CHECK_EQUAL(exact.image.size.x, 3);
	BOOST_CHECK_EQUAL(exact.image.size.y, 3);
	BOOST_CHECK_EQUAL(exact.image.rgba.size(), 3u * 3u * 4u);
}

BOOST_AUTO_TEST_CASE(Capture_FailsForAnUnknownViewOrOneWithoutATexture)
{
	BOOST_CHECK(!service.capture("nope").ok());
	cmd::registration hidden = service.add_target("hidden", []() -> texture_interface* { return nullptr; });
	const auto result = service.capture("hidden");
	BOOST_CHECK(!result.ok());
	BOOST_CHECK(result.image.rgba.empty());

	rnd::capture_service empty(drv);
	BOOST_CHECK(!empty.capture("").ok());
}

BOOST_AUTO_TEST_CASE(Capture_RefusesASizeThatWouldNeedTooMuchMemory)
{
	// 2 x 8 target: 8192 wide would derive 32768 rows.
	auto tall = drv.create_texture(target_header(2, 8));
	cmd::registration reg = service.add_target("tall", [&tall] { return tall.get(); });

	const auto derived = service.capture("tall", { rnd::capture_service::max_side, 0 });
	BOOST_CHECK(derived.status == rnd::capture_status::invalid_size);
	BOOST_CHECK(derived.image.rgba.empty());
	BOOST_CHECK(!derived.error.empty());

	// Both sides are fine, the product is not.
	const auto too_many = service.capture("scene", { rnd::capture_service::max_side, rnd::capture_service::max_side });
	BOOST_CHECK(too_many.status == rnd::capture_status::invalid_size);

	const auto above = service.capture("scene", { rnd::capture_service::max_side + 1, 1 });
	BOOST_CHECK(above.status == rnd::capture_status::invalid_size);

	// The limit itself is allowed: 8192 x 4096 would be, so a smaller one surely is.
	BOOST_CHECK(service.capture("scene", { 64, 0 }).ok());
}

BOOST_AUTO_TEST_CASE(Capture_LimitsOfTheCallerBoundTheDerivedSideBeforeReading)
{
	// 2 x 8 target: a width of 64 derives 256 rows; fine under the defaults, over a ceiling of 100.
	auto tall = drv.create_texture(target_header(2, 8));
	cmd::registration reg = service.add_target("tall", [&tall] { return tall.get(); });
	BOOST_CHECK(service.capture("tall", { 64, 0 }).ok());

	rnd::capture_limits side;
	side.max_side = 100;
	const auto derived = service.capture("tall", { 64, 0 }, side);
	BOOST_CHECK(derived.status == rnd::capture_status::invalid_size);
	BOOST_CHECK(derived.image.rgba.empty());
	BOOST_CHECK(derived.error.find("100 px a side") != std::string::npos);

	rnd::capture_limits pixels;
	pixels.max_pixels = 7;
	BOOST_CHECK(service.capture("scene", { 4, 2 }, pixels).status == rnd::capture_status::invalid_size);
	pixels.max_pixels = 8;
	BOOST_CHECK(service.capture("scene", { 4, 2 }, pixels).ok());

	// A limit cannot raise the ceiling of the service.
	rnd::capture_limits above;
	above.max_side = 100000;
	above.max_pixels = 1LL << 40;
	BOOST_CHECK(service.capture("scene", { rnd::capture_service::max_side + 1, 1 }, above).status == rnd::capture_status::invalid_size);
}

BOOST_AUTO_TEST_CASE(Capture_TighterLimitsAlsoApplyToTheNativeSize)
{
	rnd::capture_limits narrow;
	narrow.max_side = 3;
	BOOST_CHECK(service.capture("scene", { 0, 0 }, narrow).status == rnd::capture_status::invalid_size);
	narrow.max_side = tex_width;
	BOOST_CHECK(service.capture("scene", { 0, 0 }, narrow).ok());
	// Default limits keep returning the target as it is.
	BOOST_CHECK(service.capture("scene").ok());
}

BOOST_AUTO_TEST_CASE(Capture_FailsBeforeReadingATargetOverThePixelBudget)
{
	// The null driver keeps no pixels of a texture, so a huge one costs nothing here; the service must refuse it
	// before it asks the driver for a buffer of that size.
	auto huge = drv.create_texture(target_header(8192, 8192));
	cmd::registration reg = service.add_target("huge", [&huge] { return huge.get(); });

	const auto result = service.capture("huge");

	BOOST_CHECK(result.status == rnd::capture_status::failed);
	BOOST_CHECK(result.image.rgba.empty());
	BOOST_CHECK(!service.capture("huge", { 16, 16 }).ok());
}

BOOST_AUTO_TEST_CASE(Capture_FailsForATextureTheDriverCannotRead)
{
	texture_header header = target_header();
	header.data.format = texture_header::TYPE::D24_S8;
	header.usage = texture_usage::depth_target;
	auto depth = drv.create_texture(header);
	cmd::registration reg = service.add_target("depth", [&depth] { return depth.get(); });

	BOOST_CHECK(!service.capture("depth").ok());
}

BOOST_AUTO_TEST_CASE(EncodePng_IsAnRgbPngOfTheImageSize)
{
	const auto result = service.capture("scene", { 8, 4 });
	BOOST_REQUIRE(result.ok());

	const auto png = rnd::capture_service::encode_png(result.image);

	BOOST_REQUIRE(is_png(png));
	BOOST_CHECK_EQUAL(be32(png, 16), 8u);	// IHDR width
	BOOST_CHECK_EQUAL(be32(png, 20), 4u);	// IHDR height
	BOOST_CHECK_EQUAL(at(png, 25), 2);		// IHDR color type: RGB, no alpha
	BOOST_CHECK(rnd::capture_service::encode_png({}).empty());
}

BOOST_AUTO_TEST_SUITE_END()

namespace {

struct command_fixture : capture_fixture
{
	cmd::command_registry commands;
	cmd::registration command = rnd::register_capture_commands(commands, service);

	cmd::result capture(const std::string& args)
	{
		return commands.invoke("viewport.capture", json::parse(args), cmd::origin::test());
	}
};

}

BOOST_FIXTURE_TEST_SUITE(RndCaptureCommandTests, command_fixture)

BOOST_AUTO_TEST_CASE(Registered_WithStrictSchemasAndNoRisk)
{
	BOOST_REQUIRE(command.active());
	const cmd::command_desc* desc = commands.find("viewport.capture");
	BOOST_REQUIRE(desc != nullptr);
	BOOST_CHECK(desc->expose_as_tool);
	BOOST_CHECK(!desc->mutates);
	BOOST_CHECK(!desc->long_running);
	BOOST_CHECK(desc->risk == cmd::danger::none);
	BOOST_CHECK(!desc->args_schema.as_object().at("additionalProperties").as_bool());
	BOOST_CHECK(!desc->result_schema.as_object().at("additionalProperties").as_bool());
}

BOOST_AUTO_TEST_CASE(Capture_ReturnsThePngAsBase64)
{
	const auto result = capture("{}");

	BOOST_REQUIRE_MESSAGE(result.ok, result.error);
	const json::object& value = result.value.as_object();
	BOOST_CHECK_EQUAL(std::string(value.at("viewport").as_string()), "scene");
	BOOST_CHECK_EQUAL(value.at("width").as_int64(), tex_width);
	BOOST_CHECK_EQUAL(value.at("height").as_int64(), tex_height);
	BOOST_CHECK_EQUAL(std::string(value.at("format").as_string()), "png");
	const auto png = cmd::base64::decode(std::string(value.at("png_base64").as_string()));
	BOOST_REQUIRE(png.has_value());
	BOOST_CHECK(is_png(*png));
	BOOST_CHECK_EQUAL(be32(*png, 16), static_cast<std::uint32_t>(tex_width));
}

BOOST_AUTO_TEST_CASE(Capture_NamesTheViewportAndScales)
{
	auto game = service.add_target("game", [this] { return texture.get(); });

	const auto result = capture(R"({"viewport":"game","width":8,"height":6})");

	BOOST_REQUIRE_MESSAGE(result.ok, result.error);
	BOOST_CHECK_EQUAL(std::string(result.value.as_object().at("viewport").as_string()), "game");
	BOOST_CHECK_EQUAL(result.value.as_object().at("width").as_int64(), 8);
	BOOST_CHECK_EQUAL(result.value.as_object().at("height").as_int64(), 6);
}

BOOST_AUTO_TEST_CASE(Capture_AcceptsIntegersWrittenAsDoubles)
{
	const auto result = capture(R"({"width":8.0})");
	BOOST_REQUIRE_MESSAGE(result.ok, result.error);
	BOOST_CHECK_EQUAL(result.value.as_object().at("height").as_int64(), 4);
}

BOOST_AUTO_TEST_CASE(Capture_RefusesBadArguments)
{
	BOOST_CHECK_EQUAL(capture(R"({"width":0})").code, cmd::codes::invalid_args);
	BOOST_CHECK_EQUAL(capture(R"({"height":8193})").code, cmd::codes::invalid_args);
	BOOST_CHECK_EQUAL(capture(R"({"width":1.5})").code, cmd::codes::invalid_args);
	BOOST_CHECK_EQUAL(capture(R"({"viewport":""})").code, cmd::codes::invalid_args);
	BOOST_CHECK_EQUAL(capture(R"({"viewport":3})").code, cmd::codes::invalid_args);
	BOOST_CHECK_EQUAL(capture(R"({"scale":2})").code, cmd::codes::invalid_args);
}

BOOST_AUTO_TEST_CASE(Capture_SizeOverTheLimitsIsInvalidArgs)
{
	auto tall = drv.create_texture(target_header(2, 8));
	cmd::registration reg = service.add_target("tall", [&tall] { return tall.get(); });

	const auto derived = capture(R"({"viewport":"tall","width":8192})");
	BOOST_CHECK(!derived.ok);
	BOOST_CHECK_EQUAL(derived.code, cmd::codes::invalid_args);

	const auto product = capture(R"({"width":8192,"height":8192})");
	BOOST_CHECK(!product.ok);
	BOOST_CHECK_EQUAL(product.code, cmd::codes::invalid_args);
}

BOOST_AUTO_TEST_CASE(Capture_MaxSideAndMaxPixelsBoundTheDerivedSide)
{
	auto tall = drv.create_texture(target_header(2, 8));
	cmd::registration reg = service.add_target("tall", [&tall] { return tall.get(); });

	BOOST_CHECK(capture(R"({"viewport":"tall","width":64})").ok);
	const auto side = capture(R"({"viewport":"tall","width":64,"max_side":100})");
	BOOST_CHECK(!side.ok);
	BOOST_CHECK_EQUAL(side.code, cmd::codes::invalid_args);
	const auto pixels = capture(R"({"width":4,"height":2,"max_pixels":7})");
	BOOST_CHECK_EQUAL(pixels.code, cmd::codes::invalid_args);
	BOOST_CHECK(capture(R"({"width":4,"height":2,"max_pixels":8,"max_side":4})").ok);

	BOOST_CHECK_EQUAL(capture(R"({"max_side":0})").code, cmd::codes::invalid_args);
	BOOST_CHECK_EQUAL(capture(R"({"max_side":8193})").code, cmd::codes::invalid_args);
	BOOST_CHECK_EQUAL(capture(R"({"max_pixels":33554433})").code, cmd::codes::invalid_args);
	BOOST_CHECK_EQUAL(capture(R"({"max_pixels":"many"})").code, cmd::codes::invalid_args);
}

BOOST_AUTO_TEST_CASE(Capture_TargetOverThePixelBudgetFails)
{
	auto huge = drv.create_texture(target_header(8192, 8192));
	cmd::registration reg = service.add_target("huge", [&huge] { return huge.get(); });

	const auto result = capture(R"({"viewport":"huge"})");

	BOOST_CHECK(!result.ok);
	BOOST_CHECK_EQUAL(result.code, cmd::codes::failed);
}

BOOST_AUTO_TEST_CASE(Capture_UnknownViewportIsNotFound)
{
	const auto result = capture(R"({"viewport":"nope"})");
	BOOST_CHECK(!result.ok);
	BOOST_CHECK_EQUAL(result.code, cmd::codes::not_found);
}

BOOST_AUTO_TEST_CASE(Capture_WithoutAnyViewportIsNotFound)
{
	scene.reset();
	BOOST_CHECK_EQUAL(capture("{}").code, cmd::codes::not_found);
}

BOOST_AUTO_TEST_CASE(Capture_ViewWithoutATextureFails)
{
	texture.reset();
	const auto result = capture("{}");
	BOOST_CHECK(!result.ok);
	BOOST_CHECK_EQUAL(result.code, cmd::codes::failed);
}

BOOST_AUTO_TEST_CASE(Capture_FromAnotherThreadWaitsForTheMainDrain)
{
	ds::task_system tasks{ test_util::manual_tasks() };
	cmd::main_thread_queue queue{ commands, tasks };

	auto outcome = queue.post("viewport.capture", json::object{}, cmd::origin::agent("session-1"));
	BOOST_CHECK(outcome.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
	tasks.drain_main(std::chrono::hours(1));

	BOOST_REQUIRE(outcome.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
	const cmd::result result = outcome.get();
	BOOST_CHECK_MESSAGE(result.ok, result.error);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(RndCaptureInTheEngineTests, test_util::null_engine_fixture)

BOOST_AUTO_TEST_CASE(RenderInit_OffersTheServiceAndTheCommand)
{
	auto& commands = storage.require<cmd::command_registry>();
	BOOST_CHECK(commands.contains("viewport.capture"));
	BOOST_CHECK(storage.has_value<rnd::capture_service>());
}

BOOST_AUTO_TEST_CASE(CaptureOfARenderTarget_PassesTheValidationDecorator)
{
	auto& commands = storage.require<cmd::command_registry>();
	auto& service = storage.require<rnd::capture_service>();
	auto texture = storage.require<rnd::render_system>().get_driver()->create_texture(target_header());
	cmd::registration view = service.add_target("test", [&texture] { return texture.get(); });

	const auto result = commands.invoke("viewport.capture", json::parse(R"({"viewport":"test"})"), cmd::origin::test());

	BOOST_REQUIRE_MESSAGE(result.ok, result.error);
	BOOST_CHECK_EQUAL(result.value.as_object().at("width").as_int64(), tex_width);
	BOOST_CHECK_EQUAL(validating_driver()->state().total(), 0u);
}

BOOST_AUTO_TEST_SUITE_END()
