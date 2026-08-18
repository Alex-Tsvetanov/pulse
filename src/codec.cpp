#include "pulse/codec.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace pulse::codec {
namespace {

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

bool is_name_start(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':';
}

bool is_name_char(char c) {
  return is_name_start(c) || (c >= '0' && c <= '9') || c == '-' || c == '.';
}

void append_utf8(std::string& out, unsigned int code_point) {
  if (code_point < 0x80) {
    out.push_back(static_cast<char>(code_point));
  } else if (code_point < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else if (code_point < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  }
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// ---------------------------------------------------------------------------
// JSON reader
// ---------------------------------------------------------------------------
class JsonReader {
 public:
  explicit JsonReader(std::string_view text) : text_(text) {}

  bool document(Json& out) {
    skip_space();
    if (!value(out, 0)) return false;
    skip_space();
    return position_ == text_.size();
  }

 private:
  // Nesting is bounded so that a hostile document cannot drive the recursion into the
  // stack guard page.
  static constexpr int kMaxDepth = 64;

  void skip_space() {
    while (position_ < text_.size() && is_space(text_[position_])) ++position_;
  }

  bool literal(std::string_view word) {
    if (text_.compare(position_, word.size(), word) != 0) return false;
    position_ += word.size();
    return true;
  }

  bool value(Json& out, int depth) {
    if (depth > kMaxDepth || position_ >= text_.size()) return false;
    switch (text_[position_]) {
      case 'n':
        out.kind = Json::Kind::Null;
        return literal("null");
      case 't':
        out.kind = Json::Kind::Boolean;
        out.boolean = true;
        return literal("true");
      case 'f':
        out.kind = Json::Kind::Boolean;
        out.boolean = false;
        return literal("false");
      case '"':
        out.kind = Json::Kind::String;
        return string(out.text);
      case '[':
        return array(out, depth);
      case '{':
        return object(out, depth);
      default:
        return number_value(out);
    }
  }

  bool string(std::string& out) {
    if (position_ >= text_.size() || text_[position_] != '"') return false;
    ++position_;
    out.clear();
    while (position_ < text_.size()) {
      const char c = text_[position_++];
      if (c == '"') return true;
      if (c != '\\') {
        // Control characters must be escaped in a JSON string, RFC 8259 section 7.
        if (static_cast<unsigned char>(c) < 0x20) return false;
        out.push_back(c);
        continue;
      }
      if (position_ >= text_.size()) return false;
      const char escape = text_[position_++];
      switch (escape) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          if (position_ + 4 > text_.size()) return false;
          unsigned int code = 0;
          for (int i = 0; i < 4; ++i) {
            const int digit = hex_value(text_[position_ + i]);
            if (digit < 0) return false;
            code = code * 16 + static_cast<unsigned int>(digit);
          }
          position_ += 4;
          // A leading surrogate is only meaningful with its trailing partner.
          if (code >= 0xD800 && code <= 0xDBFF && position_ + 6 <= text_.size() &&
              text_[position_] == '\\' && text_[position_ + 1] == 'u') {
            unsigned int low = 0;
            bool ok = true;
            for (int i = 0; i < 4; ++i) {
              const int digit = hex_value(text_[position_ + 2 + i]);
              if (digit < 0) ok = false;
              low = low * 16 + static_cast<unsigned int>(digit < 0 ? 0 : digit);
            }
            if (ok && low >= 0xDC00 && low <= 0xDFFF) {
              position_ += 6;
              code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
            }
          }
          append_utf8(out, code);
          break;
        }
        default:
          return false;
      }
    }
    return false;
  }

  bool number_value(Json& out) {
    const std::size_t start = position_;
    if (position_ < text_.size() && text_[position_] == '-') ++position_;
    if (position_ >= text_.size()) return false;
    if (text_[position_] == '0') {
      ++position_;
    } else if (text_[position_] >= '1' && text_[position_] <= '9') {
      while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
        ++position_;
      }
    } else {
      return false;
    }
    if (position_ < text_.size() && text_[position_] == '.') {
      ++position_;
      const std::size_t digits = position_;
      while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
        ++position_;
      }
      if (position_ == digits) return false;
    }
    if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
      ++position_;
      if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) {
        ++position_;
      }
      const std::size_t digits = position_;
      while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
        ++position_;
      }
      if (position_ == digits) return false;
    }
    const std::string digits(text_.substr(start, position_ - start));
    out.kind = Json::Kind::Number;
    out.number = std::strtod(digits.c_str(), nullptr);
    return true;
  }

  bool array(Json& out, int depth) {
    out.kind = Json::Kind::Array;
    ++position_;  // consume the opening bracket
    skip_space();
    if (position_ < text_.size() && text_[position_] == ']') {
      ++position_;
      return true;
    }
    while (true) {
      Json item;
      skip_space();
      if (!value(item, depth + 1)) return false;
      out.items.push_back(std::move(item));
      skip_space();
      if (position_ >= text_.size()) return false;
      if (text_[position_] == ',') {
        ++position_;
        continue;
      }
      if (text_[position_] == ']') {
        ++position_;
        return true;
      }
      return false;
    }
  }

  bool object(Json& out, int depth) {
    out.kind = Json::Kind::Object;
    ++position_;  // consume the opening brace
    skip_space();
    if (position_ < text_.size() && text_[position_] == '}') {
      ++position_;
      return true;
    }
    while (true) {
      skip_space();
      std::string key;
      if (!string(key)) return false;
      skip_space();
      if (position_ >= text_.size() || text_[position_] != ':') return false;
      ++position_;
      skip_space();
      Json member;
      if (!value(member, depth + 1)) return false;
      out.members.emplace_back(std::move(key), std::move(member));
      skip_space();
      if (position_ >= text_.size()) return false;
      if (text_[position_] == ',') {
        ++position_;
        continue;
      }
      if (text_[position_] == '}') {
        ++position_;
        return true;
      }
      return false;
    }
  }

  std::string_view text_;
  std::size_t position_ = 0;
};

