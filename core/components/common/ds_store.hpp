#pragma once
#include "ds_type_id.hpp"
#include "engine_assert.h"
#include <any>
#include <memory>
#include <unordered_map>

namespace ds {

	struct DataStoragePolicy
	{
		template<class T>
		using ITEM_T = std::shared_ptr<T>;

		template <class T>
		T& cast_ref(ITEM_T<T> it) const
		{
			return *it;
		}

		template<class T>
		ITEM_T<T> cast(const std::any& any)
		{
			return std::any_cast<ITEM_T<T>>(any);
		}

		template<class T, class ...ARGS>
		auto construct(ARGS&&... args)
		{
			return std::make_any<ITEM_T<T>>(std::make_shared<T>(std::forward<ARGS>(args)...));
		}

		template<class T>
		auto construct()
		{
			return std::make_shared<T>();
		}
	};

	template<class POLICY_T>
	class DataStorageT : public POLICY_T
	{
	public:
		static DataStorageT& instance() {
			static DataStorageT inst;
			return inst;
		}

		template<class T>
		bool has_value() const {
			auto it = data.find(ds::type_id::value<T>());
			if (it != data.end()) {
				return it->second.has_value();
			}
			return false;
		}

		template<class T>
		auto require_shared()
		{
			ASSERT_MSG(has_value<T>(), "Missing the required type");

			return POLICY_T::template cast<T>(data[ds::type_id::value<T>()]);
		}

		template<class T>
		T& require() 
		{
			return POLICY_T::template cast_ref<T>(require_shared<T>());
		}

		template<class T, class ...ARGS>
		T& construct(ARGS&&... args)
		{
			data[ds::type_id::value<T>()] = POLICY_T::template construct<T>(std::forward<ARGS>(args)...);
			return require<T>();
		}

		template<class T>
		void destruct()
		{
			data[ds::type_id::value<T>()] = nullptr;
		}

	private:
		std::unordered_map<size_t, std::any> data;
	};

	using AppDataStorage = DataStorageT<DataStoragePolicy>;
}