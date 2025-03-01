#pragma once
#include <array>

namespace ds
{
	template<class T, std::size_t N>
	class fixed_vector : std::array<T, N>
	{
	public:
		using parent = std::array<T, N>;
		using parent::value_type;
		using parent::iterator;
		using parent::const_iterator;
		using parent::reverse_iterator;
		using parent::const_reverse_iterator;
		using parent::reference;
		using parent::const_reference;
		using parent::const_pointer;
		using parent::pointer;
		using parent::difference_type;
		using parent::size_type;

		using parent::at;
		using parent::operator[];
		using parent::begin;
		using parent::rend;
		using parent::max_size;
		using parent::front;
		using parent::data;
		using parent::swap;

		constexpr fixed_vector() noexcept = default;

		constexpr fixed_vector(std::initializer_list<T> list)
			: std::array<T, N>(list) {
		}

		constexpr void pop_back() {
			static_assert(N > 0);
			ASSERT_MSG(length > 0, "Vector is empty");
			length = std::clamp(length - 1, 0, N);
		}

		constexpr void push_back(T&& val) {
			static_assert(N > 0);
			if (length >= N) {
				ASSERT_FAIL("Vector is full");
				return;
			}

			(*this)[length++] = std::forward<T>(val);
		}

		constexpr std::size_t size() const noexcept { return length; }

		constexpr bool empty() const noexcept {
			return length == 0;
		}

		constexpr iterator end() noexcept {
			return iterator(data(), length);
		}

		constexpr const_iterator end() const noexcept {
			return const_iterator(data(), length);
		}

		constexpr reverse_iterator rbegin() noexcept {
			return reverse_iterator(end());
		}

		constexpr const_reverse_iterator rbegin() const noexcept {
			return const_reverse_iterator(end());
		}

		constexpr reference back() noexcept {
			return (*this)[length - 1];
		}

		constexpr const_reference back() const noexcept {
			return (*this)[length - 1];
		}
	private:
		std::size_t length = 0;
	};
}