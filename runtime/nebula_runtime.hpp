#pragma once

#include "runtime/region_allocator.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <cerrno>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <limits>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/event.h>
#else
#include <sys/epoll.h>
#endif
#endif

#if !defined(_WIN32)
extern char** environ;
#endif

namespace nebula::rt {

namespace hostfs = std::filesystem;

class UserPanic final : public std::exception {
  std::string message_;

public:
  explicit UserPanic(std::string message) : message_(std::move(message)) {}

  const char* what() const noexcept override { return message_.c_str(); }
};

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
    if (ctx != nullptr) std::cerr << " (" << ctx << ")";
    std::cerr << "\n";
    std::abort();
  }
}

inline void print(const std::string& msg) {
  std::cout << msg << "\n";
}

inline void info(const std::string& msg) {
  std::cerr << "[info] " << msg << "\n";
}

inline void error(const std::string& msg) {
  std::cerr << "[error] " << msg << "\n";
}

[[noreturn]] inline void panic_host(std::string msg) {
  throw UserPanic(std::move(msg));
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
  if (!cond) panic(msg.empty() ? "assertion failed" : msg);
}

struct Duration {
  std::int64_t millis = 0;
};

inline Duration make_duration_millis(std::int64_t value) {
  return Duration{value};
}

inline std::int64_t unix_millis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

inline std::int64_t steady_millis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

template <typename T, typename E>
struct Result {
  struct Ok {
    T value;
  };
  struct Err {
    E value;
  };

  std::variant<Ok, Err> data;

  Result() = default;
  Result(Ok ok) : data(std::move(ok)) {}
  Result(Err err) : data(std::move(err)) {}
};

template <typename E>
struct Result<void, E> {
  struct Ok {};
  struct Err {
    E value;
  };

  std::variant<Ok, Err> data;

  Result() = default;
  Result(Ok ok) : data(std::move(ok)) {}
  Result(Err err) : data(std::move(err)) {}
};

template <typename T, typename E>
inline bool result_is_ok(const Result<T, E>& result) {
  return std::holds_alternative<typename Result<T, E>::Ok>(result.data);
}

template <typename T, typename E>
inline bool result_is_err(const Result<T, E>& result) {
  return std::holds_alternative<typename Result<T, E>::Err>(result.data);
}

template <typename T, typename E>
inline decltype(auto) result_ok_ref(Result<T, E>& result) {
  return (std::get<typename Result<T, E>::Ok>(result.data).value);
}

template <typename T, typename E>
inline decltype(auto) result_ok_ref(const Result<T, E>& result) {
  return (std::get<typename Result<T, E>::Ok>(result.data).value);
}

template <typename T, typename E>
inline decltype(auto) result_ok_move(Result<T, E>& result) {
  return std::move(std::get<typename Result<T, E>::Ok>(result.data).value);
}

template <typename E>
inline void result_ok_move(Result<void, E>& result) {
  std::get<typename Result<void, E>::Ok>(result.data);
}

template <typename E>
inline void result_ok_ref(Result<void, E>& result) {
  std::get<typename Result<void, E>::Ok>(result.data);
}

template <typename E>
inline void result_ok_ref(const Result<void, E>& result) {
  std::get<typename Result<void, E>::Ok>(result.data);
}

template <typename T, typename E>
inline decltype(auto) result_err_ref(Result<T, E>& result) {
  return (std::get<typename Result<T, E>::Err>(result.data).value);
}

template <typename T, typename E>
inline decltype(auto) result_err_ref(const Result<T, E>& result) {
  return (std::get<typename Result<T, E>::Err>(result.data).value);
}

template <typename T, typename E>
inline decltype(auto) result_err_move(Result<T, E>& result) {
  return std::move(std::get<typename Result<T, E>::Err>(result.data).value);
}

template <typename E>
inline decltype(auto) result_err_move(Result<void, E>& result) {
  return std::move(std::get<typename Result<void, E>::Err>(result.data).value);
}

struct Bytes {
  std::string data;

  Bytes() = default;
  explicit Bytes(std::string value) : data(std::move(value)) {}
};

#if defined(_WIN32)
using NativeSocket = SOCKET;
using SocketLength = int;
inline constexpr NativeSocket kInvalidNativeSocket = INVALID_SOCKET;
#else
using NativeSocket = int;
using SocketLength = socklen_t;
inline constexpr NativeSocket kInvalidNativeSocket = -1;
#endif

inline bool native_socket_valid(NativeSocket fd) {
#if defined(_WIN32)
  return fd != INVALID_SOCKET;
#else
  return fd >= 0;
#endif
}

inline void close_native_socket(NativeSocket fd) {
  if (!native_socket_valid(fd)) return;
#if defined(_WIN32)
  closesocket(fd);
#else
  close(fd);
#endif
}

inline int last_socket_error_code() {
#if defined(_WIN32)
  return WSAGetLastError();
#else
  return errno;
#endif
}

inline bool socket_error_is_would_block(int err) {
#if defined(_WIN32)
  return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS || err == WSAEALREADY;
#else
  return err == EAGAIN || err == EWOULDBLOCK || err == EINPROGRESS || err == EALREADY;
#endif
}

inline bool socket_error_is_interrupted(int err) {
#if defined(_WIN32)
  return err == WSAEINTR;
#else
  return err == EINTR;
#endif
}

inline std::string socket_error_message(int err) {
#if defined(_WIN32)
  return "winsock error " + std::to_string(err);
#else
  return std::system_category().message(err);
#endif
}

inline void ensure_socket_runtime_initialized() {
#if defined(_WIN32)
  static bool initialized = []() {
    WSADATA data{};
    const int rc = WSAStartup(MAKEWORD(2, 2), &data);
    if (rc != 0) {
      panic("failed to initialize Winsock: " + std::to_string(rc));
    }
    return true;
  }();
  (void)initialized;
#endif
}

inline bool set_socket_nonblocking(NativeSocket fd) {
#if defined(_WIN32)
  u_long mode = 1;
  return ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return false;
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) return false;
#if defined(__APPLE__)
  int no_sigpipe = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
  return true;
#endif
}

inline int socket_send_flags() {
#if defined(MSG_NOSIGNAL)
  return MSG_NOSIGNAL;
#else
  return 0;
#endif
}

struct SocketAddr {
  sockaddr_storage storage{};
  SocketLength length = 0;
};

struct SocketHandle {
  NativeSocket fd = kInvalidNativeSocket;

  SocketHandle() = default;
  explicit SocketHandle(NativeSocket native_fd) : fd(native_fd) {}

  SocketHandle(const SocketHandle&) = delete;
  SocketHandle& operator=(const SocketHandle&) = delete;

  SocketHandle(SocketHandle&& other) noexcept : fd(std::exchange(other.fd, kInvalidNativeSocket)) {}

  SocketHandle& operator=(SocketHandle&& other) noexcept {
    if (this == &other) return *this;
    close_native_socket(fd);
    fd = std::exchange(other.fd, kInvalidNativeSocket);
    return *this;
  }

  ~SocketHandle() {
    close_native_socket(fd);
  }

  bool valid() const { return native_socket_valid(fd); }
};

struct WakeSocketPair {
  NativeSocket reader = kInvalidNativeSocket;
  NativeSocket writer = kInvalidNativeSocket;
};

inline Result<void, std::string> ok_void_result();
inline Result<void, std::string> err_void_result(std::string message);
inline std::shared_ptr<SocketHandle> require_open_handle(const std::shared_ptr<SocketHandle>& handle,
                                                         const char* label,
                                                         std::string& error);

struct TcpListener {
  std::shared_ptr<SocketHandle> handle;
};

struct TcpStream {
  std::shared_ptr<SocketHandle> handle;
};

struct TlsTrustStoreState;
struct TlsClientIdentityState;
struct TlsVersionPolicyState;
struct TlsAlpnPolicyState;
struct TlsServerNameState;
struct TlsClientConfigState;
struct TlsClientStreamState;
struct TlsServerIdentityState;
struct TlsServerConfigState;
struct TlsServerListenerState;
struct TlsServerStreamState;
struct SqliteConnectionState;
struct SqliteTransactionState;
struct SqliteResultSetState;
struct PostgresConnectionState;
struct PostgresTransactionState;
struct PostgresResultSetState;

struct TlsTrustStore {
  std::shared_ptr<TlsTrustStoreState> state;
};

struct TlsClientIdentity {
  std::shared_ptr<TlsClientIdentityState> state;
};

struct TlsVersionPolicy {
  std::shared_ptr<TlsVersionPolicyState> state;
};

struct TlsAlpnPolicy {
  std::shared_ptr<TlsAlpnPolicyState> state;
};

struct TlsServerName {
  std::shared_ptr<TlsServerNameState> state;
};

struct TlsClientConfig {
  std::shared_ptr<TlsClientConfigState> state;
};

struct TlsClientStream {
  std::shared_ptr<TlsClientStreamState> state;
};

struct TlsServerIdentity {
  std::shared_ptr<TlsServerIdentityState> state;
};

struct TlsServerConfig {
  std::shared_ptr<TlsServerConfigState> state;
};

struct TlsServerListener {
  std::shared_ptr<TlsServerListenerState> state;
};

struct TlsServerStream {
  std::shared_ptr<TlsServerStreamState> state;
};

struct SqliteConnection {
  std::shared_ptr<SqliteConnectionState> state;
};

struct SqliteTransaction {
  std::shared_ptr<SqliteTransactionState> state;
};

struct SqliteResultSet {
  std::shared_ptr<SqliteResultSetState> state;
};

struct SqliteRow {
  std::shared_ptr<SqliteResultSetState> state;
  std::int64_t index = -1;
};

struct PostgresConnection {
  std::shared_ptr<PostgresConnectionState> state;
};

struct PostgresTransaction {
  std::shared_ptr<PostgresTransactionState> state;
};

struct PostgresResultSet {
  std::shared_ptr<PostgresResultSetState> state;
};

struct PostgresRow {
  std::shared_ptr<PostgresResultSetState> state;
  std::int64_t index = -1;
};

struct HttpMethod {
  struct Get {};
  struct Head {};
  struct Post {};
  struct Put {};
  struct Delete {};

  std::variant<Get, Head, Post, Put, Delete> data;

  HttpMethod() = default;
  HttpMethod(Get value) : data(std::move(value)) {}
  HttpMethod(Head value) : data(std::move(value)) {}
  HttpMethod(Post value) : data(std::move(value)) {}
  HttpMethod(Put value) : data(std::move(value)) {}
  HttpMethod(Delete value) : data(std::move(value)) {}
};

struct HttpRequest {
  HttpMethod method;
  std::string path;
  Bytes body;
  std::string headers;
  bool close_connection = false;

  HttpRequest() = default;
  HttpRequest(HttpMethod method_value, std::string path_value, Bytes body_value)
      : method(std::move(method_value)),
        path(std::move(path_value)),
        body(std::move(body_value)) {}
  HttpRequest(HttpMethod method_value,
              std::string path_value,
              Bytes body_value,
              std::string headers_value)
      : method(std::move(method_value)),
        path(std::move(path_value)),
        body(std::move(body_value)),
        headers(std::move(headers_value)) {}
  HttpRequest(HttpMethod method_value,
              std::string path_value,
              Bytes body_value,
              std::string headers_value,
              bool close_connection_value)
      : method(std::move(method_value)),
        path(std::move(path_value)),
        body(std::move(body_value)),
        headers(std::move(headers_value)),
        close_connection(close_connection_value) {}
};

struct HttpResponse {
  std::int64_t status = 200;
  std::string content_type;
  Bytes body;
  std::string headers;

  HttpResponse() = default;
  HttpResponse(std::int64_t status_value, std::string content_type_value, Bytes body_value)
      : status(status_value),
        content_type(std::move(content_type_value)),
        body(std::move(body_value)) {}
  HttpResponse(std::int64_t status_value,
               std::string content_type_value,
               Bytes body_value,
               std::string headers_value)
      : status(status_value),
        content_type(std::move(content_type_value)),
        body(std::move(body_value)),
        headers(std::move(headers_value)) {}
};

struct HttpClientRequest {
  HttpMethod method;
  std::string authority;
  std::string path;
  std::string content_type;
  Bytes body;
  std::string headers;

  HttpClientRequest() = default;
  HttpClientRequest(HttpMethod method_value,
                    std::string authority_value,
                    std::string path_value,
                    std::string content_type_value,
                    Bytes body_value)
      : method(std::move(method_value)),
        authority(std::move(authority_value)),
        path(std::move(path_value)),
        content_type(std::move(content_type_value)),
        body(std::move(body_value)) {}
  HttpClientRequest(HttpMethod method_value,
                    std::string authority_value,
                    std::string path_value,
                    std::string content_type_value,
                    Bytes body_value,
                    std::string headers_value)
      : method(std::move(method_value)),
        authority(std::move(authority_value)),
        path(std::move(path_value)),
        content_type(std::move(content_type_value)),
        body(std::move(body_value)),
        headers(std::move(headers_value)) {}
};

struct HttpClientResponse {
  std::int64_t status = 200;
  std::string content_type;
  Bytes body;
  std::string headers;

  HttpClientResponse() = default;
  HttpClientResponse(std::int64_t status_value, std::string content_type_value, Bytes body_value)
      : status(status_value),
        content_type(std::move(content_type_value)),
        body(std::move(body_value)) {}
  HttpClientResponse(std::int64_t status_value,
                     std::string content_type_value,
                     Bytes body_value,
                     std::string headers_value)
      : status(status_value),
        content_type(std::move(content_type_value)),
        body(std::move(body_value)),
        headers(std::move(headers_value)) {}
};

struct HttpRouteParams2 {
  std::string first;
  std::string second;
};

struct HttpRouteParams3 {
  std::string first;
  std::string second;
  std::string third;
};

struct HttpRoutePatternSegment {
  bool parameter = false;
  std::string text;
};

struct HttpRoutePattern {
  std::string pattern;
  std::vector<HttpRoutePatternSegment> segments;
  std::size_t param_count = 0;
};

struct HttpRouteCaptureViews {
  std::array<std::string_view, 3> values{};
};

enum class JsonValueKind : std::uint8_t {
  Unknown,
  String,
  Int,
  Bool,
  Null,
  Object,
  Array,
};

struct JsonValueView {
  JsonValueKind kind = JsonValueKind::Unknown;
  std::size_t raw_start = 0;
  std::size_t raw_end = 0;
  bool string_needs_decode = false;
  std::int64_t int_value = 0;
  bool bool_value = false;
};

struct JsonObjectField {
  std::size_t key_start = 0;
  std::size_t key_end = 0;
  bool key_needs_decode = false;
  JsonValueView value;
};

inline constexpr std::size_t kJsonInlineObjectFieldLimit = 8;

struct JsonObjectIndex {
  std::array<JsonObjectField, kJsonInlineObjectFieldLimit> fields{};
  std::uint8_t count = 0;
  bool complete = true;

  void reset() {
    count = 0;
    complete = true;
  }

  void mark_incomplete() {
    count = 0;
    complete = false;
  }

  bool push_back(JsonObjectField field) {
    if (!complete) return false;
    if (count >= fields.size()) {
      mark_incomplete();
      return false;
    }
    fields[count] = std::move(field);
    count = static_cast<std::uint8_t>(count + 1);
    return true;
  }
};

struct JsonArrayIndex {
  std::vector<JsonValueView> items;
};

struct JsonStructuredValue;

struct JsonStructuredField {
  std::string key;
  std::shared_ptr<const JsonStructuredValue> value;
};

struct JsonStructuredValue {
  JsonValueKind kind = JsonValueKind::Unknown;
  std::string string_value;
  std::int64_t int_value = 0;
  bool bool_value = false;
  std::array<JsonStructuredField, kJsonInlineObjectFieldLimit> object_fields{};
  std::uint8_t object_count = 0;
  mutable std::string materialized_text;
  mutable bool materialized = false;
};

struct JsonValue {
  std::string text;
  JsonValueView parsed;
  JsonObjectIndex object_fields;
  bool has_object_fields = false;
  JsonArrayIndex array_items;
  bool has_array_items = false;
  std::shared_ptr<const JsonStructuredValue> structured;

  JsonValue() = default;
  explicit JsonValue(std::string text_value) : text(std::move(text_value)) {}
  JsonValue(std::string text_value, JsonValueView parsed_value)
      : text(std::move(text_value)),
        parsed(std::move(parsed_value)) {}
  JsonValue(std::string text_value, JsonValueView parsed_value, JsonObjectIndex object_fields_value)
      : text(std::move(text_value)),
        parsed(std::move(parsed_value)),
        object_fields(std::move(object_fields_value)) {
    has_object_fields = object_fields.complete;
  }
  JsonValue(std::string text_value, JsonValueView parsed_value, JsonArrayIndex array_items_value)
      : text(std::move(text_value)),
        parsed(std::move(parsed_value)),
        array_items(std::move(array_items_value)),
        has_array_items(true) {}
  explicit JsonValue(std::shared_ptr<const JsonStructuredValue> structured_value)
      : structured(std::move(structured_value)) {
    if (structured != nullptr) parsed.kind = structured->kind;
  }
};

struct JsonArrayBuilder {
  std::vector<JsonValue> items;
};

struct ProcessCommand {
  std::string program;
  JsonValue args;
  std::string cwd;
  JsonValue env;
  bool inherit_env = true;
  Bytes stdin;
  std::int64_t timeout_ms = 0;
  std::int64_t max_stdout_bytes = 0;
  std::int64_t max_stderr_bytes = 0;

  ProcessCommand() = default;
  ProcessCommand(std::string program_value,
                 JsonValue args_value,
                 std::string cwd_value,
                 JsonValue env_value,
                 bool inherit_env_value,
                 Bytes stdin_value,
                 std::int64_t timeout_ms_value,
                 std::int64_t max_stdout_bytes_value,
                 std::int64_t max_stderr_bytes_value)
      : program(std::move(program_value)),
        args(std::move(args_value)),
        cwd(std::move(cwd_value)),
        env(std::move(env_value)),
        inherit_env(inherit_env_value),
        stdin(std::move(stdin_value)),
        timeout_ms(timeout_ms_value),
        max_stdout_bytes(max_stdout_bytes_value),
        max_stderr_bytes(max_stderr_bytes_value) {}
};

struct ProcessOutput {
  std::int64_t exit_code = -1;
  std::int64_t signal = 0;
  bool timed_out = false;
  Bytes stdout;
  Bytes stderr;

  ProcessOutput() = default;
  ProcessOutput(std::int64_t exit_code_value,
                std::int64_t signal_value,
                bool timed_out_value,
                Bytes stdout_value,
                Bytes stderr_value)
      : exit_code(exit_code_value),
        signal(signal_value),
        timed_out(timed_out_value),
        stdout(std::move(stdout_value)),
        stderr(std::move(stderr_value)) {}
};

inline Result<SocketAddr, std::string> make_socketaddr_ipv4(const std::string& host,
                                                            std::int64_t port) {
  ensure_socket_runtime_initialized();
  if (port < 0 || port > 65535) {
    return typename Result<SocketAddr, std::string>::Err{"port out of range"};
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<std::uint16_t>(port));
  if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    return typename Result<SocketAddr, std::string>::Err{"invalid IPv4 literal: " + host};
  }

  SocketAddr out;
  std::memcpy(&out.storage, &addr, sizeof(addr));
  out.length = static_cast<SocketLength>(sizeof(addr));
  return typename Result<SocketAddr, std::string>::Ok{out};
}

inline std::string addrinfo_error_message(int code) {
#if defined(_WIN32)
  const char* msg = gai_strerrorA(code);
#else
  const char* msg = gai_strerror(code);
#endif
  if (msg != nullptr && msg[0] != '\0') return std::string(msg);
  return "error " + std::to_string(code);
}

inline Result<SocketAddr, std::string> resolve_socketaddr_blocking(const std::string& host,
                                                                   std::int64_t port) {
  ensure_socket_runtime_initialized();
  if (port < 0 || port > 65535) {
    return typename Result<SocketAddr, std::string>::Err{"port out of range"};
  }
  if (host.empty()) {
    return typename Result<SocketAddr, std::string>::Err{"host must not be empty"};
  }
  for (unsigned char ch : host) {
    if (std::isspace(ch) || ch < 0x20) {
      return typename Result<SocketAddr, std::string>::Err{
          "resolve failed for host '" + host + "': host contains whitespace or control characters"};
    }
  }

  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  addrinfo* addr_list = nullptr;
  const std::string port_text = std::to_string(port);
  const int rc = getaddrinfo(host.c_str(), port_text.c_str(), &hints, &addr_list);
  if (rc != 0) {
    return typename Result<SocketAddr, std::string>::Err{
        "resolve failed for host '" + host + "': " + addrinfo_error_message(rc)};
  }

  std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> guard(addr_list, freeaddrinfo);
  for (addrinfo* it = addr_list; it != nullptr; it = it->ai_next) {
    if (it->ai_addr == nullptr || it->ai_addrlen <= 0 || it->ai_family != AF_INET) continue;
    SocketAddr out;
    const auto copy_len =
        static_cast<std::size_t>(std::min<SocketLength>(it->ai_addrlen, sizeof(out.storage)));
    std::memcpy(&out.storage, it->ai_addr, copy_len);
    out.length = static_cast<SocketLength>(copy_len);
    return typename Result<SocketAddr, std::string>::Ok{out};
  }

  return typename Result<SocketAddr, std::string>::Err{
      "resolve failed for host '" + host + "': no IPv4 address found"};
}

inline Result<int, std::string> socketaddr_family(const SocketAddr& addr) {
  if (addr.length <= 0) {
    return typename Result<int, std::string>::Err{"socket address is uninitialized"};
  }
  const int family = static_cast<int>(addr.storage.ss_family);
  if (family != AF_INET) {
    return typename Result<int, std::string>::Err{"unsupported socket family"};
  }
  return typename Result<int, std::string>::Ok{family};
}

inline void configure_wake_socket(NativeSocket fd) {
  if (!native_socket_valid(fd)) return;
  set_socket_nonblocking(fd);
#if defined(__APPLE__)
  int no_sigpipe = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
}

inline Result<WakeSocketPair, std::string> create_wake_socket_pair() {
  ensure_socket_runtime_initialized();
#if defined(_WIN32)
  NativeSocket listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (!native_socket_valid(listener)) {
    return typename Result<WakeSocketPair, std::string>::Err{
        "wake listener socket failed: " + socket_error_message(last_socket_error_code())};
  }
  auto listener_guard = std::shared_ptr<SocketHandle>(new SocketHandle(listener));

  sockaddr_in bind_addr{};
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  bind_addr.sin_port = 0;
  if (::bind(listener, reinterpret_cast<const sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
    return typename Result<WakeSocketPair, std::string>::Err{
        "wake listener bind failed: " + socket_error_message(last_socket_error_code())};
  }
  if (::listen(listener, 1) != 0) {
    return typename Result<WakeSocketPair, std::string>::Err{
        "wake listener listen failed: " + socket_error_message(last_socket_error_code())};
  }

  sockaddr_in listen_addr{};
  SocketLength listen_len = static_cast<SocketLength>(sizeof(listen_addr));
  if (getsockname(listener, reinterpret_cast<sockaddr*>(&listen_addr), &listen_len) != 0) {
    return typename Result<WakeSocketPair, std::string>::Err{
        "wake listener getsockname failed: " + socket_error_message(last_socket_error_code())};
  }

  NativeSocket writer = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (!native_socket_valid(writer)) {
    return typename Result<WakeSocketPair, std::string>::Err{
        "wake writer socket failed: " + socket_error_message(last_socket_error_code())};
  }
  auto writer_guard = std::shared_ptr<SocketHandle>(new SocketHandle(writer));
  if (::connect(writer, reinterpret_cast<const sockaddr*>(&listen_addr), listen_len) != 0) {
    return typename Result<WakeSocketPair, std::string>::Err{
        "wake writer connect failed: " + socket_error_message(last_socket_error_code())};
  }

  NativeSocket reader = ::accept(listener, nullptr, nullptr);
  if (!native_socket_valid(reader)) {
    return typename Result<WakeSocketPair, std::string>::Err{
        "wake reader accept failed: " + socket_error_message(last_socket_error_code())};
  }
  listener_guard.reset();
  configure_wake_socket(reader);
  configure_wake_socket(writer);
  WakeSocketPair out{};
  out.reader = reader;
  out.writer = writer;
  writer_guard->fd = kInvalidNativeSocket;
  return typename Result<WakeSocketPair, std::string>::Ok{out};
#else
  int fds[2] = {-1, -1};
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    return typename Result<WakeSocketPair, std::string>::Err{
        "wake socketpair failed: " + socket_error_message(last_socket_error_code())};
  }
  configure_wake_socket(fds[0]);
  configure_wake_socket(fds[1]);
  WakeSocketPair out{};
  out.reader = fds[0];
  out.writer = fds[1];
  return typename Result<WakeSocketPair, std::string>::Ok{out};
#endif
}

inline void notify_wake_socket(NativeSocket fd) {
  if (!native_socket_valid(fd)) return;
  const char byte = 1;
  while (true) {
#if defined(_WIN32)
    const int rc = ::send(fd, &byte, 1, 0);
#else
    const ssize_t rc =
        ::send(fd, &byte, 1,
#if defined(MSG_NOSIGNAL)
               MSG_NOSIGNAL
#else
               0
#endif
        );
#endif
    if (rc > 0) return;
    const int err = last_socket_error_code();
    if (socket_error_is_interrupted(err)) continue;
    return;
  }
}

inline Result<void, std::string> wait_for_wake_socket(const std::shared_ptr<SocketHandle>& handle) {
  std::string error;
  auto wake = require_open_handle(handle, "resolver wake", error);
  if (!wake) return err_void_result(std::move(error));

  while (true) {
    char buffer[32];
#if defined(_WIN32)
    const int rc = ::recv(wake->fd, buffer, static_cast<int>(sizeof(buffer)), 0);
#else
    const ssize_t rc = ::recv(wake->fd, buffer, sizeof(buffer), 0);
#endif
    if (rc > 0 || rc == 0) return ok_void_result();
    const int err = last_socket_error_code();
    if (socket_error_is_interrupted(err)) continue;
    if (socket_error_is_would_block(err)) {
      return err_void_result("resolver wake pending");
    }
    return err_void_result("resolver wake failed: " + socket_error_message(err));
  }
}

inline Result<SocketAddr, std::string> socket_name_of(NativeSocket fd, bool peer) {
  if (!native_socket_valid(fd)) {
    return typename Result<SocketAddr, std::string>::Err{"socket is closed"};
  }
  SocketAddr addr;
  addr.length = static_cast<SocketLength>(sizeof(addr.storage));
  const int rc = peer ? getpeername(fd, reinterpret_cast<sockaddr*>(&addr.storage), &addr.length)
                      : getsockname(fd, reinterpret_cast<sockaddr*>(&addr.storage), &addr.length);
  if (rc != 0) {
    const int err = last_socket_error_code();
    return typename Result<SocketAddr, std::string>::Err{
        std::string(peer ? "getpeername failed: " : "getsockname failed: ") + socket_error_message(err)};
  }
  return typename Result<SocketAddr, std::string>::Ok{addr};
}

inline Result<std::string, std::string> socket_addr_host_text(SocketAddr addr) {
  if (addr.length == 0) {
    return typename Result<std::string, std::string>::Err{"socket address is empty"};
  }
  const auto* storage = reinterpret_cast<const sockaddr*>(&addr.storage);
  if (storage->sa_family != AF_INET) {
    return typename Result<std::string, std::string>::Err{"unsupported socket address family"};
  }
  const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&addr.storage);
  char buffer[INET_ADDRSTRLEN] = {};
  const char* rc = inet_ntop(AF_INET, &ipv4->sin_addr, buffer, sizeof(buffer));
  if (rc == nullptr) {
    return typename Result<std::string, std::string>::Err{"inet_ntop failed"};
  }
  return typename Result<std::string, std::string>::Ok{std::string(buffer)};
}

inline Result<std::int64_t, std::string> socket_addr_port(SocketAddr addr) {
  if (addr.length == 0) {
    return typename Result<std::int64_t, std::string>::Err{"socket address is empty"};
  }
  const auto* storage = reinterpret_cast<const sockaddr*>(&addr.storage);
  if (storage->sa_family != AF_INET) {
    return typename Result<std::int64_t, std::string>::Err{"unsupported socket address family"};
  }
  const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&addr.storage);
  return typename Result<std::int64_t, std::string>::Ok{
      static_cast<std::int64_t>(ntohs(ipv4->sin_port))};
}

class Reactor {
public:
  enum class Interest : std::uint8_t { Read, Write };
  using Handle = std::coroutine_handle<>;

private:
  struct WaitSet {
    std::vector<Handle> read;
    std::vector<Handle> write;
  };

#if !defined(_WIN32)
  struct InterestMask {
    bool read = false;
    bool write = false;
  };
#endif

  std::unordered_map<NativeSocket, WaitSet> waiters_;

#if defined(__APPLE__)
  int queue_fd_ = -1;
  std::unordered_map<NativeSocket, InterestMask> registered_;
#elif defined(_WIN32)
  // WSAPoll rebuilds interest tables on demand.
#else
  int poll_fd_ = -1;
  std::unordered_map<NativeSocket, InterestMask> registered_;
#endif

public:
  Reactor() {
    ensure_socket_runtime_initialized();
#if defined(__APPLE__)
    queue_fd_ = kqueue();
    if (queue_fd_ < 0) panic("failed to create kqueue reactor");
#elif !defined(_WIN32)
    poll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (poll_fd_ < 0) panic("failed to create epoll reactor");
#endif
  }

  ~Reactor() {
#if defined(__APPLE__)
    if (queue_fd_ >= 0) close(queue_fd_);
#elif !defined(_WIN32)
    if (poll_fd_ >= 0) close(poll_fd_);
#endif
  }

  bool has_waiters() const { return !waiters_.empty(); }

  void add_waiter(NativeSocket fd, Interest interest, Handle handle) {
    if (!native_socket_valid(fd)) {
      if (handle && !handle.done()) handle.resume();
      return;
    }
    auto& entry = waiters_[fd];
    auto& bucket = (interest == Interest::Read) ? entry.read : entry.write;
    bucket.push_back(handle);
    update_registration(fd);
  }

  std::vector<Handle> cancel(NativeSocket fd) {
    std::vector<Handle> ready;
    auto it = waiters_.find(fd);
    if (it == waiters_.end()) return ready;
    ready.insert(ready.end(), it->second.read.begin(), it->second.read.end());
    ready.insert(ready.end(), it->second.write.begin(), it->second.write.end());
    waiters_.erase(it);
    update_registration(fd);
    return ready;
  }

