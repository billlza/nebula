#pragma once

#include "runtime/region_allocator.hpp"

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace nebula::rt {

[[noreturn]] inline void panic(const std::string& msg) {
  std::cerr << "nebula panic: " << msg << "\n";
  std::abort();
}

inline std::vector<std::string>& process_args_storage() {
  static std::vector<std::string> args;
  return args;
}

inline void set_process_args(int argc, char** argv) {
  auto& args = process_args_storage();
  args.clear();
  args.reserve(static_cast<std::size_t>(argc));
  for (int i = 0; i < argc; ++i) args.emplace_back(argv[i] ? argv[i] : "");
}

inline void expect_eq_i64(std::int64_t a, std::int64_t b, const char* ctx) {
  if (a != b) {
    std::cerr << "expect_eq failed: " << a << " != " << b;
    if (ctx != nullptr) {
      std::cerr << " (" << ctx << ")";
    }
    std::cerr << "\n";
    std::abort();
  }
}

inline void print(const std::string& msg) {
  std::cout << msg << "\n";
}

[[noreturn]] inline void panic_host(std::string msg) {
  nebula::rt::panic(msg);
}

inline std::int64_t argc() {
  return static_cast<std::int64_t>(process_args_storage().size());
}

inline std::string argv(std::int64_t index) {
  const auto& args = process_args_storage();
  if (index < 0 || static_cast<std::size_t>(index) >= args.size()) {
    panic("argv index out of range");
  }
  return args[static_cast<std::size_t>(index)];
}

inline void assert(bool cond, std::string msg) {
  if (!cond) {
    panic(msg.empty() ? "assertion failed" : msg);
  }
}

} // namespace nebula::rt

inline void print(std::string msg) {
  nebula::rt::print(msg);
}

[[noreturn]] inline void panic(std::string msg) {
  nebula::rt::panic_host(msg);
}

inline std::int64_t argc() {
  return nebula::rt::argc();
}

inline std::string argv(std::int64_t index) {
  return nebula::rt::argv(index);
}

inline void assert(bool cond, std::string msg) {
  nebula::rt::assert(cond, msg);
}

inline std::int64_t args_count() {
  return nebula::rt::argc();
}

inline std::string args_get(std::int64_t index) {
  return nebula::rt::argv(index);
}
