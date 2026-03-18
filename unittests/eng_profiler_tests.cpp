#include <boost/test/unit_test.hpp>
#include <eng_profiler.h>
#include <thread>
#include <chrono>

BOOST_AUTO_TEST_SUITE(ProfilerTests)

BOOST_AUTO_TEST_CASE(ScopedZone_DurationPositive)
{
	auto& td = ds::profiler_get_thread_data();
	uint32_t before = td.write_index;

	{
		ds::scoped_profiler_zone zone("TestZone");
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	BOOST_TEST(td.write_index == before + 1);
	auto& entry = td.entries[(td.write_index - 1) % ds::PROFILER_RING_BUFFER_SIZE];
	BOOST_TEST(entry.duration_us > 0.0f);
	BOOST_TEST(std::string(entry.name) == "TestZone");
}

BOOST_AUTO_TEST_CASE(ScopedZone_NestedDepth)
{
	auto& td = ds::profiler_get_thread_data();
	uint32_t before = td.write_index;

	{
		ds::scoped_profiler_zone outer("Outer");
		{
			ds::scoped_profiler_zone inner("Inner");
		}
		// inner wrote at before, outer not yet
	}
	// now outer wrote at before+1

	BOOST_TEST(td.write_index == before + 2);

	// Inner was pushed first (destroyed first in nested scope)
	auto& inner_entry = td.entries[(before) % ds::PROFILER_RING_BUFFER_SIZE];
	auto& outer_entry = td.entries[(before + 1) % ds::PROFILER_RING_BUFFER_SIZE];

	BOOST_TEST(std::string(inner_entry.name) == "Inner");
	BOOST_TEST(inner_entry.depth == 1);

	BOOST_TEST(std::string(outer_entry.name) == "Outer");
	BOOST_TEST(outer_entry.depth == 0);
}

BOOST_AUTO_TEST_CASE(RingBuffer_WrapsCorrectly)
{
	auto& td = ds::profiler_get_thread_data();

	// Push more than buffer size
	for (uint32_t i = 0; i < ds::PROFILER_RING_BUFFER_SIZE + 100; ++i) {
		td.push_entry("Wrap", static_cast<float>(i));
	}

	BOOST_TEST(td.entry_count() == ds::PROFILER_RING_BUFFER_SIZE);

	// Oldest entry should be 100 (the first 100 were overwritten)
	uint32_t start = td.write_index % ds::PROFILER_RING_BUFFER_SIZE;
	BOOST_TEST(td.entries[start].duration_us == 100.0f);
}

BOOST_AUTO_TEST_CASE(FrameMark_IncrementsFrameNumber)
{
	auto& td = ds::profiler_get_thread_data();
	uint64_t frame_before = td.frame_number;

	ds::profiler_mark_frame("TestFrame");
	BOOST_TEST(td.frame_number == frame_before + 1);

	ds::profiler_mark_frame("TestFrame");
	BOOST_TEST(td.frame_number == frame_before + 2);
}

BOOST_AUTO_TEST_CASE(FrameMark_RecordsFrameDuration)
{
	auto& td = ds::profiler_get_thread_data();

	// First call just sets the start time
	ds::profiler_mark_frame("Frame");
	uint32_t idx_after_first = td.write_index;

	std::this_thread::sleep_for(std::chrono::milliseconds(1));

	// Second call records duration of previous frame
	ds::profiler_mark_frame("Frame");
	BOOST_TEST(td.write_index == idx_after_first + 1);

	auto& entry = td.entries[(td.write_index - 1) % ds::PROFILER_RING_BUFFER_SIZE];
	BOOST_TEST(entry.duration_us > 0.0f);
	BOOST_TEST(std::string(entry.name) == "Frame");
}

BOOST_AUTO_TEST_CASE(Snapshot_ReturnsEntries)
{
	auto& td = ds::profiler_get_thread_data();

	{
		ds::scoped_profiler_zone zone("SnapshotTest");
	}

	auto snapshot = ds::profiler_snapshot();
	BOOST_TEST(snapshot.size() > 0u);

	bool found = false;
	for (auto& e : snapshot) {
		if (e.name && std::string(e.name) == "SnapshotTest") {
			found = true;
			break;
		}
	}
	BOOST_TEST(found);
}

BOOST_AUTO_TEST_CASE(Macro_ProfileScope_Compiles)
{
	PROFILE_SCOPE("MacroTest");
	// If this compiles, the macro works
	BOOST_TEST(true);
}

BOOST_AUTO_TEST_CASE(Macro_ProfileFunction_Compiles)
{
	PROFILE_FUNCTION();
	BOOST_TEST(true);
}

BOOST_AUTO_TEST_CASE(Macro_ProfileFrame_Compiles)
{
	PROFILE_FRAME("TestFrame");
	BOOST_TEST(true);
}

BOOST_AUTO_TEST_CASE(CurrentFrame_ReturnsValue)
{
	uint64_t frame = ds::profiler_current_frame();
	ds::profiler_mark_frame("Frame");
	BOOST_TEST(ds::profiler_current_frame() == frame + 1);
}

BOOST_AUTO_TEST_SUITE_END()