  std::vector<Handle> poll(std::optional<std::chrono::milliseconds> timeout) {
    if (waiters_.empty()) return {};
#if defined(_WIN32)
    return poll_windows(timeout);
#elif defined(__APPLE__)
    return poll_kqueue(timeout);
#else
    return poll_epoll(timeout);
#endif
  }

private:
#if defined(_WIN32)
  std::vector<Handle> poll_windows(std::optional<std::chrono::milliseconds> timeout) {
    std::vector<WSAPOLLFD> pollfds;
    std::vector<NativeSocket> keys;
    pollfds.reserve(waiters_.size());
    keys.reserve(waiters_.size());
    for (const auto& [fd, waits] : waiters_) {
      short events = 0;
      if (!waits.read.empty()) events |= POLLRDNORM;
      if (!waits.write.empty()) events |= POLLWRNORM;
      if (events == 0) continue;
      WSAPOLLFD entry{};
      entry.fd = fd;
      entry.events = events;
      pollfds.push_back(entry);
      keys.push_back(fd);
    }
    if (pollfds.empty()) return {};
    const int timeout_ms =
        timeout.has_value() ? static_cast<int>(std::max<std::int64_t>(0, timeout->count())) : -1;
    const int rc = WSAPoll(pollfds.data(), static_cast<ULONG>(pollfds.size()), timeout_ms);
    if (rc <= 0) return {};
    std::vector<Handle> ready;
    for (std::size_t i = 0; i < pollfds.size(); ++i) {
      const short revents = pollfds[i].revents;
      if (revents == 0) continue;
      auto it = waiters_.find(keys[i]);
      if (it == waiters_.end()) continue;
      const bool wake_read = (revents & (POLLRDNORM | POLLERR | POLLHUP | POLLNVAL)) != 0;
      const bool wake_write = (revents & (POLLWRNORM | POLLERR | POLLHUP | POLLNVAL)) != 0;
      if (wake_read) {
        ready.insert(ready.end(), it->second.read.begin(), it->second.read.end());
        it->second.read.clear();
      }
      if (wake_write) {
        ready.insert(ready.end(), it->second.write.begin(), it->second.write.end());
        it->second.write.clear();
      }
      if (it->second.read.empty() && it->second.write.empty()) waiters_.erase(it);
    }
    return ready;
  }
#elif defined(__APPLE__)
  std::vector<Handle> poll_kqueue(std::optional<std::chrono::milliseconds> timeout) {
    constexpr int kMaxEvents = 64;
    struct kevent events[kMaxEvents];
    timespec ts{};
    timespec* ts_ptr = nullptr;
    if (timeout.has_value()) {
      const auto count = std::max<std::int64_t>(0, timeout->count());
      ts.tv_sec = static_cast<time_t>(count / 1000);
      ts.tv_nsec = static_cast<long>((count % 1000) * 1000000);
      ts_ptr = &ts;
    }
    const int rc = kevent(queue_fd_, nullptr, 0, events, kMaxEvents, ts_ptr);
    if (rc <= 0) return {};
    std::vector<Handle> ready;
    for (int i = 0; i < rc; ++i) {
      const auto fd = static_cast<NativeSocket>(events[i].ident);
      auto it = waiters_.find(fd);
      if (it == waiters_.end()) continue;
      const bool is_read = events[i].filter == EVFILT_READ;
      const bool is_write = events[i].filter == EVFILT_WRITE;
      const bool wake_read = is_read || (events[i].flags & EV_ERROR) != 0 || (events[i].flags & EV_EOF) != 0;
      const bool wake_write = is_write || (events[i].flags & EV_ERROR) != 0;
      if (wake_read) {
        ready.insert(ready.end(), it->second.read.begin(), it->second.read.end());
        it->second.read.clear();
      }
      if (wake_write) {
        ready.insert(ready.end(), it->second.write.begin(), it->second.write.end());
        it->second.write.clear();
      }
      if (it->second.read.empty() && it->second.write.empty()) {
        waiters_.erase(it);
      }
      update_registration(fd);
    }
    return ready;
  }
#else
  std::vector<Handle> poll_epoll(std::optional<std::chrono::milliseconds> timeout) {
    constexpr int kMaxEvents = 64;
    epoll_event events[kMaxEvents];
    const int timeout_ms =
        timeout.has_value() ? static_cast<int>(std::max<std::int64_t>(0, timeout->count())) : -1;
    const int rc = epoll_wait(poll_fd_, events, kMaxEvents, timeout_ms);
    if (rc <= 0) return {};
    std::vector<Handle> ready;
    for (int i = 0; i < rc; ++i) {
      const auto fd = static_cast<NativeSocket>(events[i].data.fd);
      auto it = waiters_.find(fd);
      if (it == waiters_.end()) continue;
      const bool wake_read = (events[i].events & (EPOLLIN | EPOLLERR | EPOLLHUP)) != 0;
      const bool wake_write = (events[i].events & (EPOLLOUT | EPOLLERR | EPOLLHUP)) != 0;
      if (wake_read) {
        ready.insert(ready.end(), it->second.read.begin(), it->second.read.end());
        it->second.read.clear();
      }
      if (wake_write) {
        ready.insert(ready.end(), it->second.write.begin(), it->second.write.end());
        it->second.write.clear();
      }
      if (it->second.read.empty() && it->second.write.empty()) {
        waiters_.erase(it);
      }
      update_registration(fd);
    }
    return ready;
  }
#endif

  void update_registration(NativeSocket fd) {
#if defined(_WIN32)
    (void)fd;
#else
    const auto it = waiters_.find(fd);
    const bool want_read = it != waiters_.end() && !it->second.read.empty();
    const bool want_write = it != waiters_.end() && !it->second.write.empty();
    auto reg_it = registered_.find(fd);
    const bool has_reg = reg_it != registered_.end();
    const bool has_read = has_reg && reg_it->second.read;
    const bool has_write = has_reg && reg_it->second.write;
    if (want_read == has_read && want_write == has_write) return;

#if defined(__APPLE__)
    std::vector<struct kevent> changes;
    changes.reserve(4);
    auto add_change = [&](short filter, unsigned short flags) {
      struct kevent change{};
      EV_SET(&change, static_cast<uintptr_t>(fd), filter, flags, 0, 0, nullptr);
      changes.push_back(change);
    };
    if (has_read && !want_read) add_change(EVFILT_READ, EV_DELETE);
    if (has_write && !want_write) add_change(EVFILT_WRITE, EV_DELETE);
    if (!has_read && want_read) add_change(EVFILT_READ, EV_ADD | EV_ENABLE);
    if (!has_write && want_write) add_change(EVFILT_WRITE, EV_ADD | EV_ENABLE);
    if (!changes.empty() && kevent(queue_fd_, changes.data(), static_cast<int>(changes.size()), nullptr, 0,
                                   nullptr) != 0) {
      panic("failed to update kqueue registration");
    }
#else
    if (!want_read && !want_write) {
      if (has_reg) {
        epoll_ctl(poll_fd_, EPOLL_CTL_DEL, fd, nullptr);
      }
    } else {
      epoll_event ev{};
      if (want_read) ev.events |= EPOLLIN;
      if (want_write) ev.events |= EPOLLOUT;
      ev.events |= EPOLLERR | EPOLLHUP;
      ev.data.fd = fd;
      const int op = has_reg ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
      if (epoll_ctl(poll_fd_, op, fd, &ev) != 0) {
        panic("failed to update epoll registration");
      }
    }
#endif

    if (!want_read && !want_write) {
      if (has_reg) registered_.erase(fd);
    } else {
      registered_[fd] = InterestMask{want_read, want_write};
    }
#endif
  }
};

struct TaskStateBase;

class Scheduler {
public:
  using Clock = std::chrono::steady_clock;
  using Action = std::function<void()>;

private:
  struct TimerEntry {
    Clock::time_point deadline{};
    std::uint64_t seq = 0;
    Action action;
  };

  struct TimerLater {
    bool operator()(const TimerEntry& lhs, const TimerEntry& rhs) const {
      if (lhs.deadline != rhs.deadline) return lhs.deadline > rhs.deadline;
      return lhs.seq > rhs.seq;
    }
  };

  std::deque<Action> ready_;
  std::priority_queue<TimerEntry, std::vector<TimerEntry>, TimerLater> timers_;
  std::uint64_t next_seq_ = 1;
  std::vector<std::shared_ptr<TaskStateBase>> detached_tasks_;
  Reactor reactor_;

public:
  void schedule_action(Action action) { ready_.push_back(std::move(action)); }

  void schedule_handle(std::coroutine_handle<> handle) {
    schedule_action([handle]() mutable {
      if (handle && !handle.done()) handle.resume();
    });
  }

  Reactor& reactor() { return reactor_; }
  void detach_task(std::shared_ptr<TaskStateBase> task);

  void cancel_socket_waiters(NativeSocket fd) {
    for (auto handle : reactor_.cancel(fd)) schedule_handle(handle);
  }

  void schedule_after(Duration delay, Action action) {
    const auto clamped = std::max<std::int64_t>(0, delay.millis);
    timers_.push(TimerEntry{
        Clock::now() + std::chrono::milliseconds(clamped),
        next_seq_++,
        std::move(action),
    });
  }

  void pump_once() {
    reap_detached_tasks();
    while (ready_.empty()) {
      fire_due_timers();
      if (!ready_.empty()) break;

      std::optional<std::chrono::milliseconds> timeout;
      if (!timers_.empty()) {
        const auto now = Clock::now();
        const auto deadline = timers_.top().deadline;
        const auto delta = deadline > now ? deadline - now : Clock::duration::zero();
        timeout = std::chrono::duration_cast<std::chrono::milliseconds>(delta);
      }

      if (!reactor_.has_waiters()) {
        if (!timeout.has_value()) {
          panic("async runtime stalled: no ready tasks, timers, or io waiters");
        }
        std::this_thread::sleep_for(*timeout);
        continue;
      }

      for (auto handle : reactor_.poll(timeout)) schedule_handle(handle);
    }

    Action action = std::move(ready_.front());
    ready_.pop_front();
    action();
    fire_due_timers();
    reap_detached_tasks();
  }

  template <typename Pred>
  void run_until(Pred pred) {
    while (!pred()) pump_once();
  }

private:
  void reap_detached_tasks();

  void fire_due_timers() {
    const auto now = Clock::now();
    while (!timers_.empty() && timers_.top().deadline <= now) {
      Action action = std::move(const_cast<TimerEntry&>(timers_.top()).action);
      timers_.pop();
      ready_.push_back(std::move(action));
    }
  }
};

inline thread_local Scheduler* g_current_scheduler = nullptr;

inline Scheduler& current_scheduler() {
  if (g_current_scheduler == nullptr) panic("async runtime is not running");
  return *g_current_scheduler;
}

inline Result<void, std::string> ok_void_result() {
  return typename Result<void, std::string>::Ok{};
}

inline Result<void, std::string> err_void_result(std::string message) {
  return typename Result<void, std::string>::Err{std::move(message)};
}

template <typename T>
inline Result<T, std::string> ok_result(T value) {
  return typename Result<T, std::string>::Ok{std::move(value)};
}

template <typename T>
inline Result<T, std::string> err_result(std::string message) {
  return typename Result<T, std::string>::Err{std::move(message)};
}

inline bool env_has(const std::string& name) {
  return std::getenv(name.c_str()) != nullptr;
}

inline Result<std::string, std::string> env_get(const std::string& name) {
  const char* value = std::getenv(name.c_str());
  if (value == nullptr) return err_result<std::string>("environment variable is not set");
  return ok_result(std::string(value));
}

inline std::string env_or(std::string name, std::string fallback) {
  const char* value = std::getenv(name.c_str());
  if (value == nullptr) return fallback;
  return std::string(value);
}

inline Result<std::int64_t, std::string> parse_env_int(const std::string& text) {
  try {
    std::size_t parsed = 0;
    const auto value = std::stoll(text, &parsed);
    if (parsed != text.size()) {
      return err_result<std::int64_t>("environment variable is not a valid Int");
    }
    return ok_result<std::int64_t>(value);
  } catch (const std::exception&) {
    return err_result<std::int64_t>("environment variable is not a valid Int");
  }
}

inline Result<std::int64_t, std::string> env_get_int(const std::string& name) {
  auto value = env_get(name);
  if (std::holds_alternative<typename Result<std::string, std::string>::Err>(value.data)) {
    return err_result<std::int64_t>(
        std::get<typename Result<std::string, std::string>::Err>(value.data).value);
  }
  return parse_env_int(std::get<typename Result<std::string, std::string>::Ok>(value.data).value);
}

inline Result<std::int64_t, std::string> env_get_int_or(const std::string& name,
                                                        std::int64_t fallback) {
  if (!env_has(name)) return ok_result<std::int64_t>(fallback);
  return env_get_int(name);
}

inline bool utf8_continuation_byte(unsigned char byte) {
  return (byte & 0xC0u) == 0x80u;
}

inline bool is_valid_utf8(std::string_view text) {
  std::size_t i = 0;
  while (i < text.size()) {
    const unsigned char lead = static_cast<unsigned char>(text[i]);
    if (lead <= 0x7Fu) {
      i += 1;
      continue;
    }
    if (lead >= 0xC2u && lead <= 0xDFu) {
      if (i + 1 >= text.size()) return false;
      if (!utf8_continuation_byte(static_cast<unsigned char>(text[i + 1]))) return false;
      i += 2;
      continue;
    }
    if (lead == 0xE0u) {
      if (i + 2 >= text.size()) return false;
      const unsigned char b1 = static_cast<unsigned char>(text[i + 1]);
      const unsigned char b2 = static_cast<unsigned char>(text[i + 2]);
      if (b1 < 0xA0u || b1 > 0xBFu || !utf8_continuation_byte(b2)) return false;
      i += 3;
      continue;
    }
    if (lead >= 0xE1u && lead <= 0xECu) {
      if (i + 2 >= text.size()) return false;
      if (!utf8_continuation_byte(static_cast<unsigned char>(text[i + 1])) ||
          !utf8_continuation_byte(static_cast<unsigned char>(text[i + 2]))) {
        return false;
      }
      i += 3;
      continue;
    }
    if (lead == 0xEDu) {
      if (i + 2 >= text.size()) return false;
      const unsigned char b1 = static_cast<unsigned char>(text[i + 1]);
      const unsigned char b2 = static_cast<unsigned char>(text[i + 2]);
      if (b1 < 0x80u || b1 > 0x9Fu || !utf8_continuation_byte(b2)) return false;
      i += 3;
      continue;
    }
    if (lead >= 0xEEu && lead <= 0xEFu) {
      if (i + 2 >= text.size()) return false;
      if (!utf8_continuation_byte(static_cast<unsigned char>(text[i + 1])) ||
          !utf8_continuation_byte(static_cast<unsigned char>(text[i + 2]))) {
        return false;
      }
      i += 3;
      continue;
    }
    if (lead == 0xF0u) {
      if (i + 3 >= text.size()) return false;
      const unsigned char b1 = static_cast<unsigned char>(text[i + 1]);
      const unsigned char b2 = static_cast<unsigned char>(text[i + 2]);
      const unsigned char b3 = static_cast<unsigned char>(text[i + 3]);
      if (b1 < 0x90u || b1 > 0xBFu || !utf8_continuation_byte(b2) || !utf8_continuation_byte(b3)) {
        return false;
      }
      i += 4;
      continue;
    }
    if (lead >= 0xF1u && lead <= 0xF3u) {
      if (i + 3 >= text.size()) return false;
      if (!utf8_continuation_byte(static_cast<unsigned char>(text[i + 1])) ||
          !utf8_continuation_byte(static_cast<unsigned char>(text[i + 2])) ||
          !utf8_continuation_byte(static_cast<unsigned char>(text[i + 3]))) {
        return false;
      }
      i += 4;
      continue;
    }
    if (lead == 0xF4u) {
      if (i + 3 >= text.size()) return false;
      const unsigned char b1 = static_cast<unsigned char>(text[i + 1]);
      const unsigned char b2 = static_cast<unsigned char>(text[i + 2]);
      const unsigned char b3 = static_cast<unsigned char>(text[i + 3]);
      if (b1 < 0x80u || b1 > 0x8Fu || !utf8_continuation_byte(b2) || !utf8_continuation_byte(b3)) {
        return false;
      }
      i += 4;
      continue;
    }
    return false;
  }
  return true;
}

inline std::string fs_error(std::string_view prefix,
                            const std::string& path,
                            const std::error_code& ec) {
  if (ec) return std::string(prefix) + ": " + path + " (" + ec.message() + ")";
  return std::string(prefix) + ": " + path;
}

inline Result<bool, std::string> fs_exists(const std::string& path) {
  std::error_code ec;
  const bool value = hostfs::exists(hostfs::path(path), ec);
  if (ec) return err_result<bool>(fs_error("failed to stat path", path, ec));
  return ok_result<bool>(value);
}

inline Result<bool, std::string> fs_is_file(const std::string& path) {
  std::error_code ec;
  const bool value = hostfs::is_regular_file(hostfs::path(path), ec);
  if (ec) return err_result<bool>(fs_error("failed to stat path", path, ec));
  return ok_result<bool>(value);
}

inline Result<bool, std::string> fs_is_dir(const std::string& path) {
  std::error_code ec;
  const bool value = hostfs::is_directory(hostfs::path(path), ec);
  if (ec) return err_result<bool>(fs_error("failed to stat path", path, ec));
  return ok_result<bool>(value);
}

inline Result<Bytes, std::string> fs_read_bytes(const std::string& path) {
  const hostfs::path file_path(path);
  std::error_code ec;
  if (!hostfs::exists(file_path, ec)) {
    if (ec) return err_result<Bytes>(fs_error("failed to stat path", path, ec));
    return err_result<Bytes>("path does not exist: " + path);
  }
  if (!hostfs::is_regular_file(file_path, ec)) {
    if (ec) return err_result<Bytes>(fs_error("failed to stat path", path, ec));
    return err_result<Bytes>("path is not a regular file: " + path);
  }
  std::ifstream in(file_path, std::ios::binary);
  if (!in) return err_result<Bytes>("failed to open file for read: " + path);
  std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (!in.good() && !in.eof()) return err_result<Bytes>("failed to read file: " + path);
  return ok_result(Bytes{std::move(data)});
}

inline Result<std::string, std::string> fs_read_string(const std::string& path) {
  auto bytes = fs_read_bytes(path);
  if (std::holds_alternative<typename Result<Bytes, std::string>::Err>(bytes.data)) {
    return err_result<std::string>(
        std::get<typename Result<Bytes, std::string>::Err>(bytes.data).value);
  }
  auto data = std::get<typename Result<Bytes, std::string>::Ok>(bytes.data).value.data;
  if (!is_valid_utf8(data)) return err_result<std::string>("file is not valid UTF-8: " + path);
  return ok_result(std::move(data));
}

inline Result<void, std::string> fs_write_bytes(const std::string& path, Bytes data) {
  const hostfs::path file_path(path);
  std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
  if (!out) return err_void_result("failed to open file for write: " + path);
  out.write(data.data.data(), static_cast<std::streamsize>(data.data.size()));
  if (!out) return err_void_result("failed to write file: " + path);
  return ok_void_result();
}

inline Result<void, std::string> fs_write_string(const std::string& path, std::string text) {
  return fs_write_bytes(path, Bytes{std::move(text)});
}

inline Result<void, std::string> fs_create_dir_all(const std::string& path) {
  const hostfs::path dir_path(path);
  std::error_code ec;
  if (hostfs::exists(dir_path, ec)) {
    if (ec) return err_void_result(fs_error("failed to stat path", path, ec));
    if (!hostfs::is_directory(dir_path, ec)) {
      if (ec) return err_void_result(fs_error("failed to stat path", path, ec));
      return err_void_result("path is not a directory: " + path);
    }
    return ok_void_result();
  }
  if (ec) return err_void_result(fs_error("failed to stat path", path, ec));
  hostfs::create_directories(dir_path, ec);
  if (ec) return err_void_result(fs_error("failed to create directories", path, ec));
  return ok_void_result();
}

inline Result<void, std::string> close_socket_handle(const std::shared_ptr<SocketHandle>& handle) {
  if (!handle || !handle->valid()) return ok_void_result();
  const NativeSocket fd = std::exchange(handle->fd, kInvalidNativeSocket);
  if (g_current_scheduler != nullptr) g_current_scheduler->cancel_socket_waiters(fd);
  close_native_socket(fd);
  return ok_void_result();
}

struct IoAwaitable {
  NativeSocket fd = kInvalidNativeSocket;
  Reactor::Interest interest = Reactor::Interest::Read;

  bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> handle) {
    current_scheduler().reactor().add_waiter(fd, interest, handle);
  }

  void await_resume() const noexcept {}
};

struct TaskStateBase {
  Scheduler* scheduler = nullptr;
  std::vector<std::coroutine_handle<>> waiters;

  void schedule_waiters();

  virtual bool done() const = 0;

  virtual ~TaskStateBase() = default;
};

inline void Scheduler::detach_task(std::shared_ptr<TaskStateBase> task) {
  if (!task || task->done()) return;
  detached_tasks_.push_back(std::move(task));
}

inline void Scheduler::reap_detached_tasks() {
  std::erase_if(detached_tasks_, [](const std::shared_ptr<TaskStateBase>& task) {
    return !task || task->done();
  });
}

inline void TaskStateBase::schedule_waiters() {
  if (scheduler == nullptr) return;
  for (auto waiter : waiters) scheduler->schedule_handle(waiter);
  waiters.clear();
}

template <typename T>
class Future;

template <typename Promise>
struct TaskStateImpl final : TaskStateBase {
  using handle_type = std::coroutine_handle<Promise>;
  using value_type = typename Promise::value_type;

  handle_type handle{};

  explicit TaskStateImpl(handle_type h) : handle(h) {}

  ~TaskStateImpl() override {
    if (handle) handle.destroy();
  }

  bool done() const override { return !handle || handle.done(); }

  decltype(auto) consume_result() {
    if constexpr (std::is_void_v<value_type>) {
      handle.promise().take_result();
    } else {
      return handle.promise().take_result();
    }
  }
};

template <typename T>
class Task {
public:
  using promise_type = typename Future<T>::promise_type;
  using state_type = TaskStateImpl<promise_type>;

private:
  std::shared_ptr<state_type> state_;

public:
  Task() = default;
  explicit Task(std::shared_ptr<state_type> state) : state_(std::move(state)) {}

  bool ready() const { return !state_ || state_->done(); }

  std::shared_ptr<TaskStateBase> abandon() {
    auto state = std::static_pointer_cast<TaskStateBase>(state_);
    state_.reset();
    return state;
  }

  struct Awaiter {
    std::shared_ptr<state_type> state;

    bool await_ready() const { return !state || state->done(); }

    bool await_suspend(std::coroutine_handle<> parent) {
      if (!state || state->done()) return false;
      state->waiters.push_back(parent);
      return true;
    }

    decltype(auto) await_resume() {
      if constexpr (std::is_void_v<T>) {
        state->consume_result();
      } else {
        return state->consume_result();
      }
    }
  };

  Awaiter operator co_await() && { return Awaiter{std::move(state_)}; }
  Awaiter operator co_await() & { return Awaiter{std::exchange(state_, {})}; }
};

template <typename T>
class Future {
public:
  struct promise_type {
    using value_type = T;
    using handle_type = std::coroutine_handle<promise_type>;

    Scheduler* scheduler = nullptr;
    std::coroutine_handle<> continuation{};
    TaskStateBase* task_state = nullptr;
    std::optional<T> value;
    std::exception_ptr error;

    Future get_return_object() { return Future(handle_type::from_promise(*this)); }
    std::suspend_always initial_suspend() noexcept { return {}; }

    struct FinalAwaiter {
      bool await_ready() const noexcept { return false; }

      template <typename Promise>
      void await_suspend(std::coroutine_handle<Promise> handle) noexcept {
        auto& promise = handle.promise();
        if (promise.task_state != nullptr) {
          promise.task_state->scheduler = promise.scheduler;
          promise.task_state->schedule_waiters();
        }
        if (promise.continuation && promise.scheduler != nullptr) {
          promise.scheduler->schedule_handle(promise.continuation);
        }
      }

      void await_resume() const noexcept {}
    };

    FinalAwaiter final_suspend() noexcept { return {}; }

    template <typename U>
    void return_value(U&& result) {
      value.emplace(std::forward<U>(result));
    }

    void unhandled_exception() { error = std::current_exception(); }

    T take_result() {
      if (error) std::rethrow_exception(error);
      if (!value.has_value()) panic("async future completed without a value");
      T out = std::move(*value);
      value.reset();
      return out;
    }
  };

  using handle_type = std::coroutine_handle<promise_type>;

private:
  handle_type handle_{};

public:
  Future() = default;
  explicit Future(handle_type handle) : handle_(handle) {}
  Future(const Future&) = delete;
  Future& operator=(const Future&) = delete;

  Future(Future&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}

  Future& operator=(Future&& other) noexcept {
    if (this == &other) return *this;
    if (handle_) handle_.destroy();
    handle_ = std::exchange(other.handle_, {});
    return *this;
  }

  ~Future() {
    if (handle_) handle_.destroy();
  }

  bool done() const { return !handle_ || handle_.done(); }

  void start(Scheduler& scheduler) {
    if (!handle_) return;
    handle_.promise().scheduler = &scheduler;
    scheduler.schedule_handle(std::coroutine_handle<>(handle_));
  }

  T consume_result() {
    if (!handle_) panic("future has no coroutine state");
    T out = handle_.promise().take_result();
    handle_.destroy();
    handle_ = {};
    return out;
  }

  handle_type release() { return std::exchange(handle_, {}); }

  struct Awaiter {
    handle_type handle{};

    bool await_ready() const { return !handle || handle.done(); }

    bool await_suspend(std::coroutine_handle<> parent) {
      if (!handle) return false;
      auto& promise = handle.promise();
      promise.continuation = parent;
      if (promise.scheduler == nullptr) promise.scheduler = &current_scheduler();
      promise.scheduler->schedule_handle(std::coroutine_handle<>(handle));
      return true;
    }

    T await_resume() {
      if (!handle) panic("awaited future has no coroutine state");
      auto& promise = handle.promise();
      T out = promise.take_result();
      handle.destroy();
      handle = {};
      return out;
    }
  };

  Awaiter operator co_await() && { return Awaiter{std::exchange(handle_, {})}; }
  Awaiter operator co_await() & { return Awaiter{std::exchange(handle_, {})}; }
};

template <>
class Future<void> {
public:
  struct promise_type {
    using value_type = void;
    using handle_type = std::coroutine_handle<promise_type>;

    Scheduler* scheduler = nullptr;
    std::coroutine_handle<> continuation{};
    TaskStateBase* task_state = nullptr;
    std::exception_ptr error;

    Future get_return_object() { return Future(handle_type::from_promise(*this)); }
    std::suspend_always initial_suspend() noexcept { return {}; }

    struct FinalAwaiter {
      bool await_ready() const noexcept { return false; }

      template <typename Promise>
      void await_suspend(std::coroutine_handle<Promise> handle) noexcept {
        auto& promise = handle.promise();
        if (promise.task_state != nullptr) {
          promise.task_state->scheduler = promise.scheduler;
          promise.task_state->schedule_waiters();
        }
        if (promise.continuation && promise.scheduler != nullptr) {
          promise.scheduler->schedule_handle(promise.continuation);
        }
      }

      void await_resume() const noexcept {}
    };

    FinalAwaiter final_suspend() noexcept { return {}; }
    void return_void() {}
    void unhandled_exception() { error = std::current_exception(); }

    void take_result() {
      if (error) std::rethrow_exception(error);
    }
  };

  using handle_type = std::coroutine_handle<promise_type>;

private:
  handle_type handle_{};

public:
  Future() = default;
  explicit Future(handle_type handle) : handle_(handle) {}
  Future(const Future&) = delete;
  Future& operator=(const Future&) = delete;

  Future(Future&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}

  Future& operator=(Future&& other) noexcept {
    if (this == &other) return *this;
    if (handle_) handle_.destroy();
    handle_ = std::exchange(other.handle_, {});
    return *this;
  }

  ~Future() {
    if (handle_) handle_.destroy();
  }

  bool done() const { return !handle_ || handle_.done(); }

  void start(Scheduler& scheduler) {
    if (!handle_) return;
    handle_.promise().scheduler = &scheduler;
    scheduler.schedule_handle(std::coroutine_handle<>(handle_));
  }

  void consume_result() {
    if (!handle_) panic("future has no coroutine state");
    handle_.promise().take_result();
    handle_.destroy();
    handle_ = {};
  }

  handle_type release() { return std::exchange(handle_, {}); }

  struct Awaiter {
    handle_type handle{};

    bool await_ready() const { return !handle || handle.done(); }

    bool await_suspend(std::coroutine_handle<> parent) {
      if (!handle) return false;
      auto& promise = handle.promise();
      promise.continuation = parent;
      if (promise.scheduler == nullptr) promise.scheduler = &current_scheduler();
      promise.scheduler->schedule_handle(std::coroutine_handle<>(handle));
      return true;
    }

    void await_resume() {
      if (!handle) panic("awaited future has no coroutine state");
      handle.promise().take_result();
      handle.destroy();
      handle = {};
    }
  };

  Awaiter operator co_await() && { return Awaiter{std::exchange(handle_, {})}; }
  Awaiter operator co_await() & { return Awaiter{std::exchange(handle_, {})}; }
};

template <typename T>
Task<T> spawn(Future<T> future) {
  using promise_type = typename Future<T>::promise_type;
  using state_type = TaskStateImpl<promise_type>;

  auto handle = future.release();
  if (!handle) panic("spawn requires a valid future");

  auto& scheduler = current_scheduler();
  handle.promise().scheduler = &scheduler;

  auto state = std::make_shared<state_type>(handle);
  handle.promise().task_state = state.get();
  state->scheduler = &scheduler;
  scheduler.schedule_action([state]() mutable {
    if (state->handle && !state->handle.done()) state->handle.resume();
  });
  return Task<T>(std::move(state));
}

template <typename T>
Task<T> join(Task<T> task) {
  return task;
}

template <typename T>
decltype(auto) block_on(Future<T> future) {
  Scheduler scheduler;
  Scheduler* prev = g_current_scheduler;
  g_current_scheduler = &scheduler;
  future.start(scheduler);
  scheduler.run_until([&future]() { return future.done(); });
  g_current_scheduler = prev;
  if constexpr (std::is_void_v<T>) {
    future.consume_result();
  } else {
    return future.consume_result();
  }
}

struct YieldAwaitable {
  bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> handle) {
    current_scheduler().schedule_handle(handle);
  }

  void await_resume() const noexcept {}
};

struct SleepAwaitable {
  Duration delay{};

  bool await_ready() const noexcept { return delay.millis <= 0; }

  void await_suspend(std::coroutine_handle<> handle) {
    current_scheduler().schedule_after(delay, [handle]() mutable {
      if (handle && !handle.done()) handle.resume();
    });
  }

  void await_resume() const noexcept {}
};

inline Future<void> yield_now() {
  co_await YieldAwaitable{};
  co_return;
}

inline Future<void> sleep(Duration delay) {
  co_await SleepAwaitable{delay};
  co_return;
}

inline std::shared_ptr<SocketHandle> require_open_handle(const std::shared_ptr<SocketHandle>& handle,
                                                         const char* label,
                                                         std::string& error) {
  if (handle && handle->valid()) return handle;
  error = std::string(label) + " is closed";
  return {};
}

