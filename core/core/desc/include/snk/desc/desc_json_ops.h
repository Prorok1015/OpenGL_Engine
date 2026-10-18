#pragma once
#include <boost/json.hpp>
#include <charconv>
#include <string>
#include <string_view>
#include <vector>

// Value-level JSON operations behind override records: recursive merge and RFC 6901 pointer writes.
namespace desc
{
	// Objects merge key by key, recursively; arrays and scalars replace whole.
	inline void json_deep_merge(boost::json::object& dst, const boost::json::object& src)
	{
		for (const auto& member : src) {
			auto it = dst.find(member.key());
			if (it != dst.end() && it->value().is_object() && member.value().is_object()) {
				json_deep_merge(it->value().as_object(), member.value().as_object());
			} else {
				dst.insert_or_assign(member.key(), member.value());
			}
		}
	}

	// "/a~1b/0" -> { "a/b", "0" }. An empty pointer is the document itself; a pointer that does not start
	// with '/' is malformed.
	inline bool json_pointer_tokens(std::string_view pointer, std::vector<std::string>& tokens)
	{
		tokens.clear();
		if (pointer.empty()) {
			return true;
		}
		if (pointer.front() != '/') {
			return false;
		}
		pointer.remove_prefix(1);
		while (true) {
			const size_t slash = pointer.find('/');
			std::string_view raw = pointer.substr(0, slash);
			std::string token;
			token.reserve(raw.size());
			for (size_t i = 0; i < raw.size(); ++i) {
				if (raw[i] == '~' && i + 1 < raw.size() && raw[i + 1] == '1') {
					token.push_back('/');
					++i;
				} else if (raw[i] == '~' && i + 1 < raw.size() && raw[i + 1] == '0') {
					token.push_back('~');
					++i;
				} else {
					token.push_back(raw[i]);
				}
			}
			tokens.push_back(std::move(token));
			if (slash == std::string_view::npos) {
				break;
			}
			pointer.remove_prefix(slash + 1);
		}
		return true;
	}

	inline bool json_array_index(const std::string& token, size_t size, size_t& index)
	{
		if (token == "-") {
			index = size;
			return true;
		}
		size_t value = 0;
		auto [end, ec] = std::from_chars(token.data(), token.data() + token.size(), value);
		if (ec != std::errc{} || end != token.data() + token.size() || value > size) {
			return false;
		}
		index = value;
		return true;
	}

	// Replaces the value at `pointer` (missing objects on the way are created). The empty pointer replaces
	// `root` itself. False if the pointer is malformed or runs through a scalar or past the end of an array.
	inline bool json_pointer_set(boost::json::value& root, std::string_view pointer, boost::json::value value)
	{
		std::vector<std::string> tokens;
		if (!json_pointer_tokens(pointer, tokens)) {
			return false;
		}
		boost::json::value* node = &root;
		for (size_t i = 0; i < tokens.size(); ++i) {
			const bool last = i + 1 == tokens.size();
			if (node->is_null()) {
				node->emplace_object();
			}
			if (node->is_object()) {
				auto& obj = node->as_object();
				if (last) {
					obj.insert_or_assign(tokens[i], std::move(value));
					return true;
				}
				auto it = obj.find(tokens[i]);
				if (it == obj.end()) {
					it = obj.emplace(tokens[i], boost::json::value{}).first;
				}
				node = &it->value();
			} else if (node->is_array()) {
				auto& arr = node->as_array();
				size_t index = 0;
				if (!json_array_index(tokens[i], arr.size(), index)) {
					return false;
				}
				if (last) {
					if (index == arr.size()) {
						arr.push_back(std::move(value));
					} else {
						arr[index] = std::move(value);
					}
					return true;
				}
				if (index == arr.size()) {
					return false;
				}
				node = &arr[index];
			} else {
				return false;
			}
		}
		root = std::move(value);
		return true;
	}

	inline const boost::json::value* json_pointer_get(const boost::json::value& root, std::string_view pointer)
	{
		std::vector<std::string> tokens;
		if (!json_pointer_tokens(pointer, tokens)) {
			return nullptr;
		}
		const boost::json::value* node = &root;
		for (const auto& token : tokens) {
			if (node->is_object()) {
				node = node->as_object().if_contains(token);
			} else if (node->is_array()) {
				size_t index = 0;
				if (!json_array_index(token, node->as_array().size(), index) || index >= node->as_array().size()) {
					return nullptr;
				}
				node = &node->as_array()[index];
			} else {
				return nullptr;
			}
			if (!node) {
				return nullptr;
			}
		}
		return node;
	}

	// Removes the value at `pointer`; true if something was removed.
	inline bool json_pointer_remove(boost::json::value& root, std::string_view pointer)
	{
		std::vector<std::string> tokens;
		if (!json_pointer_tokens(pointer, tokens) || tokens.empty()) {
			return false;
		}
		boost::json::value* node = &root;
		for (size_t i = 0; i + 1 < tokens.size(); ++i) {
			if (node->is_object()) {
				node = node->as_object().if_contains(tokens[i]);
			} else if (node->is_array()) {
				size_t index = 0;
				if (!json_array_index(tokens[i], node->as_array().size(), index) || index >= node->as_array().size()) {
					return false;
				}
				node = &node->as_array()[index];
			} else {
				return false;
			}
			if (!node) {
				return false;
			}
		}
		if (node->is_object()) {
			return node->as_object().erase(tokens.back()) > 0;
		}
		if (node->is_array()) {
			size_t index = 0;
			if (!json_array_index(tokens.back(), node->as_array().size(), index) || index >= node->as_array().size()) {
				return false;
			}
			node->as_array().erase(node->as_array().begin() + index);
			return true;
		}
		return false;
	}
}
