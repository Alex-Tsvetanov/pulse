#include "pulse/server.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "pulse/codec.hpp"
#include "pulse/http.hpp"
#include "pulse/net.hpp"
#include "pulse/websocket.hpp"

namespace pulse {
namespace {

// A client that stops reading must not be able to make the server grow without bound.
// Past this, the connection is dropped: the alternative is an out of memory kill caused
// by one slow peer.
constexpr std::size_t kMaxOutgoing = 1024 * 1024;
constexpr std::size_t kMaxIncoming = 64 * 1024;

enum class Mode { Http, WebSocket, EventStream };

struct Connection {
  net::Socket socket;
  std::string incoming;
  std::string outgoing;
  Mode mode = Mode::Http;
  bool close_after_write = false;
  bool wants_xml = false;
  bool subscribed = true;
  ws::Assembler assembler;
};

std::string read_file(const std::string& path, bool& ok) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    ok = false;
    return {};
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  ok = true;
  return buffer.str();
}

std::string join_path(const std::string& root, const std::string& path) {
  std::string out = root;
  if (!out.empty() && (out.back() == '/' || out.back() == '\\')) out.pop_back();
  out += path;
  return out;
}

}  // namespace

struct Server::State {
  Options options;
  net::Library library;
  net::Socket listener;
  std::vector<std::unique_ptr<Connection>> connections;
  metrics::Collector collector;
  metrics::Snapshot snapshot;
  std::string snapshot_json;
  std::string snapshot_xml;
  std::int64_t next_tick_us = 0;

  explicit State(Options opts) : options(std::move(opts)) {}

  void queue(Connection& connection, std::string_view data) {
    if (connection.outgoing.size() + data.size() > kMaxOutgoing) {
      connection.close_after_write = true;
      return;
    }
    connection.outgoing.append(data);
    collector.record_bytes_sent(data.size());
  }

  http::Response make_static(const std::string& path, bool& found) {
    http::Response response;
    if (!http::is_safe_path(path)) {
      found = false;
      response.status = 403;
      response.body = "Forbidden";
      return response;
    }
    const std::string file_path =
        join_path(options.web_root, path == "/" ? std::string("/index.html") : path);
    bool ok = false;
    std::string body = read_file(file_path, ok);
    if (!ok) {
      found = false;
      response.status = 404;
      response.body = "Not Found";
      return response;
    }
    found = true;
    response.status = 200;
    response.content_type =
        std::string(http::mime_for(path == "/" ? std::string("/index.html") : path));
    response.body = std::move(body);
    // The dashboard is meant to reflect the running process, so a cached copy would be
    // actively misleading during development.
    response.extra_headers.emplace_back("Cache-Control", "no-store");
    return response;
  }

  bool handshake(Connection& connection, const http::Request& request) {
    const std::string_view key = request.header("Sec-WebSocket-Key");
    const std::string_view version = request.header("Sec-WebSocket-Version");
    if (key.empty() || version != "13" || !request.has_token("Upgrade", "websocket") ||
        !request.has_token("Connection", "Upgrade")) {
      http::Response response;
      response.status = 426;
      response.body = "This endpoint requires a WebSocket upgrade, RFC 6455 version 13.";
      response.extra_headers.emplace_back("Sec-WebSocket-Version", "13");
      queue(connection, http::serialize(response, false));
      connection.close_after_write = true;
      return false;
    }

    std::string reply = "HTTP/1.1 101 Switching Protocols\r\n";
    reply += "Upgrade: websocket\r\nConnection: Upgrade\r\n";
    reply += "Sec-WebSocket-Accept: ";
    reply += ws::accept_key(key);
    reply += "\r\n\r\n";
    queue(connection, reply);
    connection.mode = Mode::WebSocket;
    return true;
  }

  void start_event_stream(Connection& connection, const http::Request& request) {
    connection.wants_xml = http::query_param(request.query, "format").value_or("json") == "xml";
    std::string head = "HTTP/1.1 200 OK\r\n";
    head += "Content-Type: text/event-stream; charset=utf-8\r\n";
    head += "Cache-Control: no-store\r\n";
    // The body has no length known in advance, so it is framed with the chunked
    // transfer coding rather than by closing the connection.
    head += "Transfer-Encoding: chunked\r\nConnection: keep-alive\r\n";
    head += "X-Accel-Buffering: no\r\n\r\n";
    queue(connection, head);
    queue(connection, http::chunk("retry: 1000\n\n"));
    connection.mode = Mode::EventStream;
    if (!snapshot_json.empty()) {
      queue(connection, http::chunk(event_text(connection.wants_xml)));
    }
  }