inline Future<Result<TcpListener, std::string>> bind_listener(SocketAddr addr) {
  ensure_socket_runtime_initialized();
  auto family = socketaddr_family(addr);
  if (std::holds_alternative<typename Result<int, std::string>::Err>(family.data)) {
    co_return err_result<TcpListener>(
        std::get<typename Result<int, std::string>::Err>(std::move(family.data)).value);
  }

  NativeSocket fd =
      ::socket(std::get<typename Result<int, std::string>::Ok>(family.data).value, SOCK_STREAM, IPPROTO_TCP);
  if (!native_socket_valid(fd)) {
    co_return err_result<TcpListener>("socket failed: " + socket_error_message(last_socket_error_code()));
  }

  auto guard = std::shared_ptr<SocketHandle>(new SocketHandle(fd));
  int reuse = 1;
#if defined(_WIN32)
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
  if (!set_socket_nonblocking(fd)) {
    const int err = last_socket_error_code();
    co_return err_result<TcpListener>("failed to configure listener nonblocking: " + socket_error_message(err));
  }
  if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr.storage), addr.length) != 0) {
    const int err = last_socket_error_code();
    co_return err_result<TcpListener>("bind failed: " + socket_error_message(err));
  }
  if (::listen(fd, SOMAXCONN) != 0) {
    const int err = last_socket_error_code();
    co_return err_result<TcpListener>("listen failed: " + socket_error_message(err));
  }
  co_return ok_result(TcpListener{std::move(guard)});
}

inline Future<Result<TcpStream, std::string>> connect_stream(SocketAddr addr) {
  ensure_socket_runtime_initialized();
  auto family = socketaddr_family(addr);
  if (std::holds_alternative<typename Result<int, std::string>::Err>(family.data)) {
    co_return err_result<TcpStream>(
        std::get<typename Result<int, std::string>::Err>(std::move(family.data)).value);
  }

  NativeSocket fd =
      ::socket(std::get<typename Result<int, std::string>::Ok>(family.data).value, SOCK_STREAM, IPPROTO_TCP);
  if (!native_socket_valid(fd)) {
    co_return err_result<TcpStream>("socket failed: " + socket_error_message(last_socket_error_code()));
  }

  auto guard = std::shared_ptr<SocketHandle>(new SocketHandle(fd));
  if (!set_socket_nonblocking(fd)) {
    const int err = last_socket_error_code();
    co_return err_result<TcpStream>("failed to configure stream nonblocking: " + socket_error_message(err));
  }

  while (true) {
    const int rc = ::connect(fd, reinterpret_cast<const sockaddr*>(&addr.storage), addr.length);
    if (rc == 0) break;
    const int err = last_socket_error_code();
    if (socket_error_is_interrupted(err)) continue;
    if (!socket_error_is_would_block(err)) {
      co_return err_result<TcpStream>("connect failed: " + socket_error_message(err));
    }
    co_await IoAwaitable{fd, Reactor::Interest::Write};
    int so_error = 0;
    SocketLength so_len = static_cast<SocketLength>(sizeof(so_error));
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &so_len) != 0) {
      const int opt_err = last_socket_error_code();
      co_return err_result<TcpStream>("connect status failed: " + socket_error_message(opt_err));
    }
    if (so_error == 0) break;
    if (!socket_error_is_would_block(so_error)) {
      co_return err_result<TcpStream>("connect failed: " + socket_error_message(so_error));
    }
  }

  co_return ok_result(TcpStream{std::move(guard)});
}

inline Future<Result<SocketAddr, std::string>> resolve_socketaddr(std::string host, std::int64_t port) {
  struct ResolveState {
    Result<SocketAddr, std::string> result =
        typename Result<SocketAddr, std::string>::Err{"resolve did not complete"};
  };

  auto wake_pair = create_wake_socket_pair();
  if (std::holds_alternative<typename Result<WakeSocketPair, std::string>::Err>(wake_pair.data)) {
    co_return err_result<SocketAddr>(
        std::get<typename Result<WakeSocketPair, std::string>::Err>(std::move(wake_pair.data)).value);
  }
  auto pair = std::get<typename Result<WakeSocketPair, std::string>::Ok>(std::move(wake_pair.data)).value;
  auto reader = std::shared_ptr<SocketHandle>(new SocketHandle(pair.reader));
  auto writer = std::shared_ptr<SocketHandle>(new SocketHandle(pair.writer));
  auto state = std::make_shared<ResolveState>();

  std::thread([state, writer, host = std::move(host), port]() mutable {
    state->result = resolve_socketaddr_blocking(host, port);
    notify_wake_socket(writer->fd);
    close_socket_handle(writer);
  }).detach();

  while (true) {
    auto wake_result = wait_for_wake_socket(reader);
    if (std::holds_alternative<typename Result<void, std::string>::Ok>(wake_result.data)) break;
    auto error = std::get<typename Result<void, std::string>::Err>(std::move(wake_result.data)).value;
    if (error == "resolver wake pending") {
      co_await IoAwaitable{reader->fd, Reactor::Interest::Read};
      continue;
    }
    close_socket_handle(reader);
    co_return err_result<SocketAddr>(std::move(error));
  }

  close_socket_handle(reader);
  co_return std::move(state->result);
}

inline Future<Result<TcpStream, std::string>> connect_stream_host(std::string host, std::int64_t port) {
  auto resolved = co_await resolve_socketaddr(std::move(host), port);
  if (std::holds_alternative<typename Result<SocketAddr, std::string>::Err>(resolved.data)) {
    co_return err_result<TcpStream>(
        std::get<typename Result<SocketAddr, std::string>::Err>(std::move(resolved.data)).value);
  }
  co_return co_await connect_stream(
      std::get<typename Result<SocketAddr, std::string>::Ok>(std::move(resolved.data)).value);
}

