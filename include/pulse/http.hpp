// HTTP/1.1 message handling: request parsing, response building, MIME lookup and the
// small helpers the router needs. Nothing here touches a socket, which is what makes
// the parser testable without a network.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulse::http {

struct Request {
  std::string method;
  std::string target;  // the raw request target, query string included
  std::string path;    // percent-decoded path, query string removed
  std::string query;   // the part after the question mark, still encoded
  int minor_version = 1;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;

  // Header names are case-insensitive per RFC 9110, so lookup is too.
  std::string_view header(std::string_view name) const;
  bool has_token(std::string_view name, std::string_view token) const;
  bool keep_alive() const;
};

enum class Parse {
  Incomplete,  // the buffer holds part of a message, wait for more bytes
  Ok,
  Error,
};

// Parses one request from the front of buffer. On Ok, consumed is set to the number of
// bytes the message occupied, so the caller can support keep-alive by erasing exactly
// that prefix and trying again.
Parse parse_request(std::string_view buffer, Request& out, std::size_t& consumed);

struct Response {
  int status = 200;
  std::string content_type = "text/plain; charset=utf-8";
  std::string body;
  std::vector<std::pair<std::string, std::string>> extra_headers;
};

std::string serialize(const Response& response, bool keep_alive);
std::string_view status_text(int status);
std::string_view mime_for(std::string_view path);

// Chunked transfer coding, RFC 9112 section 7.1. An empty payload produces the
// terminating chunk, which is how a streaming response is closed cleanly.
std::string chunk(std::string_view payload);

std::string percent_decode(std::string_view text);
std::optional<std::string> query_param(std::string_view query, std::string_view key);

// True when the path can be joined to a document root without escaping it. Rejects
// absolute paths, backslashes and any parent segment, before any file system call.
bool is_safe_path(std::string_view path);

}  // namespace pulse::http