  std::string event_text(bool xml) const {
    std::string out = "id: ";
    out += std::to_string(snapshot.sequence);
    out += "\nevent: snapshot\ndata: ";
    out += xml ? snapshot_xml : snapshot_json;
    out += "\n\n";
    return out;
  }

  std::string info_json() const {
    std::string out = "{\"interval_ms\":";
    out += std::to_string(options.interval_ms);
    out += ",\"transports\":[\"websocket\",\"sse\",\"polling\"]";
    out += ",\"formats\":[\"json\",\"xml\"]";
    out += ",\"endpoints\":{\"websocket\":\"/ws\",\"sse\":\"/api/stream\",";
    out += "\"json\":\"/api/metrics.json\",\"xml\":\"/api/metrics.xml\"}}";
    return out;
  }

  // The routing table. Kept as an explicit chain rather than a map because the order
  // matters: the static file handler is the fallback and must run last.
  void route(Connection& connection, const http::Request& request) {
    const std::int64_t began = metrics::steady_microseconds();

    if (request.method != "GET" && request.method != "HEAD") {
      http::Response response;
      response.status = 405;
      response.body = "Only GET and HEAD are served.";
      response.extra_headers.emplace_back("Allow", "GET, HEAD");
      queue(connection, http::serialize(response, request.keep_alive()));
      finish(connection, request, began);
      return;
    }

    if (request.path == "/ws") {
      handshake(connection, request);
      finish(connection, request, began);
      return;
    }
    if (request.path == "/api/stream") {
      start_event_stream(connection, request);
      finish(connection, request, began);
      return;
    }

    http::Response response;
    if (request.path == "/api/metrics.json") {
      response.content_type = "application/json; charset=utf-8";
      response.body = snapshot_json;
    } else if (request.path == "/api/metrics.xml") {
      response.content_type = "application/xml; charset=utf-8";
      response.body = snapshot_xml;
    } else if (request.path == "/api/info") {
      response.content_type = "application/json; charset=utf-8";
      response.body = info_json();
    } else {
      bool found = false;
      response = make_static(request.path, found);
    }
    if (request.method == "HEAD") {
      // A HEAD response carries the headers a GET would produce, with no body. The
      // length header still describes the body the client did not ask for.
      const std::string full = http::serialize(response, request.keep_alive());
      const std::size_t head_end = full.find("\r\n\r\n");
      queue(connection, full.substr(0, head_end + 4));
    } else {
      queue(connection, http::serialize(response, request.keep_alive()));
    }
    finish(connection, request, began);
  }

  void finish(Connection& connection, const http::Request& request, std::int64_t began) {
    collector.record_request(static_cast<double>(metrics::steady_microseconds() - began));
    if (connection.mode == Mode::Http && !request.keep_alive()) {
      connection.close_after_write = true;
    }
  }

  void on_websocket_bytes(Connection& connection) {
    while (true) {
      ws::Frame frame;
      std::size_t consumed = 0;
      const ws::Decode result = ws::decode_frame(connection.incoming, frame, consumed);
      if (result == ws::Decode::Incomplete) return;
      if (result == ws::Decode::Error) {
        queue(connection, ws::encode_close(1002, "protocol error"));
        connection.close_after_write = true;
        return;
      }
      connection.incoming.erase(0, consumed);

      // RFC 6455 section 5.1: a frame from a client is always masked. An unmasked one
      // is not a client we should keep talking to.
      if (!frame.masked) {
        queue(connection, ws::encode_close(1002, "unmasked client frame"));
        connection.close_after_write = true;
        return;
      }

      switch (connection.assembler.feed(frame)) {
        case ws::Assembler::Event::NeedMore:
          break;
        case ws::Assembler::Event::Protocol:
          queue(connection, ws::encode_close(1002, "bad fragmentation"));
          connection.close_after_write = true;
          return;
        case ws::Assembler::Event::Control:
          handle_control(connection);
          if (connection.close_after_write) return;
          break;
        case ws::Assembler::Event::Message:
          handle_command(connection, connection.assembler.payload());
          break;
      }
    }
  }

  void handle_control(Connection& connection) {
    switch (connection.assembler.opcode()) {
      case ws::Opcode::Ping:
        queue(connection, ws::encode_frame(ws::Opcode::Pong, connection.assembler.payload()));
        break;
      case ws::Opcode::Close:
        queue(connection, ws::encode_close(1000, "bye"));
        connection.close_after_write = true;
        break;
      case ws::Opcode::Pong:
        break;  // an unsolicited pong is allowed and needs no answer
      default:
        break;
    }
  }

