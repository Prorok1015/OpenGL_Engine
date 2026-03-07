#pragma once
#include <vector>
#include <functional>
#include "ds_type_id.hpp"
#include "engine_assert.h"

namespace ds
{
	struct EventSlotPolicyCopy
	{
		template <typename T>
		using SLOT = std::function<T>;
	};

	struct EventStoragePopicyVector 
	{
		template <class T>
		struct Storage {
			using TYPE = std::vector<T>;
			using ITERATOR = typename TYPE::iterator;
			using CONST_ITERATOR = typename TYPE::const_iterator;

			template <class K>
			void add(K&& val)
			{
				storage.push_back(std::forward<K>(val));
			}

			bool contains(const T& val) const { return std::find(storage.begin(), storage.end(), val) != storage.end(); }

			void erase(int idx) {
				for (int i = idx; i < storage.size() - 1; ++i) {
					std::swap(storage[i], storage[i + 1]);
				}
				storage.pop_back();
			}

			bool empty() const { return storage.empty(); }
			void clear() { storage.clear(); }

			ITERATOR begin() { return storage.begin(); }
			ITERATOR end() { return storage.end(); }
			CONST_ITERATOR begin() const { return storage.begin(); }
			CONST_ITERATOR end() const { return storage.end(); }

			TYPE storage;
		};

		template<class T>
		using STORAGE = Storage<T>;
	};

	template <typename SIGNATURE, typename STORAGE_POLICY, typename SLOT_POLICY>
	class EventImplSimpleContainer {
	public:
		using SLOT = typename SLOT_POLICY::template SLOT<SIGNATURE>;
		using HANDLE = void;

	public:
		void subscribe(SLOT slot)
		{
			storage.add(std::move(slot));
		}

		void unsubscribe_all() { storage.clear(); }
		void clear() { unsubscribe_all(); }
		bool empty() const { return storage.empty(); }

	private:
		using CONNECTION = SLOT;
		using STORAGE_T = typename STORAGE_POLICY::template STORAGE<CONNECTION>;

	protected:
		using ITERATOR = typename STORAGE_T::ITERATOR;
		using CONST_ITERATOR = typename STORAGE_T::CONST_ITERATOR;

	protected:
		ITERATOR begin() { return storage.begin(); }
		ITERATOR end() { return storage.end(); }
		CONST_ITERATOR begin() const { return storage.begin(); }
		CONST_ITERATOR end() const { return storage.end(); }

	private:
		STORAGE_T storage;
	};

	template<class SIGNATURE, class STORAGE_POLICY, class POLICY_SLOT>
	class EventImplManagedContainer
	{
	public:
		using HANDLE = std::size_t;
		using SLOT = POLICY_SLOT:: template SLOT<SIGNATURE>;
	public:
		template <class K>
		HANDLE subscribe(K&& slot);
		template <class TAG>
		void subscribe_by_tag(const SLOT& slot);
		template <class TAG>
		void subscribe_by_tag(SLOT&& slot);
		bool is_handle_valid(HANDLE handle) const { return handle != NULL_TAG; }
		template <class TAG>
		bool unsubscribe_by_tag();
		bool unsubscribe(HANDLE handle);
		void unsubscribe_all();
		void clear() { unsubscribe_all(); }
		bool empty() const { return storage.empty(); }

	private:
		using STORAGE_SLOT = typename STORAGE_POLICY::template STORAGE<SLOT>;
		using STORAGE_HANDLES = typename STORAGE_POLICY::template STORAGE<HANDLE>;

	protected:
		using ITERATOR = typename STORAGE_SLOT::ITERATOR;
		using CONST_ITERATOR = typename STORAGE_SLOT::CONST_ITERATOR;

	protected:
		ITERATOR begin() { return storage.begin(); }
		ITERATOR end() { return storage.end(); }
		CONST_ITERATOR begin() const { return storage.begin(); }
		CONST_ITERATOR end() const { return storage.end(); }

	private:
		static constexpr std::size_t TAG_HANDLE_BIT = 0x80000000;
		static constexpr HANDLE NULL_TAG = 0;

		STORAGE_SLOT storage;
		STORAGE_HANDLES handles;
		std::size_t handleCounter = 1;
	};

	template <typename SIGNATURE, typename STORAGE_POLICY, typename SLOT_POLICY>
	template <typename K>
	auto EventImplManagedContainer<SIGNATURE, STORAGE_POLICY, SLOT_POLICY>::subscribe(K&& slot) -> HANDLE
	{
		HANDLE handle(handleCounter++);

		storage.add(std::forward<K>(slot));
		handles.add(handle);

		return handle;
	}

