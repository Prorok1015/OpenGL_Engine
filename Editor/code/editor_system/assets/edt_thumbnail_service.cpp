#include "edt_thumbnail_service.h"
#include <algorithm>
#include <utility>

edt::thumbnail_service::thumbnail_service(ports from)
	: thumbnail_service(std::move(from), limits{})
{
}

edt::thumbnail_service::thumbnail_service(ports from, limits with)
	: m_ports(std::move(from))
	, m_limits(with)
{
}

edt::thumbnail_service::~thumbnail_service()
{
	release_all();
	flush_graveyard();
}

void edt::thumbnail_service::flush_graveyard()
{
	for (std::string& key : m_graveyard) {
		if (m_ports.release) {
			m_ports.release(key);
		}
		m_free_keys.push_back(std::move(key));
	}
	m_graveyard.clear();
}

std::string edt::thumbnail_service::key_of(const std::filesystem::path& root, const std::filesystem::path& relative)
{
	return (root / relative).generic_string();
}

edt::thumbnail_service::view edt::thumbnail_service::view_of(const entry& e) const
{
	view out;
	switch (e.state) {
	case phase::ready:
		out.state = view::state_t::ready;
		out.texture = e.texture;
		out.width = e.width;
		out.height = e.height;
		break;
	case phase::failed:
		out.state = view::state_t::failed;
		out.error = e.error;
		break;
	default:
		out.state = view::state_t::pending;
		break;
	}
	return out;
}

void edt::thumbnail_service::drop_texture(entry& e)
{
	if (!e.texture_key.empty()) {
		m_graveyard.push_back(std::move(e.texture_key));
		if (m_textures > 0 && e.texture != 0) {
			--m_textures;
		}
	}
	e.texture = 0;
	e.texture_key.clear();
	e.pixels.reset();
}

void edt::thumbnail_service::queue(const std::string& key, entry& e)
{
	drop_texture(e);
	e.state = phase::queued;
	e.error.clear();
	e.request = ++m_next_request;
	m_waiting.push_back(key);
}

void edt::thumbnail_service::evict_over_limit(const std::string& keep)
{
	while (m_entries.size() > m_limits.max_entries) {
		auto oldest = m_entries.end();
		for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
			if (it->first != keep && (oldest == m_entries.end() || it->second.used_tick < oldest->second.used_tick)) {
				oldest = it;
			}
		}
		if (oldest == m_entries.end()) {
			return;
		}
		drop_texture(oldest->second);
		m_entries.erase(oldest);
	}
}

edt::thumbnail_service::view edt::thumbnail_service::lookup(const std::filesystem::path& root, const std::filesystem::path& relative)
{
	const std::string key = key_of(root, relative);
	const double now = m_ports.now ? m_ports.now() : 0.0;
	auto found = m_entries.find(key);
	if (found == m_entries.end()) {
		entry fresh;
		fresh.root = root;
		fresh.relative = relative;
		fresh.stamp = m_ports.stat(root, relative);
		fresh.checked_at = now;
		fresh.used_tick = m_tick;
		found = m_entries.emplace(key, std::move(fresh)).first;
		if (found->second.stamp) {
			queue(key, found->second);
		} else {
			found->second.state = phase::failed;
			found->second.error = "it is not a regular file";
		}
		evict_over_limit(key);
		found = m_entries.find(key);
		return view_of(found->second);
	}

	entry& e = found->second;
	e.used_tick = m_tick;
	if (now - e.checked_at >= m_limits.restamp_seconds) {
		e.checked_at = now;
		const std::optional<desc_type_cache::stamp> current = m_ports.stat(root, relative);
		if (current != e.stamp) {
			e.stamp = current;
			if (current) {
				queue(key, e);
			} else {
				drop_texture(e);
				e.state = phase::failed;
				e.error = "it is not a regular file";
				e.request = ++m_next_request;
			}
		}
	}
	return view_of(e);
}

void edt::thumbnail_service::finish(const std::string& key, std::uint64_t request, thumbnail_decoded decoded)
{
	--m_running;
	const auto found = m_entries.find(key);
	if (found == m_entries.end() || found->second.request != request || found->second.state != phase::decoding) {
		return;
	}
	entry& e = found->second;
	if (decoded.pixels) {
		e.pixels = std::move(decoded.pixels);
		e.state = phase::uploading;
		m_upload_queue.push_back(key);
	} else {
		e.state = phase::failed;
		e.error = decoded.error.empty() ? std::string{ "it cannot be decoded" } : std::move(decoded.error);
	}
}

void edt::thumbnail_service::tick()
{
	++m_tick;
	flush_graveyard();

	int uploads = m_limits.uploads_per_frame;
	std::vector<std::string> later;
	for (std::string& key : m_upload_queue) {
		const auto found = m_entries.find(key);
		if (found == m_entries.end() || found->second.state != phase::uploading || !found->second.pixels) {
			continue;
		}
		if (uploads <= 0) {
			later.push_back(std::move(key));
			continue;
		}
		--uploads;
		entry& e = found->second;
		// A key given back is used again: the texture manager keeps a slot for every tag it ever saw.
		std::string texture_key;
		if (!m_free_keys.empty()) {
			texture_key = std::move(m_free_keys.back());
			m_free_keys.pop_back();
		} else {
			texture_key = "memory://editor/thumbs/" + std::to_string(++m_next_texture);
		}
		const std::uint64_t id = m_ports.upload ? m_ports.upload(texture_key, *e.pixels) : 0;
		if (id == 0) {
			m_free_keys.push_back(std::move(texture_key));
			e.state = phase::failed;
			e.error = "the picture could not be put on the GPU";
		} else {
			e.state = phase::ready;
			e.texture = id;
			e.texture_key = texture_key;
			e.width = e.pixels->width;
			e.height = e.pixels->height;
			++m_textures;
		}
		e.pixels.reset();
	}
	m_upload_queue = std::move(later);

	std::vector<std::string> still_waiting;
	for (std::string& key : m_waiting) {
		const auto found = m_entries.find(key);
		if (found == m_entries.end() || found->second.state != phase::queued) {
			continue;
		}
		entry& e = found->second;
		// Not asked for in the frame before: its row is off the screen, so the decode is not worth a worker.
		if (e.used_tick + 1 < m_tick) {
			m_entries.erase(found);
			continue;
		}
		if (m_running >= m_limits.max_running) {
			still_waiting.push_back(std::move(key));
			continue;
		}
		e.state = phase::decoding;
		++m_running;
		++m_decodes_started;
		const std::uint64_t request = e.request;
		m_ports.dispatch(
			[decode = m_ports.decode, root = e.root, relative = e.relative]() { return decode(root, relative); },
			[this, key, request](thumbnail_decoded decoded) { finish(key, request, std::move(decoded)); });
	}
	m_waiting = std::move(still_waiting);
}

void edt::thumbnail_service::release_all()
{
	for (auto& [key, e] : m_entries) {
		drop_texture(e);
	}
	m_entries.clear();
	m_waiting.clear();
	m_upload_queue.clear();
}
