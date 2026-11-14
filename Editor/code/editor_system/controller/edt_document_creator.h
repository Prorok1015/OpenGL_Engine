#pragma once
#include <snk/resource/res_tag.h>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace desc { class desc_system; }
namespace res { class resource_system; }

// A new data document made from the asset browser (EPIC-56 section 3.5, S56-10): the type is one of the desc types marked as
// documents, the file is new (never over a file of any layer) and lands in the project's own top layer.
namespace edt
{
	struct document_creation
	{
		res::tag tag;
		std::string error;

		bool ok() const { return error.empty(); }
	};

	class document_creator
	{
	public:
		struct ports
		{
			std::function<desc::desc_system*()> descs;
			// Where the file is written: the base resource system (a game layer only writes under its module's memory:// folder).
			std::function<res::resource_system*()> resources;
			// The folders behind res://, lowest layer first.
			std::function<std::vector<std::filesystem::path>()> layers;
		};

		explicit document_creator(ports wired) : m_ports(std::move(wired)) {}

		// The desc types that are documents, by name.
		std::vector<std::string> types() const;

		// "a" -> "a.desc"; a name that has the extension (any letter case) is left alone.
		static std::string file_name(std::string_view name);

		// Why `name` cannot be the name of a new document in `folder` (below res://, '/' separated, empty = the root): empty
		// when it can. A name taken in any layer is refused naming the layer, so a new file never hides one below it.
		std::string check(std::string_view folder, std::string_view name) const;

		// Writes `{"__type", "__version"}` and nothing else (the fields read as the schema's defaults, the ID of the file is
		// the asset database's). The file is made by create_sync: never over a file, also one that appears meanwhile.
		document_creation create(std::string_view folder, std::string_view type, std::string_view name) const;

	private:
		ports m_ports;
	};

	// What the "Create Document" window holds between frames, without ImGui: the folder, the type picked, the name typed
	// and what is wrong with them.
	class document_create_form
	{
	public:
		explicit document_create_form(const document_creator& creator) : m_creator(creator) {}

		void open(std::string folder);
		void close() { m_open = false; }
		bool is_open() const { return m_open; }
		const std::string& folder() const { return m_folder; }

		const std::vector<std::string>& types() const { return m_types; }
		std::size_t type_index() const { return m_type_index; }
		void set_type_index(std::size_t index) { m_type_index = index < m_types.size() ? index : m_type_index; }

		const std::string& name() const { return m_name; }
		void set_name(std::string name);

		// What is wrong with the name; empty while nothing is typed yet.
		std::string problem() const;
		bool can_submit() const { return !m_types.empty() && !m_name.empty() && problem().empty(); }
		// What the last submit said when it failed.
		const std::string& error() const { return m_error; }

		// Makes the document; on success the form closes and the tag comes back.
		std::optional<res::tag> submit();

	private:
		const document_creator& m_creator;
		bool m_open = false;
		std::string m_folder;
		std::vector<std::string> m_types;
		std::size_t m_type_index = 0;
		std::string m_name;
		std::string m_error;
	};
}
