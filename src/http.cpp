#include "pulse/http.hpp"

#include <algorithm>
#include <cctype>

namespace pulse::http {
namespace {

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

bool iequal(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (lower(a[i]) != lower(b[i])) return false;
  }
  return true;
}

std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return text;
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// A header section or body larger than these is treated as an attack rather than as a
// message, because no client of this server sends one. The check happens before any
// allocation that depends on the declared length.
constexpr std::size_t kMaxHeaderSection = 16 * 1024;
constexpr std::size_t kMaxBody = 1024 * 1024;

}  // namespace

std::string_view Request::header(std::string_view name) const {
  for (const auto& entry : headers) {
    if (iequal(entry.first, name)) return entry.second;
  }
  return {};
}

bool Request::has_token(std::string_view name, std::string_view token) const {
  const std::string_view value = header(name);
  std::size_t start = 0;
  while (start <= value.size()) {
    const std::size_t comma = value.find(',', start);
    const std::size_t end = comma == std::string_view::npos ? value.size() : comma;
    if (iequal(trim(value.substr(start, end - start)), token)) return true;
    if (comma == std::string_view::npos) break;
    start = comma + 1;
  }
  return false;
}

bool Request::keep_alive() const {
  if (has_token("Connection", "close")) return false;
  if (minor_version == 0) return has_token("Connection", "keep-alive");
  return true;  // HTTP/1.1 defaults to a persistent connection
}

Parse parse_request(std::string_view buffer, Request& out, std::size_t& consumed) {
  const std::size_t head_end = buffer.find("\r\n\r\n");
  if (head_end == std::string_view::npos) {
    return buffer.size() > kMaxHeaderSection ? Parse::Error : Parse::Incomplete;
  }
  if (head_end > kMaxHeaderSection) return Parse::Error;

  const std::string_view head = buffer.substr(0, head_end);

  const std::size_t line_end = head.find("\r\n");
  const std::string_view request_line =
      head.substr(0, line_end == std::string_view::npos ? head.size() : line_end);
  const std::size_t first_space = request_line.find(' ');
  if (first_space == std::string_view::npos) return Parse::Error;
  const std::size_t second_space = request_line.find(' ', first_space + 1);
  if (second_space == std::string_view::npos) return Parse::Error;

  out = Request{};
  out.method = std::string(request_line.substr(0, first_space));
  out.target = std::string(request_line.substr(first_space + 1, second_space - first_space - 1));
  const std::string_view version = request_line.substr(second_space + 1);
  if (version == "HTTP/1.1") {
    out.minor_version = 1;
  } else if (version == "HTTP/1.0") {
    out.minor_version = 0;
  } else {
    return Parse::Error;
  }
  if (out.method.empty() || out.target.empty()) return Parse::Error;

  const std::size_t question = out.target.find('?');
  if (question == std::string::npos) {
    out.path = percent_decode(out.target);
  } else {
    out.path = percent_decode(std::string_view(out.target).substr(0, question));
    out.query = out.target.substr(question + 1);
  }

  std::size_t cursor = line_end == std::string_view::npos ? head.size() : line_end + 2;
  while (cursor < head.size()) {
    std::size_t next = head.find("\r\n", cursor);
    if (next == std::string_view::npos) next = head.size();
    const std::string_view line = head.substr(cursor, next - cursor);
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) return Parse::Error;
    out.headers.emplace_back(std::string(trim(line.substr(0, colon))),
                             std::string(trim(line.substr(colon + 1))));
    cursor = next + 2;
  }

  std::size_t body_length = 0;
  const std::string_view length_header = out.header("Content-Length");
  if (!length_header.empty()) {
    for (char c : length_header) {
      if (c < '0' || c > '9') return Parse::Error;
      body_length = body_length * 10 + static_cast<std::size_t>(c - '0');
      if (body_length > kMaxBody) return Parse::Error;
    }
  }

  const std::size_t body_start = head_end + 4;
  if (buffer.size() < body_start + body_length) return Parse::Incomplete;
  out.body.assign(buffer.substr(body_start, body_length));
  consumed = body_start + body_length;
  return Parse::Ok;
}

