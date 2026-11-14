#include "edt_schema_inspector.h"
#include "edt_override_edit.h"
#include "edt_reference.h"
#include <snk/common/logger/engine_log.h>
#include <snk/desc/desc_json_ops.h>
#include <snk/desc/desc_schema_ops.h>
#include <snk/resource/res_tag.h>
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace json = boost::json;

namespace
{
	using desc::field_kind;
	using edt::schema_ui::context;
	using edt::schema_ui::widget;
	using edt::schema_ui::widget_spec;

	constexpr int max_depth = 8;
	constexpr std::size_t max_listed = 50;

	json::value zeros(std::size_t count)
	{
		json::array components;
		for (std::size_t i = 0; i < count; ++i) {
			components.push_back(0.0);
		}
		return json::value(std::move(components));
	}

	json::value defaults_impl(const desc::type_schema& schema, const desc::schema_registry& registry, int depth, bool fill_missing);

	// A null under a map key is an override that removes the key a parent has (the read of a null takes it out): what is
	// shown has no such key.
	void drop_null_keys(const desc::type_schema& schema, const desc::schema_registry& registry, json::value& object, int depth)
	{
		if (!object.is_object() || depth >= max_depth) {
			return;
		}
		for (const auto& field : schema.fields) {
			json::value* slot = const_cast<json::value*>(desc::json_pointer_get(object, field.key));
			if (!slot) {
				continue;
			}
			// A null for an optional field is "not set": shown as absent, with its "Set", where the null is a value of the field;
			// elsewhere the read puts the default (or the zero of the kind) in its place, and that is what is shown (#861).
			if (field.optional && slot->is_null()) {
				json::value read = edt::schema_ui::null_reads_as(field, registry);
				if (read.is_null()) {
					desc::json_pointer_remove(object, field.key);
				} else {
					*slot = std::move(read);
				}
				continue;
			}
			if (field.kind == field_kind::map && slot->is_object()) {
				json::object& entries = slot->as_object();
				for (auto it = entries.begin(); it != entries.end();) {
					it = it->value().is_null() ? entries.erase(it) : std::next(it);
				}
			} else if (field.kind == field_kind::structure) {
				if (const desc::type_schema* nested = field.ref_type.empty() ? nullptr : registry.find(field.ref_type)) {
					drop_null_keys(*nested, registry, *slot, depth + 1);
				}
			}
		}
	}

	json::value zero_of(field_kind kind, std::string_view ref_type, const desc::schema_registry& registry, int depth, bool fill_missing = true)
	{
		switch (kind) {
		case field_kind::boolean: return json::value(false);
		case field_kind::i32:
		case field_kind::i64: return json::value(std::int64_t{ 0 });
		case field_kind::u32:
		case field_kind::u64: return json::value(std::uint64_t{ 0 });
		case field_kind::f32:
		case field_kind::f64: return json::value(0.0);
		case field_kind::vec2: return zeros(2);
		case field_kind::vec3: return zeros(3);
		case field_kind::vec4: return zeros(4);
		case field_kind::quat:
		case field_kind::color: {
			json::value value = zeros(4);
			value.as_array()[3] = 1.0;
			return value;
		}
		case field_kind::string:
		case field_kind::res_tag:
		case field_kind::desc_ref:
		case field_kind::node_ref: return json::value(json::string());
		case field_kind::enumeration: {
			const desc::enum_schema* enumeration = ref_type.empty() ? nullptr : registry.find_enum(ref_type);
			if (enumeration && !enumeration->items.empty()) {
				return json::value(json::string(enumeration->items.front().name));
			}
			return json::value(json::string());
		}
		case field_kind::structure: {
			const desc::type_schema* nested = ref_type.empty() ? nullptr : registry.find(ref_type);
			if (nested && depth < max_depth) {
				return defaults_impl(*nested, registry, depth + 1, fill_missing);
			}
			return json::value(json::object());
		}
		case field_kind::array: return json::value(json::array());
		case field_kind::map: return json::value(json::object());
		case field_kind::entity:
		case field_kind::custom: break;
		}
		return json::value();
	}

	json::value defaults_impl(const desc::type_schema& schema, const desc::schema_registry& registry, int depth, bool fill_missing)
	{
		json::value out(json::object{});
		for (const auto& field : schema.fields) {
			// What a missing value reads as is shown (#861); only a std::optional member with no initializer reads it as
			// nullopt, "not set".
			if (field.optional && field.nullable && field.default_value.is_null()) {
				continue;
			}
			json::value value = field.default_value;
			if (value.is_null() && (fill_missing || field.kind == field_kind::structure)) {
				value = zero_of(field.kind, field.ref_type, registry, depth, fill_missing);
			}
			// Without fill_missing a field that has no default stays absent: nothing is inherited for it.
			if (value.is_null() || (!fill_missing && value.is_object() && value.as_object().empty())) {
				continue;
			}
			desc::json_pointer_set(out, field.key, std::move(value));
		}
		return out;
	}

	// A legacy key ("/texture") fills the field's own key when that one is missing.
	void resolve_aliases(const desc::type_schema& schema, json::value& object)
	{
		for (const auto& field : schema.fields) {
			if (field.read_aliases.empty() || desc::json_pointer_get(object, field.key)) {
				continue;
			}
			for (const auto& alias : field.read_aliases) {
				if (const json::value* found = desc::json_pointer_get(object, alias)) {
					json::value copy = *found;
					desc::json_pointer_set(object, field.key, std::move(copy));
					break;
				}
			}
		}
	}

	std::string join_pointer(const std::vector<std::string>& tokens, std::size_t count)
	{
		std::string out;
		for (std::size_t i = 0; i < count; ++i) {
			out += '/';
			for (const char c : tokens[i]) {
				if (c == '~') {
					out += "~0";
				} else if (c == '/') {
					out += "~1";
				} else {
					out += c;
				}
			}
		}
		return out;
	}

	// Removing "/a/b" leaves an empty "a" behind; the override would then name an object that has nothing in it.
	void prune_empty_parents(json::value& root, const std::string& pointer)
	{
		std::vector<std::string> tokens;
		if (!desc::json_pointer_tokens(pointer, tokens)) {
			return;
		}
		for (std::size_t depth = tokens.size(); depth > 1; --depth) {
			const std::string parent = join_pointer(tokens, depth - 1);
			const json::value* node = desc::json_pointer_get(root, parent);
			if (!node || !node->is_object() || !node->as_object().empty()) {
				return;
			}
			desc::json_pointer_remove(root, parent);
		}
	}

