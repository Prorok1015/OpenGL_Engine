#include <snk/common/hash/sha256.h>
#include <array>
#include <bit>
#include <cstdint>
#include <fstream>
#include <system_error>
#include <vector>

namespace {

	constexpr std::array<std::uint32_t, 64> k_round = {
		0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
		0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
		0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
		0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
		0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
		0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
		0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
		0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
	};

	class sha256_state
	{
	public:
		void update(std::span<const std::byte> bytes)
		{
			m_length += bytes.size();
			for (const std::byte b : bytes) {
				m_block[m_used++] = static_cast<std::uint8_t>(b);
				if (m_used == m_block.size()) {
					compress();
					m_used = 0;
				}
			}
		}

		std::string finish()
		{
			const std::uint64_t bits = static_cast<std::uint64_t>(m_length) * 8u;
			m_block[m_used++] = 0x80u;
			if (m_used > 56) {
				while (m_used < m_block.size()) {
					m_block[m_used++] = 0;
				}
				compress();
				m_used = 0;
			}
			while (m_used < 56) {
				m_block[m_used++] = 0;
			}
			for (int i = 7; i >= 0; --i) {
				m_block[m_used++] = static_cast<std::uint8_t>(bits >> (i * 8));
			}
			compress();

			constexpr char k_digits[] = "0123456789abcdef";
			std::string hex;
			hex.reserve(64);
			for (const std::uint32_t word : m_hash) {
				for (int shift = 28; shift >= 0; shift -= 4) {
					hex.push_back(k_digits[(word >> shift) & 0xfu]);
				}
			}
			return hex;
		}

	private:
		void compress()
		{
			std::array<std::uint32_t, 64> w{};
			for (std::size_t i = 0; i < 16; ++i) {
				w[i] = (std::uint32_t{ m_block[i * 4] } << 24) | (std::uint32_t{ m_block[i * 4 + 1] } << 16)
					| (std::uint32_t{ m_block[i * 4 + 2] } << 8) | std::uint32_t{ m_block[i * 4 + 3] };
			}
			for (std::size_t i = 16; i < 64; ++i) {
				const std::uint32_t s0 = std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
				const std::uint32_t s1 = std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
				w[i] = w[i - 16] + s0 + w[i - 7] + s1;
			}
			std::uint32_t a = m_hash[0], b = m_hash[1], c = m_hash[2], d = m_hash[3];
			std::uint32_t e = m_hash[4], f = m_hash[5], g = m_hash[6], h = m_hash[7];
			for (std::size_t i = 0; i < 64; ++i) {
				const std::uint32_t s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
				const std::uint32_t choice = (e & f) ^ (~e & g);
				const std::uint32_t t1 = h + s1 + choice + k_round[i] + w[i];
				const std::uint32_t s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
				const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
				const std::uint32_t t2 = s0 + majority;
				h = g;
				g = f;
				f = e;
				e = d + t1;
				d = c;
				c = b;
				b = a;
				a = t1 + t2;
			}
			m_hash[0] += a;
			m_hash[1] += b;
			m_hash[2] += c;
			m_hash[3] += d;
			m_hash[4] += e;
			m_hash[5] += f;
			m_hash[6] += g;
			m_hash[7] += h;
		}

		std::array<std::uint32_t, 8> m_hash = {
			0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
		};
		std::array<std::uint8_t, 64> m_block{};
		std::size_t m_used = 0;
		std::uint64_t m_length = 0;
	};

}

std::string cmn::sha256_hex(std::span<const std::byte> bytes)
{
	sha256_state state;
	state.update(bytes);
	return state.finish();
}

std::optional<std::string> cmn::sha256_file_hex(const std::filesystem::path& file)
{
	std::error_code ec;
	if (!std::filesystem::is_regular_file(file, ec)) {
		return std::nullopt;
	}
	std::ifstream in(file, std::ios::binary);
	if (!in) {
		return std::nullopt;
	}
	sha256_state state;
	std::vector<char> chunk(64 * 1024);
	while (in) {
		in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
		const std::streamsize got = in.gcount();
		if (got > 0) {
			state.update(std::as_bytes(std::span{ chunk.data(), static_cast<std::size_t>(got) }));
		}
	}
	if (in.bad()) {
		return std::nullopt;
	}
	return state.finish();
}
