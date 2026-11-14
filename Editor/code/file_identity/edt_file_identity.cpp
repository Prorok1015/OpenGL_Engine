#include "edt_file_identity.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <vector>

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

#ifdef _WIN32
namespace {

	// The path as the wide API takes it past MAX_PATH. A folder is refused from MAX_PATH - 12 on (room for an 8.3 name).
	std::wstring long_path(const std::filesystem::path& path)
	{
		std::filesystem::path normal = path.lexically_normal();
		std::wstring text = normal.make_preferred().native();
		if (text.size() < MAX_PATH - 12 || !normal.is_absolute() || text.starts_with(L"\\\\?\\")
			|| text.starts_with(L"\\\\.\\")) {
			return text;
		}
		if (text.starts_with(L"\\\\")) {
			return L"\\\\?\\UNC\\" + text.substr(2);
		}
		return L"\\\\?\\" + text;
	}

	edt::file_id id_of_handle(HANDLE handle)
	{
		edt::file_id id;
		FILE_ID_INFO wide{};
		BY_HANDLE_FILE_INFORMATION info{};
		if (GetFileInformationByHandleEx(handle, FileIdInfo, &wide, sizeof(wide))) {
			std::memcpy(&id.index, wide.FileId.Identifier, sizeof(id.index));
			std::memcpy(&id.index_high, wide.FileId.Identifier + sizeof(id.index), sizeof(id.index_high));
			id.volume = wide.VolumeSerialNumber;
			id.valid = true;
		} else if (GetFileInformationByHandle(handle, &info)) {
			id.volume = info.dwVolumeSerialNumber;
			id.index = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
			id.valid = true;
		}
		return id;
	}

	// The ids of the entries of the folder open as `folder`, on the volume `volume`; false when the folder could not be
	// listed whole.
	bool list_ids(HANDLE folder, std::uint64_t volume, std::unordered_map<std::wstring, edt::file_id>& by_name)
	{
		// The entries hold 64-bit fields: the buffer is aligned for them.
		std::vector<std::uint64_t> buffer(8 * 1024);
		const DWORD bytes = static_cast<DWORD>(buffer.size() * sizeof(std::uint64_t));
		// The 128-bit ids of ReFS (Windows 8 on); a file system without them answers the 64-bit listing.
		bool wide = true;
		bool first = true;
		for (;;) {
			const FILE_INFO_BY_HANDLE_CLASS kind = wide
				? (first ? FileIdExtdDirectoryRestartInfo : FileIdExtdDirectoryInfo)
				: (first ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo);
			if (!GetFileInformationByHandleEx(folder, kind, buffer.data(), bytes)) {
				const DWORD error = GetLastError();
				if (error == ERROR_NO_MORE_FILES) {
					return true;
				}
				// ERROR_INVALID_LEVEL: some network redirectors.
				if (wide && first && (error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_SUPPORTED
					|| error == ERROR_INVALID_FUNCTION || error == ERROR_INVALID_LEVEL)) {
					wide = false;
					continue;
				}
				return false;
			}
			first = false;
			const auto* at = reinterpret_cast<const std::byte*>(buffer.data());
			for (;;) {
				edt::file_id id;
				id.volume = volume;
				id.valid = true;
				DWORD next = 0;
				if (wide) {
					const auto* entry = reinterpret_cast<const FILE_ID_EXTD_DIR_INFO*>(at);
					std::memcpy(&id.index, entry->FileId.Identifier, sizeof(id.index));
					std::memcpy(&id.index_high, entry->FileId.Identifier + sizeof(id.index), sizeof(id.index_high));
					by_name.insert_or_assign(std::wstring(entry->FileName, entry->FileNameLength / sizeof(WCHAR)), id);
					next = entry->NextEntryOffset;
				} else {
					const auto* entry = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(at);
					id.index = static_cast<std::uint64_t>(entry->FileId.QuadPart);
					by_name.insert_or_assign(std::wstring(entry->FileName, entry->FileNameLength / sizeof(WCHAR)), id);
					next = entry->NextEntryOffset;
				}
				if (next == 0) {
					break;
				}
				at += next;
			}
		}
	}

}
#endif

edt::file_id edt::file_id_of(const std::filesystem::path& path)
{
	file_id id;
#ifdef _WIN32
	const HANDLE handle = CreateFileW(long_path(path).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (handle == INVALID_HANDLE_VALUE) return id;
	id = id_of_handle(handle);
	CloseHandle(handle);
#else
	struct stat info{};
	if (::lstat(path.c_str(), &info) == 0) {
		id.volume = static_cast<std::uint64_t>(info.st_dev);
		id.index = static_cast<std::uint64_t>(info.st_ino);
		id.valid = true;
	}
#endif
	return id;
}

edt::file_id edt::file_id_reader::id_of(const std::filesystem::path& path)
{
#ifdef _WIN32
	const std::wstring name = path.filename().native();
	auto [found, added] = m_folders.try_emplace(path.parent_path().lexically_normal().make_preferred().native());
	folder& listing = found->second;
	if (added) {
		const HANDLE handle = CreateFileW(long_path(path.parent_path()).c_str(), FILE_LIST_DIRECTORY,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
			nullptr);
		if (handle != INVALID_HANDLE_VALUE) {
			const file_id own = id_of_handle(handle);
			listing.listed = own.valid && list_ids(handle, own.volume, listing.by_name);
			CloseHandle(handle);
		}
		if (!listing.listed) {
			listing.by_name.clear();
		}
	}
	if (listing.listed) {
		if (const auto entry = listing.by_name.find(name); entry != listing.by_name.end()) {
			return entry->second;
		}
		// A name spelled in another letter case than the folder holds it.
		for (const auto& [entry_name, id] : listing.by_name) {
			if (CompareStringOrdinal(entry_name.data(), static_cast<int>(entry_name.size()), name.data(),
				static_cast<int>(name.size()), TRUE) == CSTR_EQUAL) {
				return id;
			}
		}
	}
	return file_id_of(path);
#else
	return file_id_of(path);
#endif
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
