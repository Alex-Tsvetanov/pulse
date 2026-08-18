// A test runner in one header.
//
// The project has no third-party dependency, and a test framework would be one. This
// gives the three things the suite actually needs: registration, a comparison that
// prints both sides on failure, and a non-zero exit status so CTest notices.
#pragma once

#include <cstdio>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace check {

struct Case {
  const char* name;
  void (*run)();
};

inline std::vector<Case>& registry() {
  static std::vector<Case> cases;
  return cases;
}

inline int& failures_in_case() {
  static int count = 0;
  return count;
}

inline int& total_checks() {
  static int count = 0;
  return count;
}

struct Registrar {
  Registrar(const char* name, void (*run)()) { registry().push_back(Case{name, run}); }
};

template <typename T>
std::string show(const T& value) {
  if constexpr (requires(std::ostringstream& out, const T& item) { out << item; }) {
    std::ostringstream out;
    out << value;
    return out.str();
  } else if constexpr (std::is_enum_v<T>) {
    // A scoped enumeration has no stream operator, and printing the underlying number
    // is more useful in a failure message than printing nothing.
    return std::to_string(static_cast<long long>(value));
  } else {
    return "<value>";
  }
}

inline std::string show(bool value) { return value ? "true" : "false"; }

// A string may hold bytes that would scramble the terminal, so it is printed with the
// non-printing ones escaped.
inline std::string show(const std::string& value) {
  std::string out = "\"";
  for (char c : value) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte == 0x7F) {
      char buffer[8];
      std::snprintf(buffer, sizeof(buffer), "\\x%02x", byte);
      out += buffer;
    } else {
      out.push_back(c);
    }
  }
  out.push_back('"');
  return out;
}

inline std::string show(const char* value) { return show(std::string(value)); }

inline void report(const char* file, int line, const std::string& message) {
  ++failures_in_case();
  std::printf("    FAIL %s:%d\n      %s\n", file, line, message.c_str());
}

template <typename A, typename B>
void equal(const char* file, int line, const char* expression, const A& actual, const B& expected) {
  ++total_checks();
  if (actual == expected) return;
  report(file, line,
         std::string(expression) + "\n      actual   " + show(actual) + "\n      expected " +
             show(expected));
}

inline void truth(const char* file, int line, const char* expression, bool value) {
  ++total_checks();
  if (value) return;
  report(file, line, std::string(expression) + " was false");
}

inline int run_all() {
  int failed_cases = 0;
  for (const Case& test : registry()) {
    failures_in_case() = 0;
    std::printf("  %s\n", test.name);
    test.run();
    if (failures_in_case() > 0) ++failed_cases;
  }
  std::printf("\n%d cases, %d checks, %d failed cases\n",
              static_cast<int>(registry().size()), total_checks(), failed_cases);
  return failed_cases == 0 ? 0 : 1;
}

}  // namespace check

#define TEST(name)                                              \
  static void name();                                           \
  static ::check::Registrar registrar_##name(#name, &name);     \
  static void name()

#define CHECK(expression) ::check::truth(__FILE__, __LINE__, #expression, (expression))
#define CHECK_EQ(actual, expected) \
  ::check::equal(__FILE__, __LINE__, #actual " == " #expected, (actual), (expected))