  void handle_command(Connection& connection, const std::string& text) {
    const auto parsed = codec::parse_json(text);
    std::string reply;
    if (!parsed || parsed->kind != codec::Json::Kind::Object) {
      reply = "{\"type\":\"error\",\"message\":\"expected a JSON object\"}";
      queue(connection, ws::encode_frame(ws::Opcode::Text, reply));
      return;
    }
    const codec::Json* command = parsed->find("cmd");
    const codec::Json* value = parsed->find("value");
    const std::string name =
        command != nullptr && command->kind == codec::Json::Kind::String ? command->text : "";

    if (name == "format" && value != nullptr && value->kind == codec::Json::Kind::String) {
      connection.wants_xml = value->text == "xml";
      reply = "{\"type\":\"ack\",\"cmd\":\"format\",\"value\":\"";
      reply += connection.wants_xml ? "xml" : "json";
      reply += "\"}";
    } else if (name == "subscribe") {
      connection.subscribed = true;
      reply = "{\"type\":\"ack\",\"cmd\":\"subscribe\",\"value\":true}";
    } else if (name == "unsubscribe") {
      connection.subscribed = false;
      reply = "{\"type\":\"ack\",\"cmd\":\"unsubscribe\",\"value\":false}";
    } else if (name == "interval" && value != nullptr && value->kind == codec::Json::Kind::Number) {
      // The interval is shared by every client, so it is clamped to a range that keeps
      // one client from making the server unusable for the rest.
      options.interval_ms = std::clamp(static_cast<int>(value->number), 50, 5000);
      reply = "{\"type\":\"ack\",\"cmd\":\"interval\",\"value\":";
      reply += std::to_string(options.interval_ms);
      reply += "}";
    } else {
      reply = "{\"type\":\"error\",\"message\":\"unknown command\"}";
    }
    queue(connection, ws::encode_frame(ws::Opcode::Text, reply));
  }

  void on_http_bytes(Connection& connection) {
    while (connection.mode == Mode::Http) {
      http::Request request;
      std::size_t consumed = 0;
      const http::Parse result = http::parse_request(connection.incoming, request, consumed);
      if (result == http::Parse::Incomplete) return;
      if (result == http::Parse::Error) {
        http::Response response;
        response.status = 400;
        response.body = "Bad Request";
        queue(connection, http::serialize(response, false));
        connection.close_after_write = true;
        return;
      }
      connection.incoming.erase(0, consumed);
      route(connection, request);
      if (connection.close_after_write) return;
    }
  }

  // Dispatches the buffered bytes according to the mode the connection is in, and keeps
  // going while progress is made. The loop matters because a client is allowed to send
  // the first WebSocket frame in the same packet as the upgrade request: the mode
  // changes halfway through the buffer, and the remainder belongs to the new protocol.
  void process(Connection& connection) {
    std::size_t before = 0;
    do {
      before = connection.incoming.size();
      switch (connection.mode) {
        case Mode::Http:
          on_http_bytes(connection);
          break;
        case Mode::WebSocket:
          on_websocket_bytes(connection);
          break;
        case Mode::EventStream:
          connection.incoming.clear();  // an event stream client sends nothing
          break;
      }
    } while (!connection.incoming.empty() && connection.incoming.size() != before &&
             !connection.close_after_write);
  }

  void tick() {
    collector.set_client_counts(
        static_cast<std::uint32_t>(std::count_if(
            connections.begin(), connections.end(),
            [](const auto& c) { return c->mode == Mode::WebSocket; })),
        static_cast<std::uint32_t>(std::count_if(
            connections.begin(), connections.end(),
            [](const auto& c) { return c->mode == Mode::EventStream; })));

    snapshot = collector.sample();
    snapshot_json = codec::to_json(snapshot);
    snapshot_xml = codec::to_xml(snapshot);

    // Each payload is framed once and the same bytes go to every client that wants it.
    const std::string json_frame = ws::encode_frame(ws::Opcode::Text, snapshot_json);
    const std::string xml_frame = ws::encode_frame(ws::Opcode::Text, snapshot_xml);
    const std::string json_event = http::chunk(event_text(false));
    const std::string xml_event = http::chunk(event_text(true));

    for (auto& connection : connections) {
      if (!connection->subscribed || connection->close_after_write) continue;
      if (connection->mode == Mode::WebSocket) {
        queue(*connection, connection->wants_xml ? xml_frame : json_frame);
      } else if (connection->mode == Mode::EventStream) {
        queue(*connection, connection->wants_xml ? xml_event : json_event);
      }
    }
  }
};

