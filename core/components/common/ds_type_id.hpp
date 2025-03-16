#pragma once
#include <string_view>
#include <source_location>

namespace ds {
	struct Type
	{
		using unique_id = size_t;
	public:
		template<class T>
		static unique_id value() {
			static unique_id classId = next();
			return classId;
		}
		
		template<class T>
		static Type make() {
			return Type{ .id = value<T>() };
		}

		auto operator<=>(const Type&) const = default;

	private:
		static unique_id next() {
			static unique_id counter = 0;
			return counter++;
		}

	public:
		unique_id id = 0;
	};

//  It doesn't work well with MSVC !!!
//	namespace detail {
//
//		template<typename T>
//		constexpr std::string_view wrapped_pretty_type_name() {
//#ifdef _MSC_VER 
//			return __FUNCSIG__;
//#else
//			return __PRETTY_FUNCTION__;
//#endif
//		}
//
//#ifdef __cpp_lib_source_location
//		template<typename T>
//		constexpr std::string_view wrapped_modern_type_name() {
//			return std::source_location::current().function_name();;
//		}
//#endif
//
//		template<typename T>
//		constexpr std::string_view wrapped_type_name() {			
//#ifdef __cpp_lib_source_location
//			constexpr auto result = wrapped_modern_type_name<void>().find("void");
//			if constexpr (result == -1) {
//				return wrapped_pretty_type_name<T>();
//			}
//			return wrapped_modern_type_name<T>();
//#else
//			return wrapped_pretty_type_name<T>();
//#endif
//		}
//
//		constexpr std::size_t wrapped_prefix_type_name() {
//			constexpr auto res = wrapped_type_name<void>().find("void", 0);
//			return res;
//		}
//
//		constexpr std::size_t wrapped_suffix_type_name() {
//			return wrapped_type_name<void>().length() - wrapped_prefix_type_name() - std::string_view("void").length();
//		}
//	
//	}
//
//	template<typename T>
//	constexpr std::string_view type_name() {
//		constexpr std::string_view wrapped_type_name = detail::wrapped_type_name<T>();
//		constexpr auto prefix_length = detail::wrapped_prefix_type_name();
//		constexpr auto suffix_length = detail::wrapped_suffix_type_name();
//		constexpr auto type_name_length = wrapped_type_name.length() - prefix_length - suffix_length;
//		return wrapped_type_name.substr(prefix_length, type_name_length);
//	}
//
//	// enum class Y { OPEN };
//	// template<Y value>
//	// constexpr std::string_view type_name();
//	// template<>
//	// constexpr std::string_view type_name<Y::OPEN>() {
//	// 	return "OPEN";
//	// }
//
//	static_assert(type_name<int>() == "int");
//	static_assert(type_name<int>() == type_name<int>());
//	static_assert(type_name<char*>() == "char *");
//	static_assert(type_name<Type>() == "ds::Type");
}

namespace std {
	template<>
	struct hash<ds::Type> {
		std::size_t operator()(const ds::Type& type) const {
			return type.id;
		}
	};
}