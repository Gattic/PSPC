// SPDX-License-Identifier: GPL-3.0-or-later
// A minimal ordered JSON value, so `info` can print the same facts as text or as JSON.
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common.h"

class Json {
public:
	enum class Type { Null, Bool, Int, String, Array, Object };

	Json() = default;
	Json(bool b) : type_(Type::Bool), int_(b ? 1 : 0) {}
	Json(int v) : type_(Type::Int), int_(v) {}
	Json(unsigned v) : type_(Type::Int), int_(v) {}
	Json(long long v) : type_(Type::Int), int_(v) {}
	Json(unsigned long long v) : type_(Type::Int), int_((long long)v) {}
	Json(long v) : type_(Type::Int), int_(v) {}
	Json(unsigned long v) : type_(Type::Int), int_((long long)v) {}
	Json(const char *s) : type_(Type::String), str_(s) {}
	Json(std::string s) : type_(Type::String), str_(std::move(s)) {}

	static Json Array() { Json j; j.type_ = Type::Array; return j; }
	static Json Object() { Json j; j.type_ = Type::Object; return j; }

	Type type() const { return type_; }
	bool IsNull() const { return type_ == Type::Null; }

	// Object: sets (or replaces) a key, keeping insertion order. Returns *this for chaining.
	Json &Set(const std::string &key, Json value);
	// Object: the member for key, or null.
	const Json *Get(const std::string &key) const;
	// Array: appends.
	Json &Push(Json value);
	size_t Size() const { return type_ == Type::Array ? items_.size() : members_.size(); }
	const Json &At(size_t i) const { return items_[i]; }

	long long AsInt() const { return int_; }
	bool AsBool() const { return int_ != 0; }
	const std::string &AsString() const { return str_; }

	std::string Dump(int indent = 2) const;
	// Human-readable "key: value" lines.
	std::string Text() const;

private:
	void DumpTo(std::string &out, int indent, int depth) const;
	void TextTo(std::string &out, int depth, const std::string &prefix) const;

	Type type_ = Type::Null;
	long long int_ = 0;
	std::string str_;
	std::vector<Json> items_;
	std::vector<std::pair<std::string, Json>> members_;
};

std::string JsonEscape(const std::string &s);
