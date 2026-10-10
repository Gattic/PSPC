// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/json.h"

#include <cstdio>
#include <cstdlib>

namespace json {

namespace {
const std::string kEmptyString;
const std::vector<Value> kEmptyArray;
const Value kNull;

void PutUtf8(std::string &out, unsigned cp) {
	if (cp < 0x80) {
		out.push_back((char)cp);
	} else if (cp < 0x800) {
		out.push_back((char)(0xC0 | (cp >> 6)));
		out.push_back((char)(0x80 | (cp & 0x3F)));
	} else if (cp < 0x10000) {
		out.push_back((char)(0xE0 | (cp >> 12)));
		out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
		out.push_back((char)(0x80 | (cp & 0x3F)));
	} else {
		out.push_back((char)(0xF0 | (cp >> 18)));
		out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
		out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
		out.push_back((char)(0x80 | (cp & 0x3F)));
	}
}
}  // namespace

class Parser {
public:
	explicit Parser(const std::string &s) : s_(s) {}

	bool ParseDocument(Value &out) {
		if (!ParseValue(out, 0)) return false;
		Skip();
		return pos_ == s_.size() || Fail("trailing characters");
	}
	std::string error;

private:
	bool Fail(const char *what) {
		if (error.empty()) error = std::string(what) + " at offset " + std::to_string(pos_);
		return false;
	}
	void Skip() {
		while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) pos_++;
	}
	bool Literal(const char *word) {
		size_t n = 0;
		while (word[n]) n++;
		if (s_.compare(pos_, n, word) != 0) return Fail("bad literal");
		pos_ += n;
		return true;
	}
	bool Hex4(unsigned &v) {
		if (pos_ + 4 > s_.size()) return Fail("short \\u escape");
		v = 0;
		for (int i = 0; i < 4; i++) {
			char c = s_[pos_++];
			v <<= 4;
			if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
			else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
			else return Fail("bad \\u escape");
		}
		return true;
	}
	bool ParseString(std::string &out) {
		pos_++;  // the opening quote
		while (pos_ < s_.size()) {
			char c = s_[pos_++];
			if (c == '"') return true;
			if ((unsigned char)c < 0x20) return Fail("control character in string");
			if (c != '\\') {
				out.push_back(c);
				continue;
			}
			if (pos_ >= s_.size()) break;
			char e = s_[pos_++];
			switch (e) {
			case '"': out.push_back('"'); break;
			case '\\': out.push_back('\\'); break;
			case '/': out.push_back('/'); break;
			case 'b': out.push_back('\b'); break;
			case 'f': out.push_back('\f'); break;
			case 'n': out.push_back('\n'); break;
			case 'r': out.push_back('\r'); break;
			case 't': out.push_back('\t'); break;
			case 'u': {
				unsigned cp;
				if (!Hex4(cp)) return false;
				if (cp >= 0xD800 && cp < 0xDC00 && pos_ + 6 <= s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
					pos_ += 2;
					unsigned lo;
					if (!Hex4(lo)) return false;
					if (lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
				}
				PutUtf8(out, cp);
				break;
			}
			default: return Fail("bad escape");
			}
		}
		return Fail("unterminated string");
	}
	bool ParseValue(Value &v, int depth) {
		if (depth > 64) return Fail("nested too deeply");
		Skip();
		if (pos_ >= s_.size()) return Fail("unexpected end");
		char c = s_[pos_];
		if (c == '{') {
			v.type_ = Value::Type::Object;
			pos_++;
			Skip();
			if (pos_ < s_.size() && s_[pos_] == '}') {
				pos_++;
				return true;
			}
			for (;;) {
				Skip();
				if (pos_ >= s_.size() || s_[pos_] != '"') return Fail("expected a key");
				std::string key;
				if (!ParseString(key)) return false;
				Skip();
				if (pos_ >= s_.size() || s_[pos_] != ':') return Fail("expected ':'");
				pos_++;
				Value member;
				if (!ParseValue(member, depth + 1)) return false;
				v.object_[key] = std::move(member);
				Skip();
				if (pos_ < s_.size() && s_[pos_] == ',') {
					pos_++;
					continue;
				}
				if (pos_ < s_.size() && s_[pos_] == '}') {
					pos_++;
					return true;
				}
				return Fail("expected ',' or '}'");
			}
		}
		if (c == '[') {
			v.type_ = Value::Type::Array;
			pos_++;
			Skip();
			if (pos_ < s_.size() && s_[pos_] == ']') {
				pos_++;
				return true;
			}
			for (;;) {
				Value item;
				if (!ParseValue(item, depth + 1)) return false;
				v.array_.push_back(std::move(item));
				Skip();
				if (pos_ < s_.size() && s_[pos_] == ',') {
					pos_++;
					continue;
				}
				if (pos_ < s_.size() && s_[pos_] == ']') {
					pos_++;
					return true;
				}
				return Fail("expected ',' or ']'");
			}
		}
		if (c == '"') {
			v.type_ = Value::Type::String;
			return ParseString(v.string_);
		}
		if (c == 't' || c == 'f') {
			v.type_ = Value::Type::Bool;
			v.bool_ = c == 't';
			return Literal(c == 't' ? "true" : "false");
		}
		if (c == 'n') return Literal("null");
		if (c == '-' || (c >= '0' && c <= '9')) {
			const char *start = s_.c_str() + pos_;
			char *end = nullptr;
			v.number_ = strtod(start, &end);
			if (end == start) return Fail("bad number");
			v.type_ = Value::Type::Number;
			pos_ += (size_t)(end - start);
			return true;
		}
		return Fail("unexpected character");
	}

	const std::string &s_;
	size_t pos_ = 0;
};

Value Value::Parse(const std::string &text, std::string *error) {
	Parser p(text);
	Value v;
	if (!p.ParseDocument(v)) {
		if (error) *error = p.error;
		return Value();
	}
	return v;
}

const std::string &Value::String() const { return type_ == Type::String ? string_ : kEmptyString; }
const std::vector<Value> &Value::Array() const { return type_ == Type::Array ? array_ : kEmptyArray; }

const Value &Value::operator[](const std::string &key) const {
	if (type_ != Type::Object) return kNull;
	auto it = object_.find(key);
	return it == object_.end() ? kNull : it->second;
}

const std::map<std::string, Value> &Value::Members() const {
	static const std::map<std::string, Value> kNone;
	return type_ == Type::Object ? object_ : kNone;
}

bool Value::Has(const std::string &key) const { return type_ == Type::Object && object_.count(key) != 0; }

std::string Quote(const std::string &text) {
	std::string out = "\"";
	for (unsigned char c : text) {
		switch (c) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (c < 0x20) {
				char buf[8];
				snprintf(buf, sizeof(buf), "\\u%04x", c);
				out += buf;
			} else {
				out.push_back((char)c);
			}
		}
	}
	return out + "\"";
}

}  // namespace json
