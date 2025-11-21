#pragma once
#ifndef _DEBUG
#define egLOG(format, ...)
#else
#include <format>
#include <string_view>
namespace engine
{
	struct Worrning {};
	struct Error {};
	struct Message {};

	void logger(const std::string_view file, int line, const std::string_view msg);
	void logger(const std::string_view file, int line, const std::string_view msg, Error policy);
	template<typename ...ARGS>
	void logger(const std::string_view file, int line, const std::string_view format, ARGS&&... args)
	{
		const auto msg = std::vformat(format, std::make_format_args(args...));
		logger(file, line, msg);
	}

} // namespace engine
namespace eg = engine;

#define egLOG(path, format, ...) eg::logger(__FILE__, __LINE__, format, __VA_ARGS__)

#endif