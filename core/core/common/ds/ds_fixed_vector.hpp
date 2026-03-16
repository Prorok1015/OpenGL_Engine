#pragma once
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace ds
{
    template <typename T, std::size_t N>
    class fixed_vector {
    public:
        using value_type = T;
        using size_type = std::size_t;
        using difference_type = std::ptrdiff_t;
        using reference = T&;
        using const_reference = const T&;
        using pointer = T*;
        using const_pointer = const T*;

        using iterator = T*;
        using const_iterator = const T*;

        using reverse_iterator = std::reverse_iterator<iterator>;
        using const_reverse_iterator = std::reverse_iterator<const_iterator>;

        constexpr fixed_vector() noexcept = default;

        constexpr fixed_vector(std::initializer_list<T> init) {
            if (init.size() > N) {
                throw std::length_error("fixed_vector: initializer_list too large");
            }
            for (const auto& item : init) {
                std::construct_at(reinterpret_cast<T*>(storage_) + size_, item);
                ++size_;
            }
        }

        constexpr fixed_vector(const fixed_vector& other) {
            if constexpr (std::is_trivially_copy_constructible_v<T>) {
                std::copy(other.begin(), other.end(), begin());
            }
            else {
                for (const auto& item : other) {
                    push_back(item);
                }
            }
            size_ = other.size_;
        }

        constexpr fixed_vector(fixed_vector&& other) noexcept(std::is_nothrow_move_constructible_v<T>) {
            for (auto& item : other) {
                emplace_back(std::move(item));
            }
            other.clear();
        }

        constexpr fixed_vector& operator=(const fixed_vector& other) {
            if (this != &other) {
                assign(other.begin(), other.end());
            }
            return *this;
        }

        constexpr fixed_vector& operator=(fixed_vector&& other) noexcept(std::is_nothrow_move_assignable_v<T>) {
            if (this != &other) {
                clear();
                for (auto& item : other) {
                    emplace_back(std::move(item));
                }
                other.clear();
            }
            return *this;
        }

        constexpr fixed_vector& operator=(std::initializer_list<T> ilist) {
            if (ilist.size() > N) {
                throw std::length_error("fixed_vector: initializer_list too large in assignment");
            }
            assign(ilist.begin(), ilist.end());
            return *this;
        }

        ~fixed_vector() {
            clear();
        }

        template <typename InputIt>
        constexpr void assign(InputIt first, InputIt last) {
            clear();
            for (; first != last; ++first) {
                push_back(*first);
            }
        }

        constexpr iterator begin() noexcept { return reinterpret_cast<T*>(storage_); }
        constexpr const_iterator begin() const noexcept { return reinterpret_cast<const T*>(storage_); }
        constexpr const_iterator cbegin() const noexcept { return begin(); }

        constexpr iterator end() noexcept { return begin() + size_; }
        constexpr const_iterator end() const noexcept { return begin() + size_; }
        constexpr const_iterator cend() const noexcept { return end(); }

        constexpr reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }
        constexpr const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(end()); }
        constexpr const_reverse_iterator crbegin() const noexcept { return rbegin(); }

        constexpr reverse_iterator rend() noexcept { return reverse_iterator(begin()); }
        constexpr const_reverse_iterator rend() const noexcept { return const_reverse_iterator(begin()); }
        constexpr const_reverse_iterator crend() const noexcept { return rend(); }

        [[nodiscard]] constexpr size_type size() const noexcept { return size_; }
        [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }
        [[nodiscard]] constexpr size_type capacity() const noexcept { return N; }
        [[nodiscard]] constexpr size_type max_size() const noexcept { return N; }

        [[nodiscard]] constexpr reference operator[](size_type index) noexcept {
            assert(index < size_);
            return begin()[index];
        }

        [[nodiscard]] constexpr const_reference operator[](size_type index) const noexcept {
            assert(index < size_);
            return begin()[index];
        }

        [[nodiscard]] constexpr reference at(size_type index) {
            if (index >= size_) throw std::out_of_range("fixed_vector::at");
            return begin()[index];
        }

        [[nodiscard]] constexpr const_reference at(size_type index) const {
            if (index >= size_) throw std::out_of_range("fixed_vector::at");
            return begin()[index];
        }

        [[nodiscard]] constexpr reference front() noexcept { assert(size_ > 0); return *begin(); }
        [[nodiscard]] constexpr const_reference front() const noexcept { assert(size_ > 0); return *begin(); }

        [[nodiscard]] constexpr reference back() noexcept { assert(size_ > 0); return *(end() - 1); }
        [[nodiscard]] constexpr const_reference back() const noexcept { assert(size_ > 0); return *(end() - 1); }

        [[nodiscard]] constexpr pointer data() noexcept { return begin(); }
        [[nodiscard]] constexpr const_pointer data() const noexcept { return begin(); }

        constexpr void clear() noexcept {
            if constexpr (!std::is_trivially_destructible_v<T>) {
                std::destroy(begin(), end());
            }
            size_ = 0;
        }

        constexpr void push_back(const T& value) {
            if (size_ >= N) throw std::length_error("fixed_vector is full");
            std::construct_at(begin() + size_, value);
            ++size_;
        }

        constexpr void push_back(T&& value) {
            if (size_ >= N) throw std::length_error("fixed_vector is full");
            std::construct_at(begin() + size_, std::move(value));
            ++size_;
        }

        template <typename... Args>
        constexpr reference emplace_back(Args&&... args) {
            if (size_ >= N) throw std::length_error("fixed_vector is full");
            T* ptr = std::construct_at(begin() + size_, std::forward<Args>(args)...);
            ++size_;
            return *ptr;
        }

        constexpr void pop_back() noexcept {
            assert(size_ > 0);
            if constexpr (!std::is_trivially_destructible_v<T>) {
                std::destroy_at(end() - 1);
            }
            --size_;
        }

        bool operator==(const fixed_vector& other) const noexcept {
            if (size_ != other.size_) return false;
            for (size_type i = 0; i < size_; ++i) {
                if (!(begin()[i] == other.begin()[i])) {
                    return false;
                }
            }
            return true;
		}

        constexpr iterator insert(const_iterator pos, const T& value) {
            return insert_impl(pos, value);
        }

        constexpr iterator insert(const_iterator pos, T&& value) {
            return insert_impl(pos, std::move(value));
        }

        constexpr iterator erase(const_iterator pos) {
            assert(pos >= begin() && pos < end());
            iterator p = const_cast<iterator>(pos);

            std::move(p + 1, end(), p);

            pop_back();

            return p;
        }

        constexpr iterator erase(const_iterator first, const_iterator last) {
            assert(first >= begin() && first <= end());
            assert(last >= first && last <= end());

            iterator p_first = const_cast<iterator>(first);
            iterator p_last = const_cast<iterator>(last);

            if (p_first == p_last) return p_first;

            iterator new_end = std::move(p_last, end(), p_first);

            size_type elements_to_destroy = std::distance(new_end, end());
            for (size_type i = 0; i < elements_to_destroy; ++i) {
                pop_back();
            }

            return p_first;
        }

    private:
        alignas(T) std::byte storage_[N * sizeof(T)]{};
        size_type size_ = 0;

        template <typename U>
        constexpr iterator insert_impl(const_iterator pos, U&& value) {
            if (size_ >= N) throw std::length_error("fixed_vector is full");

            iterator p = const_cast<iterator>(pos);
            assert(p >= begin() && p <= end());

            if (p == end()) {
                emplace_back(std::forward<U>(value));
                return end() - 1;
            }

            std::construct_at(end(), std::move(back()));
            ++size_;

            std::move_backward(p, end() - 2, end() - 1);

            *p = std::forward<U>(value);

            return p;
        }
    };
}