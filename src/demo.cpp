// The demonstration and the measurement harness in one binary.
//
// Run with no arguments: it starts the server, measures itself over all three
// transports, prints the numbers, and then keeps serving so the page can be opened.
// Run with --bench: the same measurements with more repetitions, and no serving.
//
// Every number this program prints was produced by the run that printed it. Nothing is
// stored, assumed or carried over from a previous run.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "pulse/codec.hpp"
#include "pulse/http.hpp"
#include "pulse/metrics.hpp"
#include "pulse/net.hpp"
#include "pulse/server.hpp"
#include "pulse/websocket.hpp"

#ifndef PULSE_WEB_ROOT
#  define PULSE_WEB_ROOT "web"
#endif

using namespace pulse;

namespace {

pulse::Server* g_server = nullptr;

extern "C" void on_interrupt(int) {
  if (g_server != nullptr) g_server->stop();
}

struct Stats {
  std::size_t count = 0;
  double mean = 0.0;
  double p50 = 0.0;
  double p95 = 0.0;
  double max = 0.0;
};

Stats summarise(std::vector<double> samples) {
  Stats stats;
  if (samples.empty()) return stats;
  std::sort(samples.begin(), samples.end());
  stats.count = samples.size();
  double sum = 0.0;
  for (double value : samples) sum += value;
  stats.mean = sum / static_cast<double>(samples.size());
  const auto at = [&samples](double q) {
    const auto index = static_cast<std::size_t>(q * static_cast<double>(samples.size() - 1));
    return samples[index];
  };
  stats.p50 = at(0.50);
  stats.p95 = at(0.95);
  stats.max = samples.back();
  return stats;
}

metrics::Snapshot representative_snapshot() {
  metrics::Collector collector;
  // A snapshot with a populated histogram, which is the shape that actually travels.
  const double values[] = {40, 45, 60, 90, 150, 300, 700, 1200, 2500, 6000, 12000};
  for (double value : values) {
    for (int i = 0; i < 9; ++i) collector.record_request(value);
  }
  collector.record_bytes_sent(1234567);
  collector.set_client_counts(3, 2);
  return collector.sample();
}

void rule() { std::printf("  %s\n", std::string(74, '-').c_str()); }

// ---------------------------------------------------------------------------
// Measurement one: the two representations of the same snapshot
// ---------------------------------------------------------------------------
void measure_formats(int repetitions) {
  const metrics::Snapshot snapshot = representative_snapshot();
  const std::string json_text = codec::to_json(snapshot);
  const std::string xml_text = codec::to_xml(snapshot);

  // A sink the optimiser cannot discard, so the loop below really does the work.
  std::size_t sink = 0;

  const std::int64_t json_encode_start = metrics::steady_microseconds();
  for (int i = 0; i < repetitions; ++i) sink += codec::to_json(snapshot).size();
  const double json_encode =
      static_cast<double>(metrics::steady_microseconds() - json_encode_start) / repetitions;

  const std::int64_t xml_encode_start = metrics::steady_microseconds();
  for (int i = 0; i < repetitions; ++i) sink += codec::to_xml(snapshot).size();
  const double xml_encode =
      static_cast<double>(metrics::steady_microseconds() - xml_encode_start) / repetitions;

  const std::int64_t json_parse_start = metrics::steady_microseconds();
  for (int i = 0; i < repetitions; ++i) sink += codec::parse_json(json_text)->node_count();
  const double json_parse =
      static_cast<double>(metrics::steady_microseconds() - json_parse_start) / repetitions;

  const std::int64_t xml_parse_start = metrics::steady_microseconds();
  for (int i = 0; i < repetitions; ++i) sink += codec::parse_xml(xml_text)->node_count();
  const double xml_parse =
      static_cast<double>(metrics::steady_microseconds() - xml_parse_start) / repetitions;

  std::printf("\nJSON against XML, same snapshot, %d repetitions each\n", repetitions);
  rule();
  std::printf("  %-8s %10s %14s %14s %12s\n", "format", "bytes", "encode (us)", "parse (us)",
              "tree nodes");
  rule();
  std::printf("  %-8s %10zu %14.3f %14.3f %12zu\n", "JSON", json_text.size(), json_encode,
              json_parse, codec::parse_json(json_text)->node_count());
  std::printf("  %-8s %10zu %14.3f %14.3f %12zu\n", "XML", xml_text.size(), xml_encode, xml_parse,
              codec::parse_xml(xml_text)->node_count());
  rule();
  std::printf("  XML is %.1f%% larger, encodes %.2fx and parses %.2fx the JSON time.\n",
              100.0 * (static_cast<double>(xml_text.size()) /
                           static_cast<double>(json_text.size()) - 1.0),
              json_encode > 0 ? xml_encode / json_encode : 0.0,
              json_parse > 0 ? xml_parse / json_parse : 0.0);
  if (sink == 0) std::printf("  (unreachable)\n");
}

// ---------------------------------------------------------------------------
// Measurement two: the three transports
// ---------------------------------------------------------------------------

// Reads until the deadline, appending to buffer. Returns false when the peer closed.
bool pump(net::Handle socket, std::string& buffer, int wait_ms) {
  char chunk[8192];
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    const long received = net::recv_some(socket, chunk, sizeof(chunk));
    if (received > 0) {
      buffer.append(chunk, static_cast<std::size_t>(received));
      return true;
    }
    if (received == 0) return false;
    if (!net::last_error_was_would_block()) return false;
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  return true;
}

double age_of(const std::string& payload) {
  const auto document = codec::parse_json(payload);
  if (!document) return -1.0;
  const codec::Json* timestamp = document->find("ts");
  if (timestamp == nullptr) return -1.0;
  return (static_cast<double>(metrics::wall_microseconds()) - timestamp->number) / 1000.0;
}

struct TransportResult {
  const char* name = "";
  Stats latency_ms;
  double bytes_per_update = 0.0;
  double total_bytes = 0.0;
};

TransportResult measure_websocket(std::uint16_t port, int wanted) {
  TransportResult result;
  result.name = "WebSocket";
  std::string error;
  net::Socket socket = net::connect_to("127.0.0.1", port, error);
  if (!socket.valid()) return result;
  net::set_nodelay(socket.get());

  net::send_all(socket.get(),
                "GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
                "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                "Sec-WebSocket-Version: 13\r\n\r\n");
  net::set_nonblocking(socket.get(), true);

  std::string buffer;
  std::vector<double> latencies;
  double bytes = 0;
  bool upgraded = false;

  while (static_cast<int>(latencies.size()) < wanted) {
    if (!pump(socket.get(), buffer, 4000)) break;
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
      if (frame.opcode != ws::Opcode::Text) continue;
      const double age = age_of(frame.payload);
      if (age >= 0.0) {
        latencies.push_back(age);
        bytes += static_cast<double>(consumed);
      }
    }
  }
  result.latency_ms = summarise(latencies);
  result.total_bytes = bytes;
  result.bytes_per_update = latencies.empty() ? 0.0 : bytes / static_cast<double>(latencies.size());
  return result;
}

TransportResult measure_sse(std::uint16_t port, int wanted) {
  TransportResult result;
  result.name = "SSE";
  std::string error;
  net::Socket socket = net::connect_to("127.0.0.1", port, error);
  if (!socket.valid()) return result;
  net::set_nodelay(socket.get());
  net::send_all(socket.get(), "GET /api/stream HTTP/1.1\r\nHost: localhost\r\n\r\n");
  net::set_nonblocking(socket.get(), true);

  std::string buffer;
  std::vector<double> latencies;
  double bytes = 0;
  bool past_head = false;
  std::size_t previous_size = 0;

  while (static_cast<int>(latencies.size()) < wanted) {
    if (!pump(socket.get(), buffer, 4000)) break;
    if (!past_head) {
      const std::size_t end = buffer.find("\r\n\r\n");
      if (end == std::string::npos) continue;
      buffer.erase(0, end + 4);
      past_head = true;
      previous_size = 0;
    }
    // The transfer coding wraps each event, so the payload is found inside the chunk
    // rather than at a fixed offset.
    std::size_t at = 0;
    while ((at = buffer.find("data: ", at)) != std::string::npos) {
      const std::size_t end = buffer.find("\n\n", at);
      if (end == std::string::npos) break;
      const std::string payload = buffer.substr(at + 6, end - at - 6);
      const double age = age_of(payload);
      if (age >= 0.0) {
        latencies.push_back(age);
        // Bytes on the wire for this event, the chunk framing included.
        bytes += static_cast<double>(end + 2 - previous_size);
        previous_size = end + 2;
      }
      at = end + 2;
      if (static_cast<int>(latencies.size()) >= wanted) break;
    }
    if (previous_size > 0) {
      buffer.erase(0, previous_size);
      previous_size = 0;
    }
  }
  result.latency_ms = summarise(latencies);
  result.total_bytes = bytes;
  result.bytes_per_update = latencies.empty() ? 0.0 : bytes / static_cast<double>(latencies.size());
  return result;
}

TransportResult measure_polling(std::uint16_t port, int wanted, int interval_ms) {
  TransportResult result;
  result.name = "Polling";
  std::string error;
  net::Socket socket = net::connect_to("127.0.0.1", port, error);
  if (!socket.valid()) return result;
  net::set_nodelay(socket.get());

  std::vector<double> latencies;
  double bytes = 0;
  const std::string request =
      "GET /api/metrics.json HTTP/1.1\r\nHost: localhost\r\nAccept: application/json\r\n\r\n";

  while (static_cast<int>(latencies.size()) < wanted) {
    const auto sent_at = std::chrono::steady_clock::now();
    if (!net::send_all(socket.get(), request)) break;
    bytes += static_cast<double>(request.size());

    std::string response;
    bool complete = false;
    const auto deadline = sent_at + std::chrono::seconds(3);
    while (!complete && std::chrono::steady_clock::now() < deadline) {
      char chunk[8192];
      const long received = net::recv_some(socket.get(), chunk, sizeof(chunk));
      if (received > 0) {
        response.append(chunk, static_cast<std::size_t>(received));
      } else if (received == 0) {
        break;
      }
      const std::size_t head_end = response.find("\r\n\r\n");
      if (head_end == std::string::npos) continue;
      const std::size_t marker = response.find("Content-Length: ");
      if (marker == std::string::npos) break;
      const std::size_t length =
          static_cast<std::size_t>(std::atoi(response.c_str() + marker + 16));
      complete = response.size() >= head_end + 4 + length;
    }
    if (!complete) break;
    bytes += static_cast<double>(response.size());
    const std::size_t head_end = response.find("\r\n\r\n");
    const double age = age_of(response.substr(head_end + 4));
    if (age >= 0.0) latencies.push_back(age);

    // The poll runs at the publish rate, which is the fair comparison: the same number
    // of updates arrives by each transport.
    std::this_thread::sleep_until(sent_at + std::chrono::milliseconds(interval_ms));
  }
  result.latency_ms = summarise(latencies);
  result.total_bytes = bytes;
  result.bytes_per_update = latencies.empty() ? 0.0 : bytes / static_cast<double>(latencies.size());
  return result;
}

// ---------------------------------------------------------------------------
// Measurement three: how the single loop behaves as clients are added
// ---------------------------------------------------------------------------
struct ScalingRow {
  int clients = 0;
  std::size_t delivered = 0;
  std::size_t expected = 0;
  double mean_latency_ms = 0.0;
  double p95_latency_ms = 0.0;
};

ScalingRow measure_scaling(std::uint16_t port, int clients, int window_ms, int interval_ms) {
  ScalingRow row;
  row.clients = clients;
  row.expected = static_cast<std::size_t>(clients) *
                 static_cast<std::size_t>(window_ms / interval_ms);

  std::vector<net::Socket> sockets;
  std::vector<std::string> buffers(static_cast<std::size_t>(clients));
  std::vector<bool> upgraded(static_cast<std::size_t>(clients), false);
  sockets.reserve(static_cast<std::size_t>(clients));

  for (int i = 0; i < clients; ++i) {
    std::string error;
    net::Socket socket = net::connect_to("127.0.0.1", port, error);
    if (!socket.valid()) break;
    net::set_nodelay(socket.get());
    net::send_all(socket.get(),
                  "GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
                  "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                  "Sec-WebSocket-Version: 13\r\n\r\n");
    net::set_nonblocking(socket.get(), true);
    sockets.push_back(std::move(socket));
  }

  std::vector<double> latencies;
  char chunk[16384];
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(window_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    bool idle = true;
    for (std::size_t i = 0; i < sockets.size(); ++i) {
      const long received = net::recv_some(sockets[i].get(), chunk, sizeof(chunk));
      if (received <= 0) continue;
      idle = false;
      buffers[i].append(chunk, static_cast<std::size_t>(received));
      if (!upgraded[i]) {
        const std::size_t end = buffers[i].find("\r\n\r\n");
        if (end == std::string::npos) continue;
        buffers[i].erase(0, end + 4);
        upgraded[i] = true;
      }
      while (true) {
        ws::Frame frame;
        std::size_t consumed = 0;
        if (ws::decode_frame(buffers[i], frame, consumed) != ws::Decode::Ok) break;
        buffers[i].erase(0, consumed);
        const double age = age_of(frame.payload);
        if (age >= 0.0) latencies.push_back(age);
      }
    }
    if (idle) std::this_thread::sleep_for(std::chrono::microseconds(300));
  }

  const Stats stats = summarise(latencies);
  row.delivered = stats.count;
  row.mean_latency_ms = stats.mean;
  row.p95_latency_ms = stats.p95;
  return row;
}

void print_scaling(const std::vector<ScalingRow>& rows, int window_ms, int interval_ms) {
  std::printf("\nConcurrent WebSocket clients, %d ms window, %d ms publish interval\n", window_ms,
              interval_ms);
  rule();
  std::printf("  %8s %12s %12s %10s %10s %10s\n", "clients", "delivered", "expected", "kept %",
              "mean ms", "p95 ms");
  rule();
  for (const ScalingRow& row : rows) {
    const double kept = row.expected == 0
                            ? 0.0
                            : 100.0 * static_cast<double>(row.delivered) /
                                  static_cast<double>(row.expected);
    std::printf("  %8d %12zu %12zu %10.1f %10.2f %10.2f\n", row.clients, row.delivered,
                row.expected, kept, row.mean_latency_ms, row.p95_latency_ms);
  }
  rule();
  std::printf("  The expected count is clients times windows per interval. Both the server\n");
  std::printf("  and the measuring clients run on the same machine and share its cores.\n");
}

void print_transports(const std::vector<TransportResult>& results, int interval_ms) {
  std::printf("\nTransports, %d ms publish interval, one client, loopback\n", interval_ms);
  rule();
  std::printf("  %-11s %8s %10s %10s %10s %14s\n", "transport", "updates", "mean ms", "p50 ms",
              "p95 ms", "bytes/update");
  rule();
  for (const TransportResult& result : results) {
    std::printf("  %-11s %8zu %10.2f %10.2f %10.2f %14.1f\n", result.name, result.latency_ms.count,
                result.latency_ms.mean, result.latency_ms.p50, result.latency_ms.p95,
                result.bytes_per_update);
  }
  rule();
  std::printf("  Latency is the age of the value on arrival: the client clock minus the\n");
  std::printf("  timestamp the server wrote when it sampled. Both ends share one machine.\n");
}

}  // namespace

int main(int argc, char** argv) {
  bool bench_only = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--bench") == 0) bench_only = true;
  }

