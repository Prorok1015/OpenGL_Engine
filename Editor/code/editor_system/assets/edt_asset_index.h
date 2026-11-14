#pragma once
#include <snk/resource/res_asset_id.h>
#include <snk/resource/res_asset_index.h>
#include <snk/resource/res_tag.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace res
{
	class resource_system;
}

namespace edt
{
	class asset_database;
	struct asset_batch;
	struct asset_record;

	// The asset database as res::asset_index (EPIC-57 section 5, S57-7): an immutable snapshot of its records, swapped on
	// the main thread and read from any thread (pool threads request dependencies) without waiting for it.
	class asset_index_snapshot final : public res::asset_index
	{
	public:
		std::optional<res::asset_id> id_of(const res::tag& tag) const override;
		std::optional<res::tag> path_of(const res::asset_id& id) const override;

		// Main thread: takes the records of `assets` now; true when any answer changed. Of files that share an ID (a copy
		// made with its .resource), the one holds_id_before puts first keeps it (EPIC-57 section 10: the lowest layer,
		// then the oldest, then the smaller path, as in Problems) and the others answer none: they load as files of their
		// own. Each such copy is warned about once per (ID, path).
		bool refresh(const asset_database& assets);
		// Main thread: as refresh, for the IDs `batch` touches only: the ID each of its paths held here and the one its
		// record carries now. Their holders are taken from `assets` as it is now, so a later batch that moved a path
		// again is answered as well. Copies the snapshot only when an answer changes (#866).
		bool apply_batch(const asset_database& assets, const asset_batch& batch);
		std::size_t full_refreshes() const { return m_full_refreshes; }

	private:
		struct data
		{
			bool case_insensitive = false;
			// By the merge key of the path (asset_layers): the spellings of one file share an entry.
			std::unordered_map<std::string, res::asset_id> ids;
			std::unordered_map<res::asset_id, res::tag> paths;

			bool operator==(const data&) const = default;
		};

		std::shared_ptr<const data> current() const;
		static std::string key_of(const res::tag& tag, bool case_insensitive);
		void warn_copy(const asset_record& copy, const asset_record& holder);

		mutable std::mutex m_mutex;
		std::shared_ptr<const data> m_data = std::make_shared<const data>();
		std::set<std::pair<res::asset_id, std::string>> m_warned;
		std::size_t m_full_refreshes = 0;
	};

	// Keeps resource_system's asset index in step with the asset database: attached once the database has walked the
	// tree, refreshed and handed on (apply_asset_batch) after every batch the database takes in, detached when the link
	// goes. Detached too when the database dropped changes waiting behind a walk (they could not decide anything), and
	// attached again after the next walk. Main thread.
	class asset_index_link
	{
	public:
		asset_index_link(asset_database& assets, res::resource_system& resources);
		~asset_index_link();
		asset_index_link(const asset_index_link&) = delete;
		asset_index_link& operator=(const asset_index_link&) = delete;

		// Once per frame: attaches the index after the first walk, and follows what changed outside a batch.
		void poll();
		bool attached() const { return m_attached; }
		const asset_index_snapshot& index() const { return *m_index; }

	private:
		void attach();
		void detach();
		// Detaches when the database dropped changes since the link last looked; true when attached afterwards.
		bool follow_drops();
		void apply(std::span<const std::pair<res::tag, res::tag>> moved);
		void on_batch(const asset_batch& batch);

		// A move handed on with `through`: a walk pairs a move of a batch it went ahead of, whose files wait for a later
		// number. Handed on again with every later batch until one with a higher number has decided them.
		struct handed_move
		{
			std::pair<res::tag, res::tag> move;
			std::uint64_t through = 0;
		};

		asset_database& m_assets;
		res::resource_system& m_resources;
		std::shared_ptr<asset_index_snapshot> m_index = std::make_shared<asset_index_snapshot>();
		bool m_attached = false;
		std::uint64_t m_revision = 0;
		std::vector<handed_move> m_moves;
		std::uint64_t m_drops_seen = 0;
		// Attached only after more walks than this have been applied.
		std::uint64_t m_attach_after = 0;
	};
}
