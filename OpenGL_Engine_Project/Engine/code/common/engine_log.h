#pragma once
#ifndef _DEBUG
#define enLOG(format, ...)
#else
#include <format>
#include <string_view>
namespace engine
{
	void logger(const std::string_view file, int line, const std::string_view msg);
	template<typename ...ARGS>
	void logger(const std::string_view file, int line, const std::string_view format, ARGS&&... args)
	{
		const auto msg = std::vformat(format, std::make_format_args(args...));
		logger(file, line, msg);
	}

} // namespace engine
namespace en = engine;

#define enLOG(path, format, ...) en::logger(__FILE__, __LINE__, format, __VA_ARGS__)

#endif