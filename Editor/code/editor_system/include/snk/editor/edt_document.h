#pragma once
#include <snk/resource/res_tag.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace edt
{
	// 0 = a document that was never registered in a document_manager.
	using document_id = std::uint64_t;

	struct save_result
	{
		bool saved = false;
		std::string error;

		static save_result ok() { return { true, {} }; }
		static save_result failure(std::string message) { return { false, std::move(message) }; }
	};

	class document_manager;

	// Something the editor edits and can show a title, a dirty mark and a source for. document_manager owns the
	// documents and hands out their ids.
	class document
	{
	public:
		virtual ~document() = default;

		document_id id() const { return m_id; }
		virtual std::string_view kind() const = 0;
		virtual const std::string& title() const = 0;
		virtual const res::tag& source() const = 0;
		virtual bool is_dirty() const = 0;
		// Bumped on every change of what the document holds; caches of derived views key on it.
		virtual std::uint64_t generation() const = 0;

		virtual bool can_save() const { return false; }
		virtual save_result save() { return save_result::failure("the document cannot be saved"); }
		virtual save_result save_as(const res::tag&) { return save_result::failure("the document cannot be saved"); }

	private:
		friend class document_manager;
		document_id m_id = 0;
	};
}
