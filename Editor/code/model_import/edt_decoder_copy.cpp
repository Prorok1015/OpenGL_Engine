#include "edt_decoder_copy.h"
#include "edt_decode_codec.h"
#include "edt_file_identity.h"
#include <snk/common/logger/engine_log.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <format>
#include <fstream>
#include <iterator>
#include <span>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

std::string utf8_name(const fs::path& path)
{
	const std::u8string text = path.u8string();
	return std::string(text.begin(), text.end());
}

std::optional<std::vector<std::byte>> read_all(const fs::path& file)
{
	std::ifstream in(file, std::ios::binary);
	if (!in) return std::nullopt;
	std::vector<char> chars{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	if (in.bad()) return std::nullopt;
	std::vector<std::byte> bytes(chars.size());
	std::transform(chars.begin(), chars.end(), bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
	return bytes;
}

bool write_all(const fs::path& file, const std::vector<std::byte>& bytes)
{
	std::ofstream out(file, std::ios::binary | std::ios::trunc);
	if (!out) return false;
	out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	out.close();
	return static_cast<bool>(out);
}

// A plain file name: nothing that leaves the decoder's folder.
bool plain_name(std::string_view name)
{
	return !name.empty() && name != "." && name != ".." && name.find_first_of("/\\:") == std::string_view::npos;
}

bool runtime_dll(std::string name)
{
	std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	if (!name.ends_with(".dll")) return false;
	for (const std::string_view prefix : { "vcruntime", "msvcp", "concrt", "ucrtbase", "clang_rt" }) {
		if (name.starts_with(prefix)) return true;
	}
	return false;
}

struct listed_file
{
	std::string name;
	std::vector<std::byte> bytes;
	std::string digest;
};

} // namespace

std::optional<edt::decoder_copy> edt::make_decoder_copy(const fs::path& decoder, const fs::path& base, std::string& error)
{
	// #1158: how long hashing takes in a Debug build, measured before anything is optimized.
	const auto started = std::chrono::steady_clock::now();
	std::chrono::steady_clock::duration hashing{};
	std::uint64_t hashed_bytes = 0;
	const auto timed_digest = [&](std::span<const std::byte> bytes) {
		const auto begin = std::chrono::steady_clock::now();
		std::string digest = digest_to_hex(digest_of(bytes));
		hashing += std::chrono::steady_clock::now() - begin;
		hashed_bytes += bytes.size();
		return digest;
	};
	if (base.empty()) {
		error = "there is no folder for a copy of the decoder";
		return std::nullopt;
	}
	const fs::path source = decoder.parent_path();
	std::vector<std::string> names{ utf8_name(decoder.filename()) };
	const fs::path deps = source / (utf8_name(decoder.stem()) + ".deps");
	const std::optional<std::vector<std::byte>> listed = read_all(deps);
	if (!listed) {
		error = std::format("'{}' cannot be read", utf8_name(deps));
		return std::nullopt;
	}
	std::string line;
	for (std::size_t i = 0; i <= listed->size(); ++i) {
		const char c = i < listed->size() ? static_cast<char>((*listed)[i]) : '\n';
		if (c != '\n' && c != '\r') {
			line.push_back(c);
			continue;
		}
		if (line.empty()) continue;
		if (!plain_name(line)) {
			error = std::format("'{}' names '{}', which is no file beside the decoder", utf8_name(deps), line);
			return std::nullopt;
		}
		names.push_back(std::move(line));
		line.clear();
	}
	std::error_code ec;
	for (const fs::directory_entry& entry : fs::directory_iterator(source, ec)) {
		const std::string name = utf8_name(entry.path().filename());
		if (runtime_dll(name) && entry.is_regular_file(ec)) names.push_back(name);
	}
	std::sort(names.begin(), names.end());
	names.erase(std::unique(names.begin(), names.end()), names.end());

	std::vector<listed_file> files;
	files.reserve(names.size());
	std::string manifest;
	for (const std::string& name : names) {
		std::optional<std::vector<std::byte>> bytes = read_all(source / fs::path(std::u8string(name.begin(), name.end())));
		if (!bytes) {
			error = std::format("'{}' beside the decoder cannot be read", name);
			return std::nullopt;
		}
		std::string digest = timed_digest(*bytes);
		manifest += name + " " + digest + "\n";
		files.push_back({ name, std::move(*bytes), std::move(digest) });
	}
	std::vector<std::byte> manifest_bytes(manifest.size());
	std::transform(manifest.begin(), manifest.end(), manifest_bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
	const std::string key = digest_to_hex(digest_of(manifest_bytes));
	// One folder of copies per decoder: the shipped one and the test build do not remove each other's.
	const fs::path copies = base / decoder.stem();
	const fs::path folder = copies / key;
	const auto file_in_copy = [&folder](const std::string& name) { return folder / fs::path(std::u8string(name.begin(), name.end())); };

	const auto copy_is_whole = [&] {
		for (const listed_file& file : files) {
			const std::optional<std::vector<std::byte>> copied = read_all(file_in_copy(file.name));
			if (!copied || timed_digest(*copied) != file.digest) return false;
		}
		return true;
	};
	if (!fs::is_directory(folder, ec) || !copy_is_whole()) {
		fs::remove_all(folder, ec);
		fs::create_directories(folder, ec);
		if (ec) {
			error = std::format("cannot make the folder '{}': {}", utf8_name(folder), ec.message());
			return std::nullopt;
		}
		for (const listed_file& file : files) {
			const fs::path target = file_in_copy(file.name);
			fs::path part = target;
			part += ".part";
			if (!write_all(part, file.bytes)) {
				error = std::format("cannot write '{}'", utf8_name(part));
				return std::nullopt;
			}
			fs::rename(part, target, ec);
			if (ec) {
				error = std::format("cannot write '{}': {}", utf8_name(target), ec.message());
				return std::nullopt;
			}
		}
		// Another editor may have written the same folder meanwhile: what counts is what is there now.
		if (!copy_is_whole()) {
			error = std::format("the copy in '{}' does not match the decoder", utf8_name(folder));
			return std::nullopt;
		}
	}

	// Copies of other builds pile up otherwise; one still in use by another editor stays (its files cannot be removed).
	for (const fs::directory_entry& entry : fs::directory_iterator(copies, ec)) {
		std::error_code ignored;
		if (entry.path().filename() != folder.filename() && entry.is_directory(ignored)) fs::remove_all(entry.path(), ignored);
	}
	const auto ms = [](std::chrono::steady_clock::duration d) { return std::chrono::duration_cast<std::chrono::milliseconds>(d).count(); };
	egLOG_INFO("edt/decoder", "copy of the decoder: {} files, {} bytes hashed in {} ms, {} ms in all", files.size(), hashed_bytes,
		ms(hashing), ms(std::chrono::steady_clock::now() - started));
	return decoder_copy{ file_in_copy(utf8_name(decoder.filename())), folder };
}

fs::path edt::decoder_copy_base()
{
#if defined(_WIN32)
	const DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
	if (size == 0) return {};
	std::wstring value(size, L'\0');
	value.resize(GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), size));
	if (value.empty()) return {};
	return fs::path(value) / "snk" / "decoder";
#else
	return {};
#endif
}

bool edt::is_loader_failure(int exit_code)
{
	switch (static_cast<std::uint32_t>(exit_code)) {
	case 0xC0000135u:	// STATUS_DLL_NOT_FOUND
	case 0xC0000022u:	// STATUS_ACCESS_DENIED
	case 0xC0000142u:	// STATUS_DLL_INIT_FAILED
	case 0xC000007Bu:	// STATUS_INVALID_IMAGE_FORMAT
	case 0xC0000139u:	// STATUS_ENTRYPOINT_NOT_FOUND
		return true;
	default:
		return false;
	}
}