// ---------------------------------------------------------------------------
// XML reader
// ---------------------------------------------------------------------------
class XmlReader {
 public:
  explicit XmlReader(std::string_view text) : text_(text) {}

  bool document(XmlNode& out) {
    skip_prolog();
    if (!element(out, 0)) return false;
    skip_misc();
    return position_ == text_.size();
  }

 private:
  static constexpr int kMaxDepth = 64;

  void skip_space() {
    while (position_ < text_.size() && is_space(text_[position_])) ++position_;
  }

  bool starts_with(std::string_view word) const {
    return text_.compare(position_, word.size(), word) == 0;
  }

  void skip_misc() {
    while (position_ < text_.size()) {
      skip_space();
      if (starts_with("<!--")) {
        const std::size_t end = text_.find("-->", position_);
        if (end == std::string_view::npos) {
          position_ = text_.size();
          return;
        }
        position_ = end + 3;
        continue;
      }
      return;
    }
  }

  void skip_prolog() {
    skip_space();
    if (starts_with("<?")) {
      const std::size_t end = text_.find("?>", position_);
      position_ = end == std::string_view::npos ? text_.size() : end + 2;
    }
    skip_misc();
  }

  std::string name() {
    const std::size_t start = position_;
    if (position_ >= text_.size() || !is_name_start(text_[position_])) return {};
    ++position_;
    while (position_ < text_.size() && is_name_char(text_[position_])) ++position_;
    return std::string(text_.substr(start, position_ - start));
  }

