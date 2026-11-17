#pragma once
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>
#include <snk/common/snk_api.h>
#include <snk/model_codec/res_tag.h>

// The pure part of a desc's binary file (EPIC-79, #1068): layouts, ranges, the names of the pair, encode and save_lock.
// Reading and saving through a resource_system are in <snk/resource/res_desc_binary.h>.
namespace res
{
	enum class binary_scalar : std::uint8_t
	{
		u8,
		u32,
		f32,
	};

	// The element of a range: "u8", "u32", "f32", or "u32xN" / "f32xN" with N in 1..32.
	struct binary_layout
	{
		binary_scalar scalar = binary_scalar::u8;
		std::uint8_t count = 1;

		std::size_t element_bytes() const { return (scalar == binary_scalar::u8 ? 1u : 4u) * count; }
	};

	// One run of bytes in the binary file, as the desc records it. `offset` counts from the start of the file.
	struct binary_range
	{
		std::uint64_t offset = 0;
		std::uint64_t size = 0;
		// SHA-256 of the range's bytes, 64 lowercase hex digits.
		std::string sha256;
		std::string layout;
	};

	// Bytes to write as one range.
	struct binary_chunk
	{
		std::span<const std::byte> bytes;
		std::string_view layout;
	};

	// Why a read was refused; a caller tells a hash mismatch (a pair caught mid-save) from a broken file by it.
	enum class desc_binary_error : std::uint8_t
	{
		none,
		missing,
		header,
		range,
		layout,
		hash,
		too_large,
		io,
	};

	// Move-only: `ranges` point into `file`, whose buffer a move keeps.
	struct desc_binary_data
	{
		desc_binary_error error = desc_binary_error::none;
		// Names the file and the range.
		std::string message;
		// The whole file as read.
		std::vector<std::byte> file;
		// One per requested range, in the order asked, into `file`.
		std::vector<std::span<const std::byte>> ranges;
		// Read from the staged file a save left behind (it matched the desc, the binary file did not).
		bool from_staged = false;

		desc_binary_data() = default;
		desc_binary_data(desc_binary_data&&) noexcept = default;
		desc_binary_data& operator=(desc_binary_data&&) noexcept = default;
		desc_binary_data(const desc_binary_data&) = delete;
		desc_binary_data& operator=(const desc_binary_data&) = delete;

		explicit operator bool() const { return error == desc_binary_error::none; }
	};

	SNK_API std::optional<binary_layout> parse_binary_layout(std::string_view text);

	SNK_API boost::json::object to_json(const binary_range& range);
	// nullopt with `error` set when a field is missing, negative, not an integer, or the hash is not 64 hex digits.
	SNK_API std::optional<binary_range> binary_range_from_json(const boost::json::value& value, std::string& error);

	// The values of a range as T, copied (the bytes in a file have no alignment). False when the size is not a multiple.
	template<class T>
	bool copy_binary_values(std::span<const std::byte> bytes, std::vector<T>& out)
	{
		static_assert(std::is_trivially_copyable_v<T>);
		static_assert(std::endian::native == std::endian::little, "the binary file is little-endian");
		if (bytes.size() % sizeof(T) != 0) {
			return false;
		}
		out.resize(bytes.size() / sizeof(T));
		if (!bytes.empty()) {
			std::memcpy(out.data(), bytes.data(), bytes.size());
		}
		return true;
	}

	namespace desc_binary
	{
		inline constexpr std::uint32_t version = 1;
		// "SNKB", then the version as a little-endian u32.
		inline constexpr std::array<char, 4> magic{ 'S', 'N', 'K', 'B' };
		inline constexpr std::size_t header_bytes = 8;
		// The writer starts every range at a multiple of this.
		inline constexpr std::size_t range_alignment = 16;
		// Binary data is denser than its JSON text, so the geometry JSON limit (res::max_geometry_json_bytes) bounds it
		// with room to spare; res_desc_binary.h checks the two stay equal.
		inline constexpr std::uint64_t max_file_bytes = std::uint64_t{ 256 } * 1024 * 1024;
		// More ranges in one request are refused.
		inline constexpr std::size_t max_ranges = 64;

		// `<name>.<ext>.desc` -> `<name>.<ext>.bin` in the same folder; nullopt for any other name.
		SNK_API std::optional<tag> binary_tag(const tag& desc);
		// `<name>.<ext>.bin.new`: what a save writes before the desc.
		SNK_API std::optional<tag> staged_tag(const tag& desc);

		// The file for `chunks`: header, then each chunk at the next multiple of range_alignment. Fills `ranges`.
		// nullopt with `error` set for more than max_ranges chunks, an invalid layout, a size that is not a multiple of it,
		// or more than max_file_bytes.
		SNK_API std::optional<std::vector<std::byte>> encode(std::span<const binary_chunk> chunks,
			std::vector<binary_range>& ranges, std::string& error);

		// Serializes in this process a save of `desc` (write_pending, the desc's store, commit_pending) and a committing
		// read; other processes are kept off only by the compare before a remove. Nests on one thread.
		class SNK_API save_lock
		{
		public:
			explicit save_lock(const tag& desc);
			~save_lock();
			save_lock(const save_lock&) = delete;
			save_lock& operator=(const save_lock&) = delete;

			// Whether this lock serializes the saves of `desc` (the lock of its binary file).
			bool guards(const tag& desc) const;

		private:
			std::shared_ptr<std::recursive_mutex> m_mutex;
		};
	}
}
