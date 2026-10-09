// SPDX-License-Identifier: GPL-3.0-or-later
#include "json.h"

Json &Json::Set(const std::string &key, Json value) {
	type_ = Type::Object;
	for (auto &member : members_) {
		if (member.first == key) {
			member.second = std::move(value);
			return *this;
		}
	}
	members_.emplace_back(key, std::move(value));
	return *this;
}

const Json *Json::Get(const std::string &key) const {
	for (const auto &member : members_) {
		if (member.first == key) {
			return &member.second;
		}
	}
	return nullptr;
}

Json &Json::Push(Json value) {
	type_ = Type::Array;
	items_.push_back(std::move(value));
	return *this;
}

std::string JsonEscape(const std::string &s) {
	std::string out;
	out.reserve(s.size() + 2);
	for (unsigned char c : s) {
		switch (c) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (c < 0x20) {
				out += StringPrintf("\\u%04x", c);
			} else {
				out.push_back((char)c);
			}
		}
	}
	return out;
}

std::string Json::Dump(int indent) const {
	std::string out;
	DumpTo(out, indent, 0);
	return out;
}

void Json::DumpTo(std::string &out, int indent, int depth) const {
	auto newline = [&](int d) {
		if (indent > 0) {
			out.push_back('\n');
			out.append((size_t)(indent * d), ' ');
		}
	};
	switch (type_) {
	case Type::Null: out += "null"; break;
	case Type::Bool: out += int_ ? "true" : "false"; break;
	case Type::Int: out += std::to_string(int_); break;
	case Type::String: out += '"' + JsonEscape(str_) + '"'; break;
	case Type::Array:
		if (items_.empty()) {
			out += "[]";
			break;
		}
		out.push_back('[');
		for (size_t i = 0; i < items_.size(); i++) {
			if (i) out.push_back(',');
			newline(depth + 1);
			items_[i].DumpTo(out, indent, depth + 1);
		}
		newline(depth);
		out.push_back(']');
		break;
	case Type::Object:
		if (members_.empty()) {
			out += "{}";
			break;
		}
		out.push_back('{');
		for (size_t i = 0; i < members_.size(); i++) {
			if (i) out.push_back(',');
			newline(depth + 1);
			out += '"' + JsonEscape(members_[i].first) + "\":";
			if (indent > 0) out.push_back(' ');
			members_[i].second.DumpTo(out, indent, depth + 1);
		}
		newline(depth);
		out.push_back('}');
		break;
	}
}

std::string Json::Text() const {
	std::string out;
	TextTo(out, 0, std::string());
	return out;
}

void Json::TextTo(std::string &out, int depth, const std::string &prefix) const {
	const std::string pad((size_t)depth * 2, ' ');
	auto scalar = [](const Json &j) -> std::string {
		switch (j.type_) {
		case Type::Null: return "-";
		case Type::Bool: return j.int_ ? "yes" : "no";
		case Type::Int: return std::to_string(j.int_);
		case Type::String: {
			// One fact per line: keep embedded line breaks from splitting a value.
			std::string s = j.str_;
			for (char &c : s) {
				if (c == '\n' || c == '\r') {
					c = ' ';
				}
			}
			return s;
		}
		default: return std::string();
		}
	};
	auto isScalar = [](const Json &j) { return j.type_ != Type::Array && j.type_ != Type::Object; };
	if (type_ == Type::Object) {
		for (const auto &[key, value] : members_) {
			if (isScalar(value)) {
				out += pad + key + ": " + scalar(value) + "\n";
			} else if (value.Size() == 0) {
				out += pad + key + ": (none)\n";
			} else {
				out += pad + key + ":\n";
				value.TextTo(out, depth + 1, key);
			}
		}
	} else if (type_ == Type::Array) {
		bool allScalar = true;
		for (const Json &item : items_) {
			allScalar = allScalar && isScalar(item);
		}
		if (allScalar) {
			std::string line;
			for (size_t i = 0; i < items_.size(); i++) {
				line += (i ? ", " : "") + scalar(items_[i]);
			}
			out += pad + line + "\n";
			return;
		}
		for (size_t i = 0; i < items_.size(); i++) {
			out += pad + "[" + std::to_string(i) + "]\n";
			items_[i].TextTo(out, depth + 1, prefix);
		}
	} else {
		out += pad + scalar(*this) + "\n";
	}
}