  // Resolves the five predefined entities and numeric character references. An unknown
  // entity is an error, because silently passing it through would corrupt the value.
  bool decode_text(std::string_view raw, std::string& out) {
    out.clear();
    for (std::size_t i = 0; i < raw.size(); ++i) {
      if (raw[i] != '&') {
        out.push_back(raw[i]);
        continue;
      }
      const std::size_t semicolon = raw.find(';', i);
      if (semicolon == std::string_view::npos) return false;
      const std::string_view entity = raw.substr(i + 1, semicolon - i - 1);
      if (entity == "lt") {
        out.push_back('<');
      } else if (entity == "gt") {
        out.push_back('>');
      } else if (entity == "amp") {
        out.push_back('&');
      } else if (entity == "quot") {
        out.push_back('"');
      } else if (entity == "apos") {
        out.push_back('\'');
      } else if (!entity.empty() && entity[0] == '#') {
        unsigned int code = 0;
        if (entity.size() > 2 && (entity[1] == 'x' || entity[1] == 'X')) {
          for (std::size_t k = 2; k < entity.size(); ++k) {
            const int digit = hex_value(entity[k]);
            if (digit < 0) return false;
            code = code * 16 + static_cast<unsigned int>(digit);
          }
        } else {
          for (std::size_t k = 1; k < entity.size(); ++k) {
            if (entity[k] < '0' || entity[k] > '9') return false;
            code = code * 10 + static_cast<unsigned int>(entity[k] - '0');
          }
        }
        append_utf8(out, code);
      } else {
        return false;
      }
      i = semicolon;
    }
    return true;
  }

  bool attributes(XmlNode& out) {
    while (true) {
      skip_space();
      if (position_ >= text_.size()) return false;
      if (text_[position_] == '>' || starts_with("/>")) return true;
      const std::string key = name();
      if (key.empty()) return false;
      skip_space();
      if (position_ >= text_.size() || text_[position_] != '=') return false;
      ++position_;
      skip_space();
      if (position_ >= text_.size() || (text_[position_] != '"' && text_[position_] != '\'')) {
        return false;
      }
      const char quote = text_[position_++];
      const std::size_t start = position_;
      while (position_ < text_.size() && text_[position_] != quote) ++position_;
      if (position_ >= text_.size()) return false;
      std::string value;
      if (!decode_text(text_.substr(start, position_ - start), value)) return false;
      ++position_;
      out.attributes.emplace_back(key, std::move(value));
    }
  }

  bool element(XmlNode& out, int depth) {
    if (depth > kMaxDepth) return false;
    if (position_ >= text_.size() || text_[position_] != '<') return false;
    ++position_;
    out.name = name();
    if (out.name.empty()) return false;
    if (!attributes(out)) return false;

    if (starts_with("/>")) {
      position_ += 2;
      return true;
    }
    if (position_ >= text_.size() || text_[position_] != '>') return false;
    ++position_;

    std::string raw_text;
    while (position_ < text_.size()) {
      if (text_[position_] != '<') {
        const std::size_t start = position_;
        while (position_ < text_.size() && text_[position_] != '<') ++position_;
        raw_text.append(text_.substr(start, position_ - start));
        continue;
      }
      if (starts_with("</")) {
        position_ += 2;
        const std::string closing = name();
        skip_space();
        if (closing != out.name || position_ >= text_.size() || text_[position_] != '>') {
          return false;
        }
        ++position_;
        return decode_text(raw_text, out.text);
      }
      if (starts_with("<!--")) {
        const std::size_t end = text_.find("-->", position_);
        if (end == std::string_view::npos) return false;
        position_ = end + 3;
        continue;
      }
      XmlNode child;
      if (!element(child, depth + 1)) return false;
      out.children.push_back(std::move(child));
    }
    return false;  // the document ended before the closing tag
  }

  std::string_view text_;
  std::size_t position_ = 0;
};

}  // namespace

std::string escape_json(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buffer[7];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(c));
          out += buffer;
        } else {
          out.push_back(c);
        }
    }
  }
  return out;
}

std::string escape_xml(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (char c : text) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default: out.push_back(c);
    }
  }
  return out;
}

std::string number(double value, int decimals) {
  if (!std::isfinite(value)) value = 0.0;
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
  return buffer;
}

