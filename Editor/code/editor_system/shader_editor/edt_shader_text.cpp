#include "edt_shader_text.h"
#include <algorithm>
#include <cstring>

namespace edt
{
	namespace
	{
		constexpr std::string_view k_bom = "\xEF\xBB\xBF";
	}

	bool is_valid_utf8(std::string_view text)
	{
		std::size_t i = 0;
		const std::size_t size = text.size();
		while (i < size) {
			const unsigned char lead = static_cast<unsigned char>(text[i]);
			std::size_t extra = 0;
			unsigned min_code = 0;
			unsigned code = 0;
			if (lead < 0x80) {
				++i;
				continue;
			}
			if ((lead & 0xE0) == 0xC0) {
				extra = 1;
				min_code = 0x80;
				code = lead & 0x1F;
			} else if ((lead & 0xF0) == 0xE0) {
				extra = 2;
				min_code = 0x800;
				code = lead & 0x0F;
			} else if ((lead & 0xF8) == 0xF0) {
				extra = 3;
				min_code = 0x10000;
				code = lead & 0x07;
			} else {
				return false;
			}
			if (i + extra >= size) return false;
			for (std::size_t k = 1; k <= extra; ++k) {
				const unsigned char next = static_cast<unsigned char>(text[i + k]);
				if ((next & 0xC0) != 0x80) return false;
				code = (code << 6) | (next & 0x3F);
			}
			if (code < min_code || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) return false;
			i += extra + 1;
		}
		return true;
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
				++crlf;
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
