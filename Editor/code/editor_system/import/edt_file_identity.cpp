#include "edt_file_identity.h"
#include <algorithm>
#include <bit>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace {

constexpr std::array<std::uint32_t, 64> k_round_constants = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };

}

edt::file_id edt::file_id_of(const std::filesystem::path& path)
{
	file_id id;
#ifdef _WIN32
	const HANDLE handle = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (handle == INVALID_HANDLE_VALUE) return id;
	BY_HANDLE_FILE_INFORMATION info{};
	if (GetFileInformationByHandle(handle, &info)) {
		id.volume = info.dwVolumeSerialNumber;
		id.index = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
		id.valid = true;
	}
	CloseHandle(handle);
#else
	struct stat info{};
	if (::stat(path.c_str(), &info) == 0) {
		id.volume = static_cast<std::uint64_t>(info.st_dev);
		id.index = static_cast<std::uint64_t>(info.st_ino);
		id.valid = true;
	}
#endif
	return id;
}

edt::content_hasher::content_hasher()
	: m_state{ 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 }
{
}

void edt::content_hasher::process_block(const std::uint8_t* block)
{
	std::array<std::uint32_t, 64> w{};
	for (std::size_t i = 0; i < 16; ++i) {
		w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) | (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16)
			| (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) | static_cast<std::uint32_t>(block[i * 4 + 3]);
	}
	for (std::size_t i = 16; i < 64; ++i) {
		const std::uint32_t s0 = std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
		const std::uint32_t s1 = std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	auto [a, b, c, d, e, f, g, h] = m_state;
	for (std::size_t i = 0; i < 64; ++i) {
		const std::uint32_t big_s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
		const std::uint32_t choice = (e & f) ^ (~e & g);
		const std::uint32_t first = h + big_s1 + choice + k_round_constants[i] + w[i];
		const std::uint32_t big_s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
		const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
		const std::uint32_t second = big_s0 + majority;
		h = g; g = f; f = e; e = d + first; d = c; c = b; b = a; a = first + second;
	}
	m_state[0] += a; m_state[1] += b; m_state[2] += c; m_state[3] += d;
	m_state[4] += e; m_state[5] += f; m_state[6] += g; m_state[7] += h;
}

void edt::content_hasher::update(std::span<const std::byte> bytes)
{
	m_length += bytes.size();
	const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
	std::size_t left = bytes.size();
	if (m_buffered > 0) {
		const std::size_t take = (std::min)(left, m_buffer.size() - m_buffered);
		std::memcpy(m_buffer.data() + m_buffered, data, take);
		m_buffered += take;
		data += take;
		left -= take;
		if (m_buffered < m_buffer.size()) return;
		process_block(m_buffer.data());
		m_buffered = 0;
	}
	for (; left >= m_buffer.size(); data += m_buffer.size(), left -= m_buffer.size()) process_block(data);
	if (left > 0) {
		std::memcpy(m_buffer.data(), data, left);
		m_buffered = left;
	}
}

edt::content_digest edt::content_hasher::finish()
{
	const std::uint64_t bits = m_length * 8;
	std::array<std::byte, 72> tail{};
	tail[0] = std::byte{ 0x80 };
	const std::size_t pad = (m_buffered < 56 ? 56 : 120) - m_buffered;
	for (std::size_t i = 0; i < 8; ++i) tail[pad + i] = static_cast<std::byte>(bits >> (56 - 8 * i));
	update(std::span<const std::byte>(tail.data(), pad + 8));
	content_digest out{};
	for (std::size_t i = 0; i < 8; ++i) {
		for (std::size_t j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<std::uint8_t>(m_state[i] >> (24 - 8 * j));
	}
	return out;
}

edt::content_digest edt::digest_of(std::span<const std::byte> bytes)
{
	content_hasher hasher;
	hasher.update(bytes);
	return hasher.finish();
}
