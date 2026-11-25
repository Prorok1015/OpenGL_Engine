#include "edt_shader_text.h"
#include "edt_code_commands.h"
#include <algorithm>
#include <cstring>

namespace edt
{
	namespace
	{
		constexpr std::string_view k_bom = "\xEF\xBB\xBF";
	}

	std::optional<shader_text_file> read_shader_text(std::span<const std::byte> bytes, std::string& error)
	{
		if (bytes.size() > k_max_shader_text_bytes) {
			error = "the file is larger than 4 MiB";
			return std::nullopt;
		}
		const std::string_view raw(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		if (raw.find('\0') != std::string_view::npos) {
			error = "the file holds a NUL byte: it is not a text file";
			return std::nullopt;
		}
		shader_text_file file;
		std::string_view body = raw;
		if (body.starts_with(k_bom)) {
			file.bom = true;
			body.remove_prefix(k_bom.size());
		}
		if (!is_valid_utf8(body)) {
			error = "the file is not valid UTF-8";
			return std::nullopt;
		}
		std::size_t crlf = 0;
		std::size_t lf = 0;
		file.text.reserve(body.size());
		for (std::size_t i = 0; i < body.size(); ++i) {
			if (body[i] == '\r' && i + 1 < body.size() && body[i + 1] == '\n') {
				// One line break: the pair is counted once and becomes one LF.
				++crlf;
				file.text.push_back('\n');
				++i;
				continue;
			}
			if (body[i] == '\n') ++lf;
			file.text.push_back(body[i]);
		}
		file.ending = crlf > lf ? line_ending::crlf : line_ending::lf;
		file.mixed_endings = crlf > 0 && lf > 0;
		return file;
	}

	std::string with_line_ending(std::string_view text, line_ending ending)
	{
		if (ending == line_ending::lf) return std::string(text);
		std::string out;
		out.reserve(text.size() + text.size() / 16);
		for (char c : text) {
			if (c == '\n') out.push_back('\r');
			out.push_back(c);
		}
		return out;
	}

	std::string write_shader_text(const shader_text_file& file)
	{
		std::string out;
		if (file.bom) out.append(k_bom);
		out += with_line_ending(file.text, file.ending);
		return out;
	}
}
