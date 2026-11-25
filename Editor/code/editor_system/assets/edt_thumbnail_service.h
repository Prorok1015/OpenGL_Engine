#pragma once
#include "edt_asset_filter.h"
#include "edt_thumbnail.h"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace edt
{
	// The thumbnails of the asset browser's picture files (#1192): asked for by the rows that are on the screen, decoded on
	// a worker, uploaded a few a frame, kept in a bounded LRU and given back when a file changes or the folder is left.
	// Main thread only; the GPU and the workers are ports, so everything here runs in a test without either.
	class thumbnail_service
	{
	public:
		struct view
		{
			enum class state_t : std::uint8_t { pending, ready, failed };
			state_t state = state_t::pending;
			std::uint64_t texture = 0; // what to draw (the GUI backend's id), for `ready`
			int width = 0;
			int height = 0;
			std::string error; // for `failed`
		};

		struct ports
		{
			// Runs `work` on a worker and then `done` with its result on the main thread; `done` may never run (a cancelled job).
			std::function<void(std::function<thumbnail_decoded()> work, std::function<void(thumbnail_decoded)> done)> dispatch;
			// The stamp of root/relative as it is, nothing read; nullopt: no regular file there.
			std::function<std::optional<desc_type_cache::stamp>(const std::filesystem::path& root, const std::filesystem::path& relative)> stat;
			// The worker's job: the file as a thumbnail.
			std::function<thumbnail_decoded(const std::filesystem::path& root, const std::filesystem::path& relative)> decode;
			// The picture on the GPU under `key` (unique); 0: it could not be made.
			std::function<std::uint64_t(const std::string& key, const thumbnail_pixels& pixels)> upload;
			// Gives the texture made under `key` back.
			std::function<void(const std::string& key)> release;
			// Seconds, any origin.
			std::function<double()> now;
			// Called first in every tick() (optional): where the owner looks at what the workers depend on (the project, its trust).
			std::function<void()> frame;
		};

		struct limits
		{
			int max_running = 4;
			int uploads_per_frame = 4;
			std::size_t max_entries = 256;
			// A shown thumbnail's file is looked at again after this long (a stat, nothing read).
			double restamp_seconds = 1.0;
		};

		explicit thumbnail_service(ports from);
		thumbnail_service(ports from, limits with);
		~thumbnail_service();
		thumbnail_service(const thumbnail_service&) = delete;
		thumbnail_service& operator=(const thumbnail_service&) = delete;

		// What to show for the file now; asking is what makes it wanted: a row that is not asked for in a frame is dropped
		// from the queue at the next tick. A new or changed file is queued; a failed one is not tried again until it changes.
		view lookup(const std::filesystem::path& root, const std::filesystem::path& relative);

		// Once per frame: uploads, then starts the decodes that wait (those still asked for), up to the limits.
		void tick();

		// Forgets every entry (a folder was left, the layers changed); results of decodes still running are dropped when
		// they arrive. The textures are not freed here: this may run in the middle of a frame whose draw list already holds
		// them, so they go to a graveyard that the next tick() empties before it uploads anything.
		void release_all();

		std::size_t entries() const { return m_entries.size(); }
		int running() const { return m_running; }
		std::size_t decodes_started() const { return m_decodes_started; }
		// Textures held by entries (not those waiting in the graveyard).
		std::size_t textures() const { return m_textures; }
		// Texture keys that wait for the next tick() to be given back.
		std::size_t graveyard() const { return m_graveyard.size(); }
		// The keys ever made: bounded by max_entries plus what one frame buries, because a given-back key is used again.
		std::size_t keys_made() const { return static_cast<std::size_t>(m_next_texture); }

	private:
		enum class phase : std::uint8_t { queued, decoding, uploading, ready, failed };

		struct entry
		{
			std::filesystem::path root;
			std::filesystem::path relative;
			std::optional<desc_type_cache::stamp> stamp;
			double checked_at = 0.0;
			std::uint64_t used_tick = 0;
			std::uint64_t request = 0;
			phase state = phase::queued;
			std::string error;
			std::string texture_key;
			std::uint64_t texture = 0;
			int width = 0;
			int height = 0;
			std::optional<thumbnail_pixels> pixels; // decoded, waiting for its upload
		};

		static std::string key_of(const std::filesystem::path& root, const std::filesystem::path& relative);
		view view_of(const entry& e) const;
		void queue(const std::string& key, entry& e);
		void drop_texture(entry& e);
		void evict_over_limit(const std::string& keep);
		void finish(const std::string& key, std::uint64_t request, thumbnail_decoded decoded);
		void flush_graveyard();

		ports m_ports;
		limits m_limits;
		std::unordered_map<std::string, entry> m_entries;
		std::vector<std::string> m_waiting;
		std::vector<std::string> m_upload_queue;
		std::vector<std::string> m_graveyard;
		std::vector<std::string> m_free_keys;
		std::uint64_t m_tick = 0;
		std::uint64_t m_next_request = 0;
		std::uint64_t m_next_texture = 0;
		int m_running = 0;
		std::size_t m_decodes_started = 0;
		std::size_t m_textures = 0;
	};
}