	bool same_value(const json::value& a, const json::value& b)
	{
		if (a.is_number() && b.is_number()) {
			const double x = a.to_number<double>();
			const double y = b.to_number<double>();
			return x == y || std::fabs(x - y) <= desc::float_relative_tolerance * std::max(std::fabs(x), std::fabs(y));
		}
		if (a.is_array() && b.is_array()) {
			const json::array& left = a.as_array();
			const json::array& right = b.as_array();
			if (left.size() != right.size()) {
				return false;
			}
			for (std::size_t i = 0; i < left.size(); ++i) {
				if (!same_value(left[i], right[i])) {
					return false;
				}
			}
			return true;
		}
		if (a.is_object() && b.is_object()) {
			const json::object& left = a.as_object();
			const json::object& right = b.as_object();
			if (left.size() != right.size()) {
				return false;
			}
			for (const auto& member : left) {
				const json::value* other = right.if_contains(member.key());
				if (!other || !same_value(member.value(), *other)) {
					return false;
				}
			}
			return true;
		}
		return a == b;
	}

	bool all_numbers(const json::array& array)
	{
		return std::ranges::all_of(array, [](const json::value& item) { return item.is_number(); });
	}

	std::size_t vector_size(field_kind kind)
	{
		switch (kind) {
		case field_kind::vec2: return 2;
		case field_kind::vec3: return 3;
		default: return 4;
		}
	}

	// Whether the JSON value has the shape the widget reads; anything else goes to the JSON editor untouched.
	bool suits(const desc::field_schema& field, widget kind, const json::value& value)
	{
		switch (kind) {
		case widget::checkbox: return value.is_bool();
		case widget::integer:
		case widget::real: return value.is_number();
		case widget::vector: return value.is_array() && value.as_array().size() == vector_size(field.kind) && all_numbers(value.as_array());
		case widget::color: return value.is_array() && (value.as_array().size() == 3 || value.as_array().size() == 4) && all_numbers(value.as_array());
		case widget::combo: return value.is_string() || value.is_number();
		case widget::text: return value.is_string();
		// A tag, or a {guid, path} reference (EPIC-57 section 4.4): the field shows and edits its path.
		case widget::asset: return value.is_string() || (edt::is_reference_object(value) && edt::reference_object_problem(value.as_object()).empty());
		case widget::group: return value.is_object();
		case widget::list: return value.is_array();
		case widget::map: return value.is_object();
		case widget::hidden:
		case widget::label:
		case widget::json: return true;
		}
		return true;
	}

	bool matches_filter(std::string_view path, std::string_view filter)
	{
		if (filter.empty()) {
			return true;
		}
		std::size_t start = 0;
		while (start <= filter.size()) {
			std::size_t end = filter.find_first_of(",;| ", start);
			if (end == std::string_view::npos) {
				end = filter.size();
			}
			std::string_view token = filter.substr(start, end - start);
			while (!token.empty() && (token.front() == '*' || token.front() == '.')) {
				token.remove_prefix(1);
			}
			if (!token.empty() && path.size() > token.size() && path[path.size() - token.size() - 1] == '.') {
				const std::string_view tail = path.substr(path.size() - token.size());
				const bool same = std::ranges::equal(tail, token, [](char a, char b) {
					return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
				});
				if (same) {
					return true;
				}
			}
			start = end + 1;
		}
		return false;
	}

	const desc::enum_schema* enum_of(const desc::field_schema& field, const desc::schema_registry& registry)
	{
		return field.kind == field_kind::enumeration && !field.ref_type.empty() ? registry.find_enum(field.ref_type) : nullptr;
	}

	const desc::type_schema* nested_of(const desc::field_schema& field, const desc::schema_registry& registry)
	{
		return field.kind == field_kind::structure && !field.ref_type.empty() ? registry.find(field.ref_type) : nullptr;
	}

	void show_tooltip(const desc::field_schema& field, const widget_spec& spec)
	{
		std::string text = field.tooltip;
		if (spec.kind == widget::asset && !field.filter.empty()) {
			text += text.empty() ? "Accepts: " : "\nAccepts: ";
			text += field.filter;
		}
		if (!text.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
			ImGui::SetTooltip("%s", text.c_str());
		}
	}

	// The field a JSON pointer from the root names, through the structures it passes (a nested field's key is relative to its
	// structure): what its initializer says decides whether a removal still has to be written.
	const desc::field_schema* field_at(const desc::type_schema& schema, const desc::schema_registry& registry, std::string_view pointer,
		std::string_view prefix = {}, int depth = 0)
	{
		if (depth > max_depth) {
			return nullptr;
		}
		for (const auto& field : schema.fields) {
			const std::string full = std::string(prefix) + field.key;
			if (full == pointer) {
				return &field;
			}
			if (pointer.size() > full.size() && pointer.starts_with(full) && pointer[full.size()] == '/') {
				if (const desc::type_schema* nested = nested_of(field, registry)) {
					if (const desc::field_schema* found = field_at(*nested, registry, pointer, full, depth + 1)) {
						return found;
					}
				}
			}
		}
		return nullptr;
	}

	bool draw_field(const desc::field_schema& field, json::value& value, const std::string& pointer, const context& ctx, int depth);

