// The two wire formats behind one contract.
//
// The same Snapshot is written as JSON (RFC 8259) and as XML (XML 1.0), and both are
// read back into a document tree. Encoding and decoding exist on both sides so the two
// representations can be compared on payload size and on parse cost with the same kind
// of work happening in each case: a full tree is built either way, which is what
// JSON.parse and DOMParser do in the browser.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pulse/metrics.hpp"

namespace pulse::codec {

std::string escape_json(std::string_view text);
std::string escape_xml(std::string_view text);

std::string to_json(const metrics::Snapshot& snapshot);
std::string to_xml(const metrics::Snapshot& snapshot);

// Fixed point rendering with a fixed number of decimals. Both encoders use it so that
// a size difference between the formats comes from the markup and not from a number
// that happened to be printed with more digits in one of them.
std::string number(double value, int decimals);

struct Json {
  enum class Kind { Null, Boolean, Number, String, Array, Object };

  Kind kind = Kind::Null;
  bool boolean = false;
  double number = 0.0;
  std::string text;
  std::vector<Json> items;                            // Array
  std::vector<std::pair<std::string, Json>> members;  // Object

  const Json* find(std::string_view key) const;
  std::size_t node_count() const;
};

// Returns nothing when the text is not a complete, well formed JSON document. Trailing
// content after the top level value is rejected.
std::optional<Json> parse_json(std::string_view text);

struct XmlNode {
  std::string name;
  std::vector<std::pair<std::string, std::string>> attributes;
  std::string text;
  std::vector<XmlNode> children;

  const XmlNode* child(std::string_view name) const;
  std::string_view attribute(std::string_view name) const;
  std::size_t node_count() const;
};

// Handles the subset the project emits: a declaration, elements, attributes, character
// data, the five predefined entities and numeric character references, empty element
// tags and comments. Anything outside that subset is rejected rather than guessed at.
std::optional<XmlNode> parse_xml(std::string_view text);

}  // namespace pulse::codec
