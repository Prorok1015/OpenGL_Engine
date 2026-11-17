#pragma once
#include <memory>
#include <utility>
#include <boost/json/storage_ptr.hpp>

namespace ds
{
	// JSON the engine keeps must not live in a resource of the caller's (a module's arena, its own Boost.JSON on MSVC) that
	// may be gone first (#826, #827). Boost.JSON assignment keeps the target's resource and copy construction takes the
	// source's, so neither moves a value that already sits in another resource: it is rebuilt in place.
	// Works for boost::json::value, object, array and string.
	template<class Json>
	void keep_in_default_resource(Json& json)
	{
		const boost::json::storage_ptr default_storage;
		if (json.storage().get() == default_storage.get()) {
			return;
		}
		Json owned(json, default_storage);
		std::destroy_at(&json);
		std::construct_at(&json, std::move(owned));
	}
}