	// Tells the test hook where the item just drawn is on the screen.
	void report_target(const context& ctx, const std::string& target)
	{
		if (ctx.on_target) {
			const ImVec2 min = ImGui::GetItemRectMin();
			const ImVec2 max = ImGui::GetItemRectMax();
			ctx.on_target(target, (min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
		}
	}

	// The mark in front of a field (S56-7): a star when the own data sets it, a quiet dot otherwise, the source in its tooltip.
	// A click opens its menu: Reset takes the record out (the field is inherited or default again), Unset (a field that
	// can be absent, now present) takes the value out; the person's choice is queued in the origin and applied with the edit.
	void draw_origin_mark(const context& ctx, const desc::field_schema& field, const std::string& pointer, const json::value* shown)
	{
		const edt::schema_ui::field_origin* origin = ctx.origin;
		if (!origin) {
			return;
		}
		const bool here = edt::schema_ui::set_here(*origin, pointer);
		// A read-only field is the code's: there is nothing to take out of the file or to take away.
		const bool can_reset = here && !field.read_only;
		const bool offers_unset = field.optional && shown && !field.read_only;
		// Where a null reads as the default, Unset of a value that is the default already would only write that null.
		const bool can_unset = offers_unset && edt::schema_ui::unset_changes_value(field, *shown, ctx.registry);
		if (here) {
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{ 0.45f, 0.75f, 1.0f, 1.0f });
		}
		bool clicked = false;
		if (can_reset || offers_unset) {
			clicked = ImGui::SmallButton(here ? "*##mark" : ".##mark");
		} else {
			ImGui::TextDisabled("%s", here ? "*" : ".");
		}
		if (here) {
			ImGui::PopStyleColor();
		}
		report_target(ctx, pointer + "#mark");
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
			ImGui::SetTooltip("%s", edt::schema_ui::source_text(*origin, pointer).c_str());
		}
		if (clicked) {
			ImGui::OpenPopup("mark_menu");
		}
		if (ImGui::BeginPopup("mark_menu")) {
			if (can_reset) {
				if (ImGui::MenuItem("Reset to inherited")) {
					origin->requests.resets.push_back(pointer);
				}
				report_target(ctx, pointer + "#reset");
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Take the value set here out: the parent's value, or the schema's default, comes back.");
				}
			}
			if (offers_unset) {
				if (ImGui::MenuItem("Unset", nullptr, false, can_unset)) {
					origin->requests.unsets.push_back(pointer);
				}
				report_target(ctx, pointer + "#unset");
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
					ImGui::SetTooltip("%s", can_unset ? "Make the field empty here, even when the parent sets a value."
						: "Here an empty value reads as the schema's default, which the field has already: Unset changes nothing.");
				}
			}
			ImGui::EndPopup();
		}
		ImGui::SameLine(0.0f, 4.0f);
	}

	bool draw_object(const desc::type_schema& schema, json::value& object, const std::string& prefix, const context& ctx, int depth)
	{
		if (!object.is_object()) {
			return false;
		}
		bool changed = false;
		for (const auto& field : schema.fields) {
			if (field.editor_hidden || field.write == desc::write_policy::never) {
				continue;
			}
			const std::string pointer = prefix + field.key;
			ImGui::PushID(field.key.c_str());
			json::value* slot = const_cast<json::value*>(desc::json_pointer_get(object, field.key));
			if (slot) {
				changed |= draw_field(field, *slot, pointer, ctx, depth);
			} else {
				// Only optional fields (and custom ones without a default) can be missing: they are shown as not set.
				const std::string label = edt::schema_ui::display_label(field);
				draw_origin_mark(ctx, field, pointer, nullptr);
				ImGui::TextDisabled("%s: (not set)", label.c_str());
				if (ctx.on_field) {
					ctx.on_field(pointer, widget::label);
				}
				json::value initial = field.default_value;
				if (initial.is_null()) {
					initial = edt::schema_ui::zero_value(field, ctx.registry);
				}
				if (!initial.is_null() && !field.read_only) {
					ImGui::SameLine();
					if (ImGui::SmallButton("Set")) {
						desc::json_pointer_set(object, field.key, std::move(initial));
						changed = true;
					}
				}
			}
			ImGui::PopID();
		}
		return changed;
	}

	bool edit_bool(const char* label, json::value& value)
	{
		bool flag = value.as_bool();
		if (ImGui::Checkbox(label, &flag)) {
			value = flag;
			return true;
		}
		return false;
	}

	bool edit_integer(const char* label, const desc::field_schema& field, const widget_spec& spec, json::value& value)
	{
		const float speed = spec.speed > 0.0 ? static_cast<float>(spec.speed) : 1.0f;
		const bool is_signed = field.kind == field_kind::i32 || field.kind == field_kind::i64;
		if (is_signed) {
			std::int64_t current = 0;
			if (!desc::json_to_int64(value, current)) {
				return false;
			}
			std::int64_t low = field.kind == field_kind::i32 ? std::numeric_limits<std::int32_t>::min() : std::numeric_limits<std::int64_t>::min();
			std::int64_t high = field.kind == field_kind::i32 ? std::numeric_limits<std::int32_t>::max() : std::numeric_limits<std::int64_t>::max();
			if (spec.bounded) {
				low = std::max(low, static_cast<std::int64_t>(spec.min));
				high = std::min(high, static_cast<std::int64_t>(spec.max));
			}
			if (ImGui::DragScalar(label, ImGuiDataType_S64, &current, speed, &low, &high)) {
				value = current;
				return true;
			}
			return false;
		}
		std::uint64_t current = 0;
		if (!desc::json_to_uint64(value, current)) {
			return false;
		}
		std::uint64_t low = 0;
		std::uint64_t high = field.kind == field_kind::u32 ? std::numeric_limits<std::uint32_t>::max() : std::numeric_limits<std::uint64_t>::max();
		if (spec.bounded) {
			low = std::max(low, static_cast<std::uint64_t>(std::max(spec.min, 0.0)));
			high = std::min(high, static_cast<std::uint64_t>(std::max(spec.max, 0.0)));
		}
		if (ImGui::DragScalar(label, ImGuiDataType_U64, &current, speed, &low, &high)) {
			value = current;
			return true;
		}
		return false;
	}

	float drag_speed(const widget_spec& spec)
	{
		return spec.speed > 0.0 ? static_cast<float>(spec.speed) : 0.01f;
	}

	const char* number_format(const widget_spec& spec)
	{
		return spec.format.empty() ? "%.3f" : spec.format.c_str();
	}

	bool edit_real(const char* label, const desc::field_schema& field, const widget_spec& spec, json::value& value)
	{
		double current = 0.0;
		if (!desc::json_to_double(value, current)) {
			return false;
		}
		if (field.kind == field_kind::f64) {
			double low = spec.bounded ? spec.min : 0.0;
			double high = spec.bounded ? spec.max : 0.0;
			if (ImGui::DragScalar(label, ImGuiDataType_Double, &current, drag_speed(spec), &low, &high, number_format(spec))) {
				value = current;
				return true;
			}
			return false;
		}
		float single = static_cast<float>(current);
		const float low = spec.bounded ? static_cast<float>(spec.min) : 0.0f;
		const float high = spec.bounded ? static_cast<float>(spec.max) : 0.0f;
		if (ImGui::DragFloat(label, &single, drag_speed(spec), low, high, number_format(spec))) {
			value = static_cast<double>(single);
			return true;
		}
		return false;
	}

	bool edit_vector(const char* label, const widget_spec& spec, json::value& value)
	{
		json::array& array = value.as_array();
		float components[4] = {};
		const std::size_t count = std::min<std::size_t>(array.size(), 4);
		for (std::size_t i = 0; i < count; ++i) {
			double number = 0.0;
			desc::json_to_double(array[i], number);
			components[i] = static_cast<float>(number);
		}
		const float low = spec.bounded ? static_cast<float>(spec.min) : 0.0f;
		const float high = spec.bounded ? static_cast<float>(spec.max) : 0.0f;
		bool changed = false;
		if (count == 2) {
			changed = ImGui::DragFloat2(label, components, drag_speed(spec), low, high, number_format(spec));
		} else if (count == 3) {
			changed = ImGui::DragFloat3(label, components, drag_speed(spec), low, high, number_format(spec));
		} else {
			changed = ImGui::DragFloat4(label, components, drag_speed(spec), low, high, number_format(spec));
		}
		if (changed) {
			for (std::size_t i = 0; i < count; ++i) {
				array[i] = static_cast<double>(components[i]);
			}
		}
		return changed;
	}

	bool edit_color(const char* label, json::value& value)
	{
		json::array& array = value.as_array();
		float components[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		const std::size_t count = std::min<std::size_t>(array.size(), 4);
		for (std::size_t i = 0; i < count; ++i) {
			double number = 0.0;
			desc::json_to_double(array[i], number);
			components[i] = static_cast<float>(number);
		}
		const bool changed = count == 3 ? ImGui::ColorEdit3(label, components) : ImGui::ColorEdit4(label, components);
		if (changed) {
			for (std::size_t i = 0; i < count; ++i) {
				array[i] = static_cast<double>(components[i]);
			}
		}
		return changed;
	}

	// `null_form`: the field writes its null-mapped enumerator (enum_schema::null_value) as an explicit JSON null, so
	// a null reads as that enumerator and picking it writes a null again.
	bool edit_combo(const char* label, const desc::enum_schema& enumeration, json::value& value, bool null_form)
	{
		const desc::enum_item* current = nullptr;
		std::string unknown;
		if (value.is_null()) {
			if (enumeration.null_value) {
				current = enumeration.find_value(*enumeration.null_value);
			}
		} else if (value.is_string()) {
			current = enumeration.find(value.as_string());
			if (!current) {
				unknown = std::string(value.as_string());
			}
		} else {
			std::int64_t number = 0;
			if (desc::json_to_int64(value, number)) {
				current = enumeration.find_value(number);
			}
		}
		const char* preview = current ? current->name.c_str() : (unknown.empty() ? "?" : unknown.c_str());
		bool changed = false;
		if (ImGui::BeginCombo(label, preview)) {
			for (const auto& item : enumeration.items) {
				const bool selected = &item == current;
				if (ImGui::Selectable(item.name.c_str(), selected)) {
					const bool is_null_item = null_form && enumeration.null_value && item.value == *enumeration.null_value;
					value = is_null_item ? json::value() : desc::enum_to_json(enumeration, item.value);
					changed = true;
				}
				if (selected) {
					ImGui::SetItemDefaultFocus();
				}
			}
			ImGui::EndCombo();
		}
		return changed;
	}

	bool edit_text(const char* label, const desc::field_schema& field, bool asset, json::value& value)
	{
		// A {guid, path} reference shows its path; an edit writes the tag as a string, and saving the file writes the ID
		// of the file it names (EPIC-57 section 4.4).
		const json::object* reference = asset ? value.if_object() : nullptr;
		const std::string current = reference ? edt::reference_path_text(*reference) : std::string(value.as_string());
		const json::value* guid = reference ? reference->if_contains("guid") : nullptr;
		const std::string id = guid && guid->is_string() ? std::string(guid->as_string()) : std::string{};
		std::vector<char> buffer(std::max<std::size_t>(current.size() + 256, 512), '\0');
		std::copy(current.begin(), current.end(), buffer.begin());
		bool changed = false;
		if (ImGui::InputText(label, buffer.data(), buffer.size())) {
			value = json::string(buffer.data());
			changed = true;
		}
		if (!id.empty() && ImGui::IsItemHovered()) {
			ImGui::SetTooltip("ID %s", id.c_str());
		}
		if (asset && ImGui::BeginDragDropTarget()) {
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
				// The browser sends the path below res:// with its terminating zero (the viewport turns it into a tag the same way).
				if (payload->DataSize > 0) {
					const std::string path(static_cast<const char*>(payload->Data), static_cast<std::size_t>(payload->DataSize) - 1);
					if (matches_filter(path, field.filter)) {
						value = json::string(res::tag::make(path).string());
						changed = true;
					}
				}
			}
			ImGui::EndDragDropTarget();
		}
		// Below the field, so the drop target above stays the text field.
		if (asset) {
			const std::string shown = value.is_string() ? std::string(value.as_string()) : current;
			if (const std::string problem = edt::schema_ui::tag_text_problem(shown); !problem.empty()) {
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{ 1.f, 0.3f, 0.3f, 1.f });
				ImGui::TextWrapped("%s", problem.c_str());
				ImGui::PopStyleColor();
			}
		}
		return changed;
	}

	bool edit_json(const char* label, json::value& value, const context& ctx)
	{
		if (ctx.draw_json) {
			return ctx.draw_json(label, value);
		}
		ImGui::TextDisabled("%s: %s", label, json::serialize(value).c_str());
		return false;
	}

	bool edit_group(const char* label, const desc::field_schema& field, const widget_spec& spec, const desc::type_schema& nested,
		json::value& value, const std::string& pointer, const context& ctx, int depth)
	{
		bool changed = false;
		const bool open = ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen);
		show_tooltip(field, spec);
		if (open) {
			if (depth < max_depth) {
				changed = draw_object(nested, value, pointer, ctx, depth + 1);
			}
			ImGui::TreePop();
		}
		return changed;
	}

	desc::field_schema list_element(const desc::field_schema& list, field_kind element_kind, std::size_t index)
	{
		desc::field_schema element;
		element.name = std::format("[{}]", index);
		element.key = std::format("/{}", index);
		element.kind = element_kind;
		element.ref_type = list.ref_type;
		element.range = list.range;
		element.color = list.color;
		element.degrees = list.degrees;
		element.read_only = list.read_only;
		element.filter = list.filter;
		return element;
	}

	// A structure row is a tree node whose ID holds its row number: after the items trade places, the open or closed state
	// has to trade with them, or the row that was open stays open at its old place with another item in it.
	ImGuiID row_node_id(const desc::field_schema& element, std::size_t index)
	{
		ImGui::PushID(static_cast<int>(index));
		const ImGuiID id = ImGui::GetID(edt::schema_ui::display_label(element).c_str());
		ImGui::PopID();
		return id;
	}

	void swap_row_open_state(const desc::field_schema& first, std::size_t first_index, const desc::field_schema& second, std::size_t second_index)
	{
		ImGuiStorage* storage = ImGui::GetStateStorage();
		const ImGuiID first_id = row_node_id(first, first_index);
		const ImGuiID second_id = row_node_id(second, second_index);
		const int first_open = storage->GetInt(first_id, 1);
		const int second_open = storage->GetInt(second_id, 1);
		storage->SetInt(first_id, second_open);
		storage->SetInt(second_id, first_open);
	}

	bool edit_list(const char* label, const desc::field_schema& field, const widget_spec& spec, json::value& value,
		const std::string& pointer, const context& ctx, int depth)
	{
		json::array& array = value.as_array();
		const std::string header = std::format("{} ({} items)", label, array.size());
		const bool open = ImGui::TreeNodeEx("list", ImGuiTreeNodeFlags_DefaultOpen, "%s", header.c_str());
		show_tooltip(field, spec);
		if (!open) {
			return false;
		}
		bool changed = false;
		const field_kind element_kind = field.element == field_kind::array ? field_kind::custom : field.element;
		// A list is overridden whole (a merge patch replaces an array): its items carry no marks of their own.
		context item_ctx = ctx;
		item_ctx.origin = nullptr;
		edt::schema_ui::map_edit_state scratch;
		edt::schema_ui::map_edit_state& state = ctx.map_state ? *ctx.map_state : scratch;
		std::size_t& page_start = state.list_pages[ImGui::GetID("list_page")];
		page_start = edt::schema_ui::list_page_start(array.size(), page_start);
		if (array.size() > edt::schema_ui::k_list_page_size) {
			ImGui::BeginDisabled(page_start == 0);
			if (ImGui::SmallButton("< Prev")) {
				page_start -= edt::schema_ui::k_list_page_size;
			}
			report_target(ctx, pointer + "#prev");
			ImGui::EndDisabled();
			ImGui::SameLine();
			const std::size_t page_end = std::min(array.size(), page_start + edt::schema_ui::k_list_page_size);
			ImGui::TextDisabled("%zu-%zu of %zu", page_start + 1, page_end, array.size());
			ImGui::SameLine();
			ImGui::BeginDisabled(page_end >= array.size());
			if (ImGui::SmallButton("Next >")) {
				page_start += edt::schema_ui::k_list_page_size;
			}
			report_target(ctx, pointer + "#next");
			ImGui::EndDisabled();
		}
		std::optional<std::size_t> remove;
		std::optional<std::pair<std::size_t, std::size_t>> move;
		const std::size_t first = page_start;
		const std::size_t last = std::min(array.size(), first + edt::schema_ui::k_list_page_size);
		for (std::size_t i = first; i < last; ++i) {
			ImGui::PushID(static_cast<int>(i));
			const desc::field_schema element = list_element(field, element_kind, i);
			// The buttons come first: a row whose widget is wide would push them out of a narrow window.
			ImGui::BeginDisabled(field.read_only || i == 0);
			if (ImGui::SmallButton("^")) {
				move = std::pair{ i, i - 1 };
			}
			report_target(ctx, pointer + element.key + "#up");
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::BeginDisabled(field.read_only || i + 1 >= array.size());
			if (ImGui::SmallButton("v")) {
				move = std::pair{ i, i + 1 };
			}
			report_target(ctx, pointer + element.key + "#down");
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::SmallButton("x")) {
				remove = i;
			}
			ImGui::SameLine();
			if (depth < max_depth) {
				changed |= draw_field(element, array[i], pointer + element.key, item_ctx, depth + 1);
			}
			if (const std::string problem = edt::schema_ui::list_item_problem(field, array[i], ctx.registry); !problem.empty()) {
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{ 1.f, 0.3f, 0.3f, 1.f });
				ImGui::TextWrapped("%s", problem.c_str());
				ImGui::PopStyleColor();
			}
			ImGui::PopID();
		}
		if (move && edt::schema_ui::move_list_item(array, move->first, move->second)) {
			swap_row_open_state(list_element(field, element_kind, move->first), move->first, list_element(field, element_kind, move->second), move->second);
			page_start = edt::schema_ui::list_page_start(array.size(), move->second);
			changed = true;
		}
		if (remove && *remove < array.size()) {
			array.erase(array.begin() + static_cast<std::ptrdiff_t>(*remove));
			changed = true;
		}
		if (ImGui::SmallButton("+ Add")) {
			array.push_back(zero_of(element_kind, field.ref_type, ctx.registry, depth));
			page_start = edt::schema_ui::list_page_start(array.size(), array.size() - 1);
			changed = true;
		}
		ImGui::TreePop();
		return changed;
	}

	std::string escaped_token(std::string_view key)
	{
		std::string out;
		for (const char c : key) {
			if (c == '~') out += "~0";
			else if (c == '/') out += "~1";
			else out += c;
		}
		return out;
	}

	bool edit_map(const char* label, const desc::field_schema& field, const widget_spec& spec, json::value& value,
		const std::string& pointer, const context& ctx, int depth)
	{
		json::object& entries = value.as_object();
		const std::string header = std::format("{} ({} keys)", label, entries.size());
		const bool open = ImGui::TreeNodeEx("map", ImGuiTreeNodeFlags_DefaultOpen, "%s", header.c_str());
		show_tooltip(field, spec);
		if (!open) {
			return false;
		}
		edt::schema_ui::map_edit_state scratch;
		edt::schema_ui::map_edit_state& state = ctx.map_state ? *ctx.map_state : scratch;
		const field_kind element_kind = field.element == field_kind::array || field.element == field_kind::map ? field_kind::custom : field.element;
		bool changed = false;
		std::vector<std::string> keys;
		keys.reserve(entries.size());
		for (const auto& entry : entries) {
			keys.emplace_back(entry.key());
		}
		std::optional<std::string> remove;
		std::optional<std::pair<std::string, std::string>> rename;
		const std::size_t shown = std::min(keys.size(), max_listed);
		for (std::size_t i = 0; i < shown; ++i) {
			const std::string& key = keys[i];
			// By the key, not the row's place: a map that changes under the person (a parent read again, an undo) keeps each
			// text field with its own key.
			ImGui::PushID(key.c_str());
			std::vector<char> buffer(std::max<std::size_t>(key.size() + 64, edt::schema_ui::k_max_map_key + 1), '\0');
			std::copy(key.begin(), key.end(), buffer.begin());
			ImGui::SetNextItemWidth(140.0f);
			const bool entered = ImGui::InputText("##key", buffer.data(), buffer.size(), ImGuiInputTextFlags_EnterReturnsTrue);
			const ImGuiID row = ImGui::GetItemID();
			if (ImGui::IsItemActivated()) {
				state.rename_errors.erase(row);
			}
			if ((entered || ImGui::IsItemDeactivatedAfterEdit()) && buffer.data() != key) {
				rename = std::pair{ key, std::string(buffer.data()) };
				if (const std::string problem = edt::schema_ui::map_key_problem(entries, rename->second); !problem.empty()) {
					state.rename_errors[row] = problem;
					rename.reset();
				} else {
					state.rename_errors.erase(row);
				}
			}
			ImGui::SameLine();
			desc::field_schema element;
			element.name = key;
			element.key = "/" + escaped_token(key);
			element.kind = element_kind;
			element.ref_type = field.ref_type;
			element.range = field.range;
			element.color = field.color;
			element.degrees = field.degrees;
			element.read_only = field.read_only;
			element.filter = field.filter;
			if (depth < max_depth) {
				changed |= draw_field(element, entries[key], pointer + element.key, ctx, depth + 1);
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("x")) {
				remove = key;
			}
			if (const auto refused = state.rename_errors.find(row); refused != state.rename_errors.end()) {
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{ 1.f, 0.3f, 0.3f, 1.f });
				ImGui::TextWrapped("%s", refused->second.c_str());
				ImGui::PopStyleColor();
			}
			ImGui::PopID();
		}
		if (keys.size() > shown) {
			ImGui::TextDisabled("... and %zu more keys", keys.size() - shown);
		}
		if (remove && entries.erase(*remove) > 0) {
			changed = true;
		}
		if (rename && edt::schema_ui::rename_map_key(entries, rename->first, rename->second)) {
			changed = true;
		}

		std::string& fresh = state.new_keys[ImGui::GetID("new_key")];
		std::vector<char> buffer(std::max<std::size_t>(fresh.size() + 64, edt::schema_ui::k_max_map_key + 1), '\0');
		std::copy(fresh.begin(), fresh.end(), buffer.begin());
		ImGui::SetNextItemWidth(140.0f);
		const bool entered = ImGui::InputTextWithHint("##new_key", "new key", buffer.data(), buffer.size(), ImGuiInputTextFlags_EnterReturnsTrue);
		if (ImGui::IsItemEdited() || entered) {
			fresh = buffer.data();
		}
		ImGui::SameLine();
		const std::string problem = edt::schema_ui::map_key_problem(entries, fresh);
		const bool can_add = problem.empty() && !field.read_only;
		ImGui::BeginDisabled(!can_add);
		const bool clicked = ImGui::SmallButton("+ Add");
		ImGui::EndDisabled();
		if ((clicked || entered) && can_add
			&& edt::schema_ui::add_map_key(entries, fresh, zero_of(element_kind, field.ref_type, ctx.registry, depth))) {
			fresh.clear();
			changed = true;
		}
		if (!fresh.empty() && !problem.empty()) {
			ImGui::SameLine();
			ImGui::TextDisabled("%s", problem.c_str());
		}
		ImGui::TreePop();
		return changed;
	}

	bool draw_field(const desc::field_schema& field, json::value& value, const std::string& pointer, const context& ctx, int depth)
	{
		const desc::enum_schema* enumeration = enum_of(field, ctx.registry);
		const desc::type_schema* nested = nested_of(field, ctx.registry);
		widget_spec spec = edt::schema_ui::describe_widget(field, enumeration != nullptr, nested != nullptr);
		if (spec.kind == widget::hidden) {
			return false;
		}
		const bool null_form = enumeration && enumeration->null_value && field.write == desc::write_policy::null_if_empty;
		const bool null_combo = spec.kind == widget::combo && null_form && value.is_null();
		if (!null_combo && !suits(field, spec.kind, value)) {
			spec.kind = widget::json;
		}
		if (ctx.on_field) {
			ctx.on_field(pointer, spec.kind);
		}

		const std::string label = edt::schema_ui::display_label(field);
		bool changed = false;
		draw_origin_mark(ctx, field, pointer, &value);
		if (spec.disabled) {
			ImGui::BeginDisabled();
		}
		switch (spec.kind) {
		case widget::checkbox: changed = edit_bool(label.c_str(), value); break;
		case widget::integer: changed = edit_integer(label.c_str(), field, spec, value); break;
		case widget::real: changed = edit_real(label.c_str(), field, spec, value); break;
		case widget::vector: changed = edit_vector(label.c_str(), spec, value); break;
		case widget::color: changed = edit_color(label.c_str(), value); break;
		case widget::combo: changed = edit_combo(label.c_str(), *enumeration, value, null_form); break;
		case widget::text: changed = edit_text(label.c_str(), field, false, value); break;
		case widget::asset: changed = edit_text(label.c_str(), field, true, value); break;
		case widget::group: changed = edit_group(label.c_str(), field, spec, *nested, value, pointer, ctx, depth); break;
		case widget::list: changed = edit_list(label.c_str(), field, spec, value, pointer, ctx, depth); break;
		case widget::map: changed = edit_map(label.c_str(), field, spec, value, pointer, ctx, depth); break;
		case widget::label: ImGui::TextDisabled("%s: %s", label.c_str(), json::serialize(value).c_str()); break;
		case widget::json: changed = edit_json(label.c_str(), value, ctx); break;
		case widget::hidden: break;
		}
		if (spec.disabled) {
			ImGui::EndDisabled();
		}
		if (spec.kind != widget::group && spec.kind != widget::list && spec.kind != widget::map) {
			show_tooltip(field, spec);
		}
		return changed;
	}
}

