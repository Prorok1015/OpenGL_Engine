#include <snk/desc/desc_base.hpp>
#include <snk/common/logger/engine_log.h>
#include <snk/desc/desc_system.h>
#include <exception>
#include <boost/json.hpp>

void desc::desc_base::copy_through_json(desc_base& other) const
{
	if (this == &other) {
		return;
	}
	desc_system* system = other.m_system ? other.m_system : m_system;
	if (!system) {
		egLOG_WARN("desc", "cannot copy desc '{}' to '{}': different types and no desc_system to deserialize with", type, other.type);
		return;
	}
	egLOG_WARN("desc", "copying desc '{}' to '{}' goes through JSON: only the keys both know are copied", type, other.type);
	try {
		json::object transfer;
		serialize(transfer);
		other.deserialize(*system, transfer);
	} catch (const std::exception& e) {
		egLOG_WARN("desc", "copying desc '{}' to '{}' failed: {}", type, other.type, e.what());
	}
}
