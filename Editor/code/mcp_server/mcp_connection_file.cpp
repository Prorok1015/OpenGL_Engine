#include <snk/mcp/mcp_connection_file.h>

#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/string.hpp>
#include <boost/json/value.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <system_error>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
	constexpr const char* file_name = "mcp.json";

	std::string connection_json(const mcp::connection_info& info)
	{
		boost::json::object object;
		object["url"] = boost::json::string(std::string_view(info.url()));
		object["port"] = static_cast<std::int64_t>(info.port);
		object["token"] = boost::json::string(std::string_view(info.token));
		return boost::json::serialize(object) + "\n";
	}
}

std::string mcp::generate_token()
{
	static constexpr char digits[] = "0123456789abcdef";
	std::random_device source;
	std::string token;
	token.reserve(64);
	for (int word = 0; word < 8; ++word) {
		std::uint32_t value = static_cast<std::uint32_t>(source());
		for (int nibble = 0; nibble < 8; ++nibble) {
			token.push_back(digits[value & 0xFu]);
			value >>= 4;
		}
	}
	return token;
}

std::string mcp::connection_info::url() const
{
	return "http://127.0.0.1:" + std::to_string(port) + "/mcp";
}

std::filesystem::path mcp::connection_file_path(const std::filesystem::path& directory)
{
	return directory / file_name;
}

std::string mcp::write_connection_file(const std::filesystem::path& directory, const connection_info& info)
{
	std::error_code ec;
	std::filesystem::create_directories(directory, ec);
	if (ec) {
		return "cannot create " + directory.string() + ": " + ec.message();
	}
	const std::filesystem::path path = connection_file_path(directory);
	const std::string text = connection_json(info);

#ifdef _WIN32
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out) {
		return "cannot open " + path.string();
	}
	out << text;
	out.flush();
	if (!out) {
		return "cannot write " + path.string();
	}
	return {};
#else
	// Created 0600 so the token is never readable by others, not even for an instant; fchmod covers a file that
	// already existed with wider permissions.
	const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		return "cannot open " + path.string();
	}
	std::string failure;
	if (::fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
		failure = "cannot restrict the permissions of " + path.string();
	}
	std::size_t written = 0;
	while (failure.empty() && written < text.size()) {
		const ssize_t n = ::write(fd, text.data() + written, text.size() - written);
		if (n <= 0) {
			failure = "cannot write " + path.string();
		} else {
			written += static_cast<std::size_t>(n);
		}
	}
	::close(fd);
	if (!failure.empty()) {
		std::filesystem::remove(path, ec);
	}
	return failure;
#endif
}

void mcp::remove_connection_file(const std::filesystem::path& directory)
{
	std::error_code ec;
	std::filesystem::remove(connection_file_path(directory), ec);
}