edt::schema_ui::widget_spec edt::schema_ui::describe_widget(const desc::field_schema& field, bool enum_known, bool nested_known)
{
	widget_spec spec;
	spec.disabled = field.read_only;
	spec.degrees = field.degrees;
	if (field.range) {
		spec.bounded = field.range->max > field.range->min;
		spec.min = field.range->min;
		spec.max = field.range->max;
		spec.speed = field.range->step;
	}

	// A key that is only read (write = never) is never saved again, so an edit of it would be lost: not shown.
	if (field.editor_hidden || field.write == desc::write_policy::never) {
		spec.kind = widget::hidden;
		return spec;
	}
	switch (field.kind) {
	case field_kind::boolean: spec.kind = widget::checkbox; break;
	case field_kind::i32:
	case field_kind::u32:
	case field_kind::i64:
	case field_kind::u64: spec.kind = widget::integer; break;
	case field_kind::f32:
	case field_kind::f64: spec.kind = widget::real; break;
	case field_kind::vec2:
	case field_kind::vec3:
	case field_kind::vec4:
		spec.kind = field.color && field.kind != field_kind::vec2 ? widget::color : widget::vector;
		break;
	case field_kind::quat: spec.kind = widget::vector; break;
	case field_kind::color: spec.kind = widget::color; break;
	case field_kind::string: spec.kind = widget::text; break;
	case field_kind::res_tag:
	case field_kind::desc_ref: spec.kind = widget::asset; break;
	case field_kind::enumeration: spec.kind = enum_known ? widget::combo : widget::json; break;
	case field_kind::node_ref:
	case field_kind::entity: spec.kind = widget::label; break;
	case field_kind::structure: spec.kind = nested_known ? widget::group : widget::json; break;
	case field_kind::array: spec.kind = widget::list; break;
	case field_kind::map: spec.kind = widget::map; break;
	case field_kind::custom: spec.kind = widget::json; break;
	}

	if (spec.kind == widget::real || spec.kind == widget::vector) {
		int digits = 3;
		if (spec.speed >= 1.0) {
			digits = 0;
		} else if (spec.speed > 0.0) {
			digits = std::min(6, static_cast<int>(std::ceil(-std::log10(spec.speed) - 1e-9)) + 1);
		}
		spec.format = std::format("%.{}f", digits);
		if (spec.degrees) {
			spec.format += " deg";
		}
	}
	return spec;
}