Server::Server(Options options) : state_(std::make_unique<State>(std::move(options))) {}
Server::~Server() = default;

std::uint16_t Server::port() const {
  return state_->listener.valid() ? net::local_port(state_->listener.get()) : 0;
}

const metrics::Snapshot& Server::last_snapshot() const { return state_->snapshot; }

bool Server::start(std::string& error) {
  state_->listener = net::listen_on(state_->options.port, error);
  if (!state_->listener.valid()) return false;
  net::set_nonblocking(state_->listener.get(), true);
  state_->next_tick_us = metrics::steady_microseconds();
  return true;
}

void Server::run() {
  while (!stopping_.load()) {
    poll(20);
  }
}

void Server::poll(int timeout_ms) {
  State& state = *state_;
  if (!state.listener.valid()) return;

  fd_set readable;
  fd_set writable;
  FD_ZERO(&readable);
  FD_ZERO(&writable);
  FD_SET(state.listener.get(), &readable);
  net::Handle highest = state.listener.get();

  for (auto& connection : state.connections) {
    const net::Handle handle = connection->socket.get();
    FD_SET(handle, &readable);
    if (!connection->outgoing.empty()) FD_SET(handle, &writable);
    if (handle > highest) highest = handle;
  }

  const std::int64_t now = metrics::steady_microseconds();
  const std::int64_t until_tick = state.next_tick_us - now;
  const int wait_ms =
      std::clamp(static_cast<int>(until_tick / 1000), 0, std::max(0, timeout_ms));

  timeval timeout{};
  timeout.tv_sec = wait_ms / 1000;
  timeout.tv_usec = (wait_ms % 1000) * 1000;
  ::select(static_cast<int>(highest) + 1, &readable, &writable, nullptr, &timeout);

  if (FD_ISSET(state.listener.get(), &readable)) {
    // One accept per pass keeps a burst of connections from starving the tick.
    while (state.connections.size() < FD_SETSIZE - 8) {
      net::Socket accepted = net::accept_on(state.listener.get());
      if (!accepted.valid()) break;
      net::set_nonblocking(accepted.get(), true);
      net::set_nodelay(accepted.get());
      auto connection = std::make_unique<Connection>();
      connection->socket = std::move(accepted);
      state.connections.push_back(std::move(connection));
    }
  }

  char buffer[16 * 1024];
  for (auto& connection : state.connections) {
    const net::Handle handle = connection->socket.get();
    if (FD_ISSET(handle, &readable)) {
      const long received = net::recv_some(handle, buffer, sizeof(buffer));
      if (received > 0) {
        if (connection->incoming.size() + static_cast<std::size_t>(received) > kMaxIncoming) {
          connection->close_after_write = true;
        } else {
          connection->incoming.append(buffer, static_cast<std::size_t>(received));
          state.process(*connection);
        }
      } else if (received == 0 || !net::last_error_was_would_block()) {
        connection->close_after_write = true;
        connection->outgoing.clear();
      }
    }

    if (!connection->outgoing.empty() &&
        (FD_ISSET(handle, &writable) || connection->close_after_write)) {
      const long sent =
          net::send_some(handle, connection->outgoing.data(), connection->outgoing.size());
      if (sent > 0) {
        connection->outgoing.erase(0, static_cast<std::size_t>(sent));
      } else if (sent < 0 && !net::last_error_was_would_block()) {
        connection->outgoing.clear();
        connection->close_after_write = true;
      }
    }
  }

  state.connections.erase(
      std::remove_if(state.connections.begin(), state.connections.end(),
                     [](const std::unique_ptr<Connection>& connection) {
                       return connection->close_after_write && connection->outgoing.empty();
                     }),
      state.connections.end());

  if (metrics::steady_microseconds() >= state.next_tick_us) {
    state.tick();
    const std::int64_t period = static_cast<std::int64_t>(state.options.interval_ms) * 1000;
    state.next_tick_us += period;
    // After a long pause the schedule would otherwise try to catch up with a burst.
    const std::int64_t current = metrics::steady_microseconds();
    if (state.next_tick_us < current) state.next_tick_us = current + period;
  }
}

}  // namespace pulse