	template <typename SIGNATURE, typename STORAGE_POLICY, typename SLOT_POLICY>
	template <typename TAG>
	void EventImplManagedContainer<SIGNATURE, STORAGE_POLICY, SLOT_POLICY>::subscribe_by_tag(const SLOT& slot)
	{
		HANDLE handle{ TAG_HANDLE_BIT | ds::type_id::value<TAG>() };

		storage.add(slot);

		ASSERT_MSG(handles.contains(handle), "Only on subscription for the same TAG is allowed");
		handles.add(handle);
	}

	template <typename SIGNATURE, typename STORAGE_POLICY, typename SLOT_POLICY>
	template <typename TAG>
	void EventImplManagedContainer<SIGNATURE, STORAGE_POLICY, SLOT_POLICY>::subscribe_by_tag(SLOT&& slot)
	{
		HANDLE handle{ TAG_HANDLE_BIT | ds::type_id::value<TAG>() };
		if (handles.contains(handle)) {
			ASSERT_FAIL("Only one subscription for the same TAG is allowed");
			return;
		}

		storage.add(std::move(slot));
		handles.add(handle);
	}

	template <typename SIGNATURE, typename STORAGE_POLICY, typename SLOT_POLICY>
	bool EventImplManagedContainer<SIGNATURE, STORAGE_POLICY, SLOT_POLICY>::unsubscribe(HANDLE handle)
	{
		int idxFound = -1;

		typename STORAGE_HANDLES::CONST_ITERATOR it = handles.begin();
		for (int idx = 0; it != handles.end(); ++it, ++idx) {
			if (*it == handle) {
				idxFound = idx;
				break;
			}
		}

		if (idxFound == -1) {
			return false;
		}

		storage.erase(idxFound);
		handles.erase(idxFound);

		return true;
	}

	template <typename SIGNATURE, typename STORAGE_POLICY, typename SLOT_POLICY>
	template <typename TAG>
	bool EventImplManagedContainer<SIGNATURE, STORAGE_POLICY, SLOT_POLICY>::unsubscribe_by_tag()
	{
		HANDLE handle{ TAG_HANDLE_BIT | ds::type_id::value<TAG>() };

		return unsubscribe(handle);
	}

	template <typename SIGNATURE, typename STORAGE_POLICY, typename SLOT_POLICY>
	void EventImplManagedContainer<SIGNATURE, STORAGE_POLICY, SLOT_POLICY>::unsubscribe_all()
	{
		storage.clear();
		handles.clear();
	}

	template <typename STORAGE_POLICY, typename SLOT_POLICY = EventSlotPolicyCopy>
	struct EventPolicyManagedContainer {
		template <typename SIGNATURE>
		struct IMPL {
			using TYPE = EventImplManagedContainer<SIGNATURE, STORAGE_POLICY, SLOT_POLICY>;
		};
	};

	template <typename STORAGE_POLICY, typename SLOT_POLICY = EventSlotPolicyCopy>
	struct EventPolicySimpleContainer {
		template <typename SIGNATURE>
		struct IMPL {
			using TYPE = EventImplSimpleContainer<SIGNATURE, STORAGE_POLICY, SLOT_POLICY>;
		};
	};

	template <typename SIGNATURE, typename IMPL_POLICY>
	class EventBase : public IMPL_POLICY::template IMPL<SIGNATURE>::TYPE{
	public:
	   using IMPL_T = typename IMPL_POLICY::template IMPL<SIGNATURE>::TYPE;
	   using SLOT = typename IMPL_T::SLOT;

	   template <class K>
	   void operator+=(K&& slot)
	   {
		  IMPL_T::subscribe(std::forward<K>(slot));
	   }
	};

	template<class T, class IMPL_POLICY>
	class Event : public EventBase<T, IMPL_POLICY>
	{
		using PARENT_TYPE = EventBase<T, IMPL_POLICY>;

	public:
		void Invoke(auto&&... args) const
		{
			using IMPL_T = typename PARENT_TYPE::IMPL_T;

			for (auto it = IMPL_T::begin(); it != IMPL_T::end(); ++it) {
				(*it)(args...);
			}
		}

		void operator()(auto&&... args) const
		{
			using IMPL_T = typename PARENT_TYPE::IMPL_T;

			for (auto it = IMPL_T::begin(); it != IMPL_T::end(); ++it) {
				(*it)(args...);
			}
		}
	};
}
