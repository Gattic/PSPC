// SPDX-License-Identifier: GPL-2.0-or-later
//
// A small JSON reader and writer for PSPC's own web APIs (PSPC Network). Numbers are doubles;
// strings are UTF-8 (\u escapes decoded, surrogate pairs included).
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace json {

class Value {
public:
	enum class Type { Null, Bool, Number, String, Array, Object };

	Value() = default;
	static Value Parse(const std::string &text, std::string *error = nullptr);  // Null on error

	Type type() const { return type_; }
	bool IsNull() const { return type_ == Type::Null; }
	bool IsObject() const { return type_ == Type::Object; }
	bool IsArray() const { return type_ == Type::Array; }
	bool IsString() const { return type_ == Type::String; }

	// Accessors are forgiving: the wrong type gives the default.
	bool Bool(bool def = false) const { return type_ == Type::Bool ? bool_ : def; }
	double Number(double def = 0) const { return type_ == Type::Number ? number_ : def; }
	const std::string &String() const;
	std::string String(const std::string &def) const { return type_ == Type::String ? string_ : def; }
	const std::vector<Value> &Array() const;
	// Object members; a missing key gives a Null value.
	const Value &operator[](const std::string &key) const;
	bool Has(const std::string &key) const;
	// An object's members (empty for other types).
	const std::map<std::string, Value> &Members() const;

private:
	friend class Parser;
	Type type_ = Type::Null;
	bool bool_ = false;
	double number_ = 0;
	std::string string_;
	std::vector<Value> array_;
	std::map<std::string, Value> object_;
};

// "text" with quotes and escapes, for building request bodies.
std::string Quote(const std::string &text);

}  // namespace json