inline Future<Result<TcpStream, std::string>> accept_stream(TcpListener listener) {
  std::string error;
  auto handle = require_open_handle(listener.handle, "listener", error);
  if (!handle) co_return err_result<TcpStream>(std::move(error));

  while (true) {
    if (!handle->valid()) {
      co_return err_result<TcpStream>("listener is closed");
    }
    sockaddr_storage client_addr{};
    SocketLength client_len = static_cast<SocketLength>(sizeof(client_addr));
    NativeSocket client_fd =
        ::accept(handle->fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
    if (native_socket_valid(client_fd)) {
      auto client = std::shared_ptr<SocketHandle>(new SocketHandle(client_fd));
      if (!set_socket_nonblocking(client_fd)) {
        const int err = last_socket_error_code();
        close_socket_handle(client);
        co_return err_result<TcpStream>("failed to configure accepted stream nonblocking: " +
                                        socket_error_message(err));
      }
      co_return ok_result(TcpStream{std::move(client)});
    }
    const int err = last_socket_error_code();
    if (socket_error_is_interrupted(err)) continue;
    if (!socket_error_is_would_block(err)) {
      co_return err_result<TcpStream>("accept failed: " + socket_error_message(err));
    }
    co_await IoAwaitable{handle->fd, Reactor::Interest::Read};
  }
}

inline Future<Result<TcpStream, std::string>> accept_stream_timeout(TcpListener listener,
                                                                    std::int64_t timeout_ms) {
  if (timeout_ms <= 0) {
    co_return err_result<TcpStream>("accept timeout must be positive");
  }
  std::string error;
  auto handle = require_open_handle(listener.handle, "listener", error);
  if (!handle) co_return err_result<TcpStream>(std::move(error));
  const auto deadline = Scheduler::Clock::now() + std::chrono::milliseconds(timeout_ms);

  while (true) {
    if (!handle->valid()) {
      co_return err_result<TcpStream>("listener is closed");
    }
    sockaddr_storage client_addr{};
    SocketLength client_len = static_cast<SocketLength>(sizeof(client_addr));
    const NativeSocket client_fd =
        ::accept(handle->fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
    if (native_socket_valid(client_fd)) {
      auto client = std::shared_ptr<SocketHandle>(new SocketHandle(client_fd));
      if (!set_socket_nonblocking(client_fd)) {
        const int err = last_socket_error_code();
        close_socket_handle(client);
        co_return err_result<TcpStream>("failed to configure accepted stream nonblocking: " +
                                        socket_error_message(err));
      }
      co_return ok_result(TcpStream{std::move(client)});
    }

    const int err = last_socket_error_code();
    if (socket_error_is_interrupted(err)) continue;
    if (!socket_error_is_would_block(err)) {
      co_return err_result<TcpStream>("accept failed: " + socket_error_message(err));
    }

    const auto now = Scheduler::Clock::now();
    if (now >= deadline) {
      co_return err_result<TcpStream>("timeout");
    }
    const auto remaining_ms =
        std::max<std::int64_t>(1, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
    co_await sleep(Duration{std::min<std::int64_t>(remaining_ms, 10)});
  }
}

inline Future<Result<Bytes, std::string>> read_stream(TcpStream stream, std::int64_t max_bytes) {
  std::string error;
  auto handle = require_open_handle(stream.handle, "stream", error);
  if (!handle) co_return err_result<Bytes>(std::move(error));
  if (max_bytes < 0) {
    co_return err_result<Bytes>("read max_bytes must be non-negative");
  }
  if (max_bytes == 0) {
    co_return ok_result(Bytes{});
  }

  std::string buffer(static_cast<std::size_t>(max_bytes), '\0');
  while (true) {
#if defined(_WIN32)
    const int rc = recv(handle->fd, buffer.data(), static_cast<int>(buffer.size()), 0);
#else
    const ssize_t rc = recv(handle->fd, buffer.data(), buffer.size(), 0);
#endif
    if (rc > 0) {
      buffer.resize(static_cast<std::size_t>(rc));
      co_return ok_result(Bytes{std::move(buffer)});
    }
    if (rc == 0) {
      co_return ok_result(Bytes{});
    }
    const int err = last_socket_error_code();
    if (socket_error_is_interrupted(err)) continue;
    if (!socket_error_is_would_block(err)) {
      co_return err_result<Bytes>("read failed: " + socket_error_message(err));
    }
    co_await IoAwaitable{handle->fd, Reactor::Interest::Read};
  }
}

inline Future<Result<std::int64_t, std::string>> write_stream(TcpStream stream, Bytes bytes) {
  std::string error;
  auto handle = require_open_handle(stream.handle, "stream", error);
  if (!handle) co_return err_result<std::int64_t>(std::move(error));
  if (bytes.data.empty()) co_return ok_result<std::int64_t>(0);

  std::size_t offset = 0;
  while (true) {
#if defined(_WIN32)
    const int rc =
        send(handle->fd, bytes.data.data() + offset, static_cast<int>(bytes.data.size() - offset), 0);
#else
    const ssize_t rc =
        send(handle->fd, bytes.data.data() + offset, bytes.data.size() - offset, socket_send_flags());
#endif
    if (rc > 0) {
      co_return ok_result<std::int64_t>(static_cast<std::int64_t>(rc));
    }
    const int err = last_socket_error_code();
    if (socket_error_is_interrupted(err)) continue;
    if (!socket_error_is_would_block(err)) {
      co_return err_result<std::int64_t>("write failed: " + socket_error_message(err));
    }
    co_await IoAwaitable{handle->fd, Reactor::Interest::Write};
  }
}

inline Future<Result<void, std::string>> write_stream_all(TcpStream stream, Bytes bytes) {
  std::string error;
  auto handle = require_open_handle(stream.handle, "stream", error);
  if (!handle) co_return err_void_result(std::move(error));

  std::size_t offset = 0;
  while (offset < bytes.data.size()) {
#if defined(_WIN32)
    const int rc =
        send(handle->fd, bytes.data.data() + offset, static_cast<int>(bytes.data.size() - offset), 0);
#else
    const ssize_t rc =
        send(handle->fd, bytes.data.data() + offset, bytes.data.size() - offset, socket_send_flags());
#endif
    if (rc > 0) {
      offset += static_cast<std::size_t>(rc);
      continue;
    }
    const int err = last_socket_error_code();
    if (socket_error_is_interrupted(err)) continue;
    if (!socket_error_is_would_block(err)) {
      co_return err_void_result("write_all failed: " + socket_error_message(err));
    }
    co_await IoAwaitable{handle->fd, Reactor::Interest::Write};
  }
  co_return ok_void_result();
}

template <typename T>
Future<Result<T, std::string>> timeout(Duration delay, Future<T> future) {
  auto task = spawn(std::move(future));
  const auto deadline = Scheduler::Clock::now() + std::chrono::milliseconds(std::max<std::int64_t>(0, delay.millis));
  while (!task.ready()) {
    const auto now = Scheduler::Clock::now();
    if (now >= deadline) {
      current_scheduler().detach_task(task.abandon());
      co_return typename Result<T, std::string>::Err{"timeout"};
    }
    const auto remaining_ms =
        std::max<std::int64_t>(1, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
    co_await sleep(Duration{std::min<std::int64_t>(remaining_ms, 1)});
  }
  if constexpr (std::is_void_v<T>) {
    co_await std::move(task);
    co_return typename Result<void, std::string>::Ok{};
  } else {
    co_return typename Result<T, std::string>::Ok{co_await std::move(task)};
  }
}

inline Result<SocketAddr, std::string> ipv4(const std::string& host, std::int64_t port) {
  return make_socketaddr_ipv4(host, port);
}

inline Result<SocketAddr, std::string> listener_local_addr(TcpListener listener) {
  std::string error;
  auto handle = require_open_handle(listener.handle, "listener", error);
  if (!handle) return err_result<SocketAddr>(std::move(error));
  return socket_name_of(handle->fd, false);
}

inline Result<void, std::string> listener_close(TcpListener listener) {
  return close_socket_handle(listener.handle);
}

inline Result<SocketAddr, std::string> stream_peer_addr(TcpStream stream) {
  std::string error;
  auto handle = require_open_handle(stream.handle, "stream", error);
  if (!handle) return err_result<SocketAddr>(std::move(error));
  return socket_name_of(handle->fd, true);
}

inline Result<SocketAddr, std::string> stream_local_addr(TcpStream stream) {
  std::string error;
  auto handle = require_open_handle(stream.handle, "stream", error);
  if (!handle) return err_result<SocketAddr>(std::move(error));
  return socket_name_of(handle->fd, false);
}

inline Result<void, std::string> stream_close(TcpStream stream) {
  return close_socket_handle(stream.handle);
}

inline Result<std::string, std::string> socket_addr_host(SocketAddr addr) {
  return socket_addr_host_text(std::move(addr));
}

inline Result<std::int64_t, std::string> socket_addr_port_value(SocketAddr addr) {
  return socket_addr_port(std::move(addr));
}

inline Bytes bytes_from_string(std::string s) {
  return Bytes{std::move(s)};
}

inline std::string bytes_to_string(const Bytes& b) {
  return b.data;
}

inline bool bytes_equal_string(const Bytes& b, std::string_view text) {
  return b.data == text;
}

inline std::int64_t bytes_len(const Bytes& b) {
  return static_cast<std::int64_t>(b.data.size());
}

inline bool bytes_is_empty(const Bytes& b) {
  return b.data.empty();
}

inline Bytes bytes_concat(Bytes lhs, Bytes rhs) {
  if (lhs.data.empty()) return rhs;
  if (rhs.data.empty()) return lhs;
  lhs.data.reserve(lhs.data.size() + rhs.data.size());
  lhs.data += rhs.data;
  return lhs;
}

template <typename... Parts>
inline Bytes bytes_concat_all(const Parts&... parts) {
  Bytes out;
  const std::size_t total = (static_cast<std::size_t>(parts.data.size()) + ... + std::size_t{0});
  out.data.reserve(total);
  (out.data.append(parts.data), ...);
  return out;
}

inline bool bytes_equal(const Bytes& lhs, const Bytes& rhs) {
  return lhs.data == rhs.data;
}

inline std::string json_escape_string(std::string_view text) {
  const auto first_escape = std::find_if(text.begin(), text.end(), [](char ch) {
    return ch == '\\' || ch == '"' || static_cast<unsigned char>(ch) < 0x20 || ch == '\b' ||
           ch == '\f' || ch == '\n' || ch == '\r' || ch == '\t';
  });
  if (first_escape == text.end()) return std::string(text);

  std::string out;
  out.reserve(text.size() + 8);
  out.append(text.data(), static_cast<std::size_t>(first_escape - text.begin()));
  for (auto it = first_escape; it != text.end(); ++it) {
    const char ch = *it;
    switch (ch) {
    case '\\': out += "\\\\"; break;
    case '"': out += "\\\""; break;
    case '\b': out += "\\b"; break;
    case '\f': out += "\\f"; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default:
      if (static_cast<unsigned char>(ch) < 0x20) {
        constexpr char digits[] = "0123456789abcdef";
        const unsigned char value = static_cast<unsigned char>(ch);
        out += "\\u00";
        out.push_back(digits[(value >> 4) & 0x0F]);
        out.push_back(digits[value & 0x0F]);
      } else {
        out.push_back(ch);
      }
      break;
    }
  }
  return out;
}

inline bool append_utf8(std::string& out, std::uint32_t code_point) {
  if (code_point <= 0x7F) {
    out.push_back(static_cast<char>(code_point));
    return true;
  }
  if (code_point <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | ((code_point >> 6) & 0x1F)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    return true;
  }
  if (code_point >= 0xD800 && code_point <= 0xDFFF) return false;
  if (code_point <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | ((code_point >> 12) & 0x0F)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    return true;
  }
  if (code_point <= 0x10FFFF) {
    out.push_back(static_cast<char>(0xF0 | ((code_point >> 18) & 0x07)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    return true;
  }
  return false;
}

class JsonCursor {
public:
  explicit JsonCursor(std::string_view text) : text_(text) {}

  void skip_ws() {
    while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) pos_ += 1;
  }

  bool eof() const { return pos_ >= text_.size(); }
  std::size_t pos() const { return pos_; }
  void set_pos(std::size_t pos) { pos_ = pos; }
  char peek() const { return eof() ? '\0' : text_[pos_]; }
  bool consume(char expected) {
    skip_ws();
    if (eof() || text_[pos_] != expected) return false;
    pos_ += 1;
    return true;
  }
  std::string_view slice(std::size_t start, std::size_t end) const {
    return text_.substr(start, end - start);
  }
  std::string_view remaining() const { return text_.substr(pos_); }

private:
  std::string_view text_;
  std::size_t pos_ = 0;
};

inline bool parse_json_hex_quad(JsonCursor& cursor, std::uint16_t& value) {
  value = 0;
  for (int i = 0; i < 4; ++i) {
    if (cursor.eof()) return false;
    const char ch = cursor.peek();
    std::uint16_t digit = 0;
    if (ch >= '0' && ch <= '9') digit = static_cast<std::uint16_t>(ch - '0');
    else if (ch >= 'a' && ch <= 'f') digit = static_cast<std::uint16_t>(10 + ch - 'a');
    else if (ch >= 'A' && ch <= 'F') digit = static_cast<std::uint16_t>(10 + ch - 'A');
    else return false;
    value = static_cast<std::uint16_t>((value << 4) | digit);
    cursor.set_pos(cursor.pos() + 1);
  }
  return true;
}

inline bool decode_json_string_content(std::string_view encoded, std::string& out) {
  auto hex_digit = [](char ch) -> int {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
    if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
    return -1;
  };
  auto parse_code_unit = [&](std::size_t& index, std::uint16_t& value) -> bool {
    if (index + 4 > encoded.size()) return false;
    value = 0;
    for (int i = 0; i < 4; ++i) {
      const int digit = hex_digit(encoded[index + static_cast<std::size_t>(i)]);
      if (digit < 0) return false;
      value = static_cast<std::uint16_t>((value << 4) | static_cast<std::uint16_t>(digit));
    }
    index += 4;
    return true;
  };

  out.clear();
  out.reserve(encoded.size());
  for (std::size_t index = 0; index < encoded.size(); ++index) {
    const char ch = encoded[index];
    if (ch != '\\') {
      if (static_cast<unsigned char>(ch) < 0x20) return false;
      out.push_back(ch);
      continue;
    }
    index += 1;
    if (index >= encoded.size()) return false;
    const char esc = encoded[index];
    switch (esc) {
    case '"': out.push_back('"'); break;
    case '\\': out.push_back('\\'); break;
    case '/': out.push_back('/'); break;
    case 'b': out.push_back('\b'); break;
    case 'f': out.push_back('\f'); break;
    case 'n': out.push_back('\n'); break;
    case 'r': out.push_back('\r'); break;
    case 't': out.push_back('\t'); break;
    case 'u': {
      std::size_t hex_index = index + 1;
      std::uint16_t code_unit = 0;
      if (!parse_code_unit(hex_index, code_unit)) return false;
      index = hex_index - 1;
      if (code_unit >= 0xD800 && code_unit <= 0xDBFF) {
        if (index + 2 >= encoded.size() || encoded[index + 1] != '\\' || encoded[index + 2] != 'u') {
          return false;
        }
        std::size_t low_index = index + 3;
        std::uint16_t low_surrogate = 0;
        if (!parse_code_unit(low_index, low_surrogate)) return false;
        if (low_surrogate < 0xDC00 || low_surrogate > 0xDFFF) return false;
        index = low_index - 1;
        const std::uint32_t code_point =
            0x10000u +
            ((static_cast<std::uint32_t>(code_unit - 0xD800u) << 10u) |
             static_cast<std::uint32_t>(low_surrogate - 0xDC00u));
        if (!append_utf8(out, code_point)) return false;
      } else {
        if (!append_utf8(out, code_unit)) return false;
      }
      break;
    }
    default: return false;
    }
  }
  return true;
}

inline bool scan_json_string_literal(JsonCursor& cursor,
                                     std::size_t& content_start,
                                     std::size_t& content_end,
                                     bool& needs_decode) {
  cursor.skip_ws();
  if (cursor.eof() || cursor.peek() != '"') return false;
  cursor.set_pos(cursor.pos() + 1);
  content_start = cursor.pos();
  needs_decode = false;
  while (!cursor.eof()) {
    const char ch = cursor.peek();
    cursor.set_pos(cursor.pos() + 1);
    if (ch == '"') {
      content_end = cursor.pos() - 1;
      return true;
    }
    if (ch == '\\') {
      needs_decode = true;
      if (cursor.eof()) return false;
      const char esc = cursor.peek();
      cursor.set_pos(cursor.pos() + 1);
      switch (esc) {
      case '"':
      case '\\':
      case '/':
      case 'b':
      case 'f':
      case 'n':
      case 'r':
      case 't': break;
      case 'u': {
        std::uint16_t code_unit = 0;
        if (!parse_json_hex_quad(cursor, code_unit)) return false;
        if (code_unit >= 0xD800 && code_unit <= 0xDBFF) {
          if (cursor.eof() || cursor.peek() != '\\') return false;
          cursor.set_pos(cursor.pos() + 1);
          if (cursor.eof() || cursor.peek() != 'u') return false;
          cursor.set_pos(cursor.pos() + 1);
          std::uint16_t low_surrogate = 0;
          if (!parse_json_hex_quad(cursor, low_surrogate)) return false;
          if (low_surrogate < 0xDC00 || low_surrogate > 0xDFFF) return false;
        }
        break;
      }
      default: return false;
      }
      continue;
    }
    if (static_cast<unsigned char>(ch) < 0x20) return false;
  }
  return false;
}

inline bool parse_json_string_literal(JsonCursor& cursor, std::string* decoded = nullptr) {
  std::size_t content_start = 0;
  std::size_t content_end = 0;
  bool needs_decode = false;
  if (!scan_json_string_literal(cursor, content_start, content_end, needs_decode)) return false;
  if (decoded == nullptr) return true;
  if (!needs_decode) {
    decoded->assign(cursor.slice(content_start, content_end));
    return true;
  }
  return decode_json_string_content(cursor.slice(content_start, content_end), *decoded);
}

inline bool parse_json_int_literal(JsonCursor& cursor, std::int64_t* out = nullptr) {
  cursor.skip_ws();
  const std::size_t start = cursor.pos();
  if (!cursor.eof() && cursor.peek() == '-') cursor.set_pos(cursor.pos() + 1);
  if (cursor.eof()) return false;
  if (cursor.peek() == '0') {
    cursor.set_pos(cursor.pos() + 1);
  } else {
    if (!std::isdigit(static_cast<unsigned char>(cursor.peek()))) return false;
    while (!cursor.eof() && std::isdigit(static_cast<unsigned char>(cursor.peek()))) {
      cursor.set_pos(cursor.pos() + 1);
    }
  }
  if (!cursor.eof() && (cursor.peek() == '.' || cursor.peek() == 'e' || cursor.peek() == 'E')) {
    return false;
  }
  if (out != nullptr) {
    const std::string_view literal = cursor.slice(start, cursor.pos());
    const auto parsed =
        std::from_chars(literal.data(), literal.data() + literal.size(), *out);
    if (parsed.ec != std::errc{} || parsed.ptr != literal.data() + literal.size()) return false;
  }
  return cursor.pos() > start;
}

inline bool parse_json_bool_literal(JsonCursor& cursor, bool* out = nullptr) {
  cursor.skip_ws();
  const std::string_view remaining = cursor.remaining();
  if (remaining.size() >= 4 && remaining.substr(0, 4) == "true") {
    cursor.set_pos(cursor.pos() + 4);
    if (out != nullptr) *out = true;
    return true;
  }
  if (remaining.size() >= 5 && remaining.substr(0, 5) == "false") {
    cursor.set_pos(cursor.pos() + 5);
    if (out != nullptr) *out = false;
    return true;
  }
  return false;
}

inline bool parse_json_null_literal(JsonCursor& cursor) {
  cursor.skip_ws();
  const std::string_view remaining = cursor.remaining();
  if (remaining.size() < 4 || remaining.substr(0, 4) != "null") return false;
  cursor.set_pos(cursor.pos() + 4);
  return true;
}

inline bool parse_json_value(JsonCursor& cursor, JsonValueView* out);
inline bool parse_json_array(JsonCursor& cursor,
                             JsonArrayIndex* array_items = nullptr,
                             JsonValueView* out = nullptr);

inline bool parse_json_object(JsonCursor& cursor,
                              JsonObjectIndex* object_fields = nullptr,
                              JsonValueView* out = nullptr) {
  cursor.skip_ws();
  const std::size_t raw_start = cursor.pos();
  if (cursor.eof() || cursor.peek() != '{') return false;
  cursor.set_pos(cursor.pos() + 1);
  cursor.skip_ws();
  if (!cursor.eof() && cursor.peek() == '}') {
    cursor.set_pos(cursor.pos() + 1);
    if (out != nullptr) {
      out->kind = JsonValueKind::Object;
      out->raw_start = raw_start;
      out->raw_end = cursor.pos();
    }
    return true;
  }
  while (true) {
    std::size_t key_start = 0;
    std::size_t key_end = 0;
    bool key_needs_decode = false;
    if (!scan_json_string_literal(cursor, key_start, key_end, key_needs_decode)) return false;
    if (!cursor.consume(':')) return false;
    JsonValueView value_view;
    if (!parse_json_value(cursor, &value_view)) return false;
    if (object_fields != nullptr) {
      JsonObjectField field;
      field.key_start = key_start;
      field.key_end = key_end;
      field.key_needs_decode = key_needs_decode;
      field.value = value_view;
      if (!object_fields->push_back(std::move(field))) {
        object_fields = nullptr;
      }
    }
    cursor.skip_ws();
    if (cursor.eof()) return false;
    if (cursor.peek() == '}') {
      cursor.set_pos(cursor.pos() + 1);
      if (out != nullptr) {
        out->kind = JsonValueKind::Object;
        out->raw_start = raw_start;
        out->raw_end = cursor.pos();
      }
      return true;
    }
    if (cursor.peek() != ',') return false;
    cursor.set_pos(cursor.pos() + 1);
  }
}

inline bool parse_json_array(JsonCursor& cursor,
                             JsonArrayIndex* array_items,
                             JsonValueView* out) {
  cursor.skip_ws();
  const std::size_t raw_start = cursor.pos();
  if (cursor.eof() || cursor.peek() != '[') return false;
  cursor.set_pos(cursor.pos() + 1);
  cursor.skip_ws();
  if (!cursor.eof() && cursor.peek() == ']') {
    cursor.set_pos(cursor.pos() + 1);
    if (out != nullptr) {
      out->kind = JsonValueKind::Array;
      out->raw_start = raw_start;
      out->raw_end = cursor.pos();
    }
    return true;
  }
  while (true) {
    JsonValueView value_view;
    if (!parse_json_value(cursor, &value_view)) return false;
    if (array_items != nullptr) {
      array_items->items.push_back(value_view);
    }
    cursor.skip_ws();
    if (cursor.eof()) return false;
    if (cursor.peek() == ']') {
      cursor.set_pos(cursor.pos() + 1);
      if (out != nullptr) {
        out->kind = JsonValueKind::Array;
        out->raw_start = raw_start;
        out->raw_end = cursor.pos();
      }
      return true;
    }
    if (cursor.peek() != ',') return false;
    cursor.set_pos(cursor.pos() + 1);
  }
}

inline bool parse_json_value(JsonCursor& cursor, JsonValueView* out) {
  cursor.skip_ws();
  if (cursor.eof()) return false;
  const std::size_t raw_start = cursor.pos();
  const char ch = cursor.peek();
  if (ch == '"') {
    std::size_t string_start = 0;
    std::size_t string_end = 0;
    bool string_needs_decode = false;
    if (!scan_json_string_literal(cursor, string_start, string_end, string_needs_decode)) return false;
    if (out != nullptr) {
      out->kind = JsonValueKind::String;
      out->raw_start = raw_start;
      out->raw_end = cursor.pos();
      out->string_needs_decode = string_needs_decode;
    }
    return true;
  }
  if (ch == '{') {
    return parse_json_object(cursor, nullptr, out);
  }
  if (ch == '[') {
    return parse_json_array(cursor, nullptr, out);
  }
  if (ch == 't' || ch == 'f') {
    bool value = false;
    if (!parse_json_bool_literal(cursor, &value)) return false;
    if (out != nullptr) {
      out->kind = JsonValueKind::Bool;
      out->raw_start = raw_start;
      out->raw_end = cursor.pos();
      out->bool_value = value;
    }
    return true;
  }
  if (ch == 'n') {
    if (!parse_json_null_literal(cursor)) return false;
    if (out != nullptr) {
      out->kind = JsonValueKind::Null;
      out->raw_start = raw_start;
      out->raw_end = cursor.pos();
    }
    return true;
  }
  if (ch == '-' || std::isdigit(static_cast<unsigned char>(ch))) {
    std::int64_t value = 0;
    if (!parse_json_int_literal(cursor, &value)) return false;
    if (out != nullptr) {
      out->kind = JsonValueKind::Int;
      out->raw_start = raw_start;
      out->raw_end = cursor.pos();
      out->int_value = value;
    }
    return true;
  }
  return false;
}

inline bool parse_json_value(JsonCursor& cursor) {
  return parse_json_value(cursor, nullptr);
}

inline bool parse_json_document(std::string_view text,
                                JsonObjectIndex* object_fields = nullptr,
                                JsonArrayIndex* array_items = nullptr,
                                JsonValueView* out = nullptr) {
  JsonCursor cursor(text);
  cursor.skip_ws();
  const bool top_level_object = !cursor.eof() && cursor.peek() == '{';
  const bool top_level_array = !cursor.eof() && cursor.peek() == '[';
  if (top_level_object) {
    if (!parse_json_object(cursor, object_fields, out)) return false;
  } else if (top_level_array) {
    if (!parse_json_array(cursor, array_items, out)) return false;
  } else {
    if (!parse_json_value(cursor, out)) return false;
  }
  cursor.skip_ws();
  return cursor.eof();
}

inline bool json_object_field_key_equals(const JsonValue& value,
                                         const JsonObjectField& field,
                                         std::string_view key) {
  const std::string_view raw_key =
      std::string_view(value.text).substr(field.key_start, field.key_end - field.key_start);
  if (!field.key_needs_decode) return raw_key == key;
  std::string decoded_key;
  return decode_json_string_content(raw_key, decoded_key) && decoded_key == key;
}

inline std::optional<std::string_view> json_lookup_field_text(const JsonValue& value,
                                                              std::string_view key) {
  if (value.has_object_fields) {
    for (std::size_t i = 0; i < value.object_fields.count; ++i) {
      const auto& field = value.object_fields.fields[i];
      if (json_object_field_key_equals(value, field, key)) {
        return std::string_view(value.text).substr(field.value.raw_start,
                                                   field.value.raw_end - field.value.raw_start);
      }
    }
    return std::nullopt;
  }

  JsonCursor cursor(value.text);
  if (!cursor.consume('{')) return std::nullopt;
  cursor.skip_ws();
  if (!cursor.eof() && cursor.peek() == '}') return std::nullopt;
  while (true) {
    std::string candidate_key;
    if (!parse_json_string_literal(cursor, &candidate_key)) return std::nullopt;
    if (!cursor.consume(':')) return std::nullopt;
    const std::size_t value_start = cursor.pos();
    if (!parse_json_value(cursor)) return std::nullopt;
    const std::size_t value_end = cursor.pos();
    if (candidate_key == key) {
      return cursor.slice(value_start, value_end);
    }
    cursor.skip_ws();
    if (cursor.eof()) return std::nullopt;
    if (cursor.peek() == '}') return std::nullopt;
    if (cursor.peek() != ',') return std::nullopt;
    cursor.set_pos(cursor.pos() + 1);
  }
}

struct JsonIndexedSlice {
  std::string_view raw;
  JsonValueView view;
};

inline JsonValueView json_relative_view(JsonValueView view, std::size_t start) {
  view.raw_end -= start;
  view.raw_start = 0;
  return view;
}

inline std::optional<JsonIndexedSlice> json_lookup_field_indexed(const JsonValue& value,
                                                                 std::string_view key) {
  if (!value.has_object_fields) return std::nullopt;
  for (std::size_t i = 0; i < value.object_fields.count; ++i) {
    const auto& field = value.object_fields.fields[i];
    if (!json_object_field_key_equals(value, field, key)) continue;
    const auto raw = std::string_view(value.text).substr(field.value.raw_start,
                                                         field.value.raw_end - field.value.raw_start);
    return JsonIndexedSlice{raw, json_relative_view(field.value, field.value.raw_start)};
  }
  return std::nullopt;
}

inline std::optional<std::string_view> json_lookup_array_item_text(const JsonValue& value, std::int64_t index) {
  if (index < 0) return std::nullopt;
  const auto idx = static_cast<std::size_t>(index);
  if (!value.has_array_items || idx >= value.array_items.items.size()) return std::nullopt;
  const auto& item = value.array_items.items[idx];
  return std::string_view(value.text).substr(item.raw_start, item.raw_end - item.raw_start);
}

inline std::optional<JsonIndexedSlice> json_lookup_array_item_indexed(const JsonValue& value,
                                                                      std::int64_t index) {
  if (index < 0) return std::nullopt;
  const auto idx = static_cast<std::size_t>(index);
  if (!value.has_array_items || idx >= value.array_items.items.size()) return std::nullopt;
  const auto& item = value.array_items.items[idx];
  const auto raw = std::string_view(value.text).substr(item.raw_start, item.raw_end - item.raw_start);
  return JsonIndexedSlice{raw, json_relative_view(item, item.raw_start)};
}

template <typename... Sizes>
inline void reserve_json_object_text(std::string& out, Sizes... sizes) {
  out.reserve((std::size_t{2} + ... + static_cast<std::size_t>(sizes)));
}

inline JsonValueView json_shifted_view(JsonValueView value, std::size_t offset) {
  value.raw_start += offset;
  value.raw_end += offset;
  return value;
}

inline JsonValue make_structured_json_value(JsonStructuredValue value) {
  return JsonValue{std::make_shared<const JsonStructuredValue>(std::move(value))};
}

inline const std::string& json_materialize_structured(const JsonStructuredValue& value);

inline bool json_append_escaped_string(std::string& out, std::string_view text) {
  bool escaped = false;
  for (char ch : text) {
    switch (ch) {
    case '\\':
      out += "\\\\";
      escaped = true;
      break;
    case '"':
      out += "\\\"";
      escaped = true;
      break;
    case '\b':
      out += "\\b";
      escaped = true;
      break;
    case '\f':
      out += "\\f";
      escaped = true;
      break;
    case '\n':
      out += "\\n";
      escaped = true;
      break;
    case '\r':
      out += "\\r";
      escaped = true;
      break;
    case '\t':
      out += "\\t";
      escaped = true;
      break;
    default:
      if (static_cast<unsigned char>(ch) < 0x20) {
        constexpr char digits[] = "0123456789abcdef";
        const unsigned char value = static_cast<unsigned char>(ch);
        out += "\\u00";
        out.push_back(digits[(value >> 4) & 0x0F]);
        out.push_back(digits[value & 0x0F]);
        escaped = true;
      } else {
        out.push_back(ch);
      }
      break;
    }
  }
  return escaped;
}

inline const std::string& json_materialize_structured(const JsonStructuredValue& value) {
  if (value.materialized) return value.materialized_text;

  std::string out;
  switch (value.kind) {
  case JsonValueKind::String:
    out.reserve(value.string_value.size() + 2);
    out.push_back('"');
    json_append_escaped_string(out, value.string_value);
    out.push_back('"');
    break;
  case JsonValueKind::Int: out = std::to_string(value.int_value); break;
  case JsonValueKind::Bool: out = value.bool_value ? "true" : "false"; break;
  case JsonValueKind::Null: out = "null"; break;
  case JsonValueKind::Object: {
    out.push_back('{');
    for (std::size_t i = 0; i < value.object_count; ++i) {
      if (i) out.push_back(',');
      out.push_back('"');
      json_append_escaped_string(out, value.object_fields[i].key);
      out += "\":";
      out += json_materialize_structured(*value.object_fields[i].value);
    }
    out.push_back('}');
    break;
  }
  case JsonValueKind::Array: out = "[]"; break;
  case JsonValueKind::Unknown: break;
  }

  value.materialized_text = std::move(out);
  value.materialized = true;
  return value.materialized_text;
}

inline void append_json_object_field(std::string& out,
                                     JsonObjectIndex& object_fields,
                                     bool& first,
                                     std::string_view key,
                                     JsonValue value) {
  if (!first) out.push_back(',');
  first = false;
  out.push_back('"');
  const std::size_t key_start = out.size();
  const bool key_needs_decode = json_append_escaped_string(out, key);
  const std::size_t key_end = out.size();
  out += "\":";
  const std::size_t value_start = out.size();
  if (value.structured != nullptr && value.text.empty()) {
    out += json_materialize_structured(*value.structured);
  } else {
    out += value.text;
  }
  JsonObjectField field;
  field.key_start = key_start;
  field.key_end = key_end;
  field.key_needs_decode = key_needs_decode;
  field.value = json_shifted_view(value.parsed, value_start);
  field.value.raw_start = value_start;
  field.value.raw_end = out.size();
  object_fields.push_back(std::move(field));
}

inline void append_json_object_text_field(std::string& out,
                                          bool& first,
                                          std::string_view key,
                                          JsonValue value) {
  if (!first) out.push_back(',');
  first = false;
  out.push_back('"');
  json_append_escaped_string(out, key);
  out += "\":";
  if (value.structured != nullptr && value.text.empty()) {
    out += json_materialize_structured(*value.structured);
  } else {
    out += value.text;
  }
}

inline JsonValue finish_json_object(std::string text, JsonObjectIndex object_fields) {
  JsonValueView parsed;
  parsed.kind = JsonValueKind::Object;
  parsed.raw_start = 0;
  parsed.raw_end = text.size();
  return JsonValue{std::move(text), std::move(parsed), std::move(object_fields)};
}

inline JsonValue finish_json_array(std::string text, JsonArrayIndex array_items) {
  JsonValueView parsed;
  parsed.kind = JsonValueKind::Array;
  parsed.raw_start = 0;
  parsed.raw_end = text.size();
  return JsonValue{std::move(text), std::move(parsed), std::move(array_items)};
}

inline Result<JsonValue, std::string> json_parse(std::string text);

inline Result<JsonValue, std::string> json_indexed_subvalue(std::string_view raw,
                                                            JsonValueView view) {
  if (view.raw_start == 0 && view.raw_end == raw.size()) {
    if (view.kind == JsonValueKind::String ||
        view.kind == JsonValueKind::Int ||
        view.kind == JsonValueKind::Bool ||
        view.kind == JsonValueKind::Null) {
      return ok_result(JsonValue{std::string(raw), std::move(view)});
    }
    if (view.kind == JsonValueKind::Array && raw == "[]") {
      JsonArrayIndex array_items;
      return ok_result(finish_json_array("[]", std::move(array_items)));
    }
  }
  return json_parse(std::string(raw));
}

inline bool json_structured_object_push(JsonStructuredValue& object,
                                        std::string_view key,
                                        const JsonValue& value) {
  if (object.object_count >= object.object_fields.size() || value.structured == nullptr) return false;
  auto& field = object.object_fields[object.object_count];
  field.key = std::string(key);
  field.value = value.structured;
  object.object_count = static_cast<std::uint8_t>(object.object_count + 1);
  return true;
}

inline Result<JsonValue, std::string> json_parse(std::string text) {
  JsonCursor cursor(text);
  cursor.skip_ws();
  const bool top_level_object = !cursor.eof() && cursor.peek() == '{';
  const bool top_level_array = !cursor.eof() && cursor.peek() == '[';
  JsonObjectIndex object_fields;
  JsonArrayIndex array_items;
  JsonValueView parsed;
  if (!parse_json_document(text, top_level_object ? &object_fields : nullptr, top_level_array ? &array_items : nullptr, &parsed)) {
    return err_result<JsonValue>("invalid narrow JSON document");
  }
  if (top_level_object && object_fields.count > 0) {
    return ok_result(JsonValue{std::move(text), std::move(parsed), std::move(object_fields)});
  }
  if (top_level_array) {
    return ok_result(JsonValue{std::move(text), std::move(parsed), std::move(array_items)});
  }
  return ok_result(JsonValue{std::move(text), std::move(parsed)});
}

inline Result<JsonValue, std::string> json_parse_bytes(Bytes bytes) {
  return json_parse(std::move(bytes.data));
}

inline std::string json_stringify(JsonValue value) {
  return std::move(value.text);
}

inline JsonValue json_string_value(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 2);
  out.push_back('"');
  const bool string_needs_decode = json_append_escaped_string(out, value);
  out.push_back('"');
  JsonValueView parsed;
  parsed.kind = JsonValueKind::String;
  parsed.raw_start = 0;
  parsed.raw_end = out.size();
  parsed.string_needs_decode = string_needs_decode;
  return JsonValue{std::move(out), std::move(parsed)};
}

inline JsonValue json_int_value(std::int64_t value) {
  std::string text = std::to_string(value);
  JsonValueView parsed;
  parsed.kind = JsonValueKind::Int;
  parsed.raw_start = 0;
  parsed.raw_end = text.size();
  parsed.int_value = value;
  return JsonValue{std::move(text), std::move(parsed)};
}

inline JsonValue json_bool_value(bool value) {
  JsonValueView parsed;
  parsed.kind = JsonValueKind::Bool;
  parsed.raw_start = 0;
  parsed.raw_end = value ? 4 : 5;
  parsed.bool_value = value;
  return JsonValue{value ? "true" : "false", std::move(parsed)};
}

inline JsonValue json_null_value() {
  JsonValueView parsed;
  parsed.kind = JsonValueKind::Null;
  parsed.raw_start = 0;
  parsed.raw_end = 4;
  return JsonValue{"null", std::move(parsed)};
}

inline JsonValue json_object1(std::string_view key1, JsonValue value1) {
  std::string out;
  reserve_json_object_text(out, key1.size() + value1.text.size() + 4);
  JsonObjectIndex object_fields;
  bool first = true;
  out.push_back('{');
  append_json_object_field(out, object_fields, first, key1, std::move(value1));
  out.push_back('}');
  return finish_json_object(std::move(out), std::move(object_fields));
}

inline JsonValue json_object2(std::string_view key1,
                              JsonValue value1,
                              std::string_view key2,
                              JsonValue value2) {
  std::string out;
  reserve_json_object_text(
      out, key1.size() + value1.text.size() + 4, key2.size() + value2.text.size() + 4);
  JsonObjectIndex object_fields;
  bool first = true;
  out.push_back('{');
  append_json_object_field(out, object_fields, first, key1, std::move(value1));
  append_json_object_field(out, object_fields, first, key2, std::move(value2));
  out.push_back('}');
  return finish_json_object(std::move(out), std::move(object_fields));
}

inline JsonValue json_object3(std::string_view key1,
                              JsonValue value1,
                              std::string_view key2,
                              JsonValue value2,
                              std::string_view key3,
                              JsonValue value3) {
  std::string out;
  reserve_json_object_text(out,
                           key1.size() + value1.text.size() + 4,
                           key2.size() + value2.text.size() + 4,
                           key3.size() + value3.text.size() + 4);
  JsonObjectIndex object_fields;
  bool first = true;
  out.push_back('{');
  append_json_object_field(out, object_fields, first, key1, std::move(value1));
  append_json_object_field(out, object_fields, first, key2, std::move(value2));
  append_json_object_field(out, object_fields, first, key3, std::move(value3));
  out.push_back('}');
  return finish_json_object(std::move(out), std::move(object_fields));
}

inline JsonValue json_object4(std::string_view key1,
                              JsonValue value1,
                              std::string_view key2,
                              JsonValue value2,
                              std::string_view key3,
                              JsonValue value3,
                              std::string_view key4,
                              JsonValue value4) {
  std::string out;
  reserve_json_object_text(out,
                           key1.size() + value1.text.size() + 4,
                           key2.size() + value2.text.size() + 4,
                           key3.size() + value3.text.size() + 4,
                           key4.size() + value4.text.size() + 4);
  JsonObjectIndex object_fields;
  bool first = true;
  out.push_back('{');
  append_json_object_field(out, object_fields, first, key1, std::move(value1));
  append_json_object_field(out, object_fields, first, key2, std::move(value2));
  append_json_object_field(out, object_fields, first, key3, std::move(value3));
  append_json_object_field(out, object_fields, first, key4, std::move(value4));
  out.push_back('}');
  return finish_json_object(std::move(out), std::move(object_fields));
}

inline std::string json_object4_text(std::string_view key1,
                                     JsonValue value1,
                                     std::string_view key2,
                                     JsonValue value2,
                                     std::string_view key3,
                                     JsonValue value3,
                                     std::string_view key4,
                                     JsonValue value4) {
  std::string out;
  reserve_json_object_text(out,
                           key1.size() + value1.text.size() + 4,
                           key2.size() + value2.text.size() + 4,
                           key3.size() + value3.text.size() + 4,
                           key4.size() + value4.text.size() + 4);
  bool first = true;
  out.push_back('{');
  append_json_object_text_field(out, first, key1, std::move(value1));
  append_json_object_text_field(out, first, key2, std::move(value2));
  append_json_object_text_field(out, first, key3, std::move(value3));
  append_json_object_text_field(out, first, key4, std::move(value4));
  out.push_back('}');
  return out;
}

inline JsonValue json_object5(std::string_view key1,
                              JsonValue value1,
                              std::string_view key2,
                              JsonValue value2,
                              std::string_view key3,
                              JsonValue value3,
                              std::string_view key4,
                              JsonValue value4,
                              std::string_view key5,
                              JsonValue value5) {
  std::string out;
  reserve_json_object_text(out,
                           key1.size() + value1.text.size() + 4,
                           key2.size() + value2.text.size() + 4,
                           key3.size() + value3.text.size() + 4,
                           key4.size() + value4.text.size() + 4,
                           key5.size() + value5.text.size() + 4);
  JsonObjectIndex object_fields;
  bool first = true;
  out.push_back('{');
  append_json_object_field(out, object_fields, first, key1, std::move(value1));
  append_json_object_field(out, object_fields, first, key2, std::move(value2));
  append_json_object_field(out, object_fields, first, key3, std::move(value3));
  append_json_object_field(out, object_fields, first, key4, std::move(value4));
  append_json_object_field(out, object_fields, first, key5, std::move(value5));
  out.push_back('}');
  return finish_json_object(std::move(out), std::move(object_fields));
}

inline JsonValue json_object6(std::string_view key1,
                              JsonValue value1,
                              std::string_view key2,
                              JsonValue value2,
                              std::string_view key3,
                              JsonValue value3,
                              std::string_view key4,
                              JsonValue value4,
                              std::string_view key5,
                              JsonValue value5,
                              std::string_view key6,
                              JsonValue value6) {
  std::string out;
  reserve_json_object_text(out,
                           key1.size() + value1.text.size() + 4,
                           key2.size() + value2.text.size() + 4,
                           key3.size() + value3.text.size() + 4,
                           key4.size() + value4.text.size() + 4,
                           key5.size() + value5.text.size() + 4,
                           key6.size() + value6.text.size() + 4);
  JsonObjectIndex object_fields;
  bool first = true;
  out.push_back('{');
  append_json_object_field(out, object_fields, first, key1, std::move(value1));
  append_json_object_field(out, object_fields, first, key2, std::move(value2));
  append_json_object_field(out, object_fields, first, key3, std::move(value3));
  append_json_object_field(out, object_fields, first, key4, std::move(value4));
  append_json_object_field(out, object_fields, first, key5, std::move(value5));
  append_json_object_field(out, object_fields, first, key6, std::move(value6));
  out.push_back('}');
  return finish_json_object(std::move(out), std::move(object_fields));
}

inline JsonValue json_object7(std::string_view key1,
                              JsonValue value1,
                              std::string_view key2,
                              JsonValue value2,
                              std::string_view key3,
                              JsonValue value3,
                              std::string_view key4,
                              JsonValue value4,
                              std::string_view key5,
                              JsonValue value5,
                              std::string_view key6,
                              JsonValue value6,
                              std::string_view key7,
                              JsonValue value7) {
  std::string out;
  reserve_json_object_text(out,
                           key1.size() + value1.text.size() + 4,
                           key2.size() + value2.text.size() + 4,
                           key3.size() + value3.text.size() + 4,
                           key4.size() + value4.text.size() + 4,
                           key5.size() + value5.text.size() + 4,
                           key6.size() + value6.text.size() + 4,
                           key7.size() + value7.text.size() + 4);
  JsonObjectIndex object_fields;
  bool first = true;
  out.push_back('{');
  append_json_object_field(out, object_fields, first, key1, std::move(value1));
  append_json_object_field(out, object_fields, first, key2, std::move(value2));
  append_json_object_field(out, object_fields, first, key3, std::move(value3));
  append_json_object_field(out, object_fields, first, key4, std::move(value4));
  append_json_object_field(out, object_fields, first, key5, std::move(value5));
  append_json_object_field(out, object_fields, first, key6, std::move(value6));
  append_json_object_field(out, object_fields, first, key7, std::move(value7));
  out.push_back('}');
  return finish_json_object(std::move(out), std::move(object_fields));
}

inline JsonValue json_object8(std::string_view key1,
                              JsonValue value1,
                              std::string_view key2,
                              JsonValue value2,
                              std::string_view key3,
                              JsonValue value3,
                              std::string_view key4,
                              JsonValue value4,
                              std::string_view key5,
                              JsonValue value5,
                              std::string_view key6,
                              JsonValue value6,
                              std::string_view key7,
                              JsonValue value7,
                              std::string_view key8,
                              JsonValue value8) {
  std::string out;
  reserve_json_object_text(out,
                           key1.size() + value1.text.size() + 4,
                           key2.size() + value2.text.size() + 4,
                           key3.size() + value3.text.size() + 4,
                           key4.size() + value4.text.size() + 4,
                           key5.size() + value5.text.size() + 4,
                           key6.size() + value6.text.size() + 4,
                           key7.size() + value7.text.size() + 4,
                           key8.size() + value8.text.size() + 4);
  JsonObjectIndex object_fields;
  bool first = true;
  out.push_back('{');
  append_json_object_field(out, object_fields, first, key1, std::move(value1));
  append_json_object_field(out, object_fields, first, key2, std::move(value2));
  append_json_object_field(out, object_fields, first, key3, std::move(value3));
  append_json_object_field(out, object_fields, first, key4, std::move(value4));
  append_json_object_field(out, object_fields, first, key5, std::move(value5));
  append_json_object_field(out, object_fields, first, key6, std::move(value6));
  append_json_object_field(out, object_fields, first, key7, std::move(value7));
  append_json_object_field(out, object_fields, first, key8, std::move(value8));
  out.push_back('}');
  return finish_json_object(std::move(out), std::move(object_fields));
}

inline JsonValue json_object9(std::string_view key1,
                              JsonValue value1,
                              std::string_view key2,
                              JsonValue value2,
                              std::string_view key3,
                              JsonValue value3,
                              std::string_view key4,
                              JsonValue value4,
                              std::string_view key5,
                              JsonValue value5,
                              std::string_view key6,
                              JsonValue value6,
                              std::string_view key7,
                              JsonValue value7,
                              std::string_view key8,
                              JsonValue value8,
                              std::string_view key9,
                              JsonValue value9) {
  std::string out;
  reserve_json_object_text(out,
                           key1.size() + value1.text.size() + 4,
                           key2.size() + value2.text.size() + 4,
                           key3.size() + value3.text.size() + 4,
                           key4.size() + value4.text.size() + 4,
                           key5.size() + value5.text.size() + 4,
                           key6.size() + value6.text.size() + 4,
                           key7.size() + value7.text.size() + 4,
                           key8.size() + value8.text.size() + 4,
                           key9.size() + value9.text.size() + 4);
  JsonObjectIndex object_fields;
  bool first = true;
  out.push_back('{');
  append_json_object_field(out, object_fields, first, key1, std::move(value1));
  append_json_object_field(out, object_fields, first, key2, std::move(value2));
  append_json_object_field(out, object_fields, first, key3, std::move(value3));
  append_json_object_field(out, object_fields, first, key4, std::move(value4));
  append_json_object_field(out, object_fields, first, key5, std::move(value5));
  append_json_object_field(out, object_fields, first, key6, std::move(value6));
  append_json_object_field(out, object_fields, first, key7, std::move(value7));
  append_json_object_field(out, object_fields, first, key8, std::move(value8));
  append_json_object_field(out, object_fields, first, key9, std::move(value9));
  out.push_back('}');
  return finish_json_object(std::move(out), std::move(object_fields));
}

inline JsonArrayBuilder json_array_builder() {
  return JsonArrayBuilder{};
}

inline JsonArrayBuilder json_array_push(JsonArrayBuilder builder, JsonValue value) {
  builder.items.push_back(std::move(value));
  return builder;
}

inline void append_json_array_item(std::string& out,
                                   JsonArrayIndex& array_items,
                                   bool& first,
                                   JsonValue value) {
  if (!first) out.push_back(',');
  first = false;
  const std::size_t value_start = out.size();
  if (value.structured != nullptr && value.text.empty()) {
    out += json_materialize_structured(*value.structured);
  } else {
    out += value.text;
  }
  auto view = value.parsed;
  view.raw_start = value_start;
  view.raw_end = out.size();
  array_items.items.push_back(view);
}

inline JsonValue json_array0() {
  JsonArrayIndex array_items;
  return finish_json_array("[]", std::move(array_items));
}

inline JsonValue json_array1(JsonValue value1) {
  std::string out;
  JsonArrayIndex array_items;
  array_items.items.reserve(1);
  out.push_back('[');
  bool first = true;
  append_json_array_item(out, array_items, first, std::move(value1));
  out.push_back(']');
  return finish_json_array(std::move(out), std::move(array_items));
}

inline JsonValue json_array2(JsonValue value1, JsonValue value2) {
  std::string out;
  JsonArrayIndex array_items;
  array_items.items.reserve(2);
  out.push_back('[');
  bool first = true;
  append_json_array_item(out, array_items, first, std::move(value1));
  append_json_array_item(out, array_items, first, std::move(value2));
  out.push_back(']');
  return finish_json_array(std::move(out), std::move(array_items));
}

inline JsonValue json_array3(JsonValue value1, JsonValue value2, JsonValue value3) {
  std::string out;
  JsonArrayIndex array_items;
  array_items.items.reserve(3);
  out.push_back('[');
  bool first = true;
  append_json_array_item(out, array_items, first, std::move(value1));
  append_json_array_item(out, array_items, first, std::move(value2));
  append_json_array_item(out, array_items, first, std::move(value3));
  out.push_back(']');
  return finish_json_array(std::move(out), std::move(array_items));
}

inline JsonValue json_array_build(JsonArrayBuilder builder) {
  std::string out;
  JsonArrayIndex array_items;
  array_items.items.reserve(builder.items.size());
  out.push_back('[');
  bool first = true;
  for (auto& item : builder.items) {
    append_json_array_item(out, array_items, first, std::move(item));
  }
  out.push_back(']');
  return finish_json_array(std::move(out), std::move(array_items));
}

inline Result<JsonValue, std::string> json_get_value(const JsonValue& value, std::string_view key) {
  const auto indexed = json_lookup_field_indexed(value, key);
  if (indexed.has_value()) return json_indexed_subvalue(indexed->raw, indexed->view);
  const auto field = json_lookup_field_text(value, key);
  if (!field.has_value()) return err_result<JsonValue>("JSON field not found: " + std::string(key));
  return json_parse(std::string(*field));
}

inline Result<std::string, std::string> json_as_string(const JsonValue& value) {
  JsonCursor cursor(value.text);
  std::string out;
  if (!parse_json_string_literal(cursor, &out)) {
    return err_result<std::string>("JSON value is not a string");
  }
  cursor.skip_ws();
  if (!cursor.eof()) return err_result<std::string>("JSON value is not a string");
  return ok_result(out);
}

inline Result<std::int64_t, std::string> json_as_int(const JsonValue& value) {
  JsonCursor cursor(value.text);
  std::int64_t out = 0;
  if (!parse_json_int_literal(cursor, &out)) {
    return err_result<std::int64_t>("JSON value is not an int");
  }
  cursor.skip_ws();
  if (!cursor.eof()) return err_result<std::int64_t>("JSON value is not an int");
  return ok_result(out);
}

inline Result<bool, std::string> json_as_bool(const JsonValue& value) {
  JsonCursor cursor(value.text);
  bool out = false;
  if (!parse_json_bool_literal(cursor, &out)) {
    return err_result<bool>("JSON value is not a bool");
  }
  cursor.skip_ws();
  if (!cursor.eof()) return err_result<bool>("JSON value is not a bool");
  return ok_result(out);
}

inline Result<std::int64_t, std::string> json_array_len(const JsonValue& value) {
  if (value.parsed.kind != JsonValueKind::Array || !value.has_array_items) {
    return err_result<std::int64_t>("JSON value is not an array");
  }
  return ok_result(static_cast<std::int64_t>(value.array_items.items.size()));
}

inline Result<JsonValue, std::string> json_array_get(const JsonValue& value, std::int64_t index) {
  const auto indexed = json_lookup_array_item_indexed(value, index);
  if (indexed.has_value()) return json_indexed_subvalue(indexed->raw, indexed->view);
  const auto item = json_lookup_array_item_text(value, index);
  if (!item.has_value()) {
    return err_result<JsonValue>("JSON array index out of range");
  }
  return json_parse(std::string(*item));
}

inline Result<void, std::string> fs_replace_file_atomic(const hostfs::path& from, const hostfs::path& to) {
#if defined(_WIN32)
  const std::wstring from_w = from.wstring();
  const std::wstring to_w = to.wstring();
  if (!::MoveFileExW(from_w.c_str(), to_w.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    const auto err = static_cast<int>(::GetLastError());
    return err_void_result("failed to replace file atomically: " + to.string() + " (" +
                           std::system_category().message(err) + ")");
  }
  return ok_void_result();
#else
  std::error_code ec;
  hostfs::rename(from, to, ec);
  if (ec) {
    return err_void_result(fs_error("failed to replace file atomically", to.string(), ec));
  }
  return ok_void_result();
#endif
}

inline Result<void, std::string> fs_write_bytes_atomic(const std::string& path, Bytes data) {
  const hostfs::path file_path(path);
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  const hostfs::path temp_path = file_path.string() + ".tmp-" + std::to_string(nonce);
  {
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) return err_void_result("failed to open file for atomic write: " + temp_path.string());
    out.write(data.data.data(), static_cast<std::streamsize>(data.data.size()));
    if (!out) return err_void_result("failed to write file: " + temp_path.string());
    out.flush();
    if (!out) return err_void_result("failed to flush file: " + temp_path.string());
  }
  auto replaced = fs_replace_file_atomic(temp_path, file_path);
  if (std::holds_alternative<typename Result<void, std::string>::Err>(replaced.data)) {
    std::error_code cleanup_ec;
    hostfs::remove(temp_path, cleanup_ec);
    return replaced;
  }
  return ok_void_result();
}

inline Result<void, std::string> fs_write_string_atomic(const std::string& path, std::string text) {
  return fs_write_bytes_atomic(path, Bytes{std::move(text)});
}

inline Result<void, std::string> fs_remove_file(const std::string& path) {
  const hostfs::path file_path(path);
  std::error_code ec;
  if (!hostfs::exists(file_path, ec)) {
    if (ec) return err_void_result(fs_error("failed to stat path", path, ec));
    return err_void_result("path does not exist: " + path);
  }
  if (!hostfs::is_regular_file(file_path, ec)) {
    if (ec) return err_void_result(fs_error("failed to stat path", path, ec));
    return err_void_result("path is not a regular file: " + path);
  }
  if (!hostfs::remove(file_path, ec)) {
    if (ec) return err_void_result(fs_error("failed to remove file", path, ec));
    return err_void_result("failed to remove file: " + path);
  }
  return ok_void_result();
}

inline Result<JsonValue, std::string> fs_list_dir(const std::string& path) {
  const hostfs::path dir_path(path);
  std::error_code ec;
  if (!hostfs::exists(dir_path, ec)) {
    if (ec) return err_result<JsonValue>(fs_error("failed to stat path", path, ec));
    return err_result<JsonValue>("path does not exist: " + path);
  }
  if (!hostfs::is_directory(dir_path, ec)) {
    if (ec) return err_result<JsonValue>(fs_error("failed to stat path", path, ec));
    return err_result<JsonValue>("path is not a directory: " + path);
  }
  std::vector<std::string> names;
  for (hostfs::directory_iterator it(dir_path, ec), end; it != end; it.increment(ec)) {
    if (ec) return err_result<JsonValue>(fs_error("failed to read directory", path, ec));
    names.push_back(it->path().filename().string());
  }
  if (ec) return err_result<JsonValue>(fs_error("failed to read directory", path, ec));
  std::sort(names.begin(), names.end());
  auto builder = json_array_builder();
  for (auto& name : names) {
    builder = json_array_push(std::move(builder), json_string_value(name));
  }
  return ok_result(json_array_build(std::move(builder)));
}

inline Result<std::vector<std::string>, std::string> json_string_array(const JsonValue& value,
                                                                       std::string_view label) {
  auto count_result = json_array_len(value);
  if (std::holds_alternative<typename Result<std::int64_t, std::string>::Err>(count_result.data)) {
    return err_result<std::vector<std::string>>(std::string(label) + " must be a JSON array");
  }
  const auto count = std::get<typename Result<std::int64_t, std::string>::Ok>(count_result.data).value;
  std::vector<std::string> out;
  out.reserve(static_cast<std::size_t>(count));
  for (std::int64_t i = 0; i < count; ++i) {
    auto item = json_array_get(value, i);
    if (std::holds_alternative<typename Result<JsonValue, std::string>::Err>(item.data)) {
      return err_result<std::vector<std::string>>(
          std::get<typename Result<JsonValue, std::string>::Err>(item.data).value);
    }
    auto text = json_as_string(std::get<typename Result<JsonValue, std::string>::Ok>(std::move(item.data)).value);
    if (std::holds_alternative<typename Result<std::string, std::string>::Err>(text.data)) {
      return err_result<std::vector<std::string>>(std::string(label) + " entries must be strings");
    }
    out.push_back(std::get<typename Result<std::string, std::string>::Ok>(std::move(text.data)).value);
  }
  return ok_result(std::move(out));
}

inline Result<std::vector<std::string>, std::string> json_env_array(const JsonValue& value) {
  auto count_result = json_array_len(value);
  if (std::holds_alternative<typename Result<std::int64_t, std::string>::Err>(count_result.data)) {
    return err_result<std::vector<std::string>>("process env must be a JSON array");
  }
  const auto count = std::get<typename Result<std::int64_t, std::string>::Ok>(count_result.data).value;
  std::vector<std::string> out;
  out.reserve(static_cast<std::size_t>(count));
  for (std::int64_t i = 0; i < count; ++i) {
    auto item_result = json_array_get(value, i);
    if (std::holds_alternative<typename Result<JsonValue, std::string>::Err>(item_result.data)) {
      return err_result<std::vector<std::string>>(
          std::get<typename Result<JsonValue, std::string>::Err>(item_result.data).value);
    }
    const auto item = std::get<typename Result<JsonValue, std::string>::Ok>(std::move(item_result.data)).value;
    auto name_json = json_get_value(item, "name");
    auto value_json = json_get_value(item, "value");
    if (std::holds_alternative<typename Result<JsonValue, std::string>::Err>(name_json.data) ||
        std::holds_alternative<typename Result<JsonValue, std::string>::Err>(value_json.data)) {
      return err_result<std::vector<std::string>>("process env entries must contain string name/value");
    }
    auto name = json_as_string(std::get<typename Result<JsonValue, std::string>::Ok>(std::move(name_json.data)).value);
    auto value_text = json_as_string(std::get<typename Result<JsonValue, std::string>::Ok>(std::move(value_json.data)).value);
    if (std::holds_alternative<typename Result<std::string, std::string>::Err>(name.data) ||
        std::holds_alternative<typename Result<std::string, std::string>::Err>(value_text.data)) {
      return err_result<std::vector<std::string>>("process env entries must contain string name/value");
    }
    auto name_text = std::get<typename Result<std::string, std::string>::Ok>(std::move(name.data)).value;
    if (name_text.empty() || name_text.find('=') != std::string::npos || name_text.find('\0') != std::string::npos) {
      return err_result<std::vector<std::string>>("process env name must be non-empty and must not contain '=' or NUL");
    }
    auto val = std::get<typename Result<std::string, std::string>::Ok>(std::move(value_text.data)).value;
    if (val.find('\0') != std::string::npos) {
      return err_result<std::vector<std::string>>("process env value must not contain NUL");
    }
    out.push_back(std::move(name_text) + "=" + std::move(val));
  }
  return ok_result(std::move(out));
}

#if defined(_WIN32)
inline Result<ProcessOutput, std::string> process_run(ProcessCommand) {
  return err_result<ProcessOutput>("std::process is not supported on Windows in this preview");
}
#else
struct ProcessPipeSet {
  int stdin_read = -1;
  int stdin_write = -1;
  int stdout_read = -1;
  int stdout_write = -1;
  int stderr_read = -1;
  int stderr_write = -1;
  int exec_error_read = -1;
  int exec_error_write = -1;
};

inline void close_fd_if_open(int& fd) {
  if (fd >= 0) {
    close(fd);
    fd = -1;
  }
}

inline Result<void, std::string> make_pipe_pair(int& read_fd, int& write_fd, std::string_view label) {
  int fds[2] = {-1, -1};
  if (pipe(fds) != 0) {
    return err_void_result(std::string("failed to create process ") + std::string(label) + " pipe: " +
                           std::strerror(errno));
  }
  read_fd = fds[0];
  write_fd = fds[1];
  return ok_void_result();
}

inline Result<void, std::string> set_fd_nonblocking(int fd, std::string_view label) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    return err_void_result(std::string("failed to configure process ") + std::string(label) +
                           " nonblocking: " + std::strerror(errno));
  }
  return ok_void_result();
}

inline Result<void, std::string> set_fd_cloexec(int fd, std::string_view label) {
  const int flags = fcntl(fd, F_GETFD, 0);
  if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0) {
    return err_void_result(std::string("failed to configure process ") + std::string(label) +
                           " close-on-exec: " + std::strerror(errno));
  }
  return ok_void_result();
}

inline Result<void, std::string> process_prepare_parent_pipes(ProcessPipeSet& pipes) {
  auto rc = make_pipe_pair(pipes.stdin_read, pipes.stdin_write, "stdin");
  if (std::holds_alternative<typename Result<void, std::string>::Err>(rc.data)) return rc;
  rc = make_pipe_pair(pipes.stdout_read, pipes.stdout_write, "stdout");
  if (std::holds_alternative<typename Result<void, std::string>::Err>(rc.data)) return rc;
  rc = make_pipe_pair(pipes.stderr_read, pipes.stderr_write, "stderr");
  if (std::holds_alternative<typename Result<void, std::string>::Err>(rc.data)) return rc;
  rc = make_pipe_pair(pipes.exec_error_read, pipes.exec_error_write, "exec-error");
  if (std::holds_alternative<typename Result<void, std::string>::Err>(rc.data)) return rc;
  rc = set_fd_cloexec(pipes.exec_error_write, "exec-error");
  if (std::holds_alternative<typename Result<void, std::string>::Err>(rc.data)) return rc;
  rc = set_fd_nonblocking(pipes.stdin_write, "stdin");
  if (std::holds_alternative<typename Result<void, std::string>::Err>(rc.data)) return rc;
  rc = set_fd_nonblocking(pipes.stdout_read, "stdout");
  if (std::holds_alternative<typename Result<void, std::string>::Err>(rc.data)) return rc;
  rc = set_fd_nonblocking(pipes.stderr_read, "stderr");
  if (std::holds_alternative<typename Result<void, std::string>::Err>(rc.data)) return rc;
  rc = set_fd_nonblocking(pipes.exec_error_read, "exec-error");
  if (std::holds_alternative<typename Result<void, std::string>::Err>(rc.data)) return rc;
  return ok_void_result();
}

inline void process_close_all(ProcessPipeSet& pipes) {
  close_fd_if_open(pipes.stdin_read);
  close_fd_if_open(pipes.stdin_write);
  close_fd_if_open(pipes.stdout_read);
  close_fd_if_open(pipes.stdout_write);
  close_fd_if_open(pipes.stderr_read);
  close_fd_if_open(pipes.stderr_write);
  close_fd_if_open(pipes.exec_error_read);
  close_fd_if_open(pipes.exec_error_write);
}

inline bool process_text_has_nul(std::string_view text) {
  return text.find('\0') != std::string_view::npos;
}

inline Result<void, std::string> validate_process_command(const ProcessCommand& command) {
  if (command.program.empty()) return err_void_result("process program must be non-empty");
  if (process_text_has_nul(command.program)) return err_void_result("process program must not contain NUL");
  if (process_text_has_nul(command.cwd)) return err_void_result("process cwd must not contain NUL");
  if (command.timeout_ms <= 0) return err_void_result("process timeout_ms must be positive");
  if (command.max_stdout_bytes < 0 || command.max_stderr_bytes < 0) {
    return err_void_result("process output byte caps must be non-negative");
  }
  return ok_void_result();
}

inline std::vector<char*> process_argv_ptrs(const std::string& program, std::vector<std::string>& args) {
  std::vector<char*> argv;
  argv.reserve(args.size() + 2);
  argv.push_back(const_cast<char*>(program.c_str()));
  for (auto& arg : args) argv.push_back(arg.data());
  argv.push_back(nullptr);
  return argv;
}

inline std::vector<char*> process_env_ptrs(std::vector<std::string>& env) {
  std::vector<char*> envp;
  envp.reserve(env.size() + 1);
  for (auto& item : env) envp.push_back(item.data());
  envp.push_back(nullptr);
  return envp;
}

inline std::vector<std::string> inherited_environment_with_overrides(const std::vector<std::string>& overrides) {
  std::vector<std::string> env;
  std::set<std::string> override_names;
  for (const auto& entry : overrides) {
    const auto pos = entry.find('=');
    override_names.insert(pos == std::string::npos ? entry : entry.substr(0, pos));
  }
  for (char** current = ::environ; current != nullptr && *current != nullptr; ++current) {
    std::string entry(*current);
    const auto pos = entry.find('=');
    const std::string name = pos == std::string::npos ? entry : entry.substr(0, pos);
    if (!override_names.contains(name)) env.push_back(std::move(entry));
  }
  env.insert(env.end(), overrides.begin(), overrides.end());
  return env;
}

inline void process_child_exec(ProcessCommand command,
                               std::vector<std::string> args,
                               std::vector<std::string> env_overrides,
                               ProcessPipeSet pipes) {
  dup2(pipes.stdin_read, STDIN_FILENO);
  dup2(pipes.stdout_write, STDOUT_FILENO);
  dup2(pipes.stderr_write, STDERR_FILENO);
  close_fd_if_open(pipes.stdin_read);
  close_fd_if_open(pipes.stdin_write);
  close_fd_if_open(pipes.stdout_read);
  close_fd_if_open(pipes.stdout_write);
  close_fd_if_open(pipes.stderr_read);
  close_fd_if_open(pipes.stderr_write);
  close_fd_if_open(pipes.exec_error_read);

  if (!command.cwd.empty() && chdir(command.cwd.c_str()) != 0) {
    const int err = errno;
    write(pipes.exec_error_write, &err, sizeof(err));
    _exit(127);
  }

  auto argv = process_argv_ptrs(command.program, args);
  auto env_values = command.inherit_env ? inherited_environment_with_overrides(env_overrides)
                                        : std::move(env_overrides);
  auto envp = process_env_ptrs(env_values);
  if (command.program.find('/') == std::string::npos && command.inherit_env) {
    execvp(command.program.c_str(), argv.data());
  } else {
    execve(command.program.c_str(), argv.data(), envp.data());
  }
  const int err = errno;
  write(pipes.exec_error_write, &err, sizeof(err));
  _exit(127);
}

inline void append_capped(std::string& out, const char* data, std::size_t len, std::int64_t cap) {
  if (cap <= 0) return;
  const auto current = static_cast<std::int64_t>(out.size());
  if (current >= cap) return;
  const auto room = static_cast<std::size_t>(cap - current);
  out.append(data, std::min(room, len));
}

inline bool read_process_pipe(int fd, std::string& out, std::int64_t cap) {
  char buffer[4096];
  while (true) {
    const ssize_t rc = read(fd, buffer, sizeof(buffer));
    if (rc > 0) {
      append_capped(out, buffer, static_cast<std::size_t>(rc), cap);
      continue;
    }
    if (rc == 0) return true;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return false;
    return true;
  }
}

inline bool write_process_stdin(int fd, const std::string& input, std::size_t& offset) {
  while (offset < input.size()) {
    const ssize_t rc = write(fd, input.data() + offset, input.size() - offset);
    if (rc > 0) {
      offset += static_cast<std::size_t>(rc);
      continue;
    }
    if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return false;
    return true;
  }
  return true;
}

inline Result<ProcessOutput, std::string> process_run_parent(ProcessCommand command,
                                                             pid_t child,
                                                             ProcessPipeSet& pipes) {
  close_fd_if_open(pipes.stdin_read);
  close_fd_if_open(pipes.stdout_write);
  close_fd_if_open(pipes.stderr_write);
  close_fd_if_open(pipes.exec_error_write);

  std::string stdout_data;
  std::string stderr_data;
  bool stdout_closed = false;
  bool stderr_closed = false;
  bool exec_error_closed = false;
  std::optional<int> exec_error;
  std::size_t stdin_offset = 0;
  bool stdin_closed = command.stdin.data.empty();
  if (stdin_closed) close_fd_if_open(pipes.stdin_write);

  int status = 0;
  bool child_done = false;
  bool timed_out = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(command.timeout_ms);

  while (!child_done || !stdout_closed || !stderr_closed || !exec_error_closed) {
    std::vector<pollfd> fds;
    if (!stdin_closed) fds.push_back(pollfd{pipes.stdin_write, POLLOUT, 0});
    if (!stdout_closed) fds.push_back(pollfd{pipes.stdout_read, POLLIN | POLLHUP, 0});
    if (!stderr_closed) fds.push_back(pollfd{pipes.stderr_read, POLLIN | POLLHUP, 0});
    if (!exec_error_closed) fds.push_back(pollfd{pipes.exec_error_read, POLLIN | POLLHUP, 0});

    const auto now = std::chrono::steady_clock::now();
    if (!child_done && now >= deadline) {
      timed_out = true;
      kill(child, SIGKILL);
    }
    const auto wait_left = deadline > now ? std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count() : 0;
    const int timeout = child_done ? 10 : static_cast<int>(std::min<std::int64_t>(wait_left, 50));
    if (!fds.empty()) poll(fds.data(), fds.size(), timeout);

    if (!stdin_closed) {
      if (write_process_stdin(pipes.stdin_write, command.stdin.data, stdin_offset)) {
        stdin_closed = true;
        close_fd_if_open(pipes.stdin_write);
      }
    }
    if (!stdout_closed && read_process_pipe(pipes.stdout_read, stdout_data, command.max_stdout_bytes)) {
      stdout_closed = true;
      close_fd_if_open(pipes.stdout_read);
    }
    if (!stderr_closed && read_process_pipe(pipes.stderr_read, stderr_data, command.max_stderr_bytes)) {
      stderr_closed = true;
      close_fd_if_open(pipes.stderr_read);
    }
    if (!exec_error_closed) {
      int err = 0;
      const ssize_t rc = read(pipes.exec_error_read, &err, sizeof(err));
      if (rc == static_cast<ssize_t>(sizeof(err))) exec_error = err;
      if (rc == 0 || (rc < 0 && errno != EAGAIN && errno != EWOULDBLOCK) || exec_error.has_value()) {
        exec_error_closed = true;
        close_fd_if_open(pipes.exec_error_read);
      }
    }
    if (!child_done) {
      const pid_t waited = waitpid(child, &status, WNOHANG);
      if (waited == child) child_done = true;
    }
  }

  if (!child_done) waitpid(child, &status, 0);
  process_close_all(pipes);
  if (exec_error.has_value()) {
    return err_result<ProcessOutput>(std::string("process exec failed: ") + std::strerror(*exec_error));
  }
  const std::int64_t exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  const std::int64_t signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
  return ok_result(ProcessOutput(exit_code,
                                 signal,
                                 timed_out,
                                 Bytes{std::move(stdout_data)},
                                 Bytes{std::move(stderr_data)}));
}

inline Result<ProcessOutput, std::string> process_run(ProcessCommand command) {
  auto valid = validate_process_command(command);
  if (std::holds_alternative<typename Result<void, std::string>::Err>(valid.data)) {
    return err_result<ProcessOutput>(std::get<typename Result<void, std::string>::Err>(valid.data).value);
  }
  auto args_result = json_string_array(command.args, "process args");
  if (std::holds_alternative<typename Result<std::vector<std::string>, std::string>::Err>(args_result.data)) {
    return err_result<ProcessOutput>(
        std::get<typename Result<std::vector<std::string>, std::string>::Err>(std::move(args_result.data)).value);
  }
  auto env_result = json_env_array(command.env);
  if (std::holds_alternative<typename Result<std::vector<std::string>, std::string>::Err>(env_result.data)) {
    return err_result<ProcessOutput>(
        std::get<typename Result<std::vector<std::string>, std::string>::Err>(std::move(env_result.data)).value);
  }

  ProcessPipeSet pipes;
  auto pipe_status = process_prepare_parent_pipes(pipes);
  if (std::holds_alternative<typename Result<void, std::string>::Err>(pipe_status.data)) {
    process_close_all(pipes);
    return err_result<ProcessOutput>(std::get<typename Result<void, std::string>::Err>(pipe_status.data).value);
  }

  auto args = std::get<typename Result<std::vector<std::string>, std::string>::Ok>(std::move(args_result.data)).value;
  auto env = std::get<typename Result<std::vector<std::string>, std::string>::Ok>(std::move(env_result.data)).value;
  const pid_t child = fork();
  if (child < 0) {
    process_close_all(pipes);
    return err_result<ProcessOutput>(std::string("process fork failed: ") + std::strerror(errno));
  }
  if (child == 0) {
    process_child_exec(std::move(command), std::move(args), std::move(env), pipes);
  }
  return process_run_parent(std::move(command), child, pipes);
}
#endif

inline Result<std::string, std::string> json_get_string(const JsonValue& value, std::string_view key) {
  if (value.has_object_fields) {
    for (std::size_t i = 0; i < value.object_fields.count; ++i) {
      const auto& field = value.object_fields.fields[i];
      if (!json_object_field_key_equals(value, field, key)) continue;
      if (field.value.kind != JsonValueKind::String) {
        return err_result<std::string>("JSON field is not a string: " + std::string(key));
      }
      const std::string_view raw =
          std::string_view(value.text).substr(field.value.raw_start + 1,
                                              field.value.raw_end - field.value.raw_start - 2);
      if (!field.value.string_needs_decode) return ok_result(std::string(raw));
      std::string out;
      if (!decode_json_string_content(raw, out)) {
        return err_result<std::string>("JSON field is not a string: " + std::string(key));
      }
      return ok_result(out);
    }
    return err_result<std::string>("JSON string field not found: " + std::string(key));
  }
  const auto field = json_lookup_field_text(value, key);
  if (!field.has_value()) return err_result<std::string>("JSON string field not found: " + std::string(key));
  JsonCursor cursor(*field);
  std::string out;
  if (!parse_json_string_literal(cursor, &out)) {
    return err_result<std::string>("JSON field is not a string: " + std::string(key));
  }
  cursor.skip_ws();
  if (!cursor.eof()) return err_result<std::string>("JSON field is not a string: " + std::string(key));
  return ok_result(out);
}

inline Result<std::int64_t, std::string> json_get_int(const JsonValue& value, std::string_view key) {
  if (value.has_object_fields) {
    for (std::size_t i = 0; i < value.object_fields.count; ++i) {
      const auto& field = value.object_fields.fields[i];
      if (!json_object_field_key_equals(value, field, key)) continue;
      if (field.value.kind != JsonValueKind::Int) {
        return err_result<std::int64_t>("JSON field is not an int: " + std::string(key));
      }
      return ok_result(field.value.int_value);
    }
    return err_result<std::int64_t>("JSON int field not found: " + std::string(key));
  }
  const auto field = json_lookup_field_text(value, key);
  if (!field.has_value()) return err_result<std::int64_t>("JSON int field not found: " + std::string(key));
  JsonCursor cursor(*field);
  std::int64_t out = 0;
  if (!parse_json_int_literal(cursor, &out)) {
    return err_result<std::int64_t>("JSON field is not an int: " + std::string(key));
  }
  cursor.skip_ws();
  if (!cursor.eof()) return err_result<std::int64_t>("JSON field is not an int: " + std::string(key));
  return ok_result(out);
}

inline Result<bool, std::string> json_get_bool(const JsonValue& value, std::string_view key) {
  if (value.has_object_fields) {
    for (std::size_t i = 0; i < value.object_fields.count; ++i) {
      const auto& field = value.object_fields.fields[i];
      if (!json_object_field_key_equals(value, field, key)) continue;
      if (field.value.kind != JsonValueKind::Bool) {
        return err_result<bool>("JSON field is not a bool: " + std::string(key));
      }
      return ok_result(field.value.bool_value);
    }
    return err_result<bool>("JSON bool field not found: " + std::string(key));
  }
  const auto field = json_lookup_field_text(value, key);
  if (!field.has_value()) return err_result<bool>("JSON bool field not found: " + std::string(key));
  JsonCursor cursor(*field);
  bool out = false;
  if (!parse_json_bool_literal(cursor, &out)) {
    return err_result<bool>("JSON field is not a bool: " + std::string(key));
  }
  cursor.skip_ws();
  if (!cursor.eof()) return err_result<bool>("JSON field is not a bool: " + std::string(key));
  return ok_result(out);
}

inline std::string ascii_lower(std::string text) {
  for (char& ch : text) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return text;
}

inline char ascii_lower_char(char ch) {
  return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
}

inline std::string ascii_lower_copy(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char ch : text) out.push_back(ascii_lower_char(ch));
  return out;
}

