#pragma once
#include <memory>
#include "engine_assert.h"

namespace ds
{
	template<class T, class U>
	T& polymorphic_cast(U& ptr)
	{
		auto& ptr1 = static_cast<T&>(ptr);
		auto& ptr2 = dynamic_cast<T&>(ptr);
		ASSERT_MSG(&ptr1 == &ptr2, "polymorphic_cast failed");
		return ptr1;
	}

	template<class T, class U>
	T* polymorphic_cast(U* ptr)
	{
		auto* ptr1 = static_cast<T*>(ptr);
		auto* ptr2 = dynamic_cast<T*>(ptr);
		ASSERT_MSG(ptr1 == ptr2, "polymorphic_cast failed");
		return ptr1;
	}

	template <typename T, template <typename...> class Template>
	inline constexpr bool is_spec_v = false;

	template <template <typename...> class Template, typename... Args>
	inline constexpr bool is_spec_v<Template<Args...>, Template> = true;

	template <typename T, template <typename...> class Template>
	concept is_specialization_of = is_spec_v<std::remove_cvref_t<T>, Template>;

	template<class T>
	std::shared_ptr<T> polymorphic_cast(is_specialization_of<std::shared_ptr> auto&& ptr)
	{
		auto ptr1 = std::static_pointer_cast<T>(ptr);
		auto ptr2 = std::dynamic_pointer_cast<T>(ptr);
		ASSERT_MSG(ptr1 == ptr2, "polymorphic_cast failed");
		return ptr1;
	}

	template<class T>
	std::shared_ptr<T> polymorphic_pointer_cast(is_specialization_of<std::shared_ptr> auto&& ptr)
	{
		auto ptr1 = std::static_pointer_cast<T>(ptr);
		auto ptr2 = std::dynamic_pointer_cast<T>(ptr);
		ASSERT_MSG(ptr1 == ptr2, "polymorphic_cast failed");
		return ptr1;
	}

	template <typename T>
	std::unique_ptr<T> polymorphic_cast(is_specialization_of<std::unique_ptr> auto&& ptr)
	{
		auto raw_ptr = ptr.get();
		auto ptr1 = static_cast<T*>(raw_ptr);
		auto ptr2 = dynamic_cast<T*>(raw_ptr);
		ASSERT_MSG(ptr1 != ptr2, "polymorphic_cast failed");
		return std::static_pointer_cast<T>(std::move(ptr));
	}
}
