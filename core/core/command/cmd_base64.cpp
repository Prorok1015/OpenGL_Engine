#include <snk/command/cmd_base64.h>
#include <array>
#include <cstdint>

namespace {
	constexpr std::string_view ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	constexpr std::array<std::int8_t, 256> make_reverse()
	{
		std::array<std::int8_t, 256> table{};
		for (auto& entry : table) entry = -1;
		for (std::size_t i = 0; i < ALPHABET.size(); ++i) table[static_cast<unsigned char>(ALPHABET[i])] = static_cast<std::int8_t>(i);
		return table;
	}
}

std::string cmd::base64::encode(std::span<const std::byte> data)
{
	std::string out;
	out.reserve((data.size() + 2) / 3 * 4);
	std::size_t i = 0;
	for (; i + 3 <= data.size(); i += 3) {
		const std::uint32_t triple = (std::to_integer<std::uint32_t>(data[i]) << 16) | (std::to_integer<std::uint32_t>(data[i + 1]) << 8) | std::to_integer<std::uint32_t>(data[i + 2]);
		out += ALPHABET[(triple >> 18) & 63];
		out += ALPHABET[(triple >> 12) & 63];
		out += ALPHABET[(triple >> 6) & 63];
		out += ALPHABET[triple & 63];
	}
	const std::size_t rest = data.size() - i;
	if (rest > 0) {
		std::uint32_t triple = std::to_integer<std::uint32_t>(data[i]) << 16;
		if (rest == 2) triple |= std::to_integer<std::uint32_t>(data[i + 1]) << 8;
		out += ALPHABET[(triple >> 18) & 63];
		out += ALPHABET[(triple >> 12) & 63];
		out += rest == 2 ? ALPHABET[(triple >> 6) & 63] : '=';
		out += '=';
	}
	return out;
}

std::optional<std::vector<std::byte>> cmd::base64::decode(std::string_view text)
{
	static constexpr auto reverse = make_reverse();
	if (text.size() % 4 != 0) return std::nullopt;
	std::vector<std::byte> out;
	out.reserve(text.size() / 4 * 3);
	for (std::size_t i = 0; i < text.size(); i += 4) {
		const bool last = i + 4 == text.size();
		const std::size_t padding = last ? (text[i + 3] == '=' ? (text[i + 2] == '=' ? 2 : 1) : 0) : 0;
		std::uint32_t triple = 0;
		for (std::size_t k = 0; k < 4; ++k) {
			std::int32_t sextet = 0;
			if (k >= 4 - padding) {
				if (text[i + k] != '=') return std::nullopt;
			} else {
				sextet = reverse[static_cast<unsigned char>(text[i + k])];
				if (sextet < 0) return std::nullopt;
			}
			triple = (triple << 6) | static_cast<std::uint32_t>(sextet);
		}
		out.push_back(static_cast<std::byte>((triple >> 16) & 0xff));
		if (padding < 2) out.push_back(static_cast<std::byte>((triple >> 8) & 0xff));
		if (padding < 1) out.push_back(static_cast<std::byte>(triple & 0xff));
	}
	return out;
}
