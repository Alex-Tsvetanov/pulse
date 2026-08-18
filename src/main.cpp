// Entry point of the server binary.
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "pulse/server.hpp"

#ifndef PULSE_WEB_ROOT
#  define PULSE_WEB_ROOT "web"
#endif

namespace {

pulse::Server* g_server = nullptr;

extern "C" void on_interrupt(int) {
  if (g_server != nullptr) g_server->stop();
}

void print_usage() {
  std::printf(
      "Usage: pulse-server [options]\n"
      "  --port N       port to listen on (default 8080, 0 asks the system)\n"
      "  --web PATH     directory with the client files (default the source tree copy)\n"
      "  --interval N   milliseconds between snapshots (default 250)\n"
      "  --help         this text\n");
}

}  // namespace

int main(int argc, char** argv) {
  pulse::Options options;
  options.web_root = PULSE_WEB_ROOT;

  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    const bool has_value = i + 1 < argc;
    if (argument == "--help" || argument == "-h") {
      print_usage();
      return 0;
    }
    if (argument == "--port" && has_value) {
      options.port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
    } else if (argument == "--web" && has_value) {
      options.web_root = argv[++i];
    } else if (argument == "--interval" && has_value) {
      options.interval_ms = std::atoi(argv[++i]);
    } else {
      std::fprintf(stderr, "Unrecognised argument: %s\n\n", argument.c_str());
      print_usage();
      return 2;
    }
  }
  if (options.interval_ms < 50 || options.interval_ms > 5000) {
    std::fprintf(stderr, "The interval must be between 50 and 5000 milliseconds.\n");
    return 2;
  }

  pulse::Server server(options);
  std::string error;
  if (!server.start(error)) {
    std::fprintf(stderr, "Could not start: %s\n", error.c_str());
    return 1;
  }

  g_server = &server;
  std::signal(SIGINT, on_interrupt);
#ifdef SIGTERM
  std::signal(SIGTERM, on_interrupt);
#endif

  std::printf("Pulse is serving on http://localhost:%u\n", static_cast<unsigned>(server.port()));
  std::printf("  client files   %s\n", options.web_root.c_str());
  std::printf("  snapshot every %d ms\n", options.interval_ms);
  std::printf("  endpoints      /ws  /api/stream  /api/metrics.json  /api/metrics.xml\n");
  std::printf("Press Ctrl+C to stop.\n");
  std::fflush(stdout);

  server.run();
  std::printf("Stopped.\n");
  return 0;
}