inline std::string_view trim_http_token_view(std::string_view text) {
  std::size_t start = 0;
  while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) start += 1;
  std::size_t end = text.size();
  while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) end -= 1;
  return text.substr(start, end - start);
}

inline bool ascii_ieq(std::string_view lhs, std::string_view rhs) {
  if (lhs.size() != rhs.size()) return false;
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    if (ascii_lower_char(lhs[i]) != ascii_lower_char(rhs[i])) return false;
  }
  return true;
}

inline std::string trim_http_token(std::string_view text) {
  return std::string(trim_http_token_view(text));
}

inline Result<std::optional<std::size_t>, std::string> parse_http_content_length_values(std::string_view headers) {
  std::optional<std::size_t> out;
  std::size_t cursor = 0;
  while (cursor < headers.size()) {
    const std::size_t line_end = headers.find("\r\n", cursor);
    const std::string_view line =
        line_end == std::string::npos ? headers.substr(cursor) : headers.substr(cursor, line_end - cursor);
    const std::size_t colon = line.find(':');
    if (colon != std::string::npos) {
      const std::string_view name = trim_http_token_view(line.substr(0, colon));
      if (ascii_ieq(name, "content-length")) {
        std::string_view rest = line.substr(colon + 1);
        while (true) {
          const std::size_t comma = rest.find(',');
          const std::string_view token =
              trim_http_token_view(comma == std::string_view::npos ? rest : rest.substr(0, comma));
          if (token.empty()) return err_result<std::optional<std::size_t>>("invalid Content-Length header");
          std::size_t len = 0;
          for (unsigned char ch : token) {
            if (ch < '0' || ch > '9') {
              return err_result<std::optional<std::size_t>>("invalid Content-Length header");
            }
            const std::size_t digit = static_cast<std::size_t>(ch - '0');
            if (len > (std::numeric_limits<std::size_t>::max() - digit) / 10) {
              return err_result<std::optional<std::size_t>>("invalid Content-Length header");
            }
            len = len * 10 + digit;
          }
          if (out.has_value() && *out != len) {
            return err_result<std::optional<std::size_t>>("conflicting Content-Length headers");
          }
          out = len;
          if (comma == std::string_view::npos) break;
          rest = rest.substr(comma + 1);
        }
      }
    }
    if (line_end == std::string::npos) break;
    cursor = line_end + 2;
  }
  return ok_result(out);
}

inline std::string join_http_header_tokens(const std::vector<std::string>& values) {
  std::string out;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) out += ", ";
    out += values[i];
  }
  return out;
}

inline Result<std::vector<std::string>, std::string> parse_http_transfer_codings(std::string_view headers) {
  std::vector<std::string> codings;
  std::size_t cursor = 0;
  while (cursor < headers.size()) {
    const std::size_t line_end = headers.find("\r\n", cursor);
    const std::string_view line =
        line_end == std::string_view::npos ? headers.substr(cursor) : headers.substr(cursor, line_end - cursor);
    const std::size_t colon = line.find(':');
    if (colon != std::string_view::npos) {
      const std::string_view name = trim_http_token_view(line.substr(0, colon));
      if (ascii_ieq(name, "transfer-encoding")) {
        std::string_view rest = line.substr(colon + 1);
        while (true) {
          const std::size_t comma = rest.find(',');
          const std::string_view token =
              trim_http_token_view(comma == std::string_view::npos ? rest : rest.substr(0, comma));
          if (token.empty()) return err_result<std::vector<std::string>>("invalid Transfer-Encoding header");
          codings.push_back(ascii_lower_copy(token));
          if (comma == std::string_view::npos) break;
          rest = rest.substr(comma + 1);
        }
      }
    }
    if (line_end == std::string_view::npos) break;
    cursor = line_end + 2;
  }
  return ok_result(codings);
}

inline Result<std::vector<std::string>, std::string> parse_http_connection_tokens(std::string_view headers) {
  std::vector<std::string> tokens;
  std::size_t cursor = 0;
  while (cursor < headers.size()) {
    const std::size_t line_end = headers.find("\r\n", cursor);
    const std::string_view line =
        line_end == std::string_view::npos ? headers.substr(cursor) : headers.substr(cursor, line_end - cursor);
    const std::size_t colon = line.find(':');
    if (colon != std::string_view::npos) {
      const std::string_view name = trim_http_token_view(line.substr(0, colon));
      if (ascii_ieq(name, "connection")) {
        std::string_view rest = line.substr(colon + 1);
        while (true) {
          const std::size_t comma = rest.find(',');
          const std::string_view token =
              trim_http_token_view(comma == std::string_view::npos ? rest : rest.substr(0, comma));
          if (token.empty()) {
            return err_result<std::vector<std::string>>("invalid Connection header");
          }
          tokens.push_back(ascii_lower_copy(token));
          if (comma == std::string_view::npos) break;
          rest = rest.substr(comma + 1);
        }
      }
    }
    if (line_end == std::string_view::npos) break;
    cursor = line_end + 2;
  }
  return ok_result(tokens);
}

inline std::optional<std::string> parse_http_header_value(std::string_view headers,
                                                          std::string_view header_name) {
  const std::string_view wanted = trim_http_token_view(header_name);
  if (wanted.empty()) return std::nullopt;
  std::size_t cursor = 0;
  while (cursor < headers.size()) {
    const std::size_t line_end = headers.find("\r\n", cursor);
    const std::string_view line =
        line_end == std::string::npos ? std::string_view(headers).substr(cursor)
                                      : std::string_view(headers).substr(cursor, line_end - cursor);
    const std::size_t colon = line.find(':');
    if (colon != std::string::npos) {
      const std::string_view name = trim_http_token_view(line.substr(0, colon));
      if (ascii_ieq(name, wanted)) {
        return std::string(trim_http_token_view(line.substr(colon + 1)));
      }
    }
    if (line_end == std::string::npos) break;
    cursor = line_end + 2;
  }
  return std::nullopt;
}

inline Result<std::optional<std::string>, std::string> parse_http_unique_header_value(std::string_view headers,
                                                                                       std::string_view header_name) {
  const std::string_view wanted = trim_http_token_view(header_name);
  if (wanted.empty()) return ok_result(std::optional<std::string>{});
  std::optional<std::string> out;
  std::size_t cursor = 0;
  while (cursor < headers.size()) {
    const std::size_t line_end = headers.find("\r\n", cursor);
    const std::string_view line =
        line_end == std::string::npos ? std::string_view(headers).substr(cursor)
                                      : std::string_view(headers).substr(cursor, line_end - cursor);
    const std::size_t colon = line.find(':');
    if (colon != std::string::npos) {
      const std::string_view name = trim_http_token_view(line.substr(0, colon));
      if (ascii_ieq(name, wanted)) {
        const std::string value = std::string(trim_http_token_view(line.substr(colon + 1)));
        if (out.has_value()) {
          return err_result<std::optional<std::string>>(
              "duplicate HTTP header values found: " + ascii_lower_copy(wanted));
        }
        out = value;
      }
    }
    if (line_end == std::string::npos) break;
    cursor = line_end + 2;
  }
  return ok_result(out);
}

inline Result<HttpMethod, std::string> parse_http_method(std::string_view token) {
  if (token == "GET") return ok_result(HttpMethod{HttpMethod::Get{}});
  if (token == "HEAD") return ok_result(HttpMethod{HttpMethod::Head{}});
  if (token == "POST") return ok_result(HttpMethod{HttpMethod::Post{}});
  if (token == "PUT") return ok_result(HttpMethod{HttpMethod::Put{}});
  if (token == "DELETE") return ok_result(HttpMethod{HttpMethod::Delete{}});
  return err_result<HttpMethod>("unsupported HTTP method: " + std::string(token));
}

inline std::string render_http_method(const HttpMethod& method) {
  if (std::holds_alternative<HttpMethod::Get>(method.data)) return "GET";
  if (std::holds_alternative<HttpMethod::Head>(method.data)) return "HEAD";
  if (std::holds_alternative<HttpMethod::Post>(method.data)) return "POST";
  if (std::holds_alternative<HttpMethod::Put>(method.data)) return "PUT";
  return "DELETE";
}

inline bool http_method_is_head(const HttpMethod& method) {
  return std::holds_alternative<HttpMethod::Head>(method.data);
}

inline bool http_value_has_forbidden_control(std::string_view text) {
  for (unsigned char ch : text) {
    if (ch == '\r' || ch == '\n' || ch == '\0') return true;
  }
  return false;
}

inline bool http_request_authority_is_valid(std::string_view authority) {
  if (authority.empty()) return false;
  return !http_value_has_forbidden_control(authority);
}

inline bool http_request_path_is_valid(std::string_view path) {
  if (path.empty() || path.front() != '/') return false;
  if (http_value_has_forbidden_control(path)) return false;
  for (unsigned char ch : path) {
    if (ch == ' ') return false;
  }
  return true;
}

inline bool http_content_type_is_valid(std::string_view value) {
  return !http_value_has_forbidden_control(value);
}

