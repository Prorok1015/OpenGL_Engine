#include "edt_schema_inspector.h"
#include <snk/desc/desc_json_ops.h>
#include <snk/desc/desc_schema_ops.h>
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
	// float32 holds ~7 digits; relative, so a small value (near plane 0.0001) is not compared as if it were ~1
	constexpr double revert_tolerance = 1e-6;

	json::value zeros(std::size_t count)
	{
		json::array components;
		for (std::size_t i = 0; i < count; ++i) {
			components.push_back(0.0);
		}
		return json::value(std::move(components));
	}

	json::value defaults_impl(const desc::type_schema& schema, const desc::schema_registry& registry, int depth, bool fill_missing);

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
		case field_kind::entity:
		case field_kind::custom: break;
		}
		return json::value();
	}

	json::value defaults_impl(const desc::type_schema& schema, const desc::schema_registry& registry, int depth, bool fill_missing)
	{
		json::value out(json::object{});
		for (const auto& field : schema.fields) {
			if (field.optional) {
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
			return x == y || std::fabs(x - y) <= revert_tolerance * std::max(std::fabs(x), std::fabs(y));
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
		case widget::text:
		case widget::asset: return value.is_string();
		case widget::group: return value.is_object();
		case widget::list: return value.is_array();
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

	bool draw_field(const desc::field_schema& field, json::value& value, const std::string& pointer, const context& ctx, int depth);

	bool draw_object(const desc::type_schema& schema, json::value& object, const std::string& prefix, const context& ctx, int depth)
	{
		if (!object.is_object()) {
			return false;
		}
		bool changed = false;
		for (const auto& field : schema.fields) {
			if (field.editor_hidden) {
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

	bool edit_combo(const char* label, const desc::enum_schema& enumeration, json::value& value)
	{
		const desc::enum_item* current = nullptr;
		std::string unknown;
		if (value.is_string()) {
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
					value = desc::enum_to_json(enumeration, item.value);
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
		const std::string current(value.as_string());
		std::vector<char> buffer(std::max<std::size_t>(current.size() + 256, 512), '\0');
		std::copy(current.begin(), current.end(), buffer.begin());
		bool changed = false;
		if (ImGui::InputText(label, buffer.data(), buffer.size())) {
			value = json::string(buffer.data());
			changed = true;
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
		std::optional<std::size_t> remove;
		const std::size_t shown = std::min(array.size(), max_listed);
		for (std::size_t i = 0; i < shown; ++i) {
			ImGui::PushID(static_cast<int>(i));
			desc::field_schema element;
			element.name = std::format("[{}]", i);
			element.key = std::format("/{}", i);
			element.kind = element_kind;
			element.ref_type = field.ref_type;
			element.range = field.range;
			element.color = field.color;
			element.degrees = field.degrees;
			element.read_only = field.read_only;
			element.filter = field.filter;
			if (depth < max_depth) {
				changed |= draw_field(element, array[i], pointer + element.key, ctx, depth + 1);
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("x")) {
				remove = i;
			}
			ImGui::PopID();
		}
		if (array.size() > shown) {
			ImGui::TextDisabled("... and %zu more items", array.size() - shown);
		}
		if (remove && *remove < array.size()) {
			array.erase(array.begin() + static_cast<std::ptrdiff_t>(*remove));
			changed = true;
		}
		if (ImGui::SmallButton("+ Add")) {
			array.push_back(zero_of(element_kind, field.ref_type, ctx.registry, depth));
			changed = true;
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
		if (!suits(field, spec.kind, value)) {
			spec.kind = widget::json;
		}
		if (ctx.on_field) {
			ctx.on_field(pointer, spec.kind);
		}

		const std::string label = edt::schema_ui::display_label(field);
		bool changed = false;
		if (spec.disabled) {
			ImGui::BeginDisabled();
		}
		switch (spec.kind) {
		case widget::checkbox: changed = edit_bool(label.c_str(), value); break;
		case widget::integer: changed = edit_integer(label.c_str(), field, spec, value); break;
		case widget::real: changed = edit_real(label.c_str(), field, spec, value); break;
		case widget::vector: changed = edit_vector(label.c_str(), spec, value); break;
		case widget::color: changed = edit_color(label.c_str(), value); break;
		case widget::combo: changed = edit_combo(label.c_str(), *enumeration, value); break;
		case widget::text: changed = edit_text(label.c_str(), field, false, value); break;
		case widget::asset: changed = edit_text(label.c_str(), field, true, value); break;
		case widget::group: changed = edit_group(label.c_str(), field, spec, *nested, value, pointer, ctx, depth); break;
		case widget::list: changed = edit_list(label.c_str(), field, spec, value, pointer, ctx, depth); break;
		case widget::label: ImGui::TextDisabled("%s: %s", label.c_str(), json::serialize(value).c_str()); break;
		case widget::json: changed = edit_json(label.c_str(), value, ctx); break;
		case widget::hidden: break;
		}
		if (spec.disabled) {
			ImGui::EndDisabled();
		}
		if (spec.kind != widget::group && spec.kind != widget::list) {
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

	if (field.editor_hidden) {
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

json::value edt::schema_ui::zero_value(const desc::field_schema& field, const desc::schema_registry& registry)
{
	return zero_of(field.kind, field.ref_type, registry, 0);
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
		desc::json_deep_merge(result.as_object(), copy.as_object());
	}
	return result;
}

bool edt::schema_ui::draw_fields(const desc::type_schema& schema, json::value& object, const context& ctx)
{
	return draw_object(schema, object, std::string(), ctx, 0);
}

std::size_t edt::schema_ui::apply_edit(json::object& overrides, const desc::type_schema& schema, const desc::schema_registry& registry,
	const json::value& base, const json::value& before, const json::value& after)
{
	const std::vector<desc::json_change> changes = desc::diff(schema, before, after, &registry);
	if (changes.empty()) {
		return 0;
	}
	json::value holder(overrides); // a copy: `overrides` stays intact if anything below throws
	for (const auto& change : changes) {
		const json::value* inherited = desc::json_pointer_get(base, change.pointer);
		if (inherited && same_value(*inherited, change.value)) {
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
