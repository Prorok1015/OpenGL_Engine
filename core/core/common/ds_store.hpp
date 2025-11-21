#pragma once
#include "ds_type_id.hpp"
#include "engine_assert.h"
#include <any>
#include <memory>
#include <unordered_map>

namespace ds {

	struct shared_storage_policy
	{
		template<class T>
		using ITEM_T = std::shared_ptr<T>;
		using STORAGE_ITEM = std::any;

		template <class T>
		T& cast_ref(ITEM_T<T> it) const
		{
			return *it;
		}

		template<class T>
		ITEM_T<T> cast(const STORAGE_ITEM& any) const
		{
			if (!any.has_value()) return nullptr;

			return std::any_cast<ITEM_T<T>>(any);
		}

		template<class T, class ...ARGS>
		auto construct(ARGS&&... args) const
		{
			return std::make_any<ITEM_T<T>>(std::make_shared<T>(std::forward<ARGS>(args)...));
		}

		template<class KEY, class INSTANCE, class ...ARGS>
		auto construct(ARGS&&... args) const
		{
			return std::make_any<ITEM_T<KEY>>(std::make_shared<INSTANCE>(std::forward<ARGS>(args)...));
		}

		template<class T>
		auto destruct(ITEM_T<T> it) const
		{
			// shared_ptr will be destructed automatically
		}
	};

	struct raw_storage_policy
	{
		template<class T>
		using ITEM_T = T*;
		using STORAGE_ITEM = void*;

		template <class T>
		T& cast_ref(ITEM_T<T> it) const
		{
			return *it;
		}

		template<class T>
		ITEM_T<T> cast(const STORAGE_ITEM& any) const
		{
			if (!any) return nullptr;

			return static_cast<ITEM_T<T>>(any);
		}

		template<class T, class ...ARGS>
		auto construct(ARGS&&... args) const
		{
			return new T(std::forward<ARGS>(args)...);
		}

		template<class T>
		auto destruct(ITEM_T<T> it) const
		{
			delete it;
		}
	};

	template<class POLICY_T>
	class data_storage_t : protected POLICY_T
	{
		using STORAGE_ITEM = POLICY_T::STORAGE_ITEM;
	public:
		data_storage_t() = default;
		data_storage_t(ds::data_storage_t<POLICY_T>* parent)
			: parent_storage(parent) {
		}
		data_storage_t(data_storage_t&&) = default;
		data_storage_t(const data_storage_t&) = delete;
		data_storage_t& operator=(data_storage_t&&) = default;
		data_storage_t& operator=(const data_storage_t&) = delete;
		~data_storage_t() = default;	

		template<class T>
		bool has_value() const {
			auto it = data.find(ds::type_id::make<T>());
			if (it != data.end()) {
				return it->second;
			}
			return false;
		}

		template<class T>
		auto require_parent_shared() const {
			if (parent_storage) {
				return parent_storage->template require_shared<T>();
			}

			return POLICY_T::template cast<T>({});
		}

		template<class T>
		auto require_shared() const {
			auto it = data.find(ds::type_id::make<T>());
			if (it != data.end() && it->second) {
				return POLICY_T::template cast<T>(it->second);
			}
			
			return require_parent_shared<T>();
		}

		template<class T>
		T& require() const {
			auto item = require_shared<T>();
			ASSERT_MSG(item, "Trying to require non existing type");
			return POLICY_T::template cast_ref<T>(item);
		}

		template<class T, class ...ARGS>
		T& construct(ARGS&&... args) {
			data[ds::type_id::make<T>()] = POLICY_T::template construct<T>(std::forward<ARGS>(args)...);
			return require<T>();
		}

		template<class KEY, class INSTANCE, class ...ARGS>
		KEY& construct(ARGS&&... args) {
			data[ds::type_id::make<KEY>()] = POLICY_T::template construct<KEY, INSTANCE>(std::forward<ARGS>(args)...);
			return require<KEY>();
		}

		template<class T>
		void destruct() {
			if (auto it = data.find(ds::type_id::make<T>()); it != data.end()) {
				POLICY_T::template destruct<T>(POLICY_T::template cast<T>(it->second));
				data.erase(it);
			} else {
				ASSERT_FAIL("Trying to destruct non existing type");
			}
		}

	private:
		std::unordered_map<ds::type_id, STORAGE_ITEM> data;
		ds::data_storage_t<POLICY_T>* parent_storage = nullptr;
	};

	using app_data_storage = data_storage_t<shared_storage_policy>;
}