std::string_view status_text(int status) {
  switch (status) {
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Content Too Large";
    case 426: return "Upgrade Required";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    default: return "Unknown";
  }
}

std::string serialize(const Response& response, bool keep_alive) {
  std::string out = "HTTP/1.1 ";
  out += std::to_string(response.status);
  out += ' ';
  out += status_text(response.status);
  out += "\r\n";
  out += "Content-Type: ";
  out += response.content_type;
  out += "\r\nContent-Length: ";
  out += std::to_string(response.body.size());
  out += "\r\nConnection: ";
  out += keep_alive ? "keep-alive" : "close";
  out += "\r\n";
  for (const auto& entry : response.extra_headers) {
    out += entry.first;
    out += ": ";
    out += entry.second;
    out += "\r\n";
  }
  out += "\r\n";
  out += response.body;
  return out;
}

std::string_view mime_for(std::string_view path) {
  const std::size_t dot = path.rfind('.');
  if (dot == std::string_view::npos) return "application/octet-stream";
  std::string extension;
  for (char c : path.substr(dot)) extension.push_back(lower(c));
  if (extension == ".html" || extension == ".htm") return "text/html; charset=utf-8";
  if (extension == ".css") return "text/css; charset=utf-8";
  if (extension == ".js" || extension == ".mjs") return "text/javascript; charset=utf-8";
  if (extension == ".json") return "application/json; charset=utf-8";
  if (extension == ".xml") return "application/xml; charset=utf-8";
  if (extension == ".svg") return "image/svg+xml";
  if (extension == ".png") return "image/png";
  if (extension == ".ico") return "image/x-icon";
  if (extension == ".txt") return "text/plain; charset=utf-8";
  return "application/octet-stream";
}

std::string chunk(std::string_view payload) {
  static const char* const digits = "0123456789abcdef";
  std::string size_line;
  std::size_t size = payload.size();
  if (size == 0) {
    size_line = "0";
  } else {
    while (size > 0) {
      size_line.insert(size_line.begin(), digits[size & 0xF]);
      size >>= 4;
    }
  }
  std::string out = size_line;
  out += "\r\n";
  out.append(payload);
  out += "\r\n";
  return out;
}

std::string percent_decode(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size()) {
      const int high = hex_value(text[i + 1]);
      const int low = hex_value(text[i + 2]);
      if (high >= 0 && low >= 0) {
        out.push_back(static_cast<char>(high * 16 + low));
        i += 2;
        continue;
      }
    }
    out.push_back(text[i]);
  }
  return out;
}

std::optional<std::string> query_param(std::string_view query, std::string_view key) {
  std::size_t start = 0;
  while (start < query.size()) {
    std::size_t end = query.find('&', start);
    if (end == std::string_view::npos) end = query.size();
    const std::string_view pair = query.substr(start, end - start);
    const std::size_t equals = pair.find('=');
    const std::string_view name = equals == std::string_view::npos ? pair : pair.substr(0, equals);
    if (name == key) {
      const std::string_view value =
          equals == std::string_view::npos ? std::string_view{} : pair.substr(equals + 1);
      return percent_decode(value);
    }
    start = end + 1;
  }
  return std::nullopt;
}

bool is_safe_path(std::string_view path) {
  if (path.empty() || path.front() != '/') return false;
  if (path.find('\\') != std::string_view::npos) return false;
  if (path.find('\0') != std::string_view::npos) return false;
  // A drive letter would turn the join into an absolute path on Windows.
  if (path.size() >= 3 && path[2] == ':') return false;
  std::size_t start = 1;
  while (start <= path.size()) {
    std::size_t end = path.find('/', start);
    if (end == std::string_view::npos) end = path.size();
    const std::string_view segment = path.substr(start, end - start);
    if (segment == ".." || segment == ".") return false;
    start = end + 1;
  }
  return true;
}

}  // namespace pulse::http
