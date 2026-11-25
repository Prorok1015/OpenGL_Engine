#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace edt
{
	enum class line_ending : std::uint8_t
	{
		lf,
		crlf,
	};

	// A shader file as the editor holds it: the text with LF line breaks and no byte order mark, and what Save puts back, so a file
	// written with CRLF or a BOM comes back the way it was.
	struct shader_text_file
	{
		std::string text;
		line_ending ending = line_ending::lf;
		bool bom = false;
		// The file had both kinds of line break: Save writes the dominant one everywhere.
		bool mixed_endings = false;

		bool operator==(const shader_text_file&) const = default;
	};

	// Past this a file is not opened in the editor (the renderer reads at most 4 MiB of expanded text per stage).
	inline constexpr std::size_t k_max_shader_text_bytes = 4u * 1024u * 1024u;

	// Reads the bytes of a shader source: UTF-8 (a BOM is noted and dropped), no NUL, at most k_max_shader_text_bytes. nullopt with
	// `error` set when it is not text the editor can hold.
	std::optional<shader_text_file> read_shader_text(std::span<const std::byte> bytes, std::string& error);

	// The bytes Save writes for `file`.
	std::string write_shader_text(const shader_text_file& file);

	// The same text with `ending` for every line break (the text is LF).
	std::string with_line_ending(std::string_view text, line_ending ending);
}
