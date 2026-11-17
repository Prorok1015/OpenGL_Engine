#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>

namespace edt {

	// Which file on the disk a path reaches: the same value for every path to one file, another one once the path is
	// pointed at a different file (a swap or a rename over it). Together with size and time it cannot be forged by
	// writing the old size and time back. `index_high` is the upper half of the 128-bit id of ReFS; zero elsewhere.
	struct file_id
	{
		std::uint64_t volume = 0;
		std::uint64_t index = 0;
		std::uint64_t index_high = 0;
		bool valid = false;

		bool operator==(const file_id&) const = default;
		// Two ids that could not be read are equal to each other, so a check must ask for `valid` first.
		bool same_as(const file_id& other) const { return valid && other.valid && *this == other; }
	};

	// The file the path itself is (a link is not followed); invalid when it cannot be told. On Windows it opens the file
	// for a moment: see file_id_reader.
	file_id file_id_of(const std::filesystem::path& path);

	// Identities of many files without a handle on any of them: on Windows a rename over a file (MoveFileExW with
	// REPLACE_EXISTING, the atomic save of the editor and of other tools) fails while the file is open anywhere, so
	// each folder is listed once (FileIdExtdDirectoryInfo) and the ids come from the listing. A listing is kept for the
	// reader's life: take a new reader to see the files as they are now. Elsewhere it is file_id_of.
	class file_id_reader
	{
	public:
		file_id id_of(const std::filesystem::path& path);

	private:
#ifdef _WIN32
		struct folder
		{
			bool listed = false;
			std::unordered_map<std::wstring, file_id> by_name;
		};
		std::unordered_map<std::wstring, folder> m_folders;
#endif
	};

	using content_digest = std::array<std::uint8_t, 32>;

	// SHA-256: the bytes of a model file as a person was shown them.
	class content_hasher
	{
	public:
		content_hasher();
		void update(std::span<const std::byte> bytes);
		content_digest finish();

	private:
		void process_block(const std::uint8_t* block);

		std::array<std::uint32_t, 8> m_state;
		std::array<std::uint8_t, 64> m_buffer{};
		std::size_t m_buffered = 0;
		std::uint64_t m_length = 0;
	};

	content_digest digest_of(std::span<const std::byte> bytes);

} // namespace edt