inline bool http_header_name_is_valid(std::string_view name) {
  if (name.empty()) return false;
  for (unsigned char ch : name) {
    const bool token = (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
                       (ch >= 'a' && ch <= 'z') || ch == '!' || ch == '#' || ch == '$' ||
                       ch == '%' || ch == '&' || ch == '\'' || ch == '*' || ch == '+' ||
                       ch == '-' || ch == '.' || ch == '^' || ch == '_' || ch == '`' ||
                       ch == '|' || ch == '~';
    if (!token) return false;
  }
  return true;
}

inline bool http_reserved_request_header(std::string_view name) {
  const std::string lower = ascii_lower(trim_http_token(name));
  return lower == "host" || lower == "content-length" || lower == "connection" ||
         lower == "transfer-encoding" || lower == "content-type";
}

inline bool http_reserved_response_header(std::string_view name) {
  const std::string lower = ascii_lower(trim_http_token(name));
  return lower == "content-length" || lower == "connection" ||
         lower == "transfer-encoding" || lower == "content-type";
}

inline Result<std::string, std::string> build_http_extra_header_block1(std::string name1,
                                                                       std::string value1) {
  if (!http_header_name_is_valid(name1)) {
    return err_result<std::string>("invalid HTTP header name: " + name1);
  }
  if (http_reserved_request_header(name1)) {
    return err_result<std::string>("reserved HTTP header cannot be set explicitly: " + name1);
  }
  if (http_value_has_forbidden_control(value1)) {
    return err_result<std::string>("invalid HTTP header value for: " + name1);
  }
  return ok_result(name1 + ": " + value1 + "\r\n");
}

inline Result<std::string, std::string> build_http_extra_header_block2(std::string name1,
                                                                       std::string value1,
                                                                       std::string name2,
                                                                       std::string value2) {
  auto first = build_http_extra_header_block1(std::move(name1), std::move(value1));
  if (std::holds_alternative<typename Result<std::string, std::string>::Err>(first.data)) {
    return err_result<std::string>(
        std::get<typename Result<std::string, std::string>::Err>(std::move(first.data)).value);
  }
  auto second = build_http_extra_header_block1(std::move(name2), std::move(value2));
  if (std::holds_alternative<typename Result<std::string, std::string>::Err>(second.data)) {
    return err_result<std::string>(
        std::get<typename Result<std::string, std::string>::Err>(std::move(second.data)).value);
  }
  return ok_result(
      std::get<typename Result<std::string, std::string>::Ok>(std::move(first.data)).value +
      std::get<typename Result<std::string, std::string>::Ok>(std::move(second.data)).value);
}

inline Result<std::string, std::string> build_http_extra_header_block3(std::string name1,
                                                                       std::string value1,
                                                                       std::string name2,
                                                                       std::string value2,
                                                                       std::string name3,
                                                                       std::string value3) {
  auto first_two =
      build_http_extra_header_block2(std::move(name1), std::move(value1), std::move(name2), std::move(value2));
  if (std::holds_alternative<typename Result<std::string, std::string>::Err>(first_two.data)) {
    return err_result<std::string>(
        std::get<typename Result<std::string, std::string>::Err>(std::move(first_two.data)).value);
  }
  auto third = build_http_extra_header_block1(std::move(name3), std::move(value3));
  if (std::holds_alternative<typename Result<std::string, std::string>::Err>(third.data)) {
    return err_result<std::string>(
        std::get<typename Result<std::string, std::string>::Err>(std::move(third.data)).value);
  }
  return ok_result(
      std::get<typename Result<std::string, std::string>::Ok>(std::move(first_two.data)).value +
      std::get<typename Result<std::string, std::string>::Ok>(std::move(third.data)).value);
}

inline Result<std::string, std::string> build_http_extra_response_header_block1(std::string name1,
                                                                                std::string value1) {
  if (!http_header_name_is_valid(name1)) {
    return err_result<std::string>("invalid HTTP header name: " + name1);
  }
  if (http_reserved_response_header(name1)) {
    return err_result<std::string>("reserved HTTP header cannot be set explicitly: " + name1);
  }
  if (http_value_has_forbidden_control(value1)) {
    return err_result<std::string>("invalid HTTP header value for: " + name1);
  }
  return ok_result(name1 + ": " + value1 + "\r\n");
}

inline Result<std::string, std::string> build_http_extra_response_header_block2(std::string name1,
                                                                                std::string value1,
                                                                                std::string name2,
                                                                                std::string value2) {
  auto first = build_http_extra_response_header_block1(std::move(name1), std::move(value1));
  if (std::holds_alternative<typename Result<std::string, std::string>::Err>(first.data)) {
    return err_result<std::string>(
        std::get<typename Result<std::string, std::string>::Err>(std::move(first.data)).value);
  }
  auto second = build_http_extra_response_header_block1(std::move(name2), std::move(value2));
  if (std::holds_alternative<typename Result<std::string, std::string>::Err>(second.data)) {
    return err_result<std::string>(
        std::get<typename Result<std::string, std::string>::Err>(std::move(second.data)).value);
  }
  return ok_result(
      std::get<typename Result<std::string, std::string>::Ok>(std::move(first.data)).value +
      std::get<typename Result<std::string, std::string>::Ok>(std::move(second.data)).value);
}

inline Result<std::string, std::string> build_http_extra_response_header_block3(std::string name1,
                                                                                std::string value1,
                                                                                std::string name2,
                                                                                std::string value2,
                                                                                std::string name3,
                                                                                std::string value3) {
  auto first_two = build_http_extra_response_header_block2(std::move(name1),
                                                           std::move(value1),
                                                           std::move(name2),
                                                           std::move(value2));
  if (std::holds_alternative<typename Result<std::string, std::string>::Err>(first_two.data)) {
    return err_result<std::string>(
        std::get<typename Result<std::string, std::string>::Err>(std::move(first_two.data)).value);
  }
  auto third = build_http_extra_response_header_block1(std::move(name3), std::move(value3));
  if (std::holds_alternative<typename Result<std::string, std::string>::Err>(third.data)) {
    return err_result<std::string>(
        std::get<typename Result<std::string, std::string>::Err>(std::move(third.data)).value);
  }
  return ok_result(
      std::get<typename Result<std::string, std::string>::Ok>(std::move(first_two.data)).value +
      std::get<typename Result<std::string, std::string>::Ok>(std::move(third.data)).value);
}

inline HttpResponse http_text_response(std::int64_t status, std::string body) {
  return HttpResponse(status, "text/plain; charset=utf-8", bytes_from_string(std::move(body)));
}

inline HttpResponse http_ok_text(std::string body) {
  return http_text_response(200, std::move(body));
}

inline HttpResponse http_bad_request_text(std::string body) {
  return http_text_response(400, std::move(body));
}

inline HttpResponse http_method_not_allowed_text(std::string body) {
  return http_text_response(405, std::move(body));
}

inline HttpResponse http_not_found_text(std::string body) {
  return http_text_response(404, std::move(body));
}

inline HttpResponse http_internal_error_text(std::string body) {
  return http_text_response(500, std::move(body));
}

inline bool next_http_path_segment(std::string_view path, std::size_t& cursor, std::string_view& segment) {
  if (cursor >= path.size()) return false;
  if (path[cursor] != '/') return false;
  cursor += 1;
  const std::size_t start = cursor;
  while (cursor < path.size() && path[cursor] != '/') cursor += 1;
  segment = path.substr(start, cursor - start);
  return true;
}

inline Result<HttpRoutePattern, std::string> http_compile_route_uncached(std::string pattern) {
  if (pattern.empty() || pattern.front() != '/') {
    return err_result<HttpRoutePattern>("route pattern must start with '/'");
  }

  HttpRoutePattern compiled;
  compiled.pattern = std::move(pattern);
  std::size_t cursor = 0;
  std::string_view segment;
  while (next_http_path_segment(compiled.pattern, cursor, segment)) {
    HttpRoutePatternSegment out;
    if (!segment.empty() && segment.front() == ':') {
      if (segment.size() == 1) {
        return err_result<HttpRoutePattern>("route pattern contains an empty parameter segment");
      }
      out.parameter = true;
      out.text = std::string(segment.substr(1));
      compiled.param_count += 1;
    } else {
      out.text = std::string(segment);
    }
    compiled.segments.push_back(std::move(out));
  }
  return ok_result(std::move(compiled));
}

inline Result<HttpRoutePattern, std::string> http_compile_route(std::string pattern) {
  return http_compile_route_uncached(std::move(pattern));
}

struct HttpRoutePatternCacheEntry {
  std::string pattern;
  Result<HttpRoutePattern, std::string> compiled;
};

inline const Result<HttpRoutePattern, std::string>& http_cached_route_pattern(std::string_view pattern) {
  constexpr std::size_t kMaxHttpRoutePatternCacheEntries = 64;
  thread_local std::vector<HttpRoutePatternCacheEntry> cache;
  for (const auto& entry : cache) {
    if (entry.pattern == pattern) return entry.compiled;
  }
  if (cache.size() >= kMaxHttpRoutePatternCacheEntries) {
    cache.erase(cache.begin());
  }
  std::string key(pattern);
  auto compiled = http_compile_route_uncached(key);
  cache.push_back(HttpRoutePatternCacheEntry{std::move(key), std::move(compiled)});
  return cache.back().compiled;
}

inline bool http_compiled_path_matches(const HttpRoutePattern& route, std::string_view path) {
  if (path.empty() || path.front() != '/') return false;
  std::size_t path_cursor = 0;
  for (const auto& segment : route.segments) {
    std::string_view path_segment;
    if (!next_http_path_segment(path, path_cursor, path_segment)) return false;
    if (segment.parameter) {
      if (path_segment.empty()) return false;
      continue;
    }
    if (path_segment != segment.text) return false;
  }
  std::string_view extra_segment;
  return !next_http_path_segment(path, path_cursor, extra_segment);
}

inline Result<HttpRouteCaptureViews, std::string>
http_compiled_route_capture_views(const HttpRoutePattern& route,
                                  std::string_view path,
                                  std::size_t expected_count,
                                  std::string wrong_arity_message) {
  if (path.empty() || path.front() != '/') {
    return err_result<HttpRouteCaptureViews>("request path must start with '/'");
  }

  HttpRouteCaptureViews captured;
  std::size_t captured_count = 0;
  std::size_t path_cursor = 0;
  for (const auto& segment : route.segments) {
    std::string_view path_segment;
    if (!next_http_path_segment(path, path_cursor, path_segment)) {
      return err_result<HttpRouteCaptureViews>("request path does not match route pattern");
    }
    if (segment.parameter) {
      if (path_segment.empty()) {
        return err_result<HttpRouteCaptureViews>("request path does not match route pattern");
      }
      if (captured_count >= expected_count) {
        return err_result<HttpRouteCaptureViews>(std::move(wrong_arity_message));
      }
      captured.values[captured_count] = path_segment;
      captured_count += 1;
      continue;
    }
    if (path_segment != segment.text) {
      return err_result<HttpRouteCaptureViews>("request path does not match route pattern");
    }
  }

  std::string_view extra_segment;
  if (next_http_path_segment(path, path_cursor, extra_segment)) {
    return err_result<HttpRouteCaptureViews>("request path does not match route pattern");
  }
  if (captured_count != expected_count) {
    return err_result<HttpRouteCaptureViews>(std::move(wrong_arity_message));
  }
  return ok_result(captured);
}

inline Result<std::string, std::string> http_compiled_route_param1(const HttpRoutePattern& route,
                                                                    std::string_view path) {
  auto captured = http_compiled_route_capture_views(route,
                                                    path,
                                                    1,
                                                    "route pattern must contain exactly one :param segment");
  if (result_is_err(captured)) return err_result<std::string>(result_err_ref(captured));
  return ok_result(std::string(result_ok_ref(captured).values[0]));
}

inline Result<HttpRouteParams2, std::string> http_compiled_route_params2(const HttpRoutePattern& route,
                                                                         std::string_view path) {
  auto captured = http_compiled_route_capture_views(route,
                                                    path,
                                                    2,
                                                    "route pattern does not expose the expected parameter count");
  if (result_is_err(captured)) return err_result<HttpRouteParams2>(result_err_ref(captured));
  const auto& values = result_ok_ref(captured).values;
  return ok_result(HttpRouteParams2{std::string(values[0]), std::string(values[1])});
}

inline Result<HttpRouteParams3, std::string> http_compiled_route_params3(const HttpRoutePattern& route,
                                                                         std::string_view path) {
  auto captured = http_compiled_route_capture_views(route,
                                                    path,
                                                    3,
                                                    "route pattern does not expose the expected parameter count");
  if (result_is_err(captured)) return err_result<HttpRouteParams3>(result_err_ref(captured));
  const auto& values = result_ok_ref(captured).values;
  return ok_result(
      HttpRouteParams3{std::string(values[0]), std::string(values[1]), std::string(values[2])});
}

struct HttpSingleParamPattern {
  bool recognized = false;
  bool valid = false;
  std::string_view prefix;
  std::string_view suffix;
};

inline HttpSingleParamPattern analyze_http_single_param_pattern(std::string_view pattern) {
  HttpSingleParamPattern out;
  const std::size_t colon = pattern.find("/:");
  if (colon == std::string_view::npos) return out;
  const std::size_t param_start = colon + 2;
  const std::size_t param_end = pattern.find('/', param_start);
  const std::size_t suffix_start =
      (param_end == std::string_view::npos) ? pattern.size() : param_end;
  if (pattern.find(':', suffix_start) != std::string_view::npos) return out;
  out.recognized = true;
  if (param_start >= pattern.size() || suffix_start == param_start) return out;
  out.valid = true;
  out.prefix = pattern.substr(0, colon + 1);
  out.suffix = pattern.substr(suffix_start);
  return out;
}

inline bool http_path_matches_single_param(std::string_view prefix,
                                           std::string_view suffix,
                                           std::string_view path);

inline bool http_path_matches(std::string_view pattern, std::string_view path) {
  if (pattern.empty() || path.empty() || pattern.front() != '/' || path.front() != '/') return false;
  if (pattern == path) return true;

  const auto single_param = analyze_http_single_param_pattern(pattern);
  if (single_param.recognized) {
    return single_param.valid &&
           http_path_matches_single_param(single_param.prefix, single_param.suffix, path);
  }
  if (pattern.find(':') == std::string_view::npos) return false;

  const auto& compiled = http_cached_route_pattern(pattern);
  if (result_is_err(compiled)) return false;
  return http_compiled_path_matches(result_ok_ref(compiled), path);
}

inline Result<std::string, std::string> http_route_param1_single_param(std::string_view prefix,
                                                                       std::string_view suffix,
                                                                       std::string_view path);

inline std::optional<Result<std::string, std::string>>
http_route_param1_single_param_fast(std::string_view pattern, std::string_view path) {
  const auto single_param = analyze_http_single_param_pattern(pattern);
  if (!single_param.recognized) return std::nullopt;
  if (!single_param.valid) {
    return err_result<std::string>("route pattern contains an empty parameter segment");
  }
  return http_route_param1_single_param(single_param.prefix, single_param.suffix, path);
}

inline bool http_path_matches_single_param(std::string_view prefix,
                                           std::string_view suffix,
                                           std::string_view path) {
  if (path.size() <= prefix.size() + suffix.size()) return false;
  if (path.substr(0, prefix.size()) != prefix) return false;
  if (!suffix.empty()) {
    if (path.size() < suffix.size() || path.substr(path.size() - suffix.size()) != suffix) return false;
  }
  const std::string_view captured =
      path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
  return !captured.empty() && captured.find('/') == std::string_view::npos;
}

inline bool http_path_matches_trailing_prefix(std::string_view prefix, std::string_view path) {
  return http_path_matches_single_param(prefix, "", path);
}

inline Result<std::string, std::string> http_route_param1_single_param(std::string_view prefix,
                                                                       std::string_view suffix,
                                                                       std::string_view path) {
  if (!http_path_matches_single_param(prefix, suffix, path)) {
    return err_result<std::string>("request path does not match route pattern");
  }
  return ok_result(std::string(path.substr(prefix.size(), path.size() - prefix.size() - suffix.size())));
}

inline Result<std::string, std::string> http_route_param1_trailing_prefix(std::string_view prefix,
                                                                          std::string_view path) {
  return http_route_param1_single_param(prefix, "", path);
}

inline Result<std::string, std::string> http_route_param1(std::string_view pattern, std::string_view path) {
  if (pattern.empty() || pattern.front() != '/') {
    return err_result<std::string>("route pattern must start with '/'");
  }
  if (path.empty() || path.front() != '/') {
    return err_result<std::string>("request path must start with '/'");
  }
  if (auto fast = http_route_param1_single_param_fast(pattern, path); fast.has_value()) return *fast;

  const auto& compiled = http_cached_route_pattern(pattern);
  if (result_is_err(compiled)) return err_result<std::string>(result_err_ref(compiled));
  return http_compiled_route_param1(result_ok_ref(compiled), path);
}

inline Result<std::vector<std::string>, std::string> http_route_params(std::string_view pattern,
                                                                       std::string_view path,
                                                                       std::size_t expected_count) {
  if (expected_count > 3) {
    return err_result<std::vector<std::string>>("route pattern does not expose the expected parameter count");
  }
  const auto& compiled = http_cached_route_pattern(pattern);
  if (result_is_err(compiled)) {
    return err_result<std::vector<std::string>>(result_err_ref(compiled));
  }
  auto captured = http_compiled_route_capture_views(result_ok_ref(compiled),
                                                    path,
                                                    expected_count,
                                                    "route pattern does not expose the expected parameter count");
  if (result_is_err(captured)) {
    return err_result<std::vector<std::string>>(result_err_ref(captured));
  }

  std::vector<std::string> out;
  out.reserve(expected_count);
  const auto& values = result_ok_ref(captured).values;
  for (std::size_t i = 0; i < expected_count; ++i) {
    out.emplace_back(values[i]);
  }
  return ok_result(std::move(out));
}

inline Result<HttpRouteParams2, std::string> http_route_params2(std::string_view pattern, std::string_view path) {
  const auto& compiled = http_cached_route_pattern(pattern);
  if (result_is_err(compiled)) return err_result<HttpRouteParams2>(result_err_ref(compiled));
  return http_compiled_route_params2(result_ok_ref(compiled), path);
}

inline Result<HttpRouteParams3, std::string> http_route_params3(std::string_view pattern, std::string_view path) {
  const auto& compiled = http_cached_route_pattern(pattern);
  if (result_is_err(compiled)) return err_result<HttpRouteParams3>(result_err_ref(compiled));
  return http_compiled_route_params3(result_ok_ref(compiled), path);
}

struct HttpRequestFraming {
  std::size_t content_length = 0;
};

inline Result<HttpRequestFraming, std::string> parse_http_request_framing(std::string_view request_head,
                                                                          std::size_t max_body_bytes) {
  auto transfer_encoding = parse_http_transfer_codings(request_head);
  if (std::holds_alternative<typename Result<std::vector<std::string>, std::string>::Err>(transfer_encoding.data)) {
    return err_result<HttpRequestFraming>(
        std::get<typename Result<std::vector<std::string>, std::string>::Err>(std::move(transfer_encoding.data)).value);
  }
  auto transfer_codings =
      std::get<typename Result<std::vector<std::string>, std::string>::Ok>(std::move(transfer_encoding.data)).value;
  auto content_length = parse_http_content_length_values(request_head);
  if (std::holds_alternative<typename Result<std::optional<std::size_t>, std::string>::Err>(content_length.data)) {
    return err_result<HttpRequestFraming>(
        std::get<typename Result<std::optional<std::size_t>, std::string>::Err>(std::move(content_length.data)).value);
  }
  const auto maybe_length =
      std::get<typename Result<std::optional<std::size_t>, std::string>::Ok>(std::move(content_length.data)).value;

  if (!transfer_codings.empty() && maybe_length.has_value()) {
    return err_result<HttpRequestFraming>(
        "HTTP request must not include both Transfer-Encoding and Content-Length");
  }
  if (!transfer_codings.empty()) {
    return err_result<HttpRequestFraming>("HTTP request Transfer-Encoding is unsupported");
  }

  HttpRequestFraming out;
  out.content_length = maybe_length.value_or(0);
  if (out.content_length > max_body_bytes) {
    return err_result<HttpRequestFraming>("HTTP request body exceeds configured limit");
  }
  return ok_result(out);
}

inline Result<HttpRequest, std::string> parse_http_request_message(const std::string& message,
                                                                   std::size_t max_body_bytes) {
  const std::string_view message_view = message;
  const std::size_t header_end = message_view.find("\r\n\r\n");
  if (header_end == std::string::npos) {
    return err_result<HttpRequest>("incomplete HTTP request headers");
  }

  const std::size_t body_start = header_end + 4;
  const std::string_view request_head = message_view.substr(0, header_end);
  const std::string_view body = message_view.substr(body_start);

  const std::size_t line_end = request_head.find("\r\n");
  const std::string_view request_line =
      line_end == std::string::npos ? request_head : request_head.substr(0, line_end);
  const std::string_view headers =
      line_end == std::string::npos ? std::string_view{} : request_head.substr(line_end + 2);

  const std::size_t sp1 = request_line.find(' ');
  if (sp1 == std::string::npos) return err_result<HttpRequest>("malformed HTTP request line");
  const std::size_t sp2 = request_line.find(' ', sp1 + 1);
  if (sp2 == std::string::npos) return err_result<HttpRequest>("malformed HTTP request line");

  const std::string_view method_text = request_line.substr(0, sp1);
  const std::string_view path = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
  const std::string_view version = request_line.substr(sp2 + 1);
  if (version != "HTTP/1.1" && version != "HTTP/1.0") {
    return err_result<HttpRequest>("unsupported HTTP version: " + std::string(version));
  }

  auto method = parse_http_method(method_text);
  if (std::holds_alternative<typename Result<HttpMethod, std::string>::Err>(method.data)) {
    return err_result<HttpRequest>(
        std::get<typename Result<HttpMethod, std::string>::Err>(std::move(method.data)).value);
  }

  auto framing = parse_http_request_framing(request_head, max_body_bytes);
  if (std::holds_alternative<typename Result<HttpRequestFraming, std::string>::Err>(framing.data)) {
    return err_result<HttpRequest>(
        std::get<typename Result<HttpRequestFraming, std::string>::Err>(std::move(framing.data)).value);
  }
  const auto parsed_framing =
      std::get<typename Result<HttpRequestFraming, std::string>::Ok>(std::move(framing.data)).value;
  if (body.size() != parsed_framing.content_length) {
    if (body.size() > parsed_framing.content_length) {
      return err_result<HttpRequest>("unexpected extra bytes after HTTP request body");
    }
    return err_result<HttpRequest>("incomplete HTTP request body");
  }

  auto connection_tokens = parse_http_connection_tokens(headers);
  if (std::holds_alternative<typename Result<std::vector<std::string>, std::string>::Err>(connection_tokens.data)) {
    return err_result<HttpRequest>(
        std::get<typename Result<std::vector<std::string>, std::string>::Err>(std::move(connection_tokens.data)).value);
  }
  bool close_connection = version == "HTTP/1.0";
  for (const auto& token :
       std::get<typename Result<std::vector<std::string>, std::string>::Ok>(connection_tokens.data).value) {
    if (token == "close") {
      close_connection = true;
    } else if (token == "keep-alive") {
      close_connection = false;
    }
  }

  std::string owned_path(path);
  std::string owned_headers(headers);
  std::string owned_body(body);
  return ok_result(HttpRequest{
      std::get<typename Result<HttpMethod, std::string>::Ok>(std::move(method.data)).value,
      std::move(owned_path),
      Bytes{std::move(owned_body)},
      std::move(owned_headers),
      close_connection,
  });
}

inline Result<std::string, std::string> http_header(const HttpRequest& request, std::string_view name) {
  const auto value = parse_http_header_value(request.headers, name);
  if (!value.has_value()) {
    return err_result<std::string>("HTTP header not found: " + std::string(name));
  }
  return ok_result(*value);
}

inline Result<std::string, std::string> http_unique_header(const HttpRequest& request, std::string_view name) {
  auto value = parse_http_unique_header_value(request.headers, name);
  if (std::holds_alternative<typename Result<std::optional<std::string>, std::string>::Err>(value.data)) {
    return err_result<std::string>(
        std::get<typename Result<std::optional<std::string>, std::string>::Err>(std::move(value.data)).value);
  }
  auto maybe_value =
      std::get<typename Result<std::optional<std::string>, std::string>::Ok>(std::move(value.data)).value;
  if (!maybe_value.has_value()) {
    return err_result<std::string>("HTTP header not found: " + std::string(name));
  }
  return ok_result(*maybe_value);
}

inline Result<std::string, std::string> http_content_type(const HttpRequest& request) {
  return http_header(request, "Content-Type");
}

inline bool http_request_close_connection(const HttpRequest& request) {
  return request.close_connection;
}

inline const char* http_reason_phrase(std::int64_t status) {
  switch (status) {
  case 200: return "OK";
  case 401: return "Unauthorized";
  case 403: return "Forbidden";
  case 400: return "Bad Request";
  case 408: return "Request Timeout";
  case 412: return "Precondition Failed";
  case 428: return "Precondition Required";
  case 405: return "Method Not Allowed";
  case 404: return "Not Found";
  case 503: return "Service Unavailable";
  case 500: return "Internal Server Error";
  default: return "OK";
  }
}

enum class HttpClientBodyMode {
  NoBody,
  ContentLength,
  Chunked,
  UntilEof,
};

struct HttpClientResponseHead {
  std::int64_t status = 200;
  std::string content_type;
  std::string headers;
  HttpClientBodyMode body_mode = HttpClientBodyMode::ContentLength;
  std::size_t content_length = 0;
};

struct HttpChunkDecode {
  std::string decoded_body;
  std::size_t wire_size = 0;
};

enum class HttpChunkedDecodeAction {
  NeedMoreData,
  Complete,
};

struct HttpChunkedDecodeProgress {
  HttpChunkedDecodeAction action = HttpChunkedDecodeAction::NeedMoreData;
  std::size_t wire_size = 0;
};

enum class HttpChunkedDecodePhase {
  SizeLine,
  Data,
  DataCrlf,
  Trailers,
};

struct HttpChunkedDecodeState {
  HttpChunkedDecodePhase phase = HttpChunkedDecodePhase::SizeLine;
  std::size_t cursor = 0;
  std::size_t current_chunk_size = 0;
  std::string decoded_body;
};

enum class HttpClientResponseReadAction {
  NeedMoreData,
  SkipInterim,
  ReturnResponse,
};

struct HttpClientResponseReadResult {
  HttpClientResponseReadAction action = HttpClientResponseReadAction::NeedMoreData;
  std::size_t consume_prefix = 0;
  std::size_t body_offset = 0;
  std::optional<HttpClientResponseHead> head;
  HttpClientResponse response;
};

inline Result<std::string, std::string> build_http_request_message(const HttpClientRequest& request) {
  if (!http_request_authority_is_valid(request.authority)) {
    return err_result<std::string>("invalid HTTP authority");
  }
  if (!http_request_path_is_valid(request.path)) {
    return err_result<std::string>("invalid HTTP request path");
  }
  if (!request.content_type.empty() && !http_content_type_is_valid(request.content_type)) {
    return err_result<std::string>("invalid Content-Type header");
  }

  std::ostringstream out;
  out << render_http_method(request.method) << " " << request.path << " HTTP/1.1\r\n";
  out << "Host: " << request.authority << "\r\n";
  out << "Content-Length: " << request.body.data.size() << "\r\n";
  if (!request.content_type.empty()) {
    out << "Content-Type: " << request.content_type << "\r\n";
  }
  out << request.headers;
  out << "Connection: close\r\n\r\n";
  out << request.body.data;
  return ok_result(out.str());
}

inline Result<HttpClientRequest, std::string> http_request1(HttpMethod method,
                                                            std::string authority,
                                                            std::string path,
                                                            std::string content_type,
                                                            Bytes body,
                                                            std::string name1,
                                                            std::string value1) {
  auto headers = build_http_extra_header_block1(std::move(name1), std::move(value1));
  if (std::holds_alternative<typename Result<std::string, std::string>::Err>(headers.data)) {
    return err_result<HttpClientRequest>(
        std::get<typename Result<std::string, std::string>::Err>(std::move(headers.data)).value);
  }
  return ok_result(HttpClientRequest{
      std::move(method),
      std::move(authority),
      std::move(path),
      std::move(content_type),
      std::move(body),
      std::get<typename Result<std::string, std::string>::Ok>(std::move(headers.data)).value,
  });
}

inline Result<HttpClientRequest, std::string> http_request2(HttpMethod method,
                                                            std::string authority,
                                                            std::string path,
                                                            std::string content_type,
                                                            Bytes body,
                                                            std::string name1,
                                                            std::string value1,
                                                            std::string name2,
                                                            std::string value2) {
  auto headers = build_http_extra_header_block2(std::move(name1),
                                                std::move(value1),
                                                std::move(name2),
                                                std::move(value2));
  if (std::holds_alternative<typename Result<std::string, std::string>::Err>(headers.data)) {
    return err_result<HttpClientRequest>(
        std::get<typename Result<std::string, std::string>::Err>(std::move(headers.data)).value);
  }
  return ok_result(HttpClientRequest{
      std::move(method),
      std::move(authority),
      std::move(path),
      std::move(content_type),
      std::move(body),
      std::get<typename Result<std::string, std::string>::Ok>(std::move(headers.data)).value,
  });
}

inline Result<HttpClientRequest, std::string> http_request3(HttpMethod method,
                                                            std::string authority,
                                                            std::string path,
                                                            std::string content_type,
                                                            Bytes body,
                                                            std::string name1,
                                                            std::string value1,
                                                            std::string name2,
                                                            std::string value2,
                                                            std::string name3,
                                                            std::string value3) {
  auto headers = build_http_extra_header_block3(std::move(name1),
                                                std::move(value1),
                                                std::move(name2),
                                                std::move(value2),
                                                std::move(name3),
                                                std::move(value3));
  if (std::holds_alternative<typename Result<std::string, std::string>::Err>(headers.data)) {
    return err_result<HttpClientRequest>(
        std::get<typename Result<std::string, std::string>::Err>(std::move(headers.data)).value);
  }
  return ok_result(HttpClientRequest{
      std::move(method),
      std::move(authority),
      std::move(path),
      std::move(content_type),
      std::move(body),
      std::get<typename Result<std::string, std::string>::Ok>(std::move(headers.data)).value,
  });
}

inline Result<HttpResponse, std::string> http_response1(std::int64_t status,
                                                        std::string content_type,
                                                        Bytes body,
                                                        std::string name1,
                                                        std::string value1) {
  auto headers = build_http_extra_response_header_block1(std::move(name1), std::move(value1));
  if (std::holds_alternative<typename Result<std::string, std::string>::Err>(headers.data)) {
    return err_result<HttpResponse>(
        std::get<typename Result<std::string, std::string>::Err>(std::move(headers.data)).value);
  }
  return ok_result(HttpResponse{
      status,
      std::move(content_type),
      std::move(body),
      std::get<typename Result<std::string, std::string>::Ok>(std::move(headers.data)).value,
  });
}

inline Result<HttpResponse, std::string> http_response2(std::int64_t status,
                                                        std::string content_type,
                                                        Bytes body,
                                                        std::string name1,
                                                        std::string value1,
                                                        std::string name2,
                                                        std::string value2) {
  auto headers = build_http_extra_response_header_block2(std::move(name1),
                                                         std::move(value1),
                                                         std::move(name2),
                                                         std::move(value2));
  if (std::holds_alternative<typename Result<std::string, std::string>::Err>(headers.data)) {
    return err_result<HttpResponse>(
        std::get<typename Result<std::string, std::string>::Err>(std::move(headers.data)).value);
  }
  return ok_result(HttpResponse{
      status,
      std::move(content_type),
      std::move(body),
      std::get<typename Result<std::string, std::string>::Ok>(std::move(headers.data)).value,
  });
}

inline Result<HttpResponse, std::string> http_response3(std::int64_t status,
                                                        std::string content_type,
                                                        Bytes body,
                                                        std::string name1,
                                                        std::string value1,
                                                        std::string name2,
                                                        std::string value2,
                                                        std::string name3,
                                                        std::string value3) {
  auto headers = build_http_extra_response_header_block3(std::move(name1),
                                                         std::move(value1),
                                                         std::move(name2),
                                                         std::move(value2),
                                                         std::move(name3),
                                                         std::move(value3));
  if (std::holds_alternative<typename Result<std::string, std::string>::Err>(headers.data)) {
    return err_result<HttpResponse>(
        std::get<typename Result<std::string, std::string>::Err>(std::move(headers.data)).value);
  }
  return ok_result(HttpResponse{
      status,
      std::move(content_type),
      std::move(body),
      std::get<typename Result<std::string, std::string>::Ok>(std::move(headers.data)).value,
  });
}

inline Result<HttpResponse, std::string> http_response_with_header(HttpResponse response,
                                                                   std::string name,
                                                                   std::string value) {
  auto header = build_http_extra_response_header_block1(std::move(name), std::move(value));
  if (std::holds_alternative<typename Result<std::string, std::string>::Err>(header.data)) {
    return err_result<HttpResponse>(
        std::get<typename Result<std::string, std::string>::Err>(std::move(header.data)).value);
  }
  response.headers += std::get<typename Result<std::string, std::string>::Ok>(std::move(header.data)).value;
  return ok_result(std::move(response));
}

inline HttpResponse http_response_without_body(HttpResponse response) {
  response.body = Bytes{};
  return response;
}

inline bool http_response_status_is_informational(std::int64_t status) {
  return status >= 100 && status < 200;
}

inline bool http_response_status_forbids_body(std::int64_t status) {
  return http_response_status_is_informational(status) || status == 204 || status == 205 || status == 304;
}

inline Result<HttpClientResponseHead, std::string> parse_http_client_response_head(
    std::string_view response_head,
    std::size_t max_body_bytes,
    const std::optional<HttpMethod>& request_method = std::nullopt) {
  const std::size_t line_end = response_head.find("\r\n");
  const std::string_view status_line =
      line_end == std::string::npos ? response_head : response_head.substr(0, line_end);
  const std::string_view headers =
      line_end == std::string::npos ? std::string_view{} : response_head.substr(line_end + 2);

  const std::size_t sp1 = status_line.find(' ');
  if (sp1 == std::string::npos) return err_result<HttpClientResponseHead>("malformed HTTP status line");
  const std::size_t sp2 = status_line.find(' ', sp1 + 1);
  const std::string_view version = status_line.substr(0, sp1);
  if (version != "HTTP/1.1" && version != "HTTP/1.0") {
    return err_result<HttpClientResponseHead>("unsupported HTTP version: " + std::string(version));
  }

  const std::string_view code_text =
      sp2 == std::string::npos ? status_line.substr(sp1 + 1) : status_line.substr(sp1 + 1, sp2 - sp1 - 1);
  std::int64_t status = 0;
  const auto* code_begin = code_text.data();
  const auto* code_end = code_begin + code_text.size();
  const auto parse_result = std::from_chars(code_begin, code_end, status);
  if (parse_result.ec != std::errc{} || parse_result.ptr != code_end) {
    return err_result<HttpClientResponseHead>("invalid HTTP status code");
  }

  auto transfer_encoding = parse_http_transfer_codings(headers);
  if (std::holds_alternative<typename Result<std::vector<std::string>, std::string>::Err>(transfer_encoding.data)) {
    return err_result<HttpClientResponseHead>(
        std::get<typename Result<std::vector<std::string>, std::string>::Err>(std::move(transfer_encoding.data)).value);
  }
  const auto transfer_codings =
      std::get<typename Result<std::vector<std::string>, std::string>::Ok>(std::move(transfer_encoding.data)).value;

  auto content_length = parse_http_content_length_values(headers);
  if (std::holds_alternative<typename Result<std::optional<std::size_t>, std::string>::Err>(content_length.data)) {
    return err_result<HttpClientResponseHead>(
        std::get<typename Result<std::optional<std::size_t>, std::string>::Err>(std::move(content_length.data)).value);
  }
  const auto opt_len =
      std::get<typename Result<std::optional<std::size_t>, std::string>::Ok>(content_length.data).value;

  if (!transfer_codings.empty() && opt_len.has_value()) {
    return err_result<HttpClientResponseHead>(
        "HTTP response must not include both Transfer-Encoding and Content-Length");
  }
  if (!transfer_codings.empty()) {
    if (transfer_codings.size() != 1 || transfer_codings.front() != "chunked") {
      return err_result<HttpClientResponseHead>(
          "unsupported Transfer-Encoding: " + join_http_header_tokens(transfer_codings));
    }
  }

  HttpClientResponseHead out;
  out.status = status;
  out.headers = std::string(headers);
  if (auto ct = parse_http_header_value(headers, "Content-Type"); ct.has_value()) {
    out.content_type = *ct;
  }

  if (http_response_status_forbids_body(status) ||
      (request_method.has_value() && http_method_is_head(*request_method))) {
    out.body_mode = HttpClientBodyMode::NoBody;
    out.content_length = 0;
    return ok_result(out);
  }

  if (!transfer_codings.empty()) {
    out.body_mode = HttpClientBodyMode::Chunked;
  } else {
    if (opt_len.has_value()) {
      if (*opt_len > max_body_bytes) {
        return err_result<HttpClientResponseHead>("HTTP response body exceeds configured limit");
      }
      out.body_mode = HttpClientBodyMode::ContentLength;
      out.content_length = *opt_len;
    } else {
      out.body_mode = HttpClientBodyMode::UntilEof;
    }
  }
  return ok_result(out);
}

inline Result<std::size_t, std::string> parse_http_chunk_size_token(std::string_view size_text) {
  const std::string_view trimmed = trim_http_token_view(size_text);
  if (trimmed.empty()) {
    return err_result<std::size_t>("malformed chunked HTTP body");
  }
  unsigned long long chunk_size = 0;
  const auto* begin = trimmed.data();
  const auto* end = begin + trimmed.size();
  const auto parse_result = std::from_chars(begin, end, chunk_size, 16);
  if (parse_result.ec != std::errc{} || parse_result.ptr != end ||
      chunk_size > std::numeric_limits<std::size_t>::max()) {
    return err_result<std::size_t>("invalid chunk size in HTTP response");
  }
  return ok_result(static_cast<std::size_t>(chunk_size));
}

inline Result<HttpChunkedDecodeProgress, std::string> advance_http_chunked_body_decode(
    HttpChunkedDecodeState& state,
    std::string_view wire_body,
    std::size_t max_body_bytes) {
  while (true) {
    switch (state.phase) {
    case HttpChunkedDecodePhase::SizeLine: {
      const std::size_t line_end = wire_body.find("\r\n", state.cursor);
      if (line_end == std::string_view::npos) {
        return ok_result(HttpChunkedDecodeProgress{});
      }
      std::string_view size_text = wire_body.substr(state.cursor, line_end - state.cursor);
      if (const std::size_t semi = size_text.find(';'); semi != std::string_view::npos) {
        size_text = size_text.substr(0, semi);
      }
      auto parsed_chunk_size = parse_http_chunk_size_token(size_text);
      if (std::holds_alternative<typename Result<std::size_t, std::string>::Err>(parsed_chunk_size.data)) {
        return err_result<HttpChunkedDecodeProgress>(
            std::get<typename Result<std::size_t, std::string>::Err>(std::move(parsed_chunk_size.data)).value);
      }
      state.current_chunk_size =
          std::get<typename Result<std::size_t, std::string>::Ok>(std::move(parsed_chunk_size.data)).value;
      state.cursor = line_end + 2;
      if (state.current_chunk_size > max_body_bytes ||
          state.decoded_body.size() > max_body_bytes - state.current_chunk_size) {
        return err_result<HttpChunkedDecodeProgress>("HTTP response body exceeds configured limit");
      }
      state.phase = state.current_chunk_size == 0 ? HttpChunkedDecodePhase::Trailers
                                                  : HttpChunkedDecodePhase::Data;
      break;
    }
    case HttpChunkedDecodePhase::Data:
      if (wire_body.size() < state.cursor + state.current_chunk_size) {
        return ok_result(HttpChunkedDecodeProgress{});
      }
      state.decoded_body.append(wire_body.substr(state.cursor, state.current_chunk_size));
      state.cursor += state.current_chunk_size;
      state.phase = HttpChunkedDecodePhase::DataCrlf;
      break;
    case HttpChunkedDecodePhase::DataCrlf:
      if (wire_body.size() < state.cursor + 2) {
        return ok_result(HttpChunkedDecodeProgress{});
      }
      if (wire_body.substr(state.cursor, 2) != "\r\n") {
        return err_result<HttpChunkedDecodeProgress>("malformed chunked HTTP body");
      }
      state.cursor += 2;
      state.current_chunk_size = 0;
      state.phase = HttpChunkedDecodePhase::SizeLine;
      break;
    case HttpChunkedDecodePhase::Trailers: {
      const std::size_t trailer_end = wire_body.find("\r\n", state.cursor);
      if (trailer_end == std::string_view::npos) {
        return ok_result(HttpChunkedDecodeProgress{});
      }
      const std::string_view trailer = wire_body.substr(state.cursor, trailer_end - state.cursor);
      state.cursor = trailer_end + 2;
      if (trailer.empty()) {
        HttpChunkedDecodeProgress out;
        out.action = HttpChunkedDecodeAction::Complete;
        out.wire_size = state.cursor;
        return ok_result(out);
      }
      break;
    }
    }
  }
}

inline Result<std::optional<HttpChunkDecode>, std::string> try_decode_http_chunked_body(
    std::string_view wire_body,
    std::size_t max_body_bytes) {
  HttpChunkedDecodeState state;
  auto advanced = advance_http_chunked_body_decode(state, wire_body, max_body_bytes);
  if (std::holds_alternative<typename Result<HttpChunkedDecodeProgress, std::string>::Err>(advanced.data)) {
    return err_result<std::optional<HttpChunkDecode>>(
        std::get<typename Result<HttpChunkedDecodeProgress, std::string>::Err>(std::move(advanced.data)).value);
  }
  const auto progress =
      std::get<typename Result<HttpChunkedDecodeProgress, std::string>::Ok>(std::move(advanced.data)).value;
  if (progress.action != HttpChunkedDecodeAction::Complete) {
    return ok_result(std::optional<HttpChunkDecode>{});
  }
  return ok_result(std::optional<HttpChunkDecode>{HttpChunkDecode{std::move(state.decoded_body), progress.wire_size}});
}

inline Result<HttpClientResponse, std::string> build_http_client_response(
    HttpClientResponseHead head,
    std::string_view body,
    std::size_t max_body_bytes) {
  if (body.size() > max_body_bytes) {
    return err_result<HttpClientResponse>("HTTP response body exceeds configured limit");
  }
  return ok_result(HttpClientResponse{
      head.status,
      std::move(head.content_type),
      Bytes{std::string(body)},
      std::move(head.headers),
  });
}

inline Result<HttpClientResponse, std::string> build_http_client_response(
    HttpClientResponseHead head,
    std::string body,
    std::size_t max_body_bytes) {
  if (body.size() > max_body_bytes) {
    return err_result<HttpClientResponse>("HTTP response body exceeds configured limit");
  }
  return ok_result(HttpClientResponse{
      head.status,
      std::move(head.content_type),
      Bytes{std::move(body)},
      std::move(head.headers),
  });
}

inline Result<HttpClientResponseReadResult, std::string> try_read_http_client_response_from_buffer(
    const std::string& buffer,
    bool eof,
    std::size_t max_header_bytes,
    std::size_t max_body_bytes,
    const std::optional<HttpMethod>& request_method) {
  const std::size_t header_end = buffer.find("\r\n\r\n");
  if (header_end == std::string::npos) {
    if (buffer.size() > max_header_bytes) {
      return err_result<HttpClientResponseReadResult>("HTTP headers exceed configured limit");
    }
    if (eof) {
      return err_result<HttpClientResponseReadResult>("unexpected EOF while reading HTTP response");
    }
    return ok_result(HttpClientResponseReadResult{});
  }
  if (header_end + 4 > max_header_bytes) {
    return err_result<HttpClientResponseReadResult>("HTTP headers exceed configured limit");
  }

  const std::string_view buffer_view = buffer;
  const std::string_view response_head = buffer_view.substr(0, header_end);
  auto head = parse_http_client_response_head(response_head, max_body_bytes, request_method);
  if (std::holds_alternative<typename Result<HttpClientResponseHead, std::string>::Err>(head.data)) {
    return err_result<HttpClientResponseReadResult>(
        std::get<typename Result<HttpClientResponseHead, std::string>::Err>(std::move(head.data)).value);
  }
  HttpClientResponseHead parsed_head =
      std::get<typename Result<HttpClientResponseHead, std::string>::Ok>(std::move(head.data)).value;
  const std::size_t body_offset = header_end + 4;
  const std::string_view wire_body = buffer_view.substr(body_offset);

  if (parsed_head.status == 101) {
    return err_result<HttpClientResponseReadResult>("HTTP upgrade responses are unsupported");
  }
  if (parsed_head.body_mode == HttpClientBodyMode::NoBody) {
    if (http_response_status_is_informational(parsed_head.status)) {
      HttpClientResponseReadResult out;
      out.action = HttpClientResponseReadAction::SkipInterim;
      out.consume_prefix = body_offset;
      return ok_result(out);
    }
    auto response = build_http_client_response(std::move(parsed_head), std::string_view{}, max_body_bytes);
    if (std::holds_alternative<typename Result<HttpClientResponse, std::string>::Err>(response.data)) {
      return err_result<HttpClientResponseReadResult>(
          std::get<typename Result<HttpClientResponse, std::string>::Err>(std::move(response.data)).value);
    }
    HttpClientResponseReadResult out;
    out.action = HttpClientResponseReadAction::ReturnResponse;
    out.response = std::get<typename Result<HttpClientResponse, std::string>::Ok>(std::move(response.data)).value;
    return ok_result(out);
  }

  if (parsed_head.body_mode == HttpClientBodyMode::ContentLength) {
    const std::size_t expected_total = body_offset + parsed_head.content_length;
    if (buffer.size() >= expected_total) {
      const std::size_t content_length = parsed_head.content_length;
      auto response = build_http_client_response(std::move(parsed_head),
                                                 wire_body.substr(0, content_length),
                                                 max_body_bytes);
      if (std::holds_alternative<typename Result<HttpClientResponse, std::string>::Err>(response.data)) {
        return err_result<HttpClientResponseReadResult>(
            std::get<typename Result<HttpClientResponse, std::string>::Err>(std::move(response.data)).value);
      }
      HttpClientResponseReadResult out;
      out.action = HttpClientResponseReadAction::ReturnResponse;
      out.response = std::get<typename Result<HttpClientResponse, std::string>::Ok>(std::move(response.data)).value;
      return ok_result(out);
    }
    if (eof) {
      return err_result<HttpClientResponseReadResult>("unexpected EOF while reading HTTP response");
    }
    HttpClientResponseReadResult out;
    out.body_offset = body_offset;
    out.head = std::move(parsed_head);
    return ok_result(out);
  }

  if (parsed_head.body_mode == HttpClientBodyMode::Chunked) {
    auto decoded = try_decode_http_chunked_body(wire_body, max_body_bytes);
    if (std::holds_alternative<typename Result<std::optional<HttpChunkDecode>, std::string>::Err>(decoded.data)) {
      return err_result<HttpClientResponseReadResult>(
          std::get<typename Result<std::optional<HttpChunkDecode>, std::string>::Err>(std::move(decoded.data)).value);
    }
    const auto maybe_decoded =
        std::get<typename Result<std::optional<HttpChunkDecode>, std::string>::Ok>(std::move(decoded.data)).value;
    if (maybe_decoded.has_value()) {
      auto response =
          build_http_client_response(std::move(parsed_head), std::move(maybe_decoded->decoded_body), max_body_bytes);
      if (std::holds_alternative<typename Result<HttpClientResponse, std::string>::Err>(response.data)) {
        return err_result<HttpClientResponseReadResult>(
            std::get<typename Result<HttpClientResponse, std::string>::Err>(std::move(response.data)).value);
      }
      HttpClientResponseReadResult out;
      out.action = HttpClientResponseReadAction::ReturnResponse;
      out.response = std::get<typename Result<HttpClientResponse, std::string>::Ok>(std::move(response.data)).value;
      return ok_result(out);
    }
    if (eof) {
      return err_result<HttpClientResponseReadResult>("unexpected EOF while reading HTTP response");
    }
    HttpClientResponseReadResult out;
    out.body_offset = body_offset;
    out.head = std::move(parsed_head);
    return ok_result(out);
  }

  if (parsed_head.body_mode == HttpClientBodyMode::UntilEof) {
    if (wire_body.size() > max_body_bytes) {
      return err_result<HttpClientResponseReadResult>("HTTP response body exceeds configured limit");
    }
    if (!eof) {
      HttpClientResponseReadResult out;
      out.body_offset = body_offset;
      out.head = std::move(parsed_head);
      return ok_result(out);
    }
    auto response = build_http_client_response(std::move(parsed_head), wire_body, max_body_bytes);
    if (std::holds_alternative<typename Result<HttpClientResponse, std::string>::Err>(response.data)) {
      return err_result<HttpClientResponseReadResult>(
          std::get<typename Result<HttpClientResponse, std::string>::Err>(std::move(response.data)).value);
    }
    HttpClientResponseReadResult out;
    out.action = HttpClientResponseReadAction::ReturnResponse;
    out.response = std::get<typename Result<HttpClientResponse, std::string>::Ok>(std::move(response.data)).value;
    return ok_result(out);
  }

  return ok_result(HttpClientResponseReadResult{});
}

inline std::string build_http_response_message(const HttpResponse& response, bool close_connection) {
  std::ostringstream out;
  out << "HTTP/1.1 " << response.status << " " << http_reason_phrase(response.status) << "\r\n";
  out << "Content-Length: " << response.body.data.size() << "\r\n";
  if (!response.content_type.empty()) {
    out << "Content-Type: " << response.content_type << "\r\n";
  }
  out << response.headers;
  out << "Connection: " << (close_connection ? "close" : "keep-alive") << "\r\n\r\n";
  out << response.body.data;
  return out.str();
}

inline Future<Result<std::string, std::string>> read_http_message(const std::shared_ptr<SocketHandle>& handle,
                                                                  std::size_t max_header_bytes,
                                                                  std::size_t max_body_bytes) {
  if (!handle || !handle->valid()) co_return err_result<std::string>("stream is closed");
  std::string buffer;
  buffer.reserve(std::min<std::size_t>(max_header_bytes + max_body_bytes, 16384));
  std::optional<std::size_t> expected_total;

  while (true) {
    if (!expected_total.has_value()) {
      const std::size_t header_end = buffer.find("\r\n\r\n");
      if (header_end != std::string::npos) {
        if (header_end + 4 > max_header_bytes) {
          co_return err_result<std::string>("HTTP headers exceed configured limit");
        }
        auto framing = parse_http_request_framing(std::string_view(buffer).substr(0, header_end),
                                                 max_body_bytes);
        if (std::holds_alternative<typename Result<HttpRequestFraming, std::string>::Err>(framing.data)) {
          co_return err_result<std::string>(
              std::get<typename Result<HttpRequestFraming, std::string>::Err>(std::move(framing.data)).value);
        }
        const auto parsed_framing =
            std::get<typename Result<HttpRequestFraming, std::string>::Ok>(std::move(framing.data)).value;
        expected_total = header_end + 4 + parsed_framing.content_length;
        if (buffer.size() >= *expected_total) {
          if (buffer.size() > *expected_total) {
            co_return err_result<std::string>("unexpected extra bytes after HTTP request body");
          }
          co_return ok_result(std::move(buffer));
        }
      } else if (buffer.size() > max_header_bytes) {
        co_return err_result<std::string>("HTTP headers exceed configured limit");
      }
    } else if (buffer.size() >= *expected_total) {
      if (buffer.size() > *expected_total) {
        co_return err_result<std::string>("unexpected extra bytes after HTTP request body");
      }
      co_return ok_result(std::move(buffer));
    }

    char chunk[4096];
#if defined(_WIN32)
    const int rc = recv(handle->fd, chunk, static_cast<int>(sizeof(chunk)), 0);
#else
    const ssize_t rc = recv(handle->fd, chunk, sizeof(chunk), 0);
#endif
    if (rc > 0) {
      buffer.append(chunk, static_cast<std::size_t>(rc));
      continue;
    }
    if (rc == 0) {
      co_return err_result<std::string>("unexpected EOF while reading HTTP message");
    }
    const int err = last_socket_error_code();
    if (socket_error_is_interrupted(err)) continue;
    if (!socket_error_is_would_block(err)) {
      co_return err_result<std::string>("HTTP read failed: " + socket_error_message(err));
    }
    co_await IoAwaitable{handle->fd, Reactor::Interest::Read};
  }
}

inline Future<Result<HttpRequest, std::string>> http_read_request(TcpStream stream,
                                                                  std::int64_t max_header_bytes,
                                                                  std::int64_t max_body_bytes) {
  std::string error;
  auto handle = require_open_handle(stream.handle, "stream", error);
  if (!handle) co_return err_result<HttpRequest>(std::move(error));
  if (max_header_bytes <= 0 || max_body_bytes < 0) {
    co_return err_result<HttpRequest>("HTTP limits must be positive");
  }
  auto message = co_await read_http_message(handle,
                                            static_cast<std::size_t>(max_header_bytes),
                                            static_cast<std::size_t>(max_body_bytes));
  if (std::holds_alternative<typename Result<std::string, std::string>::Err>(message.data)) {
    co_return err_result<HttpRequest>(
        std::get<typename Result<std::string, std::string>::Err>(std::move(message.data)).value);
  }
  co_return parse_http_request_message(
      std::get<typename Result<std::string, std::string>::Ok>(std::move(message.data)).value,
      static_cast<std::size_t>(max_body_bytes));
}

inline Future<Result<void, std::string>> http_write_response(TcpStream stream, HttpResponse response) {
  const std::string encoded = build_http_response_message(response, true);
  co_return co_await write_stream_all(stream, Bytes{encoded});
}

inline Future<Result<void, std::string>> http_write_response_with_connection(TcpStream stream,
                                                                             HttpResponse response,
                                                                             bool close_connection) {
  const std::string encoded = build_http_response_message(response, close_connection);
  co_return co_await write_stream_all(stream, Bytes{encoded});
}

inline Future<Result<void, std::string>> http_write_request(TcpStream stream, HttpClientRequest request) {
  auto encoded = build_http_request_message(request);
  if (std::holds_alternative<typename Result<std::string, std::string>::Err>(encoded.data)) {
    co_return err_void_result(
        std::get<typename Result<std::string, std::string>::Err>(std::move(encoded.data)).value);
  }
  co_return co_await write_stream_all(
      stream,
      Bytes{std::get<typename Result<std::string, std::string>::Ok>(std::move(encoded.data)).value});
}

inline Future<Result<HttpClientResponse, std::string>> http_read_response_with_context(
    TcpStream stream,
    const std::optional<HttpMethod>& request_method,
    std::int64_t max_header_bytes,
    std::int64_t max_body_bytes) {
  std::string error;
  auto handle = require_open_handle(stream.handle, "stream", error);
  if (!handle) co_return err_result<HttpClientResponse>(std::move(error));
  if (max_header_bytes <= 0 || max_body_bytes < 0) {
    co_return err_result<HttpClientResponse>("HTTP limits must be positive");
  }

  std::string buffer;
  buffer.reserve(std::min<std::size_t>(static_cast<std::size_t>(max_header_bytes) +
                                           static_cast<std::size_t>(max_body_bytes),
                                       16384));
  std::optional<HttpClientResponseHead> active_head;
  std::size_t active_body_offset = 0;
  std::optional<HttpChunkedDecodeState> active_chunked_state;

  while (true) {
    if (active_head.has_value()) {
      const std::string_view wire_body = std::string_view(buffer).substr(active_body_offset);
      if (active_head->body_mode == HttpClientBodyMode::ContentLength) {
        const std::size_t content_length = active_head->content_length;
        if (buffer.size() >= active_body_offset + content_length) {
          auto response =
              build_http_client_response(std::move(*active_head), wire_body.substr(0, content_length),
                                         static_cast<std::size_t>(max_body_bytes));
          if (std::holds_alternative<typename Result<HttpClientResponse, std::string>::Err>(response.data)) {
            co_return err_result<HttpClientResponse>(
                std::get<typename Result<HttpClientResponse, std::string>::Err>(std::move(response.data)).value);
          }
          co_return ok_result(
              std::get<typename Result<HttpClientResponse, std::string>::Ok>(std::move(response.data)).value);
        }
      } else if (active_head->body_mode == HttpClientBodyMode::Chunked) {
        if (!active_chunked_state.has_value()) active_chunked_state.emplace();
        auto advanced = advance_http_chunked_body_decode(*active_chunked_state,
                                                         wire_body,
                                                         static_cast<std::size_t>(max_body_bytes));
        if (std::holds_alternative<typename Result<HttpChunkedDecodeProgress, std::string>::Err>(advanced.data)) {
          co_return err_result<HttpClientResponse>(
              std::get<typename Result<HttpChunkedDecodeProgress, std::string>::Err>(std::move(advanced.data)).value);
        }
        const auto progress =
            std::get<typename Result<HttpChunkedDecodeProgress, std::string>::Ok>(std::move(advanced.data)).value;
        if (progress.action == HttpChunkedDecodeAction::Complete) {
          auto response = build_http_client_response(std::move(*active_head),
                                                     std::move(active_chunked_state->decoded_body),
                                                     static_cast<std::size_t>(max_body_bytes));
          if (std::holds_alternative<typename Result<HttpClientResponse, std::string>::Err>(response.data)) {
            co_return err_result<HttpClientResponse>(
                std::get<typename Result<HttpClientResponse, std::string>::Err>(std::move(response.data)).value);
          }
          co_return ok_result(
              std::get<typename Result<HttpClientResponse, std::string>::Ok>(std::move(response.data)).value);
        }
      }
    } else {
      auto parsed = try_read_http_client_response_from_buffer(buffer,
                                                              false,
                                                              static_cast<std::size_t>(max_header_bytes),
                                                              static_cast<std::size_t>(max_body_bytes),
                                                              request_method);
      if (std::holds_alternative<typename Result<HttpClientResponseReadResult, std::string>::Err>(parsed.data)) {
        co_return err_result<HttpClientResponse>(
            std::get<typename Result<HttpClientResponseReadResult, std::string>::Err>(std::move(parsed.data)).value);
      }
      auto parsed_result =
          std::get<typename Result<HttpClientResponseReadResult, std::string>::Ok>(std::move(parsed.data)).value;
      if (parsed_result.action == HttpClientResponseReadAction::ReturnResponse) {
        co_return ok_result(std::move(parsed_result.response));
      }
      if (parsed_result.action == HttpClientResponseReadAction::SkipInterim) {
        buffer.erase(0, parsed_result.consume_prefix);
        active_head.reset();
        active_chunked_state.reset();
        active_body_offset = 0;
        continue;
      }
      if (parsed_result.head.has_value()) {
        active_body_offset = parsed_result.body_offset;
        active_chunked_state.reset();
        if (parsed_result.head->body_mode == HttpClientBodyMode::Chunked) {
          active_chunked_state.emplace();
        }
        active_head = std::move(parsed_result.head);
        continue;
      }
    }

    char chunk[4096];
#if defined(_WIN32)
    const int rc = recv(handle->fd, chunk, static_cast<int>(sizeof(chunk)), 0);
#else
    const ssize_t rc = recv(handle->fd, chunk, sizeof(chunk), 0);
#endif
    if (rc > 0) {
      buffer.append(chunk, static_cast<std::size_t>(rc));
      continue;
    }
    if (rc == 0) {
      if (active_head.has_value()) {
        const std::string_view wire_body = std::string_view(buffer).substr(active_body_offset);
        if (active_head->body_mode == HttpClientBodyMode::UntilEof) {
          auto response =
              build_http_client_response(std::move(*active_head), wire_body,
                                         static_cast<std::size_t>(max_body_bytes));
          if (std::holds_alternative<typename Result<HttpClientResponse, std::string>::Err>(response.data)) {
            co_return err_result<HttpClientResponse>(
                std::get<typename Result<HttpClientResponse, std::string>::Err>(std::move(response.data)).value);
          }
          co_return ok_result(
              std::get<typename Result<HttpClientResponse, std::string>::Ok>(std::move(response.data)).value);
        }
        if (active_head->body_mode == HttpClientBodyMode::Chunked) {
          if (!active_chunked_state.has_value()) active_chunked_state.emplace();
          auto advanced = advance_http_chunked_body_decode(*active_chunked_state,
                                                           wire_body,
                                                           static_cast<std::size_t>(max_body_bytes));
          if (std::holds_alternative<typename Result<HttpChunkedDecodeProgress, std::string>::Err>(advanced.data)) {
            co_return err_result<HttpClientResponse>(
                std::get<typename Result<HttpChunkedDecodeProgress, std::string>::Err>(std::move(advanced.data)).value);
          }
          const auto progress =
              std::get<typename Result<HttpChunkedDecodeProgress, std::string>::Ok>(std::move(advanced.data)).value;
          if (progress.action == HttpChunkedDecodeAction::Complete) {
            auto response = build_http_client_response(std::move(*active_head),
                                                       std::move(active_chunked_state->decoded_body),
                                                       static_cast<std::size_t>(max_body_bytes));
            if (std::holds_alternative<typename Result<HttpClientResponse, std::string>::Err>(response.data)) {
              co_return err_result<HttpClientResponse>(
                  std::get<typename Result<HttpClientResponse, std::string>::Err>(std::move(response.data)).value);
            }
            co_return ok_result(
                std::get<typename Result<HttpClientResponse, std::string>::Ok>(std::move(response.data)).value);
          }
        }
        co_return err_result<HttpClientResponse>("unexpected EOF while reading HTTP response");
      }
      while (true) {
        auto final = try_read_http_client_response_from_buffer(buffer,
                                                               true,
                                                               static_cast<std::size_t>(max_header_bytes),
                                                               static_cast<std::size_t>(max_body_bytes),
                                                               request_method);
        if (std::holds_alternative<typename Result<HttpClientResponseReadResult, std::string>::Err>(final.data)) {
          co_return err_result<HttpClientResponse>(
              std::get<typename Result<HttpClientResponseReadResult, std::string>::Err>(std::move(final.data)).value);
        }
        auto final_result =
            std::get<typename Result<HttpClientResponseReadResult, std::string>::Ok>(std::move(final.data)).value;
        if (final_result.action == HttpClientResponseReadAction::ReturnResponse) {
          co_return ok_result(std::move(final_result.response));
        }
        if (final_result.action == HttpClientResponseReadAction::SkipInterim) {
          buffer.erase(0, final_result.consume_prefix);
          if (buffer.empty()) {
            co_return err_result<HttpClientResponse>("unexpected EOF after interim HTTP response");
          }
          continue;
        }
        co_return err_result<HttpClientResponse>("unexpected EOF while reading HTTP response");
      }
    }
    const int err = last_socket_error_code();
    if (socket_error_is_interrupted(err)) continue;
    if (!socket_error_is_would_block(err)) {
      co_return err_result<HttpClientResponse>("HTTP read failed: " + socket_error_message(err));
    }
    co_await IoAwaitable{handle->fd, Reactor::Interest::Read};
  }
}

inline Future<Result<HttpClientResponse, std::string>> http_read_response(TcpStream stream,
                                                                          std::int64_t max_header_bytes,
                                                                          std::int64_t max_body_bytes) {
  co_return co_await http_read_response_with_context(std::move(stream), std::nullopt, max_header_bytes, max_body_bytes);
}

inline Future<Result<HttpClientResponse, std::string>> http_read_response_for(TcpStream stream,
                                                                              HttpMethod request_method,
                                                                              std::int64_t max_header_bytes,
                                                                              std::int64_t max_body_bytes) {
  co_return co_await http_read_response_with_context(std::move(stream),
                                                     std::optional<HttpMethod>{std::move(request_method)},
                                                     max_header_bytes,
                                                     max_body_bytes);
}

inline Result<std::string, std::string> http_response_header(const HttpClientResponse& response,
                                                             std::string_view name) {
  const auto value = parse_http_header_value(response.headers, name);
  if (!value.has_value()) {
    return err_result<std::string>("HTTP header not found: " + std::string(name));
  }
  return ok_result(*value);
}

template <typename T>
decltype(auto) access_object(T&& value) {
  return std::forward<T>(value);
}

template <typename T>
T& access_object(T* value) {
  return *value;
}

template <typename T>
const T& access_object(const T* value) {
  return *value;
}

template <typename T>
T& access_object(std::unique_ptr<T>& value) {
  return *value;
}

template <typename T>
const T& access_object(const std::unique_ptr<T>& value) {
  return *value;
}

template <typename T>
T& access_object(std::unique_ptr<T>&& value) {
  return *value;
}

template <typename T>
T& access_object(std::shared_ptr<T>& value) {
  return *value;
}

template <typename T>
const T& access_object(const std::shared_ptr<T>& value) {
  return *value;
}

template <typename T>
T& access_object(std::shared_ptr<T>&& value) {
  return *value;
}

template <typename T, typename F>
auto project_value(T&& value, F&& projector) {
  auto&& object = access_object(std::forward<T>(value));
  return std::forward<F>(projector)(object);
}

template <typename T, typename F>
decltype(auto) project_value_ref(T&& value, F&& projector) {
  auto&& object = access_object(std::forward<T>(value));
  return std::forward<F>(projector)(object);
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

inline nebula::rt::Future<void> __nebula_rt_task_yield_now() {
  return nebula::rt::yield_now();
}

inline nebula::rt::Duration __nebula_rt_time_millis(std::int64_t value) {
  return nebula::rt::make_duration_millis(value);
}

inline nebula::rt::Future<void> __nebula_rt_time_sleep(nebula::rt::Duration delay) {
  return nebula::rt::sleep(delay);
}

inline std::int64_t __nebula_rt_time_unix_millis() {
  return nebula::rt::unix_millis();
}

inline std::int64_t __nebula_rt_time_steady_millis() {
  return nebula::rt::steady_millis();
}

inline void __nebula_rt_log_info(std::string msg) {
  nebula::rt::info(msg);
}

inline void __nebula_rt_log_error(std::string msg) {
  nebula::rt::error(msg);
}

inline nebula::rt::Bytes __nebula_rt_bytes_from_string(std::string s) {
  return nebula::rt::bytes_from_string(std::move(s));
}

inline std::string __nebula_rt_bytes_to_string(nebula::rt::Bytes b) {
  return nebula::rt::bytes_to_string(std::move(b));
}

inline std::int64_t __nebula_rt_bytes_len(nebula::rt::Bytes b) {
  return nebula::rt::bytes_len(b);
}

inline std::int64_t __nebula_rt_string_len(std::string value) {
  return static_cast<std::int64_t>(value.size());
}

inline bool __nebula_rt_bytes_is_empty(nebula::rt::Bytes b) {
  return nebula::rt::bytes_is_empty(b);
}

inline nebula::rt::Bytes __nebula_rt_bytes_concat(nebula::rt::Bytes lhs, nebula::rt::Bytes rhs) {
  return nebula::rt::bytes_concat(std::move(lhs), std::move(rhs));
}

inline bool __nebula_rt_bytes_equal(nebula::rt::Bytes lhs, nebula::rt::Bytes rhs) {
  return nebula::rt::bytes_equal(lhs, rhs);
}

inline nebula::rt::Result<nebula::rt::JsonValue, std::string>
__nebula_rt_json_parse_bytes(nebula::rt::Bytes body) {
  return nebula::rt::json_parse_bytes(std::move(body));
}

inline nebula::rt::Result<nebula::rt::JsonValue, std::string> __nebula_rt_json_parse(std::string text) {
  return nebula::rt::json_parse(std::move(text));
}

inline std::string __nebula_rt_json_stringify(nebula::rt::JsonValue value) {
  return nebula::rt::json_stringify(std::move(value));
}

inline nebula::rt::JsonValue __nebula_rt_json_string_value(std::string value) {
  return nebula::rt::json_string_value(std::move(value));
}

inline nebula::rt::JsonValue __nebula_rt_json_int_value(std::int64_t value) {
  return nebula::rt::json_int_value(value);
}

inline nebula::rt::JsonValue __nebula_rt_json_bool_value(bool value) {
  return nebula::rt::json_bool_value(value);
}

inline nebula::rt::JsonValue __nebula_rt_json_null_value() {
  return nebula::rt::json_null_value();
}

inline nebula::rt::JsonValue __nebula_rt_json_object1(std::string key1, nebula::rt::JsonValue value1) {
  return nebula::rt::json_object1(std::move(key1), std::move(value1));
}

inline nebula::rt::JsonValue __nebula_rt_json_object2(std::string key1,
                                                      nebula::rt::JsonValue value1,
                                                      std::string key2,
                                                      nebula::rt::JsonValue value2) {
  return nebula::rt::json_object2(std::move(key1), std::move(value1), std::move(key2), std::move(value2));
}

inline nebula::rt::JsonValue __nebula_rt_json_object3(std::string key1,
                                                      nebula::rt::JsonValue value1,
                                                      std::string key2,
                                                      nebula::rt::JsonValue value2,
                                                      std::string key3,
                                                      nebula::rt::JsonValue value3) {
  return nebula::rt::json_object3(std::move(key1), std::move(value1),
                                  std::move(key2), std::move(value2),
                                  std::move(key3), std::move(value3));
}

inline nebula::rt::JsonValue __nebula_rt_json_object4(std::string key1,
                                                      nebula::rt::JsonValue value1,
                                                      std::string key2,
                                                      nebula::rt::JsonValue value2,
                                                      std::string key3,
                                                      nebula::rt::JsonValue value3,
                                                      std::string key4,
                                                      nebula::rt::JsonValue value4) {
  return nebula::rt::json_object4(std::move(key1), std::move(value1),
                                  std::move(key2), std::move(value2),
                                  std::move(key3), std::move(value3),
                                  std::move(key4), std::move(value4));
}

inline std::string __nebula_rt_json_object4_text(std::string key1,
                                                 nebula::rt::JsonValue value1,
                                                 std::string key2,
                                                 nebula::rt::JsonValue value2,
                                                 std::string key3,
                                                 nebula::rt::JsonValue value3,
                                                 std::string key4,
                                                 nebula::rt::JsonValue value4) {
  return nebula::rt::json_object4_text(std::move(key1), std::move(value1),
                                       std::move(key2), std::move(value2),
                                       std::move(key3), std::move(value3),
                                       std::move(key4), std::move(value4));
}

inline nebula::rt::JsonValue __nebula_rt_json_object5(std::string key1,
                                                      nebula::rt::JsonValue value1,
                                                      std::string key2,
                                                      nebula::rt::JsonValue value2,
                                                      std::string key3,
                                                      nebula::rt::JsonValue value3,
                                                      std::string key4,
                                                      nebula::rt::JsonValue value4,
                                                      std::string key5,
                                                      nebula::rt::JsonValue value5) {
  return nebula::rt::json_object5(std::move(key1), std::move(value1),
                                  std::move(key2), std::move(value2),
                                  std::move(key3), std::move(value3),
                                  std::move(key4), std::move(value4),
                                  std::move(key5), std::move(value5));
}

inline nebula::rt::JsonValue __nebula_rt_json_object6(std::string key1,
                                                      nebula::rt::JsonValue value1,
                                                      std::string key2,
                                                      nebula::rt::JsonValue value2,
                                                      std::string key3,
                                                      nebula::rt::JsonValue value3,
                                                      std::string key4,
                                                      nebula::rt::JsonValue value4,
                                                      std::string key5,
                                                      nebula::rt::JsonValue value5,
                                                      std::string key6,
                                                      nebula::rt::JsonValue value6) {
  return nebula::rt::json_object6(std::move(key1), std::move(value1),
                                  std::move(key2), std::move(value2),
                                  std::move(key3), std::move(value3),
                                  std::move(key4), std::move(value4),
                                  std::move(key5), std::move(value5),
                                  std::move(key6), std::move(value6));
}

inline nebula::rt::JsonValue __nebula_rt_json_object7(std::string key1,
                                                      nebula::rt::JsonValue value1,
                                                      std::string key2,
                                                      nebula::rt::JsonValue value2,
                                                      std::string key3,
                                                      nebula::rt::JsonValue value3,
                                                      std::string key4,
                                                      nebula::rt::JsonValue value4,
                                                      std::string key5,
                                                      nebula::rt::JsonValue value5,
                                                      std::string key6,
                                                      nebula::rt::JsonValue value6,
                                                      std::string key7,
                                                      nebula::rt::JsonValue value7) {
  return nebula::rt::json_object7(std::move(key1), std::move(value1),
                                  std::move(key2), std::move(value2),
                                  std::move(key3), std::move(value3),
                                  std::move(key4), std::move(value4),
                                  std::move(key5), std::move(value5),
                                  std::move(key6), std::move(value6),
                                  std::move(key7), std::move(value7));
}

inline nebula::rt::JsonValue __nebula_rt_json_object8(std::string key1,
                                                      nebula::rt::JsonValue value1,
                                                      std::string key2,
                                                      nebula::rt::JsonValue value2,
                                                      std::string key3,
                                                      nebula::rt::JsonValue value3,
                                                      std::string key4,
                                                      nebula::rt::JsonValue value4,
                                                      std::string key5,
                                                      nebula::rt::JsonValue value5,
                                                      std::string key6,
                                                      nebula::rt::JsonValue value6,
                                                      std::string key7,
                                                      nebula::rt::JsonValue value7,
                                                      std::string key8,
                                                      nebula::rt::JsonValue value8) {
  return nebula::rt::json_object8(std::move(key1), std::move(value1),
                                  std::move(key2), std::move(value2),
                                  std::move(key3), std::move(value3),
                                  std::move(key4), std::move(value4),
                                  std::move(key5), std::move(value5),
                                  std::move(key6), std::move(value6),
                                  std::move(key7), std::move(value7),
                                  std::move(key8), std::move(value8));
}

inline nebula::rt::JsonValue __nebula_rt_json_object9(std::string key1,
                                                      nebula::rt::JsonValue value1,
                                                      std::string key2,
                                                      nebula::rt::JsonValue value2,
                                                      std::string key3,
                                                      nebula::rt::JsonValue value3,
                                                      std::string key4,
                                                      nebula::rt::JsonValue value4,
                                                      std::string key5,
                                                      nebula::rt::JsonValue value5,
                                                      std::string key6,
                                                      nebula::rt::JsonValue value6,
                                                      std::string key7,
                                                      nebula::rt::JsonValue value7,
                                                      std::string key8,
                                                      nebula::rt::JsonValue value8,
                                                      std::string key9,
                                                      nebula::rt::JsonValue value9) {
  return nebula::rt::json_object9(std::move(key1), std::move(value1),
                                  std::move(key2), std::move(value2),
                                  std::move(key3), std::move(value3),
                                  std::move(key4), std::move(value4),
                                  std::move(key5), std::move(value5),
                                  std::move(key6), std::move(value6),
                                  std::move(key7), std::move(value7),
                                  std::move(key8), std::move(value8),
                                  std::move(key9), std::move(value9));
}

inline nebula::rt::JsonArrayBuilder __nebula_rt_json_array_builder() {
  return nebula::rt::json_array_builder();
}

inline nebula::rt::JsonArrayBuilder __nebula_rt_json_array_push(nebula::rt::JsonArrayBuilder builder,
                                                                nebula::rt::JsonValue value) {
  return nebula::rt::json_array_push(std::move(builder), std::move(value));
}

inline nebula::rt::JsonValue __nebula_rt_json_array0() {
  return nebula::rt::json_array0();
}

inline nebula::rt::JsonValue __nebula_rt_json_array1(nebula::rt::JsonValue value1) {
  return nebula::rt::json_array1(std::move(value1));
}

inline nebula::rt::JsonValue __nebula_rt_json_array2(nebula::rt::JsonValue value1,
                                                     nebula::rt::JsonValue value2) {
  return nebula::rt::json_array2(std::move(value1), std::move(value2));
}

inline nebula::rt::JsonValue __nebula_rt_json_array3(nebula::rt::JsonValue value1,
                                                     nebula::rt::JsonValue value2,
                                                     nebula::rt::JsonValue value3) {
  return nebula::rt::json_array3(std::move(value1), std::move(value2), std::move(value3));
}

inline nebula::rt::JsonValue __nebula_rt_json_array_build(nebula::rt::JsonArrayBuilder builder) {
  return nebula::rt::json_array_build(std::move(builder));
}

inline bool __nebula_rt_env_has(std::string name) {
  return nebula::rt::env_has(name);
}

inline nebula::rt::Result<std::string, std::string> __nebula_rt_env_get(std::string name) {
  return nebula::rt::env_get(name);
}

inline std::string __nebula_rt_env_or(std::string name, std::string fallback) {
  return nebula::rt::env_or(std::move(name), std::move(fallback));
}

inline nebula::rt::Result<std::int64_t, std::string> __nebula_rt_env_get_int(std::string name) {
  return nebula::rt::env_get_int(name);
}

inline nebula::rt::Result<std::int64_t, std::string> __nebula_rt_env_get_int_or(std::string name,
                                                                                 std::int64_t fallback) {
  return nebula::rt::env_get_int_or(name, fallback);
}

inline nebula::rt::Result<bool, std::string> __nebula_rt_fs_exists(std::string path) {
  return nebula::rt::fs_exists(path);
}

inline nebula::rt::Result<bool, std::string> __nebula_rt_fs_is_file(std::string path) {
  return nebula::rt::fs_is_file(path);
}

inline nebula::rt::Result<bool, std::string> __nebula_rt_fs_is_dir(std::string path) {
  return nebula::rt::fs_is_dir(path);
}

inline nebula::rt::Result<nebula::rt::Bytes, std::string> __nebula_rt_fs_read_bytes(std::string path) {
  return nebula::rt::fs_read_bytes(path);
}

inline nebula::rt::Result<std::string, std::string> __nebula_rt_fs_read_string(std::string path) {
  return nebula::rt::fs_read_string(path);
}

inline nebula::rt::Result<void, std::string> __nebula_rt_fs_write_bytes(std::string path,
                                                                         nebula::rt::Bytes data) {
  return nebula::rt::fs_write_bytes(path, std::move(data));
}

inline nebula::rt::Result<void, std::string> __nebula_rt_fs_write_string(std::string path,
                                                                          std::string text) {
  return nebula::rt::fs_write_string(path, std::move(text));
}

inline nebula::rt::Result<void, std::string> __nebula_rt_fs_create_dir_all(std::string path) {
  return nebula::rt::fs_create_dir_all(path);
}

inline nebula::rt::Result<nebula::rt::JsonValue, std::string> __nebula_rt_fs_list_dir(std::string path) {
  return nebula::rt::fs_list_dir(path);
}

inline nebula::rt::Result<void, std::string> __nebula_rt_fs_remove_file(std::string path) {
  return nebula::rt::fs_remove_file(path);
}

inline nebula::rt::Result<void, std::string> __nebula_rt_fs_write_bytes_atomic(std::string path,
                                                                                nebula::rt::Bytes data) {
  return nebula::rt::fs_write_bytes_atomic(path, std::move(data));
}

inline nebula::rt::Result<void, std::string> __nebula_rt_fs_write_string_atomic(std::string path,
                                                                                 std::string text) {
  return nebula::rt::fs_write_string_atomic(path, std::move(text));
}

inline nebula::rt::Result<nebula::rt::ProcessOutput, std::string>
__nebula_rt_process_run(nebula::rt::ProcessCommand command) {
  return nebula::rt::process_run(std::move(command));
}

inline nebula::rt::Result<std::string, std::string> __nebula_rt_json_get_string(nebula::rt::JsonValue value,
                                                                                 std::string key) {
  return nebula::rt::json_get_string(value, key);
}

inline nebula::rt::Result<std::int64_t, std::string> __nebula_rt_json_get_int(nebula::rt::JsonValue value,
                                                                               std::string key) {
  return nebula::rt::json_get_int(value, key);
}

inline nebula::rt::Result<bool, std::string> __nebula_rt_json_get_bool(nebula::rt::JsonValue value,
                                                                        std::string key) {
  return nebula::rt::json_get_bool(value, key);
}

inline nebula::rt::Result<nebula::rt::JsonValue, std::string> __nebula_rt_json_get_value(nebula::rt::JsonValue value,
                                                                                           std::string key) {
  return nebula::rt::json_get_value(value, key);
}

inline nebula::rt::Result<std::string, std::string> __nebula_rt_json_as_string(nebula::rt::JsonValue value) {
  return nebula::rt::json_as_string(value);
}

inline nebula::rt::Result<std::int64_t, std::string> __nebula_rt_json_as_int(nebula::rt::JsonValue value) {
  return nebula::rt::json_as_int(value);
}

inline nebula::rt::Result<bool, std::string> __nebula_rt_json_as_bool(nebula::rt::JsonValue value) {
  return nebula::rt::json_as_bool(value);
}

inline nebula::rt::Result<std::int64_t, std::string> __nebula_rt_json_array_len(nebula::rt::JsonValue value) {
  return nebula::rt::json_array_len(value);
}

inline nebula::rt::Result<nebula::rt::JsonValue, std::string> __nebula_rt_json_array_get(nebula::rt::JsonValue value,
                                                                                           std::int64_t index) {
  return nebula::rt::json_array_get(value, index);
}

inline nebula::rt::Result<nebula::rt::SocketAddr, std::string> __nebula_rt_net_ipv4(std::string host,
                                                                                      std::int64_t port) {
  return nebula::rt::ipv4(host, port);
}

inline nebula::rt::Future<nebula::rt::Result<nebula::rt::SocketAddr, std::string>>
__nebula_rt_net_resolve_ipv4(std::string host, std::int64_t port) {
  return nebula::rt::resolve_socketaddr(std::move(host), port);
}

inline nebula::rt::Future<nebula::rt::Result<nebula::rt::TcpListener, std::string>>
__nebula_rt_net_bind(nebula::rt::SocketAddr addr) {
  return nebula::rt::bind_listener(addr);
}

inline nebula::rt::Future<nebula::rt::Result<nebula::rt::TcpStream, std::string>>
__nebula_rt_net_connect(nebula::rt::SocketAddr addr) {
  return nebula::rt::connect_stream(addr);
}

inline nebula::rt::Future<nebula::rt::Result<nebula::rt::TcpStream, std::string>>
__nebula_rt_net_connect_host(std::string host, std::int64_t port) {
  return nebula::rt::connect_stream_host(std::move(host), port);
}

inline nebula::rt::Future<nebula::rt::Result<nebula::rt::TcpStream, std::string>>
__nebula_rt_net_listener_accept(nebula::rt::TcpListener listener) {
  return nebula::rt::accept_stream(listener);
}

inline nebula::rt::Future<nebula::rt::Result<nebula::rt::TcpStream, std::string>>
__nebula_rt_net_listener_accept_timeout(nebula::rt::TcpListener listener, std::int64_t timeout_ms) {
  return nebula::rt::accept_stream_timeout(listener, timeout_ms);
}

inline nebula::rt::Result<nebula::rt::SocketAddr, std::string>
__nebula_rt_net_listener_local_addr(nebula::rt::TcpListener listener) {
  return nebula::rt::listener_local_addr(listener);
}

inline nebula::rt::Result<void, std::string> __nebula_rt_net_listener_close(nebula::rt::TcpListener listener) {
  return nebula::rt::listener_close(listener);
}

inline nebula::rt::Future<nebula::rt::Result<nebula::rt::Bytes, std::string>>
__nebula_rt_net_stream_read(nebula::rt::TcpStream stream, std::int64_t max_bytes) {
  return nebula::rt::read_stream(stream, max_bytes);
}

inline nebula::rt::Future<nebula::rt::Result<std::int64_t, std::string>>
__nebula_rt_net_stream_write(nebula::rt::TcpStream stream, nebula::rt::Bytes bytes) {
  return nebula::rt::write_stream(stream, std::move(bytes));
}

inline nebula::rt::Future<nebula::rt::Result<void, std::string>>
__nebula_rt_net_stream_write_all(nebula::rt::TcpStream stream, nebula::rt::Bytes bytes) {
  return nebula::rt::write_stream_all(stream, std::move(bytes));
}

inline nebula::rt::Result<nebula::rt::SocketAddr, std::string>
__nebula_rt_net_stream_peer_addr(nebula::rt::TcpStream stream) {
  return nebula::rt::stream_peer_addr(stream);
}

inline nebula::rt::Result<nebula::rt::SocketAddr, std::string>
__nebula_rt_net_stream_local_addr(nebula::rt::TcpStream stream) {
  return nebula::rt::stream_local_addr(stream);
}

inline nebula::rt::Result<std::string, std::string> __nebula_rt_net_socket_addr_host(nebula::rt::SocketAddr addr) {
  return nebula::rt::socket_addr_host(std::move(addr));
}

inline nebula::rt::Result<std::int64_t, std::string> __nebula_rt_net_socket_addr_port(nebula::rt::SocketAddr addr) {
  return nebula::rt::socket_addr_port_value(std::move(addr));
}

inline nebula::rt::Result<void, std::string> __nebula_rt_net_stream_close(nebula::rt::TcpStream stream) {
  return nebula::rt::stream_close(stream);
}

inline nebula::rt::Future<nebula::rt::Result<nebula::rt::HttpRequest, std::string>>
__nebula_rt_http_read_request(nebula::rt::TcpStream stream,
                              std::int64_t max_header_bytes,
                              std::int64_t max_body_bytes) {
  return nebula::rt::http_read_request(stream, max_header_bytes, max_body_bytes);
}

inline nebula::rt::Result<std::string, std::string> __nebula_rt_http_header(nebula::rt::HttpRequest request,
                                                                             std::string name) {
  return nebula::rt::http_header(std::move(request), std::move(name));
}

inline nebula::rt::Result<std::string, std::string> __nebula_rt_http_header_unique(nebula::rt::HttpRequest request,
                                                                                    std::string name) {
  return nebula::rt::http_unique_header(std::move(request), std::move(name));
}

inline nebula::rt::Result<std::string, std::string> __nebula_rt_http_content_type(nebula::rt::HttpRequest request) {
  return nebula::rt::http_content_type(std::move(request));
}

inline bool __nebula_rt_http_request_close_connection(nebula::rt::HttpRequest request) {
  return nebula::rt::http_request_close_connection(std::move(request));
}

inline bool __nebula_rt_http_path_matches(std::string pattern, std::string path) {
  return nebula::rt::http_path_matches(pattern, path);
}

inline nebula::rt::Result<std::string, std::string> __nebula_rt_http_route_param1(std::string pattern,
                                                                                   std::string path) {
  return nebula::rt::http_route_param1(std::move(pattern), std::move(path));
}

inline nebula::rt::Result<nebula::rt::HttpRouteParams2, std::string> __nebula_rt_http_route_params2(std::string pattern,
                                                                                                      std::string path) {
  return nebula::rt::http_route_params2(std::move(pattern), std::move(path));
}

inline nebula::rt::Result<nebula::rt::HttpRouteParams3, std::string> __nebula_rt_http_route_params3(std::string pattern,
                                                                                                      std::string path) {
  return nebula::rt::http_route_params3(std::move(pattern), std::move(path));
}

inline nebula::rt::Result<nebula::rt::HttpRoutePattern, std::string> __nebula_rt_http_compile_route(std::string pattern) {
  return nebula::rt::http_compile_route(std::move(pattern));
}

inline bool __nebula_rt_http_compiled_path_matches(nebula::rt::HttpRoutePattern route, std::string path) {
  return nebula::rt::http_compiled_path_matches(route, path);
}

inline nebula::rt::Result<std::string, std::string> __nebula_rt_http_compiled_route_param1(
    nebula::rt::HttpRoutePattern route,
    std::string path) {
  return nebula::rt::http_compiled_route_param1(route, path);
}

inline nebula::rt::Result<nebula::rt::HttpRouteParams2, std::string> __nebula_rt_http_compiled_route_params2(
    nebula::rt::HttpRoutePattern route,
    std::string path) {
  return nebula::rt::http_compiled_route_params2(route, path);
}

inline nebula::rt::Result<nebula::rt::HttpRouteParams3, std::string> __nebula_rt_http_compiled_route_params3(
    nebula::rt::HttpRoutePattern route,
    std::string path) {
  return nebula::rt::http_compiled_route_params3(route, path);
}

inline nebula::rt::Future<nebula::rt::Result<void, std::string>>
__nebula_rt_http_write_response(nebula::rt::TcpStream stream, nebula::rt::HttpResponse response) {
  return nebula::rt::http_write_response(stream, std::move(response));
}

inline nebula::rt::Future<nebula::rt::Result<void, std::string>>
__nebula_rt_http_write_response_with_connection(nebula::rt::TcpStream stream,
                                                nebula::rt::HttpResponse response,
                                                bool close_connection) {
  return nebula::rt::http_write_response_with_connection(stream, std::move(response), close_connection);
}

inline nebula::rt::Result<nebula::rt::HttpResponse, std::string>
__nebula_rt_http_response1(std::int64_t status,
                           std::string content_type,
                           nebula::rt::Bytes body,
                           std::string name1,
                           std::string value1) {
  return nebula::rt::http_response1(status,
                                    std::move(content_type),
                                    std::move(body),
                                    std::move(name1),
                                    std::move(value1));
}

inline nebula::rt::Result<nebula::rt::HttpResponse, std::string>
__nebula_rt_http_response2(std::int64_t status,
                           std::string content_type,
                           nebula::rt::Bytes body,
                           std::string name1,
                           std::string value1,
                           std::string name2,
                           std::string value2) {
  return nebula::rt::http_response2(status,
                                    std::move(content_type),
                                    std::move(body),
                                    std::move(name1),
                                    std::move(value1),
                                    std::move(name2),
                                    std::move(value2));
}

inline nebula::rt::Result<nebula::rt::HttpResponse, std::string>
__nebula_rt_http_response3(std::int64_t status,
                           std::string content_type,
                           nebula::rt::Bytes body,
                           std::string name1,
                           std::string value1,
                           std::string name2,
                           std::string value2,
                           std::string name3,
                           std::string value3) {
  return nebula::rt::http_response3(status,
                                    std::move(content_type),
                                    std::move(body),
                                    std::move(name1),
                                    std::move(value1),
                                    std::move(name2),
                                    std::move(value2),
                                    std::move(name3),
                                    std::move(value3));
}

inline nebula::rt::Result<nebula::rt::HttpResponse, std::string>
__nebula_rt_http_response_with_header(nebula::rt::HttpResponse response,
                                      std::string name,
                                      std::string value) {
  return nebula::rt::http_response_with_header(std::move(response), std::move(name), std::move(value));
}

inline nebula::rt::HttpResponse __nebula_rt_http_response_without_body(nebula::rt::HttpResponse response) {
  return nebula::rt::http_response_without_body(std::move(response));
}

inline nebula::rt::Result<nebula::rt::HttpClientRequest, std::string>
__nebula_rt_http_request1(nebula::rt::HttpMethod method,
                          std::string authority,
                          std::string path,
                          std::string content_type,
                          nebula::rt::Bytes body,
                          std::string name1,
                          std::string value1) {
  return nebula::rt::http_request1(std::move(method),
                                   std::move(authority),
                                   std::move(path),
                                   std::move(content_type),
                                   std::move(body),
                                   std::move(name1),
                                   std::move(value1));
}

inline nebula::rt::Result<nebula::rt::HttpClientRequest, std::string>
__nebula_rt_http_request2(nebula::rt::HttpMethod method,
                          std::string authority,
                          std::string path,
                          std::string content_type,
                          nebula::rt::Bytes body,
                          std::string name1,
                          std::string value1,
                          std::string name2,
                          std::string value2) {
  return nebula::rt::http_request2(std::move(method),
                                   std::move(authority),
                                   std::move(path),
                                   std::move(content_type),
                                   std::move(body),
                                   std::move(name1),
                                   std::move(value1),
                                   std::move(name2),
                                   std::move(value2));
}

inline nebula::rt::Result<nebula::rt::HttpClientRequest, std::string>
__nebula_rt_http_request3(nebula::rt::HttpMethod method,
                          std::string authority,
                          std::string path,
                          std::string content_type,
                          nebula::rt::Bytes body,
                          std::string name1,
                          std::string value1,
                          std::string name2,
                          std::string value2,
                          std::string name3,
                          std::string value3) {
  return nebula::rt::http_request3(std::move(method),
                                   std::move(authority),
                                   std::move(path),
                                   std::move(content_type),
                                   std::move(body),
                                   std::move(name1),
                                   std::move(value1),
                                   std::move(name2),
                                   std::move(value2),
                                   std::move(name3),
                                   std::move(value3));
}

inline nebula::rt::Future<nebula::rt::Result<void, std::string>>
__nebula_rt_http_write_request(nebula::rt::TcpStream stream, nebula::rt::HttpClientRequest request) {
  return nebula::rt::http_write_request(stream, std::move(request));
}

inline nebula::rt::Future<nebula::rt::Result<nebula::rt::HttpClientResponse, std::string>>
__nebula_rt_http_read_response(nebula::rt::TcpStream stream,
                               std::int64_t max_header_bytes,
                               std::int64_t max_body_bytes) {
  return nebula::rt::http_read_response(stream, max_header_bytes, max_body_bytes);
}

inline nebula::rt::Future<nebula::rt::Result<nebula::rt::HttpClientResponse, std::string>>
__nebula_rt_http_read_response_for(nebula::rt::TcpStream stream,
                                   nebula::rt::HttpMethod method,
                                   std::int64_t max_header_bytes,
                                   std::int64_t max_body_bytes) {
  return nebula::rt::http_read_response_for(std::move(stream),
                                            std::move(method),
                                            max_header_bytes,
                                            max_body_bytes);
}

inline nebula::rt::Result<std::string, std::string>
__nebula_rt_http_response_header(nebula::rt::HttpClientResponse response, std::string name) {
  return nebula::rt::http_response_header(std::move(response), std::move(name));
}

inline std::int64_t args_count() {
  return nebula::rt::argc();
}

inline std::string args_get(std::int64_t index) {
  return nebula::rt::argv(index);
}