std::string edt::schema_ui::tag_text_problem(std::string_view text)
{
	const res::tag tag{ text };
	if (text.empty() || !tag.protocol().empty()) {
		return {};
	}
	return res::no_protocol_reason(tag);
}

std::string edt::schema_ui::display_label(const desc::field_schema& field)
{
	std::string text = field.name;
	if (text.empty()) {
		text = field.key;
		if (const auto slash = text.rfind('/'); slash != std::string::npos) {
			text.erase(0, slash + 1);
		}
	}
	if (text.starts_with("m_")) {
		text.erase(0, 2);
	}
	std::ranges::replace(text, '_', ' ');
	if (!text.empty()) {
		text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
	}
	return text;
}

std::size_t edt::schema_ui::list_page_start(std::size_t count, std::size_t index)
{
	if (count == 0) {
		return 0;
	}
	const std::size_t last_item = count - 1;
	return std::min(index, last_item) / k_list_page_size * k_list_page_size;
}

bool edt::schema_ui::move_list_item(json::array& items, std::size_t from, std::size_t to)
{
	if (from >= items.size() || to >= items.size() || from == to) {
		return false;
	}
	json::value moved = std::move(items[from]);
	items.erase(items.begin() + static_cast<std::ptrdiff_t>(from));
	items.insert(items.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
	return true;
}

std::string edt::schema_ui::list_item_problem(const desc::field_schema& field, const json::value& item, const desc::schema_registry& registry)
{
	if (field.kind != field_kind::array || field.element != field_kind::structure) {
		return {};
	}
	const desc::type_schema* nested = registry.find(field.ref_type);
	if (!nested) {
		return {};
	}
	if (!item.is_object()) {
		return std::format("expected an object of '{}'", field.ref_type);
	}
	const desc::override_failure failure = desc::check_override_object(*nested, item.as_object(), nullptr, &registry);
	switch (failure.status) {
	case desc::override_status::valid: return {};
	case desc::override_status::stale: return std::format("{}: no such field", failure.pointer);
	case desc::override_status::bad_value: {
		const std::string expected = desc::override_expectation(*nested, failure, &registry);
		return expected.empty() ? std::format("{}: the value does not fit", failure.pointer) : std::format("{}: expected {}", failure.pointer, expected);
	}
	}
	return {};
}

std::string edt::schema_ui::map_key_problem(const json::object& entries, std::string_view key)
{
	if (key.empty()) return "a key cannot be empty";
	if (key.size() > k_max_map_key) return std::format("a key is at most {} bytes", k_max_map_key);
	if (entries.contains(key)) return std::format("'{}' is taken", key);
	return {};
}

bool edt::schema_ui::add_map_key(json::object& entries, std::string_view key, json::value value)
{
	if (!map_key_problem(entries, key).empty()) return false;
	entries.emplace(key, std::move(value));
	return true;
}

bool edt::schema_ui::rename_map_key(json::object& entries, std::string_view from, std::string_view to)
{
	if (!entries.contains(from)) return false;
	if (from == to) return true;
	if (!map_key_problem(entries, to).empty()) return false;
	json::object renamed;
	for (auto& entry : entries) {
		if (entry.key() == from) renamed.emplace(to, std::move(entry.value()));
		else renamed.emplace(entry.key(), std::move(entry.value()));
	}
	entries = std::move(renamed);
	return true;
}

json::value edt::schema_ui::zero_value(const desc::field_schema& field, const desc::schema_registry& registry)
{
	return zero_of(field.kind, field.ref_type, registry, 0);
}

json::value edt::schema_ui::null_reads_as(const desc::field_schema& field, const desc::schema_registry& registry)
{
	// As the read does (desc_schema_ops.cpp, desc_describe_ops.h): nullopt, an empty entity, or the empty value of a field
	// that writes null for empty, before any default.
	if (field.nullable || field.kind == field_kind::entity) {
		return json::value();
	}
	if (field.write == desc::write_policy::null_if_empty) {
		switch (field.kind) {
		case field_kind::res_tag:
		case field_kind::string:
		case field_kind::desc_ref: return json::string();
		case field_kind::array: return json::array();
		case field_kind::enumeration: {
			// the combo shows the enum's null_value from the null itself
			const desc::enum_schema* enumeration = field.ref_type.empty() ? nullptr : registry.find_enum(field.ref_type);
			if (enumeration && enumeration->null_value) {
				return json::value();
			}
			break;
		}
		default: break;
		}
	}
	return field.default_value.is_null() ? zero_value(field, registry) : field.default_value;
}

bool edt::schema_ui::unset_changes_value(const desc::field_schema& field, const json::value& shown, const desc::schema_registry& registry)
{
	if (!field.optional) {
		return false;
	}
	const json::value read = null_reads_as(field, registry);
	return read.is_null() ? !shown.is_null() : !same_value(read, shown);
}

json::value edt::schema_ui::defaults_json(const desc::type_schema& schema, const desc::schema_registry& registry, bool fill_missing)
{
	return defaults_impl(schema, registry, 0, fill_missing);
}

json::value edt::schema_ui::compose(const desc::type_schema& schema, const desc::schema_registry& registry,
	std::initializer_list<const json::object*> layers, bool fill_missing)
{
	json::value result = defaults_json(schema, registry, fill_missing);
	for (const json::object* layer : layers) {
		if (!layer) {
			continue;
		}
		json::value copy(*layer);
		resolve_aliases(schema, copy);
		override_edit::merge_layer(registry, schema, result.as_object(), copy.as_object());
	}
	drop_null_keys(schema, registry, result, 0);
	return result;
}

bool edt::schema_ui::draw_fields(const desc::type_schema& schema, json::value& object, const context& ctx)
{
	bool changed = draw_object(schema, object, std::string(), ctx, 0);
	if (ctx.origin && !ctx.origin->requests.unsets.empty()) {
		changed |= apply_unsets(object, ctx.origin->requests.unsets);
		ctx.origin->requests.unsets.clear();
	}
	return changed;
}

bool edt::schema_ui::set_here(const field_origin& origin, std::string_view pointer)
{
	return desc::json_pointer_get(origin.own, pointer) != nullptr;
}

edt::schema_ui::value_source edt::schema_ui::source_of(const field_origin& origin, std::string_view pointer)
{
	if (set_here(origin, pointer)) {
		return value_source::set_here;
	}
	if (origin.has_parent) {
		const json::value* held = desc::json_pointer_get(origin.parent, pointer);
		if (held && !held->is_null()) {
			return value_source::inherited;
		}
	}
	return value_source::schema_default;
}

std::string edt::schema_ui::source_text(const field_origin& origin, std::string_view pointer)
{
	switch (source_of(origin, pointer)) {
	case value_source::set_here: return "set here";
	case value_source::inherited: {
		// The level that really sets the value: the nearest one that has it.
		std::string name = origin.inherited_from;
		for (const origin_layer& layer : origin.layers) {
			const json::value* held = desc::json_pointer_get(layer.data, pointer);
			if (held && !held->is_null()) {
				name = layer.name;
				break;
			}
		}
		return name.empty() ? std::string{ "inherited" } : "inherited from " + name;
	}
	case value_source::schema_default: break;
	}
	return "schema default";
}

bool edt::schema_ui::apply_unsets(json::value& shown, const std::vector<std::string>& pointers)
{
	bool changed = false;
	for (const std::string& pointer : pointers) {
		changed |= desc::json_pointer_remove(shown, pointer);
	}
	return changed;
}

std::size_t edt::schema_ui::apply_resets(json::object& own, const desc::type_schema& schema, const std::vector<std::string>& pointers)
{
	json::value holder(own);
	std::size_t removed = 0;
	for (const std::string& pointer : pointers) {
		if (!desc::json_pointer_remove(holder, pointer)) {
			continue;
		}
		++removed;
		prune_empty_parents(holder, pointer);
		for (const auto& field : schema.fields) {
			if (field.key != pointer) {
				continue;
			}
			for (const auto& alias : field.read_aliases) {
				if (desc::json_pointer_remove(holder, alias)) {
					prune_empty_parents(holder, alias);
				}
			}
		}
	}
	if (removed > 0) {
		own = std::move(holder.as_object());
	}
	return removed;
}

std::size_t edt::schema_ui::apply_edit(json::object& overrides, const desc::type_schema& schema, const desc::schema_registry& registry,
	const json::value& base, const json::value& before, const json::value& after)
{
	// A map has no key without a name: the read would throw the whole map away, so nothing of this edit is written.
	for (const auto& field : schema.fields) {
		const json::value* held = field.kind == field_kind::map ? desc::json_pointer_get(after, field.key) : nullptr;
		if (held && held->is_object() && held->as_object().contains("")) {
			egLOG_WARN("editor", "the map '{}' has a key with no name: the edit is not written", field.name);
			return 0;
		}
	}
	const std::vector<desc::json_change> changes = desc::diff(schema, before, after, &registry);
	if (changes.empty()) {
		return 0;
	}
	json::value holder(overrides); // a copy: `overrides` stays intact if anything below throws
	for (const auto& change : changes) {
		const json::value* inherited = desc::json_pointer_get(base, change.pointer);
		// A null is a removal: of a key the parent has (kept as the record), or of one that is only here (no record at all) -
		// unless the null is a value of the field and it has a default of its own: without the record the read gives that,
		// not the null. Where a null reads as the default anyway, a record of it would change nothing (#861).
		const desc::field_schema* field = field_at(schema, registry, change.pointer);
		const bool null_is_kept = field && field->nullable && !field->default_value.is_null();
		const bool removes_own = change.value.is_null() && (!inherited || inherited->is_null()) && !null_is_kept;
		const bool null_reads_default = change.value.is_null() && field && field->optional && !field->nullable;
		const json::value read = null_reads_default ? null_reads_as(*field, registry) : change.value;
		if (removes_own || (inherited && same_value(*inherited, read))) {
			if (desc::json_pointer_remove(holder, change.pointer)) {
				prune_empty_parents(holder, change.pointer);
			}
			// the field is inherited again, so its legacy keys must not keep an older value alive
			for (const auto& field : schema.fields) {
				if (field.key != change.pointer) {
					continue;
				}
				for (const auto& alias : field.read_aliases) {
					if (desc::json_pointer_remove(holder, alias)) {
						prune_empty_parents(holder, alias);
					}
				}
			}
		} else {
			desc::json_pointer_set(holder, change.pointer, change.value);
		}
	}
	overrides = std::move(holder.as_object());
	return changes.size();
}

bool edt::schema_ui::edit_component(const desc::type_schema& schema, const desc::schema_registry& registry, const boost::json::object* parent,
	boost::json::object& overrides, const std::function<bool(boost::json::value& shown)>& draw)
{
	// base: what the component has without its own data; before/after: what the user sees before and after the edit.
	const boost::json::value base = compose(schema, registry, { parent }, false);
	const boost::json::value before = compose(schema, registry, { parent, &overrides });
	boost::json::value after = before;
	if (!draw(after))
		return false;
	return apply_edit(overrides, schema, registry, base, before, after) > 0;
}

bool edt::schema_ui::edit_component_marked(const desc::type_schema& schema, const desc::schema_registry& registry, const boost::json::object* parent,
	boost::json::object& overrides, std::string inherited_from, const std::vector<parent_layer>& layers,
	const std::function<bool(boost::json::value& shown, const field_origin& origin)>& draw)
{
	const boost::json::value base = compose(schema, registry, { parent }, false);
	const boost::json::value before = compose(schema, registry, { parent, &overrides });
	boost::json::value after = before;
	field_origin origin;
	origin.own = overrides;
	origin.has_parent = parent != nullptr;
	if (parent) origin.parent = *parent;
	origin.inherited_from = std::move(inherited_from);
	for (const parent_layer& layer : layers) {
		origin.layers.push_back({ layer.name, layer.data ? boost::json::value(*layer.data) : boost::json::value() });
	}
	const bool drawn = draw(after, origin);
	const std::size_t written = drawn ? apply_edit(overrides, schema, registry, base, before, after) : 0;
	const std::size_t reset = apply_resets(overrides, schema, origin.requests.resets);
	origin.requests.resets.clear();
	return written > 0 || reset > 0;
}