std::string to_json(const metrics::Snapshot& s) {
  std::string out;
  out.reserve(768);
  out += "{\"seq\":";
  out += std::to_string(s.sequence);
  out += ",\"ts\":";
  out += std::to_string(s.wall_microseconds);
  out += ",\"uptime\":";
  out += number(s.uptime_seconds, 3);
  out += ",\"cpu\":";
  out += number(s.cpu_percent, 2);
  out += ",\"rss\":";
  out += std::to_string(s.rss_bytes);
  out += ",\"requests\":";
  out += std::to_string(s.requests_total);
  out += ",\"rps\":";
  out += number(s.requests_per_second, 2);
  out += ",\"bytes\":";
  out += std::to_string(s.bytes_sent);
  out += ",\"ws\":";
  out += std::to_string(s.websocket_clients);
  out += ",\"sse\":";
  out += std::to_string(s.sse_clients);
  out += ",\"p50\":";
  out += number(s.latency_p50, 2);
  out += ",\"p90\":";
  out += number(s.latency_p90, 2);
  out += ",\"p99\":";
  out += number(s.latency_p99, 2);
  out += ",\"histogram\":[";
  const auto& bounds = metrics::Histogram::bounds();
  for (std::size_t i = 0; i < metrics::Histogram::kBucketCount; ++i) {
    if (i > 0) out += ',';
    out += "{\"le\":";
    if (i < bounds.size()) {
      out += number(bounds[i], 0);
    } else {
      out += "null";  // the open ended bucket has no upper bound
    }
    out += ",\"count\":";
    out += std::to_string(s.histogram.buckets()[i]);
    out += '}';
  }
  out += "]}";
  return out;
}

std::string to_xml(const metrics::Snapshot& s) {
  std::string out;
  out.reserve(1024);
  out += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>";
  out += "<snapshot><seq>";
  out += std::to_string(s.sequence);
  out += "</seq><ts>";
  out += std::to_string(s.wall_microseconds);
  out += "</ts><uptime>";
  out += number(s.uptime_seconds, 3);
  out += "</uptime><cpu>";
  out += number(s.cpu_percent, 2);
  out += "</cpu><rss>";
  out += std::to_string(s.rss_bytes);
  out += "</rss><requests>";
  out += std::to_string(s.requests_total);
  out += "</requests><rps>";
  out += number(s.requests_per_second, 2);
  out += "</rps><bytes>";
  out += std::to_string(s.bytes_sent);
  out += "</bytes><ws>";
  out += std::to_string(s.websocket_clients);
  out += "</ws><sse>";
  out += std::to_string(s.sse_clients);
  out += "</sse><p50>";
  out += number(s.latency_p50, 2);
  out += "</p50><p90>";
  out += number(s.latency_p90, 2);
  out += "</p90><p99>";
  out += number(s.latency_p99, 2);
  out += "</p99><histogram>";
  const auto& bounds = metrics::Histogram::bounds();
  for (std::size_t i = 0; i < metrics::Histogram::kBucketCount; ++i) {
    out += "<bucket le=\"";
    out += i < bounds.size() ? number(bounds[i], 0) : std::string("inf");
    out += "\">";
    out += std::to_string(s.histogram.buckets()[i]);
    out += "</bucket>";
  }
  out += "</histogram></snapshot>";
  return out;
}

const Json* Json::find(std::string_view key) const {
  for (const auto& member : members) {
    if (member.first == key) return &member.second;
  }
  return nullptr;
}

std::size_t Json::node_count() const {
  std::size_t total = 1;
  for (const auto& item : items) total += item.node_count();
  for (const auto& member : members) total += member.second.node_count();
  return total;
}

const XmlNode* XmlNode::child(std::string_view child_name) const {
  for (const auto& node : children) {
    if (node.name == child_name) return &node;
  }
  return nullptr;
}

std::string_view XmlNode::attribute(std::string_view attribute_name) const {
  for (const auto& entry : attributes) {
    if (entry.first == attribute_name) return entry.second;
  }
  return {};
}

std::size_t XmlNode::node_count() const {
  // An attribute is a node of the XML information set, so it is counted. Leaving it out
  // would understate the tree next to the JSON one, where the same fact costs a member.
  std::size_t total = 1 + attributes.size();
  for (const auto& node : children) total += node.node_count();
  return total;
}

std::optional<Json> parse_json(std::string_view text) {
  Json root;
  JsonReader reader(text);
  if (!reader.document(root)) return std::nullopt;
  return root;
}

std::optional<XmlNode> parse_xml(std::string_view text) {
  XmlNode root;
  XmlReader reader(text);
  if (!reader.document(root)) return std::nullopt;
  return root;
}

}  // namespace pulse::codec
