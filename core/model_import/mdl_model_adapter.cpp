#include <snk/model_import/mdl_model_adapter.h>
#include <snk/model_import/mdl_import.h>

#include <snk/common/eng_profiler.h>
#include <snk/common/engine_assert.h>
#include <snk/resource/res_model_reference.h>
#include <snk/resource/res_system.h>

#include <algorithm>
#include <atomic>
#include <format>
#include <list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace json = boost::json;

namespace {

struct written_file
{
	res::tag tag;
	std::vector<std::byte> bytes;
	// The import writes it only where it is not there yet: a file the caller has stays as it is.
	bool only_if_absent = false;
};

// What an import writes to memory://, kept so that the same bytes can be written again without running assimp (#977).
class recording_sink final : public mdl::import_sink
{
public:
	recording_sink(res::resource_system& resources, bool record) : m_resources(resources), m_record(record) {}

	// A file already there before this import is not written by it: such an import is not remembered, as writing what it
	// wrote again into another resource system would leave that file out.
	bool exists(const res::tag& tag) override
	{
		if (!m_record) return m_resources.exists(tag);
		m_checked.push_back(tag);
		const auto same = [&tag](const written_file& file) { return file.tag == tag; };
		if (std::find_if(m_files.begin(), m_files.end(), same) != m_files.end()) return true;
		const bool found = m_resources.exists(tag);
		m_found_before = m_found_before || found;
		return found;
	}

	void store(const res::tag& tag, const std::vector<std::byte>& bytes) override
	{
		m_resources.store(tag, bytes);
		if (!m_record) return;
		const bool checked = std::find(m_checked.begin(), m_checked.end(), tag) != m_checked.end();
		m_files.push_back({ tag, bytes, checked });
	}

	bool recording() const { return m_record; }
	bool found_before() const { return m_found_before; }
	std::vector<written_file>& files() { return m_files; }

private:
	res::resource_system& m_resources;
	const bool m_record;
	std::vector<written_file> m_files;
	std::vector<res::tag> m_checked;
	bool m_found_before = false;
};

// The model's own bytes from memory, the files it names from the resource system: beside it, in its protocol.
class resource_file_source final : public mdl::file_source
{
public:
	resource_file_source(res::resource_system& resources, const res::tag& model, const std::vector<std::byte>& data)
		: m_resources(resources), m_model(model), m_data(data)
	{
	}

	mdl::opened_file open(std::string_view name) override
	{
		const std::optional<res::tag> reference = res::model_reference_tag(m_model, name);
		if (!reference) {
			m_read_other_files = true;
			return { nullptr, "it is absolute, in another protocol or above its root" };
		}
		if (*reference == m_model) {
			return { &m_data, {} };
		}
		m_read_other_files = true;
		auto it = m_fetched.find(*reference);
		if (it == m_fetched.end()) {
			res::resource_system::data fetched = m_resources.fetch_data(*reference);
			if (!fetched) return {};
			it = m_fetched.emplace(*reference, std::move(fetched)).first;
		}
		return { &it->second->get(), {} };
	}

	bool exists(std::string_view) override { return true; }

	// Whether the model asked for any file besides its own bytes (#977): such an import is not remembered.
	bool read_other_files() const { return m_read_other_files; }

private:
	res::resource_system& m_resources;
	const res::tag& m_model;
	const std::vector<std::byte>& m_data;
	std::unordered_map<res::tag, res::resource_system::data> m_fetched;
	bool m_read_other_files = false;
};

struct remembered_import
{
	std::vector<std::byte> model;
	std::vector<written_file> files;
	// Not stamped: the desc system that asks stamps it (another one can have other versions).
	json::object prefab;

