#pragma once
#include <snk/model_codec/res_tag.h>
#include <boost/json/object.hpp>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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
		// The refusal is a state of the project (it may not be written, the open level holds the name), not a wrong argument: the agents' `busy`.
		bool busy = false;

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
			// Why nothing may be written now (a project migration runs, or stopped half way and is not rolled back): create()
			// refuses with it. Null or nullopt: it runs.
			std::function<std::optional<std::string>()> blocker = {};
			// The editor's asset kinds whose suffix ends with ".desc", as {id, suffix}: a kind whose id is a document
			// type names the suffix of that type's files ("shader_desc" -> ".shader.desc", #1263). Null: every type
			// gets ".desc".
			std::function<std::vector<std::pair<std::string, std::string>>()> desc_kinds = {};
			// The first own fields a new document of a type gets from create(folder, type, name) (a texture: its
			// `__parent`, #1205). Null or empty: none.
			std::function<boost::json::object(std::string_view type)> initial_fields = {};
		};

		explicit document_creator(ports wired) : m_ports(std::move(wired)) {}

		// The desc types that are documents, by name.
		std::vector<std::string> types() const;
		// Why nothing may be written now (ports::blocker); nullopt: create() may write.
		std::optional<std::string> blocker() const { return m_ports.blocker ? m_ports.blocker() : std::nullopt; }

		// `name` ending with `suffix`: the longest of `suffix`, ".desc", one of `tails` or `suffix` without its
		// ".desc" that ends the name (in any letter case) is replaced. "a" -> "a.desc", "A.DESC" -> "A.desc"; with
		// ".shader.desc" and the tail ".prefab.desc": "a", "a.desc", "a.shader" and "a.prefab.desc" all give
		// "a.shader.desc".
		static std::string file_name(std::string_view name, std::string_view suffix = ".desc",
			std::span<const std::string> tails = {});

		// The suffix of the files of a document `type`: its asset kind's (ports::desc_kinds), else ".desc".
		std::string suffix_of(std::string_view type) const;
		// The file a document of `type` named `name` gets: any kind's .desc suffix the name ends with is replaced by
		// its own.
		std::string file_name_of(std::string_view type, std::string_view name) const;

		// Why `name` cannot be the name of a new document in `folder` (below res://, '/' separated, empty = the root): empty
		// when it can. A name taken in any layer is refused naming the layer, so a new file never hides one below it; so is a
		// name whose `.resource` is still there (a deleted file's label: its ID would pass to the new file), or a leftover binary
		// file of the desc (res::name_taken; letter case never tells names apart). Walks the folder in every layer: call it
		// when the name changes, not every frame.
		std::string check(std::string_view folder, std::string_view name) const;
		// The same for the file a document of `type` gets (file_name_of).
		std::string check(std::string_view folder, std::string_view type, std::string_view name) const;

		// Writes `{"__type", "__version"}` and the type's initial fields (ports::initial_fields), nothing else (the fields read
		// as the schema's defaults, the ID of the file is the asset database's). The file is made by create_sync: never over a
		// file, also one that appears meanwhile.
		document_creation create(std::string_view folder, std::string_view type, std::string_view name) const;
		// The same with the file's first own fields after `__type` and `__version` (a texture made for a picture: its
		// `__parent` and `data`, #1205) in place of the type's initial ones; a `__type` or `__version` among them is not taken.
		document_creation create(std::string_view folder, std::string_view type, std::string_view name,
			const boost::json::object& fields) const;

	private:
		std::string check_file(std::string_view folder, std::string_view name, const std::string& file,
			std::string_view suffix) const;
		std::vector<std::string> desc_suffixes() const;

		ports m_ports;
	};

	// What the "Create Document" window holds between frames, without ImGui: the folder, the type picked, the name typed
	// and what is wrong with them.
	class document_create_form
	{
	public:
		explicit document_create_form(const document_creator& creator) : m_creator(creator) {}

		void open(std::string folder);
		const std::string& folder() const { return m_folder; }

		const std::vector<std::string>& types() const { return m_types; }
		std::size_t type_index() const { return m_type_index; }
		// A new type checks the name again: the file name follows the type.
		void set_type_index(std::size_t index);

		const std::string& name() const { return m_name; }
		void set_name(std::string name);
		// The file the document gets (the name with the type's suffix); empty while nothing is typed.
		const std::string& file_name() const { return m_file; }
		// The name ended with another suffix that the type's replaced ("a.prefab.desc" -> "a.shader.desc"), so the
		// window shows both before anything is made. Taken with file_name() at the last change of the name or type.
		bool suffix_replaced() const { return m_replaced; }

		// What is wrong with the name as of the last change of it (the folders are walked then, not at every frame); empty while
		// nothing is typed yet.
		const std::string& problem() const { return m_problem; }
		bool can_submit() const { return !m_types.empty() && !m_name.empty() && m_problem.empty(); }
		// What the last submit said when it failed.
		const std::string& error() const { return m_error; }

		// Makes the document (the name is checked again, fresh); the tag comes back on success, else error() says why. A
		// file name that is no longer the one shown (the asset kinds changed meanwhile) is shown anew, nothing is made.
		std::optional<res::tag> submit();

	private:
		void recheck();

		const document_creator& m_creator;
		std::string m_folder;
		std::vector<std::string> m_types;
		std::size_t m_type_index = 0;
		std::string m_name;
		std::string m_file;
		bool m_replaced = false;
		std::string m_problem;
		std::string m_error;
	};
}
