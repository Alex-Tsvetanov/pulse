// The unit and integration suite. Run with: ctest --test-dir build --output-on-failure
#include "check.hpp"

#include <array>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include "pulse/codec.hpp"
#include "pulse/http.hpp"
#include "pulse/metrics.hpp"
#include "pulse/net.hpp"
#include "pulse/server.hpp"
#include "pulse/websocket.hpp"

using namespace pulse;

namespace {

std::string hex(std::string_view raw) {
  static const char* const digits = "0123456789abcdef";
  std::string out;
  for (char c : raw) {
    const auto byte = static_cast<unsigned char>(c);
    out.push_back(digits[byte >> 4]);
    out.push_back(digits[byte & 0x0F]);
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// HTTP request parsing
// ---------------------------------------------------------------------------

TEST(http_parses_a_request_line_and_headers) {
  const std::string raw =
      "GET /api/metrics.json?format=xml&n=5 HTTP/1.1\r\n"
      "Host: localhost:8080\r\n"
      "User-Agent: probe\r\n"
      "\r\n";
  http::Request request;
  std::size_t consumed = 0;
  CHECK_EQ(http::parse_request(raw, request, consumed), http::Parse::Ok);
  CHECK_EQ(consumed, raw.size());
  CHECK_EQ(request.method, std::string("GET"));
  CHECK_EQ(request.path, std::string("/api/metrics.json"));
  CHECK_EQ(request.query, std::string("format=xml&n=5"));
  CHECK_EQ(std::string(request.header("host")), std::string("localhost:8080"));
  CHECK_EQ(request.headers.size(), std::size_t{2});
}

TEST(http_waits_for_the_rest_of_a_split_message) {
  const std::string whole = "POST /x HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello";
  http::Request request;
  std::size_t consumed = 0;
  // Every prefix short of the full message must ask for more bytes, never guess.
  for (std::size_t cut = 1; cut < whole.size(); ++cut) {
    CHECK_EQ(http::parse_request(whole.substr(0, cut), request, consumed),
             http::Parse::Incomplete);
  }
  CHECK_EQ(http::parse_request(whole, request, consumed), http::Parse::Ok);
  CHECK_EQ(request.body, std::string("hello"));
}

TEST(http_reports_how_much_it_consumed_so_pipelining_works) {
  const std::string two = "GET /a HTTP/1.1\r\n\r\nGET /b HTTP/1.1\r\n\r\n";
  http::Request request;
  std::size_t consumed = 0;
  CHECK_EQ(http::parse_request(two, request, consumed), http::Parse::Ok);
  CHECK_EQ(request.path, std::string("/a"));
  const std::string rest = two.substr(consumed);
  CHECK_EQ(http::parse_request(rest, request, consumed), http::Parse::Ok);
  CHECK_EQ(request.path, std::string("/b"));
  CHECK_EQ(consumed, rest.size());
}

TEST(http_rejects_malformed_messages) {
  http::Request request;
  std::size_t consumed = 0;
  CHECK_EQ(http::parse_request("GET\r\n\r\n", request, consumed), http::Parse::Error);
  CHECK_EQ(http::parse_request("GET / HTTP/2.0\r\n\r\n", request, consumed), http::Parse::Error);
  CHECK_EQ(http::parse_request("GET / HTTP/1.1\r\nBadHeader\r\n\r\n", request, consumed),
           http::Parse::Error);
  CHECK_EQ(http::parse_request("GET / HTTP/1.1\r\nContent-Length: 12x\r\n\r\n", request, consumed),
           http::Parse::Error);
  // A declared body larger than the cap is refused before anything is reserved for it.
  CHECK_EQ(http::parse_request("GET / HTTP/1.1\r\nContent-Length: 99999999\r\n\r\n", request,
                               consumed),
           http::Parse::Error);
}

TEST(http_connection_persistence_follows_the_version_default) {
  const auto parse = [](const std::string& raw) {
    http::Request request;
    std::size_t consumed = 0;
    http::parse_request(raw, request, consumed);
    return request;
  };
  CHECK(parse("GET / HTTP/1.1\r\n\r\n").keep_alive());
  CHECK(!parse("GET / HTTP/1.1\r\nConnection: close\r\n\r\n").keep_alive());
  CHECK(!parse("GET / HTTP/1.0\r\n\r\n").keep_alive());
  CHECK(parse("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n").keep_alive());
  // The upgrade request lists two tokens in one header field.
  const http::Request upgrade = parse("GET /ws HTTP/1.1\r\nConnection: keep-alive, Upgrade\r\n\r\n");
  CHECK(upgrade.has_token("Connection", "upgrade"));
  CHECK(upgrade.has_token("Connection", "KEEP-ALIVE"));
  CHECK(!upgrade.has_token("Connection", "close"));
}

TEST(http_decodes_percent_escapes_and_query_parameters) {
  CHECK_EQ(http::percent_decode("/a%20b%2Fc"), std::string("/a b/c"));
  CHECK_EQ(http::percent_decode("100%%"), std::string("100%%"));
  CHECK_EQ(http::query_param("format=xml&interval=500", "interval").value_or(""),
           std::string("500"));
  CHECK_EQ(http::query_param("format=xml", "missing").has_value(), false);
  CHECK_EQ(http::query_param("flag&x=1", "flag").value_or("unset"), std::string(""));
}

TEST(http_static_paths_cannot_escape_the_document_root) {
  CHECK(http::is_safe_path("/index.html"));
  CHECK(http::is_safe_path("/assets/app.css"));
  CHECK(!http::is_safe_path("/../secret"));
  CHECK(!http::is_safe_path("/a/../../b"));
  CHECK(!http::is_safe_path("/a/./b"));
  CHECK(!http::is_safe_path("index.html"));
  CHECK(!http::is_safe_path("/C:/windows"));
  CHECK(!http::is_safe_path("/a\\b"));
  CHECK(!http::is_safe_path(std::string_view("/a\0b", 4)));
}

TEST(http_response_carries_a_length_and_the_right_status_words) {
  http::Response response;
  response.status = 404;
  response.content_type = "text/plain; charset=utf-8";
  response.body = "Not Found";
  const std::string wire = http::serialize(response, false);
  CHECK(wire.rfind("HTTP/1.1 404 Not Found\r\n", 0) == 0);
  CHECK(wire.find("Content-Length: 9\r\n") != std::string::npos);
  CHECK(wire.find("Connection: close\r\n") != std::string::npos);
  CHECK(wire.substr(wire.size() - 9) == "Not Found");
  CHECK_EQ(std::string(http::status_text(101)), std::string("Switching Protocols"));
  CHECK_EQ(std::string(http::status_text(426)), std::string("Upgrade Required"));
}

TEST(http_chunked_coding_sizes_in_hexadecimal) {
  CHECK_EQ(http::chunk("hello"), std::string("5\r\nhello\r\n"));
  CHECK_EQ(http::chunk(std::string(255, 'x')).substr(0, 4), std::string("ff\r\n"));
  CHECK_EQ(http::chunk(std::string(4096, 'x')).substr(0, 6), std::string("1000\r\n"));
  CHECK_EQ(http::chunk(""), std::string("0\r\n\r\n"));
}

TEST(http_maps_extensions_to_media_types) {
  CHECK_EQ(std::string(http::mime_for("/index.html")), std::string("text/html; charset=utf-8"));
  CHECK_EQ(std::string(http::mime_for("/app.CSS")), std::string("text/css; charset=utf-8"));
  CHECK_EQ(std::string(http::mime_for("/app.js")), std::string("text/javascript; charset=utf-8"));
  CHECK_EQ(std::string(http::mime_for("/m.xml")), std::string("application/xml; charset=utf-8"));
  CHECK_EQ(std::string(http::mime_for("/noextension")), std::string("application/octet-stream"));
}

// ---------------------------------------------------------------------------
// WebSocket
// ---------------------------------------------------------------------------

TEST(websocket_hash_and_encoding_match_the_published_vectors) {
  CHECK_EQ(hex(ws::sha1("")), std::string("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
  CHECK_EQ(hex(ws::sha1("abc")), std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));
  CHECK_EQ(hex(ws::sha1(std::string(1000, 'a'))).size(), std::size_t{40});
  CHECK_EQ(ws::base64_encode(""), std::string(""));
  CHECK_EQ(ws::base64_encode("f"), std::string("Zg=="));
  CHECK_EQ(ws::base64_encode("fo"), std::string("Zm8="));
  CHECK_EQ(ws::base64_encode("foobar"), std::string("Zm9vYmFy"));
}

TEST(websocket_handshake_reproduces_the_rfc_6455_example) {
  // RFC 6455 section 1.3 gives this key and this expected accept value.
  CHECK_EQ(ws::accept_key("dGhlIHNhbXBsZSBub25jZQ=="),
           std::string("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
}

TEST(websocket_decodes_the_masked_client_frame_from_the_rfc) {
  // RFC 6455 section 5.7: a masked single frame text message carrying "Hello".
  const unsigned char bytes[] = {0x81, 0x85, 0x37, 0xFA, 0x21, 0x3D,
                                 0x7F, 0x9F, 0x4D, 0x51, 0x58};
  const std::string_view raw(reinterpret_cast<const char*>(bytes), sizeof(bytes));
  ws::Frame frame;
  std::size_t consumed = 0;
  CHECK_EQ(ws::decode_frame(raw, frame, consumed), ws::Decode::Ok);
  CHECK_EQ(consumed, sizeof(bytes));
  CHECK(frame.fin);
  CHECK(frame.masked);
  CHECK_EQ(frame.opcode, ws::Opcode::Text);
  CHECK_EQ(frame.payload, std::string("Hello"));
}

TEST(websocket_server_frames_are_unmasked_and_round_trip) {
  const std::string encoded = ws::encode_frame(ws::Opcode::Text, "abc");
  CHECK_EQ(static_cast<unsigned>(static_cast<unsigned char>(encoded[0])), 0x81u);
  CHECK_EQ(static_cast<unsigned>(static_cast<unsigned char>(encoded[1])), 0x03u);

  const std::array<std::uint8_t, 4> mask = {0x11, 0x22, 0x33, 0x44};
  for (std::size_t size : {std::size_t{0}, std::size_t{1}, std::size_t{125}, std::size_t{126},
                           std::size_t{4096}, std::size_t{70000}}) {
    const std::string payload(size, 'p');
    const std::string wire = ws::encode_frame(ws::Opcode::Binary, payload, true, &mask);
    ws::Frame frame;
    std::size_t consumed = 0;
    CHECK_EQ(ws::decode_frame(wire, frame, consumed, 1 << 20), ws::Decode::Ok);
    CHECK_EQ(consumed, wire.size());
    CHECK_EQ(frame.payload.size(), size);
    CHECK(frame.payload == payload);
    CHECK(frame.masked);
  }
}

TEST(websocket_needs_more_bytes_before_a_frame_is_complete) {
  const std::string wire = ws::encode_frame(ws::Opcode::Text, std::string(300, 'z'));
  ws::Frame frame;
  std::size_t consumed = 0;
  for (std::size_t cut = 0; cut < wire.size(); ++cut) {
    CHECK_EQ(ws::decode_frame(wire.substr(0, cut), frame, consumed), ws::Decode::Incomplete);
  }
  CHECK_EQ(ws::decode_frame(wire, frame, consumed), ws::Decode::Ok);
}

TEST(websocket_rejects_frames_that_break_the_protocol) {
  ws::Frame frame;
  std::size_t consumed = 0;

  const std::string reserved_bit("\xC1\x00", 2);  // a reserved bit set, no extension agreed
  CHECK_EQ(ws::decode_frame(reserved_bit, frame, consumed), ws::Decode::Error);

  const std::string unknown_opcode("\x83\x00", 2);
  CHECK_EQ(ws::decode_frame(unknown_opcode, frame, consumed), ws::Decode::Error);

  const std::string fragmented_ping("\x09\x00", 2);  // control frame without FIN
  CHECK_EQ(ws::decode_frame(fragmented_ping, frame, consumed), ws::Decode::Error);

  const std::string long_control("\x89\x7E\x01\x00", 4);  // a ping of 256 bytes
  CHECK_EQ(ws::decode_frame(long_control, frame, consumed), ws::Decode::Error);

  const std::string non_minimal("\x81\x7E\x00\x05hello", 9);  // 5 bytes sent in the 16 bit form
  CHECK_EQ(ws::decode_frame(non_minimal, frame, consumed), ws::Decode::Error);

  // A declared length beyond the cap is refused instead of reserved.
  const std::string huge("\x81\x7F\x00\x00\x00\x00\xFF\xFF\xFF\xFF", 10);
  CHECK_EQ(ws::decode_frame(huge, frame, consumed, 1024), ws::Decode::Error);
}

TEST(websocket_assembler_joins_fragments_and_passes_control_frames_through) {
  ws::Assembler assembler;
  ws::Frame first{false, ws::Opcode::Text, false, "Hel"};
  ws::Frame ping{true, ws::Opcode::Ping, false, "beat"};
  ws::Frame rest{true, ws::Opcode::Continuation, false, "lo"};

  CHECK_EQ(assembler.feed(first), ws::Assembler::Event::NeedMore);
  // A control frame may arrive between two fragments and must not disturb them.
  CHECK_EQ(assembler.feed(ping), ws::Assembler::Event::Control);
  CHECK_EQ(assembler.opcode(), ws::Opcode::Ping);
  CHECK_EQ(assembler.payload(), std::string("beat"));
  CHECK_EQ(assembler.feed(rest), ws::Assembler::Event::Message);
  CHECK_EQ(assembler.opcode(), ws::Opcode::Text);
  CHECK_EQ(assembler.payload(), std::string("Hello"));
}

TEST(websocket_assembler_refuses_broken_fragment_sequences) {
  ws::Assembler orphan;
  CHECK_EQ(orphan.feed(ws::Frame{true, ws::Opcode::Continuation, false, "x"}),
           ws::Assembler::Event::Protocol);

  ws::Assembler interleaved;
  CHECK_EQ(interleaved.feed(ws::Frame{false, ws::Opcode::Text, false, "a"}),
           ws::Assembler::Event::NeedMore);
  CHECK_EQ(interleaved.feed(ws::Frame{true, ws::Opcode::Text, false, "b"}),
           ws::Assembler::Event::Protocol);

  ws::Assembler bounded;
  bounded.set_max_message(4);
  CHECK_EQ(bounded.feed(ws::Frame{false, ws::Opcode::Text, false, "abc"}),
           ws::Assembler::Event::NeedMore);
  CHECK_EQ(bounded.feed(ws::Frame{true, ws::Opcode::Continuation, false, "de"}),
           ws::Assembler::Event::Protocol);
}

TEST(websocket_close_frame_carries_the_status_code_first) {
  const std::string wire = ws::encode_close(1002, "protocol error");
  ws::Frame frame;
  std::size_t consumed = 0;
  CHECK_EQ(ws::decode_frame(wire, frame, consumed), ws::Decode::Ok);
  CHECK_EQ(frame.opcode, ws::Opcode::Close);
  const unsigned code = (static_cast<unsigned char>(frame.payload[0]) << 8) |
                        static_cast<unsigned char>(frame.payload[1]);
  CHECK_EQ(code, 1002u);
  CHECK_EQ(frame.payload.substr(2), std::string("protocol error"));
}

// ---------------------------------------------------------------------------
// Metrics
// ---------------------------------------------------------------------------

TEST(histogram_places_values_in_the_bucket_that_holds_them) {
  metrics::Histogram histogram;
  histogram.add(10);      // bucket 0, upper bound 50
  histogram.add(50);      // bucket 0, the bound itself belongs to the bucket
  histogram.add(51);      // bucket 1
  histogram.add(999999);  // the open ended bucket
  const auto& buckets = histogram.buckets();
  CHECK_EQ(buckets[0], std::uint64_t{2});
  CHECK_EQ(buckets[1], std::uint64_t{1});
  CHECK_EQ(buckets[metrics::Histogram::kBucketCount - 1], std::uint64_t{1});
  CHECK_EQ(histogram.count(), std::uint64_t{4});
  CHECK_EQ(histogram.max(), 999999.0);
}

TEST(histogram_quantiles_stay_inside_the_bucket_bounds) {
  metrics::Histogram empty;
  CHECK_EQ(empty.quantile(0.5), 0.0);

  metrics::Histogram histogram;
  for (int i = 0; i < 90; ++i) histogram.add(30.0);    // bucket 0, up to 50
  for (int i = 0; i < 10; ++i) histogram.add(1500.0);  // bucket 5, 1000 to 2000
  const double median = histogram.quantile(0.5);
  CHECK(median > 0.0 && median <= 50.0);
  const double tail = histogram.quantile(0.99);
  CHECK(tail > 1000.0 && tail <= 2000.0);
  CHECK_EQ(histogram.quantile(0.0), 0.0 + histogram.quantile(0.0));  // no crash at the ends
  CHECK(histogram.quantile(1.0) <= 2000.0);
}

TEST(collector_counts_requests_and_reads_the_process) {
  metrics::Collector collector;
  for (int i = 0; i < 5; ++i) collector.record_request(120.0);
  collector.record_bytes_sent(2048);
  collector.set_client_counts(3, 2);
  const metrics::Snapshot snapshot = collector.sample();
  CHECK_EQ(snapshot.sequence, std::uint64_t{1});
  CHECK_EQ(snapshot.requests_total, std::uint64_t{5});
  CHECK_EQ(snapshot.bytes_sent, std::uint64_t{2048});
  CHECK_EQ(snapshot.websocket_clients, 3u);
  CHECK_EQ(snapshot.sse_clients, 2u);
  CHECK(snapshot.wall_microseconds > 0);
  CHECK(snapshot.uptime_seconds >= 0.0);
  CHECK(snapshot.histogram.count() == 5);
  CHECK_EQ(collector.sample().sequence, std::uint64_t{2});
}

TEST(the_clocks_move_forward) {
  const std::int64_t before = metrics::steady_microseconds();
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  CHECK(metrics::steady_microseconds() - before >= 1000);
  CHECK(metrics::wall_microseconds() > 1'600'000'000'000'000LL);  // later than 2020
  CHECK(metrics::process_cpu_seconds() >= 0.0);
}

// ---------------------------------------------------------------------------
// Serialisation
// ---------------------------------------------------------------------------

TEST(escaping_covers_the_characters_that_would_break_each_format) {
  CHECK_EQ(codec::escape_json("a\"b\\c"), std::string("a\\\"b\\\\c"));
  CHECK_EQ(codec::escape_json("line\nbreak\ttab"), std::string("line\\nbreak\\ttab"));
  CHECK_EQ(codec::escape_json(std::string("\x01")), std::string("\\u0001"));
  CHECK_EQ(codec::escape_xml("a<b>c&d"), std::string("a&lt;b&gt;c&amp;d"));
  CHECK_EQ(codec::escape_xml("say \"hi\" it's"), std::string("say &quot;hi&quot; it&apos;s"));
  CHECK_EQ(codec::escape_xml("plain"), std::string("plain"));
}

TEST(json_reader_accepts_valid_documents_and_refuses_the_rest) {
  CHECK(codec::parse_json("{\"a\":1,\"b\":[true,false,null],\"c\":\"x\"}").has_value());
  CHECK(codec::parse_json("  [ 1 , 2.5 , -3e2 ]  ").has_value());
  CHECK(codec::parse_json("{}").has_value());
  CHECK(!codec::parse_json("{\"a\":1}trailing").has_value());
  CHECK(!codec::parse_json("{\"a\":}").has_value());
  CHECK(!codec::parse_json("{a:1}").has_value());
  CHECK(!codec::parse_json("[1,]").has_value());
  CHECK(!codec::parse_json("01").has_value());   // a leading zero is not allowed
  CHECK(!codec::parse_json("1.").has_value());   // a decimal point needs digits after it
  CHECK(!codec::parse_json("").has_value());
}

TEST(json_reader_resolves_escapes_including_surrogate_pairs) {
  const auto simple = codec::parse_json("{\"k\":\"a\\nb\\u0041\"}");
  CHECK(simple.has_value());
  CHECK_EQ(simple->find("k")->text, std::string("a\nbA"));

  const auto cyrillic = codec::parse_json("[\"\\u0414\"]");
  CHECK(cyrillic.has_value());
  CHECK_EQ(cyrillic->items[0].text, std::string("\xD0\x94"));

  // U+1F600 written as a surrogate pair must become one four byte sequence.
  const auto pair = codec::parse_json("[\"\\ud83d\\ude00\"]");
  CHECK(pair.has_value());
  CHECK_EQ(pair->items[0].text.size(), std::size_t{4});
}

TEST(xml_reader_handles_attributes_entities_and_empty_elements) {
  const auto document = codec::parse_xml(
      "<?xml version=\"1.0\"?><root n=\"2\"><a>x &amp; y</a><b/><!-- note --><c k='v'>7</c></root>");
  CHECK(document.has_value());
  CHECK_EQ(document->name, std::string("root"));
  CHECK_EQ(std::string(document->attribute("n")), std::string("2"));
  CHECK_EQ(document->children.size(), std::size_t{3});
  CHECK_EQ(document->child("a")->text, std::string("x & y"));
  CHECK_EQ(document->child("b")->children.size(), std::size_t{0});
  CHECK_EQ(std::string(document->child("c")->attribute("k")), std::string("v"));
  CHECK_EQ(document->child("c")->text, std::string("7"));

  const auto numeric = codec::parse_xml("<r>&#65;&#x42;</r>");
  CHECK(numeric.has_value());
  CHECK_EQ(numeric->text, std::string("AB"));
}

TEST(xml_reader_refuses_documents_it_cannot_trust) {
  CHECK(!codec::parse_xml("<a></b>").has_value());        // mismatched closing tag
  CHECK(!codec::parse_xml("<a>").has_value());            // never closed
  CHECK(!codec::parse_xml("<a>&nope;</a>").has_value());  // unknown entity
  CHECK(!codec::parse_xml("<a x=1></a>").has_value());    // unquoted attribute value
  CHECK(!codec::parse_xml("").has_value());
  CHECK(!codec::parse_xml("<a></a>tail").has_value());
}

TEST(both_encoders_describe_the_same_snapshot) {
  metrics::Collector collector;
  collector.record_request(75.0);
  collector.record_request(3000.0);
  collector.set_client_counts(4, 1);
  const metrics::Snapshot snapshot = collector.sample();

  const std::string json_text = codec::to_json(snapshot);
  const std::string xml_text = codec::to_xml(snapshot);

  const auto json = codec::parse_json(json_text);
  const auto xml = codec::parse_xml(xml_text);
  CHECK(json.has_value());
  CHECK(xml.has_value());
  if (!json || !xml) return;

  CHECK_EQ(json->find("seq")->number, static_cast<double>(snapshot.sequence));
  CHECK_EQ(xml->child("seq")->text, std::to_string(snapshot.sequence));
  CHECK_EQ(json->find("ws")->number, 4.0);
  CHECK_EQ(xml->child("ws")->text, std::string("4"));
  CHECK_EQ(json->find("requests")->number, 2.0);
  CHECK_EQ(xml->child("requests")->text, std::string("2"));

  // The histogram must survive both encodings with the same bucket count.
  CHECK_EQ(json->find("histogram")->items.size(), metrics::Histogram::kBucketCount);
  CHECK_EQ(xml->child("histogram")->children.size(), metrics::Histogram::kBucketCount);

  // The comparison in the report rests on this: the XML form of the same data is the
  // larger of the two.
  CHECK(xml_text.size() > json_text.size());
}

// ---------------------------------------------------------------------------
// The server, over a real socket
// ---------------------------------------------------------------------------

namespace {

struct RunningServer {
  Server server;
  std::thread worker;

  explicit RunningServer(int interval_ms = 60)
      : server(Options{0, "web", interval_ms, true}) {
    std::string error;
    started = server.start(error);
    if (started) worker = std::thread([this] { server.run(); });
  }
  ~RunningServer() {
    server.stop();
    if (worker.joinable()) worker.join();
  }
  bool started = false;
};

// Sends a request and reads until the peer stops or the deadline passes.
std::string exchange(std::uint16_t port, const std::string& request, int milliseconds = 1500,
                     std::size_t stop_after = 0) {
  std::string error;
  net::Socket socket = net::connect_to("127.0.0.1", port, error);
  if (!socket.valid()) return {};
  if (!net::send_all(socket.get(), request)) return {};
  net::set_nonblocking(socket.get(), true);

  std::string response;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
  char buffer[4096];
  while (std::chrono::steady_clock::now() < deadline) {
    const long received = net::recv_some(socket.get(), buffer, sizeof(buffer));
    if (received > 0) {
      response.append(buffer, static_cast<std::size_t>(received));
      if (stop_after > 0 && response.size() >= stop_after) break;
      continue;
    }
    if (received == 0) break;
    if (!net::last_error_was_would_block()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return response;
}

int status_of(const std::string& response) {
  if (response.size() < 12) return 0;
  return std::atoi(response.substr(9, 3).c_str());
}

std::string body_of(const std::string& response) {
  const std::size_t head_end = response.find("\r\n\r\n");
  return head_end == std::string::npos ? std::string() : response.substr(head_end + 4);
}

}  // namespace

TEST(server_serves_the_snapshot_in_both_formats) {
  RunningServer running;
  CHECK(running.started);
  if (!running.started) return;
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  const std::uint16_t port = running.server.port();
  CHECK(port != 0);

  const std::string json_response = exchange(
      port, "GET /api/metrics.json HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  CHECK_EQ(status_of(json_response), 200);
  CHECK(json_response.find("application/json") != std::string::npos);
  const auto json = codec::parse_json(body_of(json_response));
  CHECK(json.has_value());
  if (json) CHECK(json->find("cpu") != nullptr);

  const std::string xml_response = exchange(
      port, "GET /api/metrics.xml HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  CHECK_EQ(status_of(xml_response), 200);
  CHECK(xml_response.find("application/xml") != std::string::npos);
  const auto xml = codec::parse_xml(body_of(xml_response));
  CHECK(xml.has_value());
  if (xml) CHECK(xml->child("cpu") != nullptr);
}

TEST(server_answers_with_the_status_the_situation_calls_for) {
  RunningServer running;
  CHECK(running.started);
  if (!running.started) return;
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const std::uint16_t port = running.server.port();

  CHECK_EQ(status_of(exchange(port, "GET /nothing/here HTTP/1.1\r\nConnection: close\r\n\r\n")),
           404);
  CHECK_EQ(status_of(exchange(port, "DELETE / HTTP/1.1\r\nConnection: close\r\n\r\n")), 405);
  CHECK_EQ(status_of(exchange(port, "GET /../CMakeLists.txt HTTP/1.1\r\nConnection: close\r\n\r\n")),
           403);
  CHECK_EQ(status_of(exchange(port, "not a request at all\r\n\r\n")), 400);
  // Asking for the WebSocket endpoint without the upgrade headers is not an error in
  // the client, it is a request the server cannot fulfil as written.
  CHECK_EQ(status_of(exchange(port, "GET /ws HTTP/1.1\r\nConnection: close\r\n\r\n")), 426);
  CHECK_EQ(status_of(exchange(port, "GET /api/info HTTP/1.1\r\nConnection: close\r\n\r\n")), 200);
}

TEST(server_keeps_the_connection_open_for_a_second_request) {
  RunningServer running;
  CHECK(running.started);
  if (!running.started) return;
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Two pipelined requests on one connection must produce two responses.
  const std::string response =
      exchange(running.server.port(),
               "GET /api/info HTTP/1.1\r\nHost: x\r\n\r\n"
               "GET /api/info HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
               1500);
  std::size_t count = 0;
  std::size_t at = 0;
  while ((at = response.find("HTTP/1.1 200", at)) != std::string::npos) {
    ++count;
    at += 12;
  }
  CHECK_EQ(count, std::size_t{2});
  CHECK(response.find("Connection: keep-alive") != std::string::npos);
}

TEST(server_completes_the_websocket_handshake_and_pushes_frames) {
  RunningServer running(50);
  CHECK(running.started);
  if (!running.started) return;
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  std::string error;
  net::Socket socket = net::connect_to("127.0.0.1", running.server.port(), error);
  CHECK(socket.valid());
  if (!socket.valid()) return;

  const std::string handshake =
      "GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
      "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Version: 13\r\n\r\n";
  CHECK(net::send_all(socket.get(), handshake));

  std::string buffer;
  char chunk[4096];
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  bool upgraded = false;
  std::size_t frames = 0;
  std::string last_payload;

  while (std::chrono::steady_clock::now() < deadline && frames < 2) {
    const long received = net::recv_some(socket.get(), chunk, sizeof(chunk));
    if (received <= 0) {
      if (received < 0 && net::last_error_was_would_block()) continue;
      break;
    }
    buffer.append(chunk, static_cast<std::size_t>(received));
    if (!upgraded) {
      const std::size_t end = buffer.find("\r\n\r\n");
      if (end == std::string::npos) continue;
      const std::string head = buffer.substr(0, end);
      CHECK(head.rfind("HTTP/1.1 101", 0) == 0);
      CHECK(head.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);
      buffer.erase(0, end + 4);
      upgraded = true;
    }
    while (true) {
      ws::Frame frame;
      std::size_t consumed = 0;
      if (ws::decode_frame(buffer, frame, consumed) != ws::Decode::Ok) break;
      buffer.erase(0, consumed);
      // A frame from the server must never be masked, RFC 6455 section 5.1.
      CHECK(!frame.masked);
      last_payload = frame.payload;
      ++frames;
    }
  }

  CHECK(upgraded);
  CHECK(frames >= 2);
  const auto pushed = codec::parse_json(last_payload);
  CHECK(pushed.has_value());
  if (pushed) CHECK(pushed->find("seq") != nullptr);
}

TEST(server_switches_format_when_the_client_asks_over_the_socket) {
  RunningServer running(50);
  CHECK(running.started);
  if (!running.started) return;
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  std::string error;
  net::Socket socket = net::connect_to("127.0.0.1", running.server.port(), error);
  CHECK(socket.valid());
  if (!socket.valid()) return;

  net::send_all(socket.get(),
                "GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
                "Connection: Upgrade\r\nSec-WebSocket-Key: AAAAAAAAAAAAAAAAAAAAAA==\r\n"
                "Sec-WebSocket-Version: 13\r\n\r\n");
  const std::array<std::uint8_t, 4> mask = {0x01, 0x02, 0x03, 0x04};
  net::send_all(socket.get(), ws::encode_frame(ws::Opcode::Text,
                                               "{\"cmd\":\"format\",\"value\":\"xml\"}", true,
                                               &mask));

  std::string buffer;
  char chunk[4096];
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  bool saw_ack = false;
  bool saw_xml = false;
  bool upgraded = false;

  while (std::chrono::steady_clock::now() < deadline && !(saw_ack && saw_xml)) {
    const long received = net::recv_some(socket.get(), chunk, sizeof(chunk));
    if (received <= 0) {
      if (received < 0 && net::last_error_was_would_block()) continue;
      break;
    }
    buffer.append(chunk, static_cast<std::size_t>(received));
    if (!upgraded) {
      const std::size_t end = buffer.find("\r\n\r\n");
      if (end == std::string::npos) continue;
      buffer.erase(0, end + 4);
      upgraded = true;
    }
    while (true) {
      ws::Frame frame;
      std::size_t consumed = 0;
      if (ws::decode_frame(buffer, frame, consumed) != ws::Decode::Ok) break;
      buffer.erase(0, consumed);
      if (frame.payload.find("\"ack\"") != std::string::npos) saw_ack = true;
      if (frame.payload.rfind("<?xml", 0) == 0) saw_xml = true;
    }
  }
  CHECK(saw_ack);
  CHECK(saw_xml);
}

TEST(server_streams_events_with_the_chunked_coding) {
  RunningServer running(50);
  CHECK(running.started);
  if (!running.started) return;
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  const std::string response =
      exchange(running.server.port(), "GET /api/stream HTTP/1.1\r\nHost: x\r\n\r\n", 600, 0);
  CHECK_EQ(status_of(response), 200);
  CHECK(response.find("Content-Type: text/event-stream") != std::string::npos);
  CHECK(response.find("Transfer-Encoding: chunked") != std::string::npos);
  CHECK(response.find("event: snapshot") != std::string::npos);
  CHECK(response.find("data: {") != std::string::npos);
  CHECK(response.find("id: ") != std::string::npos);
}

int main() {
  net::Library library;
  std::printf("Pulse test suite\n");
  return check::run_all();
}
