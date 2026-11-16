#include <boost/test/unit_test.hpp>
#include "edt_instance_overrides_view.h"

// The text the Inspector shows for an override record of an instance (#149). Pure: no ImGui frame.

namespace
{
	scn::override_record set_record(std::vector<uint64_t> target, std::string component, std::string path, boost::json::value value)
	{
		scn::override_record record;
		record.target = std::move(target);
		record.component = std::move(component);
		record.path = std::move(path);
		record.value = std::move(value);
		return record;
	}
}

BOOST_AUTO_TEST_SUITE(InstanceOverridesView)

BOOST_AUTO_TEST_CASE(ASetRecord_NamesTheNodeThePathAndTheValue)
{
	const auto text = edt::describe_override_record(set_record({ 0xa1, 0xb2 }, "stat", "/hp", 12));
	BOOST_TEST(text == "a1 > b2 : stat /hp = 12");
}

BOOST_AUTO_TEST_CASE(ARecordOnTheRootOfTheInstance_HasNoNodePrefix)
{
	BOOST_TEST(edt::describe_override_record(set_record({}, "stat", "/hp", 12)) == "stat /hp = 12");
}

BOOST_AUTO_TEST_CASE(ARecordOfAWholeComponent_SaysWhole)
{
	BOOST_TEST(edt::describe_override_record(set_record({ 0x1 }, "light", "", boost::json::object{ { "color", "red" } }))
		== "1 : light (whole) = {\"color\":\"red\"}");
}

BOOST_AUTO_TEST_CASE(ALongValue_IsCut)
{
	const std::string text = edt::describe_override_record(set_record({}, "c", "/p", boost::json::value_from(std::string(200, 'x'))));
	BOOST_TEST(text.size() < 100u);
	BOOST_TEST(text.ends_with("..."));
}

BOOST_AUTO_TEST_CASE(RemoveRecords_SayWhatGoes)
{
	scn::override_record component = set_record({ 0xa1 }, "light", "", {});
	component.op = scn::override_op::remove;
	BOOST_TEST(edt::describe_override_record(component) == "a1 : remove component light");

	scn::override_record node = set_record({ 0xa1 }, "", "", {});
	node.op = scn::override_op::remove;
	BOOST_TEST(edt::describe_override_record(node) == "remove node a1");
}

BOOST_AUTO_TEST_CASE(AnAddNodeRecord_NamesTheNode)
{
	scn::override_record added = set_record({}, "", "", boost::json::object{ { "name", "extra" } });
	added.op = scn::override_op::add_node;
	BOOST_TEST(edt::describe_override_record(added) == "add node 'extra'");
}

BOOST_AUTO_TEST_CASE(TextFromAFile_StaysText)
{
	// A format string in a component key or a name is not interpreted.
	const auto text = edt::describe_override_record(set_record({}, "{}%s{0}", "/p", 1));
	BOOST_TEST(text == "{}%s{0} /p = 1");
}

BOOST_AUTO_TEST_SUITE_END()