  const int interval_ms = 100;
  const int format_repetitions = bench_only ? 50000 : 20000;
  const int updates = bench_only ? 60 : 20;

  net::Library library;
  Options options;
  options.port = bench_only ? 0 : 8080;
  options.web_root = PULSE_WEB_ROOT;
  options.interval_ms = interval_ms;

  Server server(options);
  std::string error;
  if (!server.start(error)) {
    std::fprintf(stderr, "Could not start the server: %s\n", error.c_str());
    return 1;
  }
  std::thread worker([&server] { server.run(); });
  const std::uint16_t port = server.port();

  std::printf("Pulse: live metrics over HTTP/1.1, WebSocket and server-sent events\n");
  std::printf("Server listening on port %u, publishing every %d ms\n", static_cast<unsigned>(port),
              interval_ms);

  measure_formats(format_repetitions);

  std::printf("\nCollecting %d updates over each transport, this takes about %d seconds.\n",
              updates, (updates * interval_ms * 3) / 1000 + 1);
  std::vector<TransportResult> results;
  results.push_back(measure_websocket(port, updates));
  results.push_back(measure_sse(port, updates));
  results.push_back(measure_polling(port, updates, interval_ms));
  print_transports(results, interval_ms);

  const int window_ms = bench_only ? 3000 : 1500;
  std::vector<ScalingRow> scaling;
  for (int clients : {1, 8, 32, 128}) {
    scaling.push_back(measure_scaling(port, clients, window_ms, interval_ms));
  }
  print_scaling(scaling, window_ms, interval_ms);

  const metrics::Snapshot& snapshot = server.last_snapshot();
  std::printf("\nServer state after the run\n");
  rule();
  std::printf("  requests served   %llu\n",
              static_cast<unsigned long long>(snapshot.requests_total));
  std::printf("  bytes sent        %llu\n",
              static_cast<unsigned long long>(snapshot.bytes_sent));
  std::printf("  resident memory   %.2f MiB\n",
              static_cast<double>(snapshot.rss_bytes) / (1024.0 * 1024.0));
  std::printf("  process cpu       %.2f %% of one core in the last interval\n",
              snapshot.cpu_percent);
  std::printf("  request handling  p50 %.1f us, p90 %.1f us, p99 %.1f us\n", snapshot.latency_p50,
              snapshot.latency_p90, snapshot.latency_p99);
  rule();

  if (bench_only) {
    server.stop();
    worker.join();
    return 0;
  }

  g_server = &server;
  std::signal(SIGINT, on_interrupt);
  std::printf("\n  Open http://localhost:%u to watch the dashboard update live.\n",
              static_cast<unsigned>(port));
  std::printf("  Press Ctrl+C to stop.\n");
  std::fflush(stdout);
  worker.join();
  std::printf("Stopped.\n");
  return 0;
}