	std::size_t bytes() const
	{
		std::size_t total = model.size();
		for (const auto& file : files) total += file.bytes.size();
		return total;
	}
};

// Each unit test builds its own engine and imports the same models again: the memory is the tests' (#977).
// Counts the model and the files only, not the prefab JSON, so the memory held is somewhat more.
constexpr std::size_t k_remembered_bytes = 256u * 1024u * 1024u;

std::mutex g_imports_mutex;
// The most recently used first. Only immutable copies: a hit writes them into the resource system that asks.
std::list<std::pair<res::tag, std::shared_ptr<const remembered_import>>> g_imports;
std::size_t g_imports_bytes = 0;
std::atomic<std::size_t> g_assimp_runs{ 0 };
std::atomic<bool> g_remember_imports{ false };

auto find_import(const res::tag& tag)
{
	return std::find_if(g_imports.begin(), g_imports.end(), [&tag](const auto& entry) { return entry.first == tag; });
}

std::shared_ptr<const remembered_import> remembered(const res::tag& tag, const std::vector<std::byte>& model)
{
	if (!g_remember_imports.load()) return nullptr;
	const std::lock_guard guard(g_imports_mutex);
	const auto found = find_import(tag);
	if (found == g_imports.end() || found->second->model != model) return nullptr;
	g_imports.splice(g_imports.begin(), g_imports, found);
	return found->second;
}

void remember(const res::tag& tag, std::shared_ptr<const remembered_import> import)
{
	const std::lock_guard guard(g_imports_mutex);
	// Under the mutex: remember_model_imports(false) may have run since the import began.
	if (!g_remember_imports.load()) return;
	if (const auto old = find_import(tag); old != g_imports.end()) {
		g_imports_bytes -= old->second->bytes();
		g_imports.erase(old);
	}
	const std::size_t bytes = import->bytes();
	if (bytes > k_remembered_bytes) return;
	g_imports_bytes += bytes;
	g_imports.emplace_front(tag, std::move(import));
	while (g_imports_bytes > k_remembered_bytes) {
		g_imports_bytes -= g_imports.back().second->bytes();
		g_imports.pop_back();
	}
}

// The engine's names: everything beside the model's tag, the embedded textures in a folder of the model's own (#1112).
mdl::import_options options_of(const res::tag& tag, const res::tag& prefab_tag)
{
	const std::string base = std::format("{}{}", tag.path(), tag.pure_name());
	mdl::import_options options;
	options.geometry_tag = res::tag{ res::tag::memory, base + ".geom.desc" };
	options.prefab_tag = prefab_tag;
	options.embedded_folder = base + "__embedded_txm/";
	options.texture_folder = std::string{ tag.path() };
	options.materials = mdl::material_mode::inline_desc;
	options.textures = mdl::texture_mode::reference_beside_model;
	options.model_tag = tag;
	options.display_name = std::string{ tag.view() };
	return options;
}

}

void mdl::remember_model_imports(bool on)
{
	const std::lock_guard guard(g_imports_mutex);
	g_remember_imports = on;
	if (!on) {
		g_imports.clear();
		g_imports_bytes = 0;
	}
}

std::size_t mdl::model_importer_runs()
{
	return g_assimp_runs.load();
}

void mdl::register_model_adapter(res::resource_system& resources, desc::desc_system& descs)
{
	resources.registrate_adapter<model_adapter>(model_adapter::INFO, std::ref(descs), std::ref(resources));
}

mdl::model_adapter::model_adapter(desc::desc_system& descs, res::resource_system& resources)
	: m_descs(descs)
	, m_resources(resources)
{
}

std::shared_ptr<res::resource_entry> mdl::model_adapter::deserialize(const res::tag& tag, const std::vector<std::byte>& raw_data) const
{
	return operator()(tag, raw_data);
}

std::vector<std::byte> mdl::model_adapter::serialize(const res::tag&, const std::shared_ptr<res::resource_entry>&) const
{
	ASSERT_FAIL("Model importer adapter does not support serialization!");
	return {};
}

std::shared_ptr<desc::desc_base> mdl::model_adapter::operator()(const res::tag& tag, const std::vector<std::byte>& data) const
{
	PROFILE_SCOPE("LoadModel.Assimp");
	const res::tag actual_tag{ std::format("memory://{0}{1}.desc", tag.path(), tag.pure_name()) };
	json::object prefab;
	if (const std::shared_ptr<const remembered_import> import = remembered(tag, data)) {
		for (const written_file& file : import->files) {
			if (file.only_if_absent && m_resources.exists(file.tag)) continue;
			m_resources.store(file.tag, file.bytes);
		}
		prefab = import->prefab;
	}
	else {
		resource_file_source files(m_resources, tag, data);
		recording_sink sink(m_resources, g_remember_imports.load());
		import_result imported = import_model(tag.view(), files, sink, options_of(tag, actual_tag));
		++g_assimp_runs;
		if (!imported.error.empty()) {
			throw std::runtime_error("model '" + std::string{ tag.view() } + "': " + imported.error);
		}
		prefab = std::move(imported.prefab);
		// Side files can change while the model's bytes stay the same.
		if (sink.recording() && !sink.found_before() && !files.read_other_files()) {
			auto made = std::make_shared<remembered_import>();
			made->model = data;
			made->files = std::move(sink.files());
			made->prefab = prefab;
			remember(tag, std::move(made));
		}
	}

	m_descs.stamp_tree(prefab);
	auto instance = m_descs.create_instance("prefab_desc", actual_tag);
	instance->deserialize(m_descs, prefab);
	m_resources.store(actual_tag, m_descs.serialize_to_bytes(prefab));
	m_resources.register_alias(tag, actual_tag);
	return instance;
}
