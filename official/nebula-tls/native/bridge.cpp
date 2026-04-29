#include "nebula_tls_default_roots.hpp"
#include "nebula_tls_native.h"

#include "runtime/nebula_runtime.hpp"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/debug.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509.h>
#include <mbedtls/x509_crt.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nebula::rt {

struct TlsTrustStoreState {
  mbedtls_x509_crt ca_chain;

  TlsTrustStoreState() {
    mbedtls_x509_crt_init(&ca_chain);
  }

  ~TlsTrustStoreState() {
    mbedtls_x509_crt_free(&ca_chain);
  }
};

struct TlsClientIdentityState {
  mbedtls_x509_crt certificate_chain;
  mbedtls_pk_context private_key;

  TlsClientIdentityState() {
    mbedtls_x509_crt_init(&certificate_chain);
    mbedtls_pk_init(&private_key);
  }

  ~TlsClientIdentityState() {
    mbedtls_pk_free(&private_key);
    mbedtls_x509_crt_free(&certificate_chain);
  }
};

struct TlsVersionPolicyState {
  mbedtls_ssl_protocol_version min_tls_version = MBEDTLS_SSL_VERSION_TLS1_2;
  mbedtls_ssl_protocol_version max_tls_version = MBEDTLS_SSL_VERSION_TLS1_3;
};

struct TlsAlpnPolicyState {
  std::vector<std::string> protocols;
  std::vector<const char*> c_protocols;

  void rebuild() {
    c_protocols.clear();
    c_protocols.reserve(protocols.size() + 1);
    for (const auto& protocol : protocols) c_protocols.push_back(protocol.c_str());
    c_protocols.push_back(nullptr);
  }

  const char** data_or_null() {
    if (protocols.empty()) return nullptr;
    if (c_protocols.empty()) rebuild();
    return c_protocols.data();
  }
};

struct TlsServerNameState {
  std::string value;
};

struct TlsClientConfigState {
  std::shared_ptr<TlsTrustStoreState> trust_store;
  std::shared_ptr<TlsClientIdentityState> client_identity;
  std::shared_ptr<TlsVersionPolicyState> version_policy;
  std::shared_ptr<TlsAlpnPolicyState> alpn_policy;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
  std::string personal;

  TlsClientConfigState() {
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
  }

  ~TlsClientConfigState() {
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
  }
};

struct TlsTransportInfo {
  bool peer_certificate_present = false;
  bool peer_verified = false;
  std::string peer_subject;
  std::string peer_fingerprint_sha256;
  nebula::rt::JsonValue peer_san_claims = nebula::rt::json_null_value();
  nebula::rt::JsonValue peer_identity_debug = nebula::rt::json_null_value();
  std::string tls_version;
  std::string alpn_protocol;
};

struct Http2DebugEvent {
  std::uint64_t seq = 0;
  std::string kind;
  std::string direction;
  std::string frame_type;
  std::string reason;
  std::string detail;
  std::uint32_t stream_id = 0;
  std::uint8_t flags = 0;
  std::int64_t connection_window = 65535;
  std::int64_t stream_window = 65535;
  std::uint32_t error_code = 0;
};

struct Http2SessionState {
  bool client_preface_sent = false;
  bool client_preface_received = false;
  bool local_settings_sent = false;
  bool local_settings_acknowledged = false;
  bool remote_settings_seen = false;
  bool goaway_received = false;
  bool goaway_sent = false;
  std::uint32_t next_local_stream_id = 1;
  std::uint32_t active_stream_id = 0;
  std::uint32_t last_peer_stream_id = 0;
  std::uint32_t peer_max_concurrent_streams = 0;
  std::uint32_t goaway_last_stream_id = 0;
  std::uint32_t goaway_error_code = 0;
  std::uint32_t last_reset_stream_id = 0;
  std::uint32_t last_reset_error_code = 0;
  std::int64_t peer_connection_window = 65535;
  std::int64_t peer_stream_window = 65535;
  std::int64_t peer_initial_stream_window = 65535;
  std::uint32_t peer_max_frame_size = 16384;
  std::uint64_t next_debug_event_seq = 1;
  bool debug_timeline_overflowed = false;
  std::optional<HttpMethod> active_request_method;
  std::vector<Http2DebugEvent> recent_events;
  std::string read_buffer;
};

struct TlsClientStreamState {
  std::shared_ptr<SocketHandle> handle;
  std::shared_ptr<TlsClientConfigState> config;
  mbedtls_ssl_config ssl_config;
  mbedtls_ssl_context ssl;
  TlsTransportInfo transport;
  Http2SessionState http2;
  int last_socket_error = 0;

  TlsClientStreamState() {
    mbedtls_ssl_config_init(&ssl_config);
    mbedtls_ssl_init(&ssl);
  }

  ~TlsClientStreamState() {
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&ssl_config);
  }
};

struct TlsServerIdentityState {
  mbedtls_x509_crt certificate_chain;
  mbedtls_pk_context private_key;

  TlsServerIdentityState() {
    mbedtls_x509_crt_init(&certificate_chain);
    mbedtls_pk_init(&private_key);
  }

  ~TlsServerIdentityState() {
    mbedtls_pk_free(&private_key);
    mbedtls_x509_crt_free(&certificate_chain);
  }
};

enum class TlsServerClientAuthMode {
  Disabled,
  Optional,
  Required,
};

struct TlsServerConfigState {
  std::shared_ptr<TlsServerIdentityState> server_identity;
  std::shared_ptr<TlsTrustStoreState> client_trust_store;
  std::shared_ptr<TlsVersionPolicyState> version_policy;
  std::shared_ptr<TlsAlpnPolicyState> alpn_policy;
  TlsServerClientAuthMode client_auth_mode = TlsServerClientAuthMode::Disabled;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
  std::string personal;

  TlsServerConfigState() {
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
  }

  ~TlsServerConfigState() {
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
  }
};

struct TlsServerListenerState {
  std::shared_ptr<SocketHandle> handle;
  std::shared_ptr<TlsServerConfigState> config;
  mbedtls_ssl_config ssl_config;
  bool setup_complete = false;

  TlsServerListenerState() {
    mbedtls_ssl_config_init(&ssl_config);
  }

  ~TlsServerListenerState() {
    mbedtls_ssl_config_free(&ssl_config);
  }
};

struct TlsServerStreamState {
  std::shared_ptr<SocketHandle> handle;
  std::shared_ptr<TlsServerListenerState> listener;
  mbedtls_ssl_context ssl;
  TlsTransportInfo transport;
  Http2SessionState http2;
  int last_socket_error = 0;

  TlsServerStreamState() {
    mbedtls_ssl_init(&ssl);
  }

  ~TlsServerStreamState() {
    mbedtls_ssl_free(&ssl);
  }
};

} // namespace nebula::rt

namespace {

using nebula::rt::Bytes;
using nebula::rt::Future;
using nebula::rt::HttpClientRequest;
using nebula::rt::HttpClientResponse;
using nebula::rt::HttpMethod;
using nebula::rt::HttpRequest;
using nebula::rt::HttpRequestFraming;
using nebula::rt::HttpResponse;
using nebula::rt::IoAwaitable;
using nebula::rt::Reactor;
using nebula::rt::Result;
using nebula::rt::SocketAddr;
using nebula::rt::SocketHandle;
using nebula::rt::TcpListener;
using nebula::rt::TcpStream;
using nebula::rt::TlsAlpnPolicy;
using nebula::rt::TlsAlpnPolicyState;
using nebula::rt::TlsClientConfig;
using nebula::rt::TlsClientConfigState;
using nebula::rt::TlsClientIdentity;
using nebula::rt::TlsClientIdentityState;
using nebula::rt::TlsClientStream;
using nebula::rt::TlsClientStreamState;
using nebula::rt::TlsServerConfig;
using nebula::rt::TlsServerConfigState;
using nebula::rt::TlsServerIdentity;
using nebula::rt::TlsServerIdentityState;
using nebula::rt::TlsServerListener;
using nebula::rt::TlsServerListenerState;
using nebula::rt::TlsServerName;
using nebula::rt::TlsServerNameState;
using nebula::rt::TlsServerStream;
using nebula::rt::TlsServerStreamState;
using nebula::rt::TlsServerClientAuthMode;
using nebula::rt::TlsTransportInfo;
using nebula::rt::TlsTrustStore;
using nebula::rt::TlsTrustStoreState;
using nebula::rt::TlsVersionPolicy;
using nebula::rt::TlsVersionPolicyState;

namespace http2 {
nebula::rt::JsonValue http2_debug_state_json(const nebula::rt::Http2SessionState& session);
}

enum class TlsServerHandshakeOutcomeKind {
  Established,
  DroppedBeforeHandshake,
  Fatal,
};

struct TlsServerHandshakeOutcome {
  TlsServerHandshakeOutcomeKind kind = TlsServerHandshakeOutcomeKind::Fatal;
  TlsServerStream stream{};
  std::string message;
};

template <typename T>
Result<T, std::string> err_result(std::string message) {
  return nebula::rt::err_result<T>(std::move(message));
}

std::string mbedtls_error_string(int rc) {
  char buffer[256];
  std::memset(buffer, 0, sizeof(buffer));
  mbedtls_strerror(rc, buffer, sizeof(buffer));
  return std::string(buffer);
}

Result<void, std::string> seed_rng(mbedtls_entropy_context& entropy,
                                   mbedtls_ctr_drbg_context& ctr_drbg,
                                   std::string_view personal) {
  const int rc = mbedtls_ctr_drbg_seed(&ctr_drbg,
                                       mbedtls_entropy_func,
                                       &entropy,
                                       reinterpret_cast<const unsigned char*>(personal.data()),
                                       personal.size());
  if (rc != 0) {
    return nebula::rt::err_void_result("mbedtls_ctr_drbg_seed failed: " + mbedtls_error_string(rc));
  }
  return nebula::rt::ok_void_result();
}

Result<void, std::string> parse_certificate_chain(mbedtls_x509_crt& chain,
                                                  std::string_view pem_text,
                                                  std::string_view source_name) {
  std::string pem(pem_text);
  pem.push_back('\0');
  const int rc = mbedtls_x509_crt_parse(&chain,
                                        reinterpret_cast<const unsigned char*>(pem.data()),
                                        pem.size());
  if (rc > 0) {
    return nebula::rt::err_void_result("mbedtls_x509_crt_parse partially failed for " +
                                       std::string(source_name) + ": " + std::to_string(rc) +
                                       " certificate(s) could not be parsed");
  }
  if (rc < 0) {
    return nebula::rt::err_void_result("mbedtls_x509_crt_parse failed for " +
                                       std::string(source_name) + ": " + mbedtls_error_string(rc));
  }
  return nebula::rt::ok_void_result();
}

Result<void, std::string> parse_private_key(mbedtls_pk_context& private_key,
                                            mbedtls_ctr_drbg_context& ctr_drbg,
                                            std::string_view pem_text,
                                            std::string_view source_name) {
  std::string pem(pem_text);
  pem.push_back('\0');
  const int rc = mbedtls_pk_parse_key(&private_key,
                                      reinterpret_cast<const unsigned char*>(pem.data()),
                                      pem.size(),
                                      nullptr,
                                      0,
                                      mbedtls_ctr_drbg_random,
                                      &ctr_drbg);
  if (rc != 0) {
    return nebula::rt::err_void_result("mbedtls_pk_parse_key failed for " +
                                       std::string(source_name) + ": " + mbedtls_error_string(rc));
  }
  return nebula::rt::ok_void_result();
}

std::shared_ptr<TlsVersionPolicyState> default_version_policy_state() {
  auto state = std::make_shared<TlsVersionPolicyState>();
  state->min_tls_version = MBEDTLS_SSL_VERSION_TLS1_2;
  state->max_tls_version = MBEDTLS_SSL_VERSION_TLS1_3;
  return state;
}

std::shared_ptr<TlsAlpnPolicyState> default_alpn_policy_state() {
  return std::make_shared<TlsAlpnPolicyState>();
}

Result<TlsTrustStore, std::string> make_trust_store_from_pem(std::string_view pem_text,
                                                             std::string_view source_name) {
  auto state = std::make_shared<TlsTrustStoreState>();
  auto parsed = parse_certificate_chain(state->ca_chain, pem_text, source_name);
  if (result_is_err(parsed)) {
    return err_result<TlsTrustStore>(
        result_err_move(parsed));
  }
  return nebula::rt::ok_result(TlsTrustStore{std::move(state)});
}

Result<TlsClientIdentity, std::string> make_client_identity_from_pem(std::string_view certificate_pem,
                                                                     std::string_view private_key_pem) {
  auto state = std::make_shared<TlsClientIdentityState>();
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&ctr_drbg);
  auto cleanup = [&]() {
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
  };
  auto seeded = seed_rng(entropy, ctr_drbg, "nebula-tls-client-identity");
  if (result_is_err(seeded)) {
    const auto message =
        result_err_move(seeded);
    cleanup();
    return err_result<TlsClientIdentity>(message);
  }
  auto parsed_cert =
      parse_certificate_chain(state->certificate_chain, certificate_pem, "provided client certificate");
  if (result_is_err(parsed_cert)) {
    const auto message =
        result_err_move(parsed_cert);
    cleanup();
    return err_result<TlsClientIdentity>(message);
  }
  auto parsed_key =
      parse_private_key(state->private_key, ctr_drbg, private_key_pem, "provided client private key");
  if (result_is_err(parsed_key)) {
    const auto message =
        result_err_move(parsed_key);
    cleanup();
    return err_result<TlsClientIdentity>(message);
  }
  cleanup();
  return nebula::rt::ok_result(TlsClientIdentity{std::move(state)});
}

Result<TlsServerIdentity, std::string> make_server_identity_from_pem(std::string_view certificate_pem,
                                                                     std::string_view private_key_pem) {
  auto state = std::make_shared<TlsServerIdentityState>();
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&ctr_drbg);
  auto cleanup = [&]() {
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
  };
  auto seeded = seed_rng(entropy, ctr_drbg, "nebula-tls-server-identity");
  if (result_is_err(seeded)) {
    const auto message =
        result_err_move(seeded);
    cleanup();
    return err_result<TlsServerIdentity>(message);
  }
  auto parsed_cert =
      parse_certificate_chain(state->certificate_chain, certificate_pem, "provided server certificate");
  if (result_is_err(parsed_cert)) {
    const auto message =
        result_err_move(parsed_cert);
    cleanup();
    return err_result<TlsServerIdentity>(message);
  }
  auto parsed_key =
      parse_private_key(state->private_key, ctr_drbg, private_key_pem, "provided server private key");
  if (result_is_err(parsed_key)) {
    const auto message =
        result_err_move(parsed_key);
    cleanup();
    return err_result<TlsServerIdentity>(message);
  }
  cleanup();
  return nebula::rt::ok_result(TlsServerIdentity{std::move(state)});
}

Result<TlsServerName, std::string> make_server_name(std::string value) {
  if (value.empty()) {
    return err_result<TlsServerName>("tls server_name must be non-empty");
  }
  if (value.find('\0') != std::string::npos) {
    return err_result<TlsServerName>("tls server_name must not contain NUL bytes");
  }
  auto state = std::make_shared<TlsServerNameState>();
  state->value = std::move(value);
  return nebula::rt::ok_result(TlsServerName{std::move(state)});
}

Result<std::shared_ptr<TlsClientConfigState>, std::string> make_client_config_state(
    const std::shared_ptr<TlsTrustStoreState>& trust_store) {
  if (trust_store == nullptr) {
    return err_result<std::shared_ptr<TlsClientConfigState>>("tls trust store is uninitialized");
  }
  auto state = std::make_shared<TlsClientConfigState>();
  state->trust_store = trust_store;
  state->version_policy = default_version_policy_state();
  state->alpn_policy = default_alpn_policy_state();
  state->personal = "nebula-tls-client";
  auto seeded = seed_rng(state->entropy, state->ctr_drbg, state->personal);
  if (result_is_err(seeded)) {
    return err_result<std::shared_ptr<TlsClientConfigState>>(
        result_err_move(seeded));
  }
  return nebula::rt::ok_result(std::move(state));
}

Result<std::shared_ptr<TlsClientConfigState>, std::string> clone_client_config_state(TlsClientConfig base) {
  if (base.state == nullptr) {
    return err_result<std::shared_ptr<TlsClientConfigState>>("tls client config is uninitialized");
  }
  auto cloned = make_client_config_state(base.state->trust_store);
  if (result_is_err(cloned)) {
    return err_result<std::shared_ptr<TlsClientConfigState>>(
        result_err_move(cloned));
  }
  auto state =
      result_ok_move(cloned);
  state->client_identity = base.state->client_identity;
  state->version_policy = base.state->version_policy;
  state->alpn_policy = base.state->alpn_policy;
  return nebula::rt::ok_result(std::move(state));
}

Result<std::shared_ptr<TlsServerConfigState>, std::string> make_server_config_state(
    const std::shared_ptr<TlsServerIdentityState>& identity) {
  if (identity == nullptr) {
    return err_result<std::shared_ptr<TlsServerConfigState>>("tls server identity is uninitialized");
  }
  auto state = std::make_shared<TlsServerConfigState>();
  state->server_identity = identity;
  state->version_policy = default_version_policy_state();
  state->alpn_policy = default_alpn_policy_state();
  state->personal = "nebula-tls-server";
  auto seeded = seed_rng(state->entropy, state->ctr_drbg, state->personal);
  if (result_is_err(seeded)) {
    return err_result<std::shared_ptr<TlsServerConfigState>>(
        result_err_move(seeded));
  }
  return nebula::rt::ok_result(std::move(state));
}

Result<std::shared_ptr<TlsServerConfigState>, std::string> clone_server_config_state(TlsServerConfig base) {
  if (base.state == nullptr) {
    return err_result<std::shared_ptr<TlsServerConfigState>>("tls server config is uninitialized");
  }
  auto cloned = make_server_config_state(base.state->server_identity);
  if (result_is_err(cloned)) {
    return err_result<std::shared_ptr<TlsServerConfigState>>(
        result_err_move(cloned));
  }
  auto state =
      result_ok_move(cloned);
  state->client_trust_store = base.state->client_trust_store;
  state->version_policy = base.state->version_policy;
  state->alpn_policy = base.state->alpn_policy;
  state->client_auth_mode = base.state->client_auth_mode;
  return nebula::rt::ok_result(std::move(state));
}

void apply_version_policy(mbedtls_ssl_config& ssl_config,
                          const std::shared_ptr<TlsVersionPolicyState>& policy) {
  if (policy == nullptr) return;
  mbedtls_ssl_conf_min_tls_version(&ssl_config, policy->min_tls_version);
  mbedtls_ssl_conf_max_tls_version(&ssl_config, policy->max_tls_version);
}

Result<void, std::string> apply_alpn_policy(mbedtls_ssl_config& ssl_config,
                                            const std::shared_ptr<TlsAlpnPolicyState>& policy) {
  if (policy == nullptr || policy->protocols.empty()) {
    return nebula::rt::ok_void_result();
  }
  const int rc = mbedtls_ssl_conf_alpn_protocols(&ssl_config, policy->data_or_null());
  if (rc != 0) {
    return nebula::rt::err_void_result("mbedtls_ssl_conf_alpn_protocols failed: " +
                                       mbedtls_error_string(rc));
  }
  return nebula::rt::ok_void_result();
}

void tls_debug_callback(void*,
                        int level,
                        const char* file,
                        int line,
                        const char* message) {
  if (message == nullptr) return;
  std::fprintf(stderr, "[mbedtls][%d] %s:%d %s", level, file != nullptr ? file : "?", line, message);
}

void maybe_enable_tls_debug(mbedtls_ssl_config& ssl_config) {
  const char* value = std::getenv("NEBULA_TLS_DEBUG");
  if (value == nullptr || value[0] == '\0' || std::strcmp(value, "0") == 0) return;
  mbedtls_debug_set_threshold(4);
  mbedtls_ssl_conf_dbg(&ssl_config, tls_debug_callback, nullptr);
}

bool tls_debug_enabled() {
  const char* value = std::getenv("NEBULA_TLS_DEBUG");
  return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

bool socket_error_indicates_peer_drop(int err) {
#if defined(_WIN32)
  return err == WSAECONNRESET || err == WSAECONNABORTED || err == WSAENOTCONN ||
         err == WSAESHUTDOWN;
#else
  return err == ECONNRESET || err == EPIPE || err == ENOTCONN || err == ECONNABORTED;
#endif
}

bool tls_server_handshake_dropped_before_auth(int rc, const TlsServerStreamState& state) {
  if (rc == MBEDTLS_ERR_SSL_CONN_EOF || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY ||
      rc == MBEDTLS_ERR_NET_CONN_RESET) {
    return true;
  }
  if ((rc == MBEDTLS_ERR_NET_SEND_FAILED || rc == MBEDTLS_ERR_NET_RECV_FAILED) &&
      socket_error_indicates_peer_drop(state.last_socket_error)) {
    return true;
  }
  return false;
}

std::string x509_name_text(const mbedtls_x509_name* name) {
  if (name == nullptr) return "";
  char buffer[512];
  std::memset(buffer, 0, sizeof(buffer));
  const int rc = mbedtls_x509_dn_gets(buffer, sizeof(buffer), name);
  if (rc < 0) return "";
  return std::string(buffer);
}

std::string peer_subject_text(const mbedtls_x509_crt* peer_cert) {
  if (peer_cert == nullptr) return "";
  return x509_name_text(&peer_cert->subject);
}

std::string hex_encode_lower(const unsigned char* bytes, std::size_t size) {
  static constexpr char kHexDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(size * 2);
  for (std::size_t i = 0; i < size; ++i) {
    const unsigned char value = bytes[i];
    out.push_back(kHexDigits[(value >> 4) & 0x0f]);
    out.push_back(kHexDigits[value & 0x0f]);
  }
  return out;
}

std::string peer_fingerprint_sha256_text(const mbedtls_x509_crt* peer_cert) {
  if (peer_cert == nullptr || peer_cert->raw.p == nullptr || peer_cert->raw.len == 0) return "";
  unsigned char digest[32];
  if (mbedtls_sha256(peer_cert->raw.p, peer_cert->raw.len, digest, 0) != 0) return "";
  return hex_encode_lower(digest, sizeof(digest));
}

std::string peer_ip_san_text(const mbedtls_x509_subject_alternative_name& san) {
  char buffer[INET6_ADDRSTRLEN];
  std::memset(buffer, 0, sizeof(buffer));
  const auto* data = san.san.unstructured_name.p;
  const auto size = san.san.unstructured_name.len;
  if (data == nullptr) return "";
  if (size == 4) {
    if (inet_ntop(AF_INET, data, buffer, sizeof(buffer)) == nullptr) return "";
    return std::string(buffer);
  }
  if (size == 16) {
    if (inet_ntop(AF_INET6, data, buffer, sizeof(buffer)) == nullptr) return "";
    return std::string(buffer);
  }
  return "";
}

std::string peer_issuer_text(const mbedtls_x509_crt* peer_cert) {
  if (peer_cert == nullptr) return "";
  return x509_name_text(&peer_cert->issuer);
}

std::string x509_serial_hex_text(const mbedtls_x509_buf* serial) {
  if (serial == nullptr || serial->p == nullptr || serial->len == 0) return "";
  char buffer[256];
  std::memset(buffer, 0, sizeof(buffer));
  const int rc = mbedtls_x509_serial_gets(buffer, sizeof(buffer), serial);
  if (rc < 0) return "";
  return std::string(buffer);
}

std::string x509_time_text(const mbedtls_x509_time& value) {
  char buffer[32];
  std::snprintf(buffer,
                sizeof(buffer),
                "%04d-%02d-%02dT%02d:%02d:%02dZ",
                value.year,
                value.mon,
                value.day,
                value.hour,
                value.min,
                value.sec);
  return std::string(buffer);
}

nebula::rt::JsonValue peer_cert_validity_json(const mbedtls_x509_crt* peer_cert) {
  if (peer_cert == nullptr) return nebula::rt::json_null_value();
  return nebula::rt::json_object2("not_before",
                                  nebula::rt::json_string_value(x509_time_text(peer_cert->valid_from)),
                                  "not_after",
                                  nebula::rt::json_string_value(x509_time_text(peer_cert->valid_to)));
}

nebula::rt::JsonValue empty_peer_san_claims() {
  auto dns = nebula::rt::json_array_builder();
  auto ips = nebula::rt::json_array_builder();
  auto emails = nebula::rt::json_array_builder();
  auto uris = nebula::rt::json_array_builder();
  return nebula::rt::json_object4("dns_names",
                                  nebula::rt::json_array_build(std::move(dns)),
                                  "ip_addresses",
                                  nebula::rt::json_array_build(std::move(ips)),
                                  "email_addresses",
                                  nebula::rt::json_array_build(std::move(emails)),
                                  "uris",
                                  nebula::rt::json_array_build(std::move(uris)));
}

nebula::rt::JsonValue peer_san_claims_json(const mbedtls_x509_crt* peer_cert) {
  auto dns = nebula::rt::json_array_builder();
  auto ips = nebula::rt::json_array_builder();
  auto emails = nebula::rt::json_array_builder();
  auto uris = nebula::rt::json_array_builder();
  if (peer_cert == nullptr) {
    return empty_peer_san_claims();
  }

  for (const auto* item = &peer_cert->subject_alt_names; item != nullptr; item = item->next) {
    if (item->buf.p == nullptr || item->buf.len == 0) continue;
    mbedtls_x509_subject_alternative_name san;
    std::memset(&san, 0, sizeof(san));
    const int rc = mbedtls_x509_parse_subject_alt_name(&item->buf, &san);
    if (rc != 0) {
      mbedtls_x509_free_subject_alt_name(&san);
      continue;
    }

    switch (san.type) {
      case MBEDTLS_X509_SAN_DNS_NAME:
        dns = nebula::rt::json_array_push(
            std::move(dns),
            nebula::rt::json_string_value(
                std::string(reinterpret_cast<const char*>(san.san.unstructured_name.p),
                            san.san.unstructured_name.len)));
        break;
      case MBEDTLS_X509_SAN_RFC822_NAME:
        emails = nebula::rt::json_array_push(
            std::move(emails),
            nebula::rt::json_string_value(
                std::string(reinterpret_cast<const char*>(san.san.unstructured_name.p),
                            san.san.unstructured_name.len)));
        break;
      case MBEDTLS_X509_SAN_UNIFORM_RESOURCE_IDENTIFIER:
        uris = nebula::rt::json_array_push(
            std::move(uris),
            nebula::rt::json_string_value(
                std::string(reinterpret_cast<const char*>(san.san.unstructured_name.p),
                            san.san.unstructured_name.len)));
        break;
      case MBEDTLS_X509_SAN_IP_ADDRESS: {
        const auto ip_text = peer_ip_san_text(san);
        if (!ip_text.empty()) {
          ips = nebula::rt::json_array_push(std::move(ips), nebula::rt::json_string_value(ip_text));
        }
        break;
      }
      default:
        break;
    }

    mbedtls_x509_free_subject_alt_name(&san);
  }

  return nebula::rt::json_object4("dns_names",
                                  nebula::rt::json_array_build(std::move(dns)),
                                  "ip_addresses",
                                  nebula::rt::json_array_build(std::move(ips)),
                                  "email_addresses",
                                  nebula::rt::json_array_build(std::move(emails)),
                                  "uris",
                                  nebula::rt::json_array_build(std::move(uris)));
}

nebula::rt::JsonValue peer_cert_chain_debug_json(const mbedtls_x509_crt* peer_cert) {
  auto builder = nebula::rt::json_array_builder();
  std::int64_t index = 0;
  for (const auto* current = peer_cert; current != nullptr; current = current->next) {
    builder = nebula::rt::json_array_push(
        std::move(builder),
        nebula::rt::json_object7("index",
                                 nebula::rt::json_int_value(index),
                                 "subject",
                                 nebula::rt::json_string_value(peer_subject_text(current)),
                                 "issuer",
                                 nebula::rt::json_string_value(peer_issuer_text(current)),
                                 "serial_hex",
                                 nebula::rt::json_string_value(x509_serial_hex_text(&current->serial)),
                                 "validity",
                                 peer_cert_validity_json(current),
                                 "fingerprint_sha256",
                                 nebula::rt::json_string_value(peer_fingerprint_sha256_text(current)),
                                 "san_claims",
                                 peer_san_claims_json(current)));
    ++index;
  }
  return nebula::rt::json_array_build(std::move(builder));
}

nebula::rt::JsonValue peer_identity_debug_json(const mbedtls_x509_crt* peer_cert, bool peer_verified) {
  if (peer_cert == nullptr) return nebula::rt::json_null_value();
  return nebula::rt::json_object6("present",
                                  nebula::rt::json_bool_value(true),
                                  "verified",
                                  nebula::rt::json_bool_value(peer_verified),
                                  "subject",
                                  nebula::rt::json_string_value(peer_subject_text(peer_cert)),
                                  "fingerprint_sha256",
                                  nebula::rt::json_string_value(peer_fingerprint_sha256_text(peer_cert)),
                                  "san_claims",
                                  peer_san_claims_json(peer_cert),
                                  "chain",
                                  peer_cert_chain_debug_json(peer_cert));
}

std::string tls_version_text(const mbedtls_ssl_context& ssl) {
  const char* value = mbedtls_ssl_get_version(&ssl);
  if (value == nullptr) return "";
  return std::string(value);
}

std::string alpn_protocol_text(const mbedtls_ssl_context& ssl) {
  const char* value = mbedtls_ssl_get_alpn_protocol(&ssl);
  if (value == nullptr) return "";
  return std::string(value);
}

void fill_transport_info(TlsTransportInfo& out,
                         const mbedtls_ssl_context& ssl,
                         bool peer_verified,
                         const mbedtls_x509_crt* peer_cert) {
  out.peer_certificate_present = peer_cert != nullptr;
  out.peer_verified = peer_verified;
  out.peer_subject = peer_subject_text(peer_cert);
  out.peer_fingerprint_sha256 = peer_fingerprint_sha256_text(peer_cert);
  out.peer_san_claims = peer_san_claims_json(peer_cert);
  out.peer_identity_debug = peer_identity_debug_json(peer_cert, peer_verified);
  out.tls_version = tls_version_text(ssl);
  out.alpn_protocol = alpn_protocol_text(ssl);
}

template <typename StreamState>
int tls_bio_send(void* ctx, const unsigned char* buf, size_t len) {
  auto* state = static_cast<StreamState*>(ctx);
  if (state == nullptr || state->handle == nullptr || !state->handle->valid()) {
    return MBEDTLS_ERR_NET_INVALID_CONTEXT;
  }
  while (true) {
#if defined(_WIN32)
    const int rc = send(state->handle->fd,
                        reinterpret_cast<const char*>(buf),
                        static_cast<int>(len),
                        0);
#else
    const ssize_t rc = send(state->handle->fd, buf, len, nebula::rt::socket_send_flags());
#endif
    if (rc > 0) return static_cast<int>(rc);
    const int err = nebula::rt::last_socket_error_code();
    if (nebula::rt::socket_error_is_interrupted(err)) continue;
    if (nebula::rt::socket_error_is_would_block(err)) return MBEDTLS_ERR_SSL_WANT_WRITE;
    state->last_socket_error = err;
    if (tls_debug_enabled()) {
      std::fprintf(stderr,
                   "[nebula-tls-bio-send] len=%zu err=%d message=%s\n",
                   len,
                   err,
                   nebula::rt::socket_error_message(err).c_str());
    }
    return MBEDTLS_ERR_NET_SEND_FAILED;
  }
}

template <typename StreamState>
int tls_bio_recv(void* ctx, unsigned char* buf, size_t len) {
  auto* state = static_cast<StreamState*>(ctx);
  if (state == nullptr || state->handle == nullptr || !state->handle->valid()) {
    return MBEDTLS_ERR_NET_INVALID_CONTEXT;
  }
  while (true) {
#if defined(_WIN32)
    const int rc = recv(state->handle->fd,
                        reinterpret_cast<char*>(buf),
                        static_cast<int>(len),
                        0);
#else
    const ssize_t rc = recv(state->handle->fd, buf, len, 0);
#endif
    if (rc > 0) return static_cast<int>(rc);
    if (rc == 0) return 0;
    const int err = nebula::rt::last_socket_error_code();
    if (nebula::rt::socket_error_is_interrupted(err)) continue;
    if (nebula::rt::socket_error_is_would_block(err)) return MBEDTLS_ERR_SSL_WANT_READ;
    state->last_socket_error = err;
    return MBEDTLS_ERR_NET_RECV_FAILED;
  }
}

std::string http_over_tls_protocol_error(const std::string& alpn_protocol) {
  if (alpn_protocol == "h2") {
    return "HTTP/2 over TLS is not implemented yet";
  }
  return "";
}

template <typename StatePtr>
Future<Result<Bytes, std::string>> tls_stream_read_async(StatePtr state,
                                                         std::int64_t max_bytes,
                                                         const char* closed_message) {
  if (state == nullptr || state->handle == nullptr || !state->handle->valid()) {
    co_return err_result<Bytes>(closed_message);
  }
  if (max_bytes < 0) {
    co_return err_result<Bytes>("tls read max_bytes must be non-negative");
  }
  if (max_bytes == 0) {
    co_return nebula::rt::ok_result(Bytes{});
  }
  std::string buffer(static_cast<std::size_t>(max_bytes), '\0');
  while (true) {
    const int rc =
        mbedtls_ssl_read(&state->ssl, reinterpret_cast<unsigned char*>(buffer.data()), buffer.size());
    if (rc > 0) {
      buffer.resize(static_cast<std::size_t>(rc));
      co_return nebula::rt::ok_result(Bytes{std::move(buffer)});
    }
    if (rc == 0 || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
      co_return nebula::rt::ok_result(Bytes{});
    }
    if (rc == MBEDTLS_ERR_SSL_WANT_READ) {
      co_await IoAwaitable{state->handle->fd, Reactor::Interest::Read};
      continue;
    }
    if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
      co_await IoAwaitable{state->handle->fd, Reactor::Interest::Write};
      continue;
    }
    co_return err_result<Bytes>("tls read failed: " + mbedtls_error_string(rc));
  }
}

template <typename StatePtr>
Future<Result<std::int64_t, std::string>> tls_stream_write_async(StatePtr state,
                                                                 Bytes bytes,
                                                                 const char* closed_message) {
  if (state == nullptr || state->handle == nullptr || !state->handle->valid()) {
    co_return err_result<std::int64_t>(closed_message);
  }
  if (bytes.data.empty()) {
    co_return nebula::rt::ok_result<std::int64_t>(0);
  }
  while (true) {
    const int rc = mbedtls_ssl_write(&state->ssl,
                                     reinterpret_cast<const unsigned char*>(bytes.data.data()),
                                     bytes.data.size());
    if (rc > 0) {
      co_return nebula::rt::ok_result<std::int64_t>(rc);
    }
    if (rc == MBEDTLS_ERR_SSL_WANT_READ) {
      co_await IoAwaitable{state->handle->fd, Reactor::Interest::Read};
      continue;
    }
    if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
      co_await IoAwaitable{state->handle->fd, Reactor::Interest::Write};
      continue;
    }
    co_return err_result<std::int64_t>("tls write failed: " + mbedtls_error_string(rc));
  }
}

template <typename StatePtr>
Future<Result<void, std::string>> tls_stream_write_all_async(StatePtr state,
                                                             Bytes bytes,
                                                             const char* closed_message) {
  if (state == nullptr || state->handle == nullptr || !state->handle->valid()) {
    co_return nebula::rt::err_void_result(closed_message);
  }
  std::size_t offset = 0;
  while (offset < bytes.data.size()) {
    const int rc = mbedtls_ssl_write(&state->ssl,
                                     reinterpret_cast<const unsigned char*>(bytes.data.data() + offset),
                                     bytes.data.size() - offset);
    if (rc > 0) {
      offset += static_cast<std::size_t>(rc);
      continue;
    }
    if (rc == MBEDTLS_ERR_SSL_WANT_READ) {
      co_await IoAwaitable{state->handle->fd, Reactor::Interest::Read};
      continue;
    }
    if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
      co_await IoAwaitable{state->handle->fd, Reactor::Interest::Write};
      continue;
    }
    co_return nebula::rt::err_void_result("tls write failed: " + mbedtls_error_string(rc));
  }
  co_return nebula::rt::ok_void_result();
}

template <typename StatePtr>
Future<Result<void, std::string>> tls_stream_close_async(StatePtr state, const char* closed_message) {
  if (state == nullptr || state->handle == nullptr) {
    co_return nebula::rt::ok_void_result();
  }
  while (true) {
    const int rc = mbedtls_ssl_close_notify(&state->ssl);
    if (rc == 0) break;
    if (rc == MBEDTLS_ERR_SSL_WANT_READ) {
      co_await IoAwaitable{state->handle->fd, Reactor::Interest::Read};
      continue;
    }
    if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
      co_await IoAwaitable{state->handle->fd, Reactor::Interest::Write};
      continue;
    }
    break;
  }
  (void)closed_message;
  co_return nebula::rt::close_socket_handle(state->handle);
}

template <typename StatePtr>
Result<void, std::string> tls_stream_abort_result(StatePtr state) {
  if (state == nullptr) return nebula::rt::ok_void_result();
  return nebula::rt::close_socket_handle(state->handle);
}

template <typename StatePtr>
Result<SocketAddr, std::string> tls_stream_peer_addr_result(StatePtr state, const char* closed_message) {
  if (state == nullptr || state->handle == nullptr || !state->handle->valid()) {
    return err_result<SocketAddr>(closed_message);
  }
  return nebula::rt::socket_name_of(state->handle->fd, true);
}

template <typename StatePtr>
Result<SocketAddr, std::string> tls_stream_local_addr_result(StatePtr state, const char* closed_message) {
  if (state == nullptr || state->handle == nullptr || !state->handle->valid()) {
    return err_result<SocketAddr>(closed_message);
  }
  return nebula::rt::socket_name_of(state->handle->fd, false);
}

template <typename StatePtr>
Result<std::string, std::string> tls_stream_peer_subject_result(StatePtr state,
                                                                const char* closed_message) {
  if (state == nullptr || state->handle == nullptr || !state->handle->valid()) {
    return err_result<std::string>(closed_message);
  }
  if (!state->transport.peer_certificate_present) {
    return err_result<std::string>("tls peer certificate is unavailable");
  }
  return nebula::rt::ok_result(state->transport.peer_subject);
}

template <typename StatePtr>
Result<std::string, std::string> tls_stream_peer_fingerprint_sha256_result(StatePtr state,
                                                                           const char* closed_message) {
  if (state == nullptr || state->handle == nullptr || !state->handle->valid()) {
    return err_result<std::string>(closed_message);
  }
  if (!state->transport.peer_certificate_present) {
    return err_result<std::string>("tls peer certificate is unavailable");
  }
  if (state->transport.peer_fingerprint_sha256.empty()) {
    return err_result<std::string>("tls peer fingerprint is unavailable");
  }
  return nebula::rt::ok_result(state->transport.peer_fingerprint_sha256);
}

template <typename StatePtr>
Result<nebula::rt::JsonValue, std::string> tls_stream_peer_san_claims_result(StatePtr state,
                                                                              const char* closed_message) {
  if (state == nullptr || state->handle == nullptr || !state->handle->valid()) {
    return err_result<nebula::rt::JsonValue>(closed_message);
  }
  if (!state->transport.peer_certificate_present) {
    return err_result<nebula::rt::JsonValue>("tls peer certificate is unavailable");
  }
  return nebula::rt::ok_result(state->transport.peer_san_claims);
}

template <typename StatePtr>
Result<nebula::rt::JsonValue, std::string> tls_stream_peer_identity_debug_result(
    StatePtr state,
    const char* closed_message) {
  if (state == nullptr || state->handle == nullptr || !state->handle->valid()) {
    return err_result<nebula::rt::JsonValue>(closed_message);
  }
  if (!state->transport.peer_certificate_present) {
    return nebula::rt::ok_result(nebula::rt::json_null_value());
  }
  return nebula::rt::ok_result(state->transport.peer_identity_debug);
}

template <typename StatePtr>
Result<nebula::rt::JsonValue, std::string> tls_stream_http2_debug_state_result(
    StatePtr state,
    const char* closed_message) {
  if (state == nullptr || state->handle == nullptr || !state->handle->valid()) {
    return err_result<nebula::rt::JsonValue>(closed_message);
  }
  if (state->transport.alpn_protocol != "h2") {
    return nebula::rt::ok_result(nebula::rt::json_null_value());
  }
  return nebula::rt::ok_result(http2::http2_debug_state_json(state->http2));
}

Future<Result<TlsClientStream, std::string>> tls_handshake_client_async(TcpStream stream,
                                                                        TlsServerName server_name,
                                                                        TlsClientConfig config) {
  std::string error;
  auto handle = nebula::rt::require_open_handle(stream.handle, "stream", error);
  if (!handle) {
    co_return err_result<TlsClientStream>(std::move(error));
  }
  if (config.state == nullptr) {
    co_return err_result<TlsClientStream>("tls client config is uninitialized");
  }
  if (config.state->trust_store == nullptr) {
    co_return err_result<TlsClientStream>("tls trust store is uninitialized");
  }
  if (server_name.state == nullptr || server_name.state->value.empty()) {
    co_return err_result<TlsClientStream>("tls server_name must be non-empty");
  }

  auto state = std::make_shared<TlsClientStreamState>();
  state->handle = std::move(handle);
  state->config = std::move(config.state);

  int rc = mbedtls_ssl_config_defaults(&state->ssl_config,
                                       MBEDTLS_SSL_IS_CLIENT,
                                       MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT);
  if (rc != 0) {
    co_return err_result<TlsClientStream>("mbedtls_ssl_config_defaults failed: " +
                                          mbedtls_error_string(rc));
  }

  apply_version_policy(state->ssl_config, state->config->version_policy);
  auto alpn = apply_alpn_policy(state->ssl_config, state->config->alpn_policy);
  if (result_is_err(alpn)) {
    co_return err_result<TlsClientStream>(
        result_err_move(alpn));
  }

  mbedtls_ssl_conf_authmode(&state->ssl_config, MBEDTLS_SSL_VERIFY_REQUIRED);
  mbedtls_ssl_conf_ca_chain(&state->ssl_config, &state->config->trust_store->ca_chain, nullptr);
  mbedtls_ssl_conf_rng(&state->ssl_config,
                       mbedtls_ctr_drbg_random,
                       &state->config->ctr_drbg);
  maybe_enable_tls_debug(state->ssl_config);
  if (state->config->client_identity != nullptr) {
    rc = mbedtls_ssl_conf_own_cert(&state->ssl_config,
                                   &state->config->client_identity->certificate_chain,
                                   &state->config->client_identity->private_key);
    if (rc != 0) {
      co_return err_result<TlsClientStream>("mbedtls_ssl_conf_own_cert failed: " +
                                            mbedtls_error_string(rc));
    }
  }

  rc = mbedtls_ssl_setup(&state->ssl, &state->ssl_config);
  if (rc != 0) {
    co_return err_result<TlsClientStream>("mbedtls_ssl_setup failed: " + mbedtls_error_string(rc));
  }

  rc = mbedtls_ssl_set_hostname(&state->ssl, server_name.state->value.c_str());
  if (rc != 0) {
    co_return err_result<TlsClientStream>("mbedtls_ssl_set_hostname failed: " +
                                          mbedtls_error_string(rc));
  }

  mbedtls_ssl_set_bio(&state->ssl,
                      state.get(),
                      tls_bio_send<TlsClientStreamState>,
                      tls_bio_recv<TlsClientStreamState>,
                      nullptr);

  while (true) {
    rc = mbedtls_ssl_handshake(&state->ssl);
    if (rc == 0) break;
    if (rc == MBEDTLS_ERR_SSL_WANT_READ) {
      co_await IoAwaitable{state->handle->fd, Reactor::Interest::Read};
      continue;
    }
    if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
      co_await IoAwaitable{state->handle->fd, Reactor::Interest::Write};
      continue;
    }
    co_return err_result<TlsClientStream>("tls handshake failed: " + mbedtls_error_string(rc));
  }

  const auto* peer_cert = mbedtls_ssl_get_peer_cert(&state->ssl);
  const std::uint32_t verify_flags = mbedtls_ssl_get_verify_result(&state->ssl);
  if (verify_flags != 0) {
    char buffer[512];
    std::memset(buffer, 0, sizeof(buffer));
    mbedtls_x509_crt_verify_info(buffer, sizeof(buffer) - 1, "", verify_flags);
    co_return err_result<TlsClientStream>("tls peer verification failed: " + std::string(buffer));
  }

  fill_transport_info(state->transport, state->ssl, true, peer_cert);
  co_return nebula::rt::ok_result(TlsClientStream{std::move(state)});
}

Future<TlsServerHandshakeOutcome> tls_handshake_server_async(
    TlsServerListener listener,
    TcpStream accepted_stream) {
  if (listener.state == nullptr || listener.state->handle == nullptr || !listener.state->handle->valid()) {
    co_return TlsServerHandshakeOutcome{
        TlsServerHandshakeOutcomeKind::Fatal,
        {},
        "tls listener is closed",
    };
  }
  std::string error;
  auto handle = nebula::rt::require_open_handle(accepted_stream.handle, "stream", error);
  if (!handle) {
    co_return TlsServerHandshakeOutcome{
        TlsServerHandshakeOutcomeKind::Fatal,
        {},
        std::move(error),
    };
  }
  auto state = std::make_shared<TlsServerStreamState>();
  state->handle = std::move(handle);
  state->listener = listener.state;

  int rc = mbedtls_ssl_setup(&state->ssl, &state->listener->ssl_config);
  if (rc != 0) {
    co_return TlsServerHandshakeOutcome{
        TlsServerHandshakeOutcomeKind::Fatal,
        {},
        "mbedtls_ssl_setup failed: " + mbedtls_error_string(rc),
    };
  }

  mbedtls_ssl_set_bio(&state->ssl,
                      state.get(),
                      tls_bio_send<TlsServerStreamState>,
                      tls_bio_recv<TlsServerStreamState>,
                      nullptr);

  while (true) {
    rc = mbedtls_ssl_handshake(&state->ssl);
    if (rc == 0) break;
    if (rc == MBEDTLS_ERR_SSL_WANT_READ) {
      co_await IoAwaitable{state->handle->fd, Reactor::Interest::Read};
      continue;
    }
    if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
      co_await IoAwaitable{state->handle->fd, Reactor::Interest::Write};
      continue;
    }
    if (tls_server_handshake_dropped_before_auth(rc, *state)) {
      auto _ = nebula::rt::close_socket_handle(state->handle);
      (void)_;
      co_return TlsServerHandshakeOutcome{
          TlsServerHandshakeOutcomeKind::DroppedBeforeHandshake,
          {},
          "",
      };
    }
    co_return TlsServerHandshakeOutcome{
        TlsServerHandshakeOutcomeKind::Fatal,
        {},
        "tls handshake failed: " + mbedtls_error_string(rc),
    };
  }

  const auto* peer_cert = mbedtls_ssl_get_peer_cert(&state->ssl);
  const std::uint32_t verify_flags = mbedtls_ssl_get_verify_result(&state->ssl);
  bool peer_verified = false;
  const auto client_auth_mode = state->listener->config->client_auth_mode;
  if (client_auth_mode == TlsServerClientAuthMode::Disabled) {
    fill_transport_info(state->transport, state->ssl, false, peer_cert);
    co_return TlsServerHandshakeOutcome{
        TlsServerHandshakeOutcomeKind::Established,
        TlsServerStream{std::move(state)},
        "",
    };
  }

  if (verify_flags != 0) {
    const bool missing_client_is_allowed =
        peer_cert == nullptr &&
        (verify_flags == MBEDTLS_X509_BADCERT_MISSING ||
         verify_flags == MBEDTLS_X509_BADCERT_SKIP_VERIFY) &&
        client_auth_mode == TlsServerClientAuthMode::Optional;
    if (!missing_client_is_allowed) {
      char buffer[512];
      std::memset(buffer, 0, sizeof(buffer));
      mbedtls_x509_crt_verify_info(buffer, sizeof(buffer) - 1, "", verify_flags);
      co_return TlsServerHandshakeOutcome{
          TlsServerHandshakeOutcomeKind::Fatal,
          {},
          "tls peer verification failed: " + std::string(buffer),
      };
    }
  } else if (peer_cert != nullptr) {
    peer_verified = true;
  }

  fill_transport_info(state->transport, state->ssl, peer_verified, peer_cert);
  co_return TlsServerHandshakeOutcome{
      TlsServerHandshakeOutcomeKind::Established,
      TlsServerStream{std::move(state)},
      "",
  };
}

Future<Result<HttpClientResponse, std::string>> tls_read_http_response_async(
    TlsClientStream self,
    const std::optional<HttpMethod>& request_method,
    std::int64_t max_header_bytes,
    std::int64_t max_body_bytes) {
  if (self.state == nullptr || self.state->handle == nullptr || !self.state->handle->valid()) {
    co_return err_result<HttpClientResponse>("tls stream is closed");
  }
  if (max_header_bytes <= 0 || max_body_bytes < 0) {
    co_return err_result<HttpClientResponse>("HTTP limits must be positive");
  }
  const auto protocol_error = http_over_tls_protocol_error(self.state->transport.alpn_protocol);
  if (!protocol_error.empty()) {
    co_return err_result<HttpClientResponse>(protocol_error);
  }

  std::string buffer;
  buffer.reserve(std::min<std::size_t>(static_cast<std::size_t>(max_header_bytes) +
                                           static_cast<std::size_t>(max_body_bytes),
                                       16384));

  while (true) {
    auto parsed = nebula::rt::try_read_http_client_response_from_buffer(buffer,
                                                                        false,
                                                                        static_cast<std::size_t>(max_header_bytes),
                                                                        static_cast<std::size_t>(max_body_bytes),
                                                                        request_method);
    if (result_is_err(parsed)) {
      co_return err_result<HttpClientResponse>(
          result_err_move(parsed));
    }
    auto parsed_result =
        result_ok_move(parsed);
    if (parsed_result.action == nebula::rt::HttpClientResponseReadAction::ReturnResponse) {
      co_return nebula::rt::ok_result(std::move(parsed_result.response));
    }
    if (parsed_result.action == nebula::rt::HttpClientResponseReadAction::SkipInterim) {
      buffer.erase(0, parsed_result.consume_prefix);
      continue;
    }

    char chunk[4096];
    const int rc =
        mbedtls_ssl_read(&self.state->ssl, reinterpret_cast<unsigned char*>(chunk), sizeof(chunk));
    if (rc > 0) {
      buffer.append(chunk, static_cast<std::size_t>(rc));
      continue;
    }
    if (rc == 0 || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
      while (true) {
        auto final = nebula::rt::try_read_http_client_response_from_buffer(buffer,
                                                                           true,
                                                                           static_cast<std::size_t>(max_header_bytes),
                                                                           static_cast<std::size_t>(max_body_bytes),
                                                                           request_method);
        if (result_is_err(final)) {
          co_return err_result<HttpClientResponse>(
              result_err_move(final));
        }
        auto final_result =
            result_ok_move(final);
        if (final_result.action == nebula::rt::HttpClientResponseReadAction::ReturnResponse) {
          co_return nebula::rt::ok_result(std::move(final_result.response));
        }
        if (final_result.action == nebula::rt::HttpClientResponseReadAction::SkipInterim) {
          buffer.erase(0, final_result.consume_prefix);
          if (buffer.empty()) {
            co_return err_result<HttpClientResponse>("unexpected EOF after interim HTTP response");
          }
          continue;
        }
        co_return err_result<HttpClientResponse>("unexpected EOF while reading HTTP response");
      }
    }
    if (rc == MBEDTLS_ERR_SSL_WANT_READ) {
      co_await IoAwaitable{self.state->handle->fd, Reactor::Interest::Read};
      continue;
    }
    if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
      co_await IoAwaitable{self.state->handle->fd, Reactor::Interest::Write};
      continue;
    }
    co_return err_result<HttpClientResponse>("tls read failed: " + mbedtls_error_string(rc));
  }
}

Future<Result<HttpRequest, std::string>> tls_read_http_request_async(TlsServerStream self,
                                                                     std::int64_t max_header_bytes,
                                                                     std::int64_t max_body_bytes) {
  if (self.state == nullptr || self.state->handle == nullptr || !self.state->handle->valid()) {
    co_return err_result<HttpRequest>("tls stream is closed");
  }
  if (max_header_bytes <= 0 || max_body_bytes < 0) {
    co_return err_result<HttpRequest>("HTTP limits must be positive");
  }
  const auto protocol_error = http_over_tls_protocol_error(self.state->transport.alpn_protocol);
  if (!protocol_error.empty()) {
    co_return err_result<HttpRequest>(protocol_error);
  }

  std::string buffer;
  buffer.reserve(std::min<std::size_t>(static_cast<std::size_t>(max_header_bytes) +
                                           static_cast<std::size_t>(max_body_bytes),
                                       16384));
  std::optional<std::size_t> expected_total;

  while (true) {
    if (!expected_total.has_value()) {
      const std::size_t header_end = buffer.find("\r\n\r\n");
      if (header_end != std::string::npos) {
        if (header_end + 4 > static_cast<std::size_t>(max_header_bytes)) {
          co_return err_result<HttpRequest>("HTTP headers exceed configured limit");
        }
        auto framing = nebula::rt::parse_http_request_framing(std::string_view(buffer).substr(0, header_end),
                                                              static_cast<std::size_t>(max_body_bytes));
        if (result_is_err(framing)) {
          co_return err_result<HttpRequest>(
              result_err_move(framing));
        }
        const auto parsed_framing =
            result_ok_move(framing);
        expected_total = header_end + 4 + parsed_framing.content_length;
        if (buffer.size() >= *expected_total) {
          if (buffer.size() > *expected_total) {
            co_return err_result<HttpRequest>("unexpected extra bytes after HTTP request body");
          }
          co_return nebula::rt::parse_http_request_message(buffer,
                                                           static_cast<std::size_t>(max_body_bytes));
        }
      } else if (buffer.size() > static_cast<std::size_t>(max_header_bytes)) {
        co_return err_result<HttpRequest>("HTTP headers exceed configured limit");
      }
    } else if (buffer.size() >= *expected_total) {
      if (buffer.size() > *expected_total) {
        co_return err_result<HttpRequest>("unexpected extra bytes after HTTP request body");
      }
      co_return nebula::rt::parse_http_request_message(buffer,
                                                       static_cast<std::size_t>(max_body_bytes));
    }

    char chunk[4096];
    const int rc =
        mbedtls_ssl_read(&self.state->ssl, reinterpret_cast<unsigned char*>(chunk), sizeof(chunk));
    if (rc > 0) {
      buffer.append(chunk, static_cast<std::size_t>(rc));
      continue;
    }
    if (rc == 0 || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
      co_return err_result<HttpRequest>("unexpected EOF while reading HTTP message");
    }
    if (rc == MBEDTLS_ERR_SSL_WANT_READ) {
      co_await IoAwaitable{self.state->handle->fd, Reactor::Interest::Read};
      continue;
    }
    if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
      co_await IoAwaitable{self.state->handle->fd, Reactor::Interest::Write};
      continue;
    }
    co_return err_result<HttpRequest>("tls read failed: " + mbedtls_error_string(rc));
  }
}

template <typename StreamStatePtr>
Future<Result<void, std::string>> tls_write_http_payload_async(StreamStatePtr state,
                                                               const std::string& payload,
                                                               const char* closed_message) {
  if (state == nullptr || state->handle == nullptr || !state->handle->valid()) {
    co_return nebula::rt::err_void_result(closed_message);
  }
  std::size_t offset = 0;
  while (offset < payload.size()) {
    const int rc = mbedtls_ssl_write(&state->ssl,
                                     reinterpret_cast<const unsigned char*>(payload.data() + offset),
                                     payload.size() - offset);
    if (rc > 0) {
      offset += static_cast<std::size_t>(rc);
      continue;
    }
    if (rc == MBEDTLS_ERR_SSL_WANT_READ) {
      co_await IoAwaitable{state->handle->fd, Reactor::Interest::Read};
      continue;
    }
    if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
      co_await IoAwaitable{state->handle->fd, Reactor::Interest::Write};
      continue;
    }
    co_return nebula::rt::err_void_result("tls write failed: " + mbedtls_error_string(rc));
  }
  co_return nebula::rt::ok_void_result();
}

namespace http2 {

constexpr std::string_view kClientConnectionPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
constexpr std::uint8_t kFlagEndStream = 0x1;
constexpr std::uint8_t kFlagAck = 0x1;
constexpr std::uint8_t kFlagEndHeaders = 0x4;
constexpr std::size_t kFrameHeaderBytes = 9;

enum class FrameType : std::uint8_t {
  Data = 0x0,
  Headers = 0x1,
  Priority = 0x2,
  RstStream = 0x3,
  Settings = 0x4,
  PushPromise = 0x5,
  Ping = 0x6,
  Goaway = 0x7,
  WindowUpdate = 0x8,
  Continuation = 0x9,
};

enum class SettingsId : std::uint16_t {
  HeaderTableSize = 0x1,
  EnablePush = 0x2,
  MaxConcurrentStreams = 0x3,
  InitialWindowSize = 0x4,
  MaxFrameSize = 0x5,
  MaxHeaderListSize = 0x6,
};

struct Frame {
  FrameType type = FrameType::Data;
  std::uint8_t flags = 0;
  std::uint32_t stream_id = 0;
  std::string payload;
};

struct ParsedFrame {
  Frame frame;
  std::size_t consumed = 0;
};

struct HeaderField {
  std::string name;
  std::string value;
};

struct DecodedRequestHead {
  HttpMethod method;
  std::string path;
  std::string authority;
  std::vector<HeaderField> headers;
};

struct DecodedResponseHead {
  std::int64_t status = 0;
  std::vector<HeaderField> headers;
};

struct HpackStaticEntry {
  std::string_view name;
  std::string_view value;
};

constexpr std::array<HpackStaticEntry, 61> kHpackStaticTable{{
    {":authority", ""},
    {":method", "GET"},
    {":method", "POST"},
    {":path", "/"},
    {":path", "/index.html"},
    {":scheme", "http"},
    {":scheme", "https"},
    {":status", "200"},
    {":status", "204"},
    {":status", "206"},
    {":status", "304"},
    {":status", "400"},
    {":status", "404"},
    {":status", "500"},
    {"accept-charset", ""},
    {"accept-encoding", "gzip, deflate"},
    {"accept-language", ""},
    {"accept-ranges", ""},
    {"accept", ""},
    {"access-control-allow-origin", ""},
    {"age", ""},
    {"allow", ""},
    {"authorization", ""},
    {"cache-control", ""},
    {"content-disposition", ""},
    {"content-encoding", ""},
    {"content-language", ""},
    {"content-length", ""},
    {"content-location", ""},
    {"content-range", ""},
    {"content-type", ""},
    {"cookie", ""},
    {"date", ""},
    {"etag", ""},
    {"expect", ""},
    {"expires", ""},
    {"from", ""},
    {"host", ""},
    {"if-match", ""},
    {"if-modified-since", ""},
    {"if-none-match", ""},
    {"if-range", ""},
    {"if-unmodified-since", ""},
    {"last-modified", ""},
    {"link", ""},
    {"location", ""},
    {"max-forwards", ""},
    {"proxy-authenticate", ""},
    {"proxy-authorization", ""},
    {"range", ""},
    {"referer", ""},
    {"refresh", ""},
    {"retry-after", ""},
    {"server", ""},
    {"set-cookie", ""},
    {"strict-transport-security", ""},
    {"transfer-encoding", ""},
    {"user-agent", ""},
    {"vary", ""},
    {"via", ""},
    {"www-authenticate", ""},
}};

std::string frame_type_name(FrameType type) {
  switch (type) {
    case FrameType::Data:
      return "DATA";
    case FrameType::Headers:
      return "HEADERS";
    case FrameType::Priority:
      return "PRIORITY";
    case FrameType::RstStream:
      return "RST_STREAM";
    case FrameType::Settings:
      return "SETTINGS";
    case FrameType::PushPromise:
      return "PUSH_PROMISE";
    case FrameType::Ping:
      return "PING";
    case FrameType::Goaway:
      return "GOAWAY";
    case FrameType::WindowUpdate:
      return "WINDOW_UPDATE";
    case FrameType::Continuation:
      return "CONTINUATION";
  }
  return "UNKNOWN";
}

constexpr std::size_t kHttp2DebugEventLimit = 24;

std::uint32_t frame_error_code(FrameType type, std::string_view payload) {
  if (type == FrameType::Goaway) {
    if (payload.size() < 8) return 0;
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(payload[4])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(payload[5])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(payload[6])) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(payload[7]));
  }
  if (type == FrameType::RstStream) {
    if (payload.size() < 4) return 0;
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(payload[0])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(payload[1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(payload[2])) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(payload[3]));
  }
  return 0;
}

void append_http2_debug_event(nebula::rt::Http2SessionState& session,
                              std::string_view kind,
                              std::string_view direction,
                              std::string_view frame_type,
                              std::uint32_t stream_id,
                              std::uint8_t flags,
                              std::uint32_t error_code = 0) {
  nebula::rt::Http2DebugEvent event;
  event.seq = session.next_debug_event_seq++;
  event.kind = std::string(kind);
  event.direction = std::string(direction);
  event.frame_type = std::string(frame_type);
  event.stream_id = stream_id;
  event.flags = flags;
  event.connection_window = session.peer_connection_window;
  event.stream_window = session.peer_stream_window;
  event.error_code = error_code;
  if (session.recent_events.size() >= kHttp2DebugEventLimit) {
    session.recent_events.erase(session.recent_events.begin());
    session.debug_timeline_overflowed = true;
  }
  session.recent_events.push_back(std::move(event));
}

void append_http2_phase_event(nebula::rt::Http2SessionState& session,
                              std::string_view phase,
                              std::string_view direction,
                              std::string_view frame_type,
                              std::uint32_t stream_id,
                              std::string_view reason,
                              std::string_view detail,
                              std::uint32_t error_code = 0) {
  nebula::rt::Http2DebugEvent event;
  event.seq = session.next_debug_event_seq++;
  event.kind = std::string(phase);
  event.direction = std::string(direction);
  event.frame_type = std::string(frame_type);
  event.reason = std::string(reason);
  event.detail = std::string(detail);
  event.stream_id = stream_id;
  event.connection_window = session.peer_connection_window;
  event.stream_window = session.peer_stream_window;
  event.error_code = error_code;
  if (session.recent_events.size() >= kHttp2DebugEventLimit) {
    session.recent_events.erase(session.recent_events.begin());
    session.debug_timeline_overflowed = true;
  }
  session.recent_events.push_back(std::move(event));
}

std::string concurrent_stream_error(std::string_view phase,
                                    std::uint32_t active_stream_id,
                                    const Frame& frame) {
  return "HTTP/2 concurrent streams are unsupported in the current single-stream preview during " +
         std::string(phase) +
         ": active_stream_id=" + std::to_string(active_stream_id) +
         " incoming_stream_id=" + std::to_string(frame.stream_id) +
         " frame_type=" + frame_type_name(frame.type);
}

std::string http2_error_code_name(std::uint32_t code) {
  switch (code) {
    case 0x0:
      return "NO_ERROR";
    case 0x1:
      return "PROTOCOL_ERROR";
    case 0x2:
      return "INTERNAL_ERROR";
    case 0x3:
      return "FLOW_CONTROL_ERROR";
    case 0x4:
      return "SETTINGS_TIMEOUT";
    case 0x5:
      return "STREAM_CLOSED";
    case 0x6:
      return "FRAME_SIZE_ERROR";
    case 0x7:
      return "REFUSED_STREAM";
    case 0x8:
      return "CANCEL";
    case 0x9:
      return "COMPRESSION_ERROR";
    case 0xa:
      return "CONNECT_ERROR";
    case 0xb:
      return "ENHANCE_YOUR_CALM";
    case 0xc:
      return "INADEQUATE_SECURITY";
    case 0xd:
      return "HTTP_1_1_REQUIRED";
  }
  return "UNKNOWN_ERROR";
}

nebula::rt::JsonValue http2_preface_debug_json(const nebula::rt::Http2SessionState& session) {
  return nebula::rt::json_object2("client_preface_sent",
                                  nebula::rt::json_bool_value(session.client_preface_sent),
                                  "client_preface_received",
                                  nebula::rt::json_bool_value(session.client_preface_received));
}

nebula::rt::JsonValue http2_settings_debug_json(const nebula::rt::Http2SessionState& session) {
  return nebula::rt::json_object4(
      "local_settings_sent",
      nebula::rt::json_bool_value(session.local_settings_sent),
      "local_settings_acknowledged",
      nebula::rt::json_bool_value(session.local_settings_acknowledged),
      "remote_settings_seen",
      nebula::rt::json_bool_value(session.remote_settings_seen),
      "peer_max_concurrent_streams",
      nebula::rt::json_int_value(static_cast<std::int64_t>(session.peer_max_concurrent_streams)));
}

nebula::rt::JsonValue http2_streams_debug_json(const nebula::rt::Http2SessionState& session) {
  return nebula::rt::json_object3(
      "next_local_stream_id",
      nebula::rt::json_int_value(static_cast<std::int64_t>(session.next_local_stream_id)),
      "active_stream_id",
      nebula::rt::json_int_value(static_cast<std::int64_t>(session.active_stream_id)),
      "last_peer_stream_id",
      nebula::rt::json_int_value(static_cast<std::int64_t>(session.last_peer_stream_id)));
}

nebula::rt::JsonValue http2_flow_control_debug_json(const nebula::rt::Http2SessionState& session) {
  return nebula::rt::json_object4(
      "connection_window",
      nebula::rt::json_int_value(session.peer_connection_window),
      "stream_window",
      nebula::rt::json_int_value(session.peer_stream_window),
      "initial_stream_window",
      nebula::rt::json_int_value(session.peer_initial_stream_window),
      "max_frame_size",
      nebula::rt::json_int_value(static_cast<std::int64_t>(session.peer_max_frame_size)));
}

nebula::rt::JsonValue http2_goaway_debug_json(const nebula::rt::Http2SessionState& session) {
  return nebula::rt::json_object5(
      "sent",
      nebula::rt::json_bool_value(session.goaway_sent),
      "received",
      nebula::rt::json_bool_value(session.goaway_received),
      "last_stream_id",
      nebula::rt::json_int_value(static_cast<std::int64_t>(session.goaway_last_stream_id)),
      "error_code",
      nebula::rt::json_int_value(static_cast<std::int64_t>(session.goaway_error_code)),
      "error_name",
      nebula::rt::json_string_value(http2_error_code_name(session.goaway_error_code)));
}

nebula::rt::JsonValue http2_reset_debug_json(const nebula::rt::Http2SessionState& session) {
  return nebula::rt::json_object3(
      "stream_id",
      nebula::rt::json_int_value(static_cast<std::int64_t>(session.last_reset_stream_id)),
      "error_code",
      nebula::rt::json_int_value(static_cast<std::int64_t>(session.last_reset_error_code)),
      "error_name",
      nebula::rt::json_string_value(http2_error_code_name(session.last_reset_error_code)));
}

nebula::rt::JsonValue http2_event_flags_json(const nebula::rt::Http2DebugEvent& event) {
  return nebula::rt::json_object4("raw",
                                  nebula::rt::json_int_value(static_cast<std::int64_t>(event.flags)),
                                  "ack",
                                  nebula::rt::json_bool_value((event.flags & kFlagAck) != 0),
                                  "end_headers",
                                  nebula::rt::json_bool_value((event.flags & kFlagEndHeaders) != 0),
                                  "end_stream",
                                  nebula::rt::json_bool_value((event.flags & kFlagEndStream) != 0));
}

nebula::rt::JsonValue http2_event_flow_control_json(const nebula::rt::Http2DebugEvent& event) {
  return nebula::rt::json_object4("connection_window",
                                  nebula::rt::json_int_value(event.connection_window),
                                  "stream_window",
                                  nebula::rt::json_int_value(event.stream_window),
                                  "error_code",
                                  nebula::rt::json_int_value(static_cast<std::int64_t>(event.error_code)),
                                  "error_name",
                                  nebula::rt::json_string_value(http2_error_code_name(event.error_code)));
}

bool http2_event_has_classification(const nebula::rt::Http2DebugEvent& event) {
  return !event.reason.empty() || !event.detail.empty();
}

nebula::rt::JsonValue http2_event_classification_json(const nebula::rt::Http2DebugEvent& event) {
  return nebula::rt::json_object2("reason",
                                  nebula::rt::json_string_value(event.reason),
                                  "detail",
                                  nebula::rt::json_string_value(event.detail));
}

nebula::rt::JsonValue http2_timeline_debug_json(const nebula::rt::Http2SessionState& session) {
  auto events = nebula::rt::json_array_builder();
  for (const auto& event : session.recent_events) {
    nebula::rt::JsonValue event_json;
    if (http2_event_has_classification(event)) {
      event_json = nebula::rt::json_object8("seq",
                                            nebula::rt::json_int_value(static_cast<std::int64_t>(event.seq)),
                                            "kind",
                                            nebula::rt::json_string_value(event.kind),
                                            "direction",
                                            nebula::rt::json_string_value(event.direction),
                                            "frame_type",
                                            nebula::rt::json_string_value(event.frame_type),
                                            "stream_id",
                                            nebula::rt::json_int_value(static_cast<std::int64_t>(event.stream_id)),
                                            "classification",
                                            http2_event_classification_json(event),
                                            "flags",
                                            http2_event_flags_json(event),
                                            "flow_control",
                                            http2_event_flow_control_json(event));
    } else {
      event_json = nebula::rt::json_object7("seq",
                                            nebula::rt::json_int_value(static_cast<std::int64_t>(event.seq)),
                                            "kind",
                                            nebula::rt::json_string_value(event.kind),
                                            "direction",
                                            nebula::rt::json_string_value(event.direction),
                                            "frame_type",
                                            nebula::rt::json_string_value(event.frame_type),
                                            "stream_id",
                                            nebula::rt::json_int_value(static_cast<std::int64_t>(event.stream_id)),
                                            "flags",
                                            http2_event_flags_json(event),
                                            "flow_control",
                                            http2_event_flow_control_json(event));
    }
    events = nebula::rt::json_array_push(std::move(events), std::move(event_json));
  }
  return nebula::rt::json_object2("overflowed",
                                  nebula::rt::json_bool_value(session.debug_timeline_overflowed),
                                  "recent_events",
                                  nebula::rt::json_array_build(std::move(events)));
}

nebula::rt::JsonValue http2_debug_state_json(const nebula::rt::Http2SessionState& session) {
  return nebula::rt::json_object7("preface",
                                  http2_preface_debug_json(session),
                                  "settings",
                                  http2_settings_debug_json(session),
                                  "streams",
                                  http2_streams_debug_json(session),
                                  "flow_control",
                                  http2_flow_control_debug_json(session),
                                  "goaway",
                                  http2_goaway_debug_json(session),
                                  "reset",
                                  http2_reset_debug_json(session),
                                  "timeline",
                                  http2_timeline_debug_json(session));
}

std::string flow_control_wait_error(const nebula::rt::Http2SessionState& session,
                                    const Frame& frame) {
  return "HTTP/2 flow-control send wait received an unexpected frame: active_stream_id=" +
         std::to_string(session.active_stream_id) +
         " connection_window=" + std::to_string(session.peer_connection_window) +
         " stream_window=" + std::to_string(session.peer_stream_window) +
         " peer_max_concurrent_streams=" +
         (session.peer_max_concurrent_streams == 0
              ? std::string("unbounded")
              : std::to_string(session.peer_max_concurrent_streams)) +
         " incoming_stream_id=" + std::to_string(frame.stream_id) +
         " frame_type=" + frame_type_name(frame.type);
}

template <typename StatePtr>
Future<Result<std::size_t, std::string>> read_more_async(StatePtr state,
                                                         std::string& buffer,
                                                         const char* closed_message) {
  if (state == nullptr || state->handle == nullptr || !state->handle->valid()) {
    co_return err_result<std::size_t>(closed_message);
  }
  char chunk[4096];
  while (true) {
    const int rc =
        mbedtls_ssl_read(&state->ssl, reinterpret_cast<unsigned char*>(chunk), sizeof(chunk));
    if (rc > 0) {
      buffer.append(chunk, static_cast<std::size_t>(rc));
      co_return nebula::rt::ok_result<std::size_t>(static_cast<std::size_t>(rc));
    }
    if (rc == 0 || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
      co_return nebula::rt::ok_result<std::size_t>(0);
    }
    if (rc == MBEDTLS_ERR_SSL_WANT_READ) {
      co_await IoAwaitable{state->handle->fd, Reactor::Interest::Read};
      continue;
    }
    if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
      co_await IoAwaitable{state->handle->fd, Reactor::Interest::Write};
      continue;
    }
    co_return err_result<std::size_t>("tls read failed: " + mbedtls_error_string(rc));
  }
}

std::string h2_protocol_error(std::string_view protocol) {
  if (protocol != "h2") {
    return "HTTP/2 over TLS requires negotiated ALPN protocol h2";
  }
  return "";
}

Result<std::optional<ParsedFrame>, std::string> try_parse_frame(std::string_view buffer) {
  if (buffer.size() < kFrameHeaderBytes) {
    return nebula::rt::ok_result(std::optional<ParsedFrame>{});
  }
  const auto length = (static_cast<std::uint32_t>(static_cast<unsigned char>(buffer[0])) << 16) |
                      (static_cast<std::uint32_t>(static_cast<unsigned char>(buffer[1])) << 8) |
                      static_cast<std::uint32_t>(static_cast<unsigned char>(buffer[2]));
  if (buffer.size() < kFrameHeaderBytes + length) {
    return nebula::rt::ok_result(std::optional<ParsedFrame>{});
  }
  ParsedFrame out;
  out.frame.type = static_cast<FrameType>(static_cast<unsigned char>(buffer[3]));
  out.frame.flags = static_cast<std::uint8_t>(buffer[4]);
  out.frame.stream_id =
      ((static_cast<std::uint32_t>(static_cast<unsigned char>(buffer[5])) << 24) |
       (static_cast<std::uint32_t>(static_cast<unsigned char>(buffer[6])) << 16) |
       (static_cast<std::uint32_t>(static_cast<unsigned char>(buffer[7])) << 8) |
       static_cast<std::uint32_t>(static_cast<unsigned char>(buffer[8]))) &
      0x7fffffffU;
  out.frame.payload.assign(buffer.substr(kFrameHeaderBytes, length));
  out.consumed = kFrameHeaderBytes + length;
  return nebula::rt::ok_result(std::optional<ParsedFrame>{std::move(out)});
}

std::string build_frame(FrameType type,
                        std::uint8_t flags,
                        std::uint32_t stream_id,
                        std::string_view payload) {
  std::string out;
  out.reserve(kFrameHeaderBytes + payload.size());
  const auto length = static_cast<std::uint32_t>(payload.size());
  out.push_back(static_cast<char>((length >> 16) & 0xff));
  out.push_back(static_cast<char>((length >> 8) & 0xff));
  out.push_back(static_cast<char>(length & 0xff));
  out.push_back(static_cast<char>(type));
  out.push_back(static_cast<char>(flags));
  const auto sid = stream_id & 0x7fffffffU;
  out.push_back(static_cast<char>((sid >> 24) & 0x7f));
  out.push_back(static_cast<char>((sid >> 16) & 0xff));
  out.push_back(static_cast<char>((sid >> 8) & 0xff));
  out.push_back(static_cast<char>(sid & 0xff));
  out.append(payload);
  return out;
}

template <typename StatePtr>
Future<Result<void, std::string>> write_frame_async(StatePtr state,
                                                    nebula::rt::Http2SessionState& session,
                                                    FrameType type,
                                                    std::uint8_t flags,
                                                    std::uint32_t stream_id,
                                                    std::string_view payload,
                                                    const char* closed_message) {
  const std::string encoded = build_frame(type, flags, stream_id, payload);
  auto wrote = co_await tls_write_http_payload_async(state, encoded, closed_message);
  if (result_is_err(wrote)) {
    co_return wrote;
  }
  append_http2_debug_event(session,
                           "frame",
                           "outbound",
                           frame_type_name(type),
                           stream_id,
                           flags,
                           frame_error_code(type, payload));
  co_return nebula::rt::ok_void_result();
}

template <typename StatePtr>
Future<Result<Frame, std::string>> read_frame_async(StatePtr state,
                                                    nebula::rt::Http2SessionState& session,
                                                    const char* closed_message) {
  while (true) {
    auto parsed = try_parse_frame(session.read_buffer);
    if (result_is_err(parsed)) {
      co_return err_result<Frame>(
          result_err_move(parsed));
    }
    auto maybe_frame =
        result_ok_move(parsed);
    if (maybe_frame.has_value()) {
      Frame frame = std::move(maybe_frame->frame);
      session.read_buffer.erase(0, maybe_frame->consumed);
      append_http2_debug_event(session,
                               "frame",
                               "inbound",
                               frame_type_name(frame.type),
                               frame.stream_id,
                               frame.flags,
                               frame_error_code(frame.type, frame.payload));
      co_return nebula::rt::ok_result(std::move(frame));
    }
    auto read = co_await read_more_async(state, session.read_buffer, closed_message);
    if (result_is_err(read)) {
      co_return err_result<Frame>(
          result_err_move(read));
    }
    const auto bytes =
        result_ok_move(read);
    if (bytes == 0) {
      co_return err_result<Frame>("unexpected EOF while reading HTTP/2 frame");
    }
  }
}

void append_prefixed_int(std::string& out,
                         std::uint8_t prefix_bits,
                         std::uint8_t prefix_mask,
                         std::uint32_t value) {
  const std::uint32_t prefix_max = (1u << prefix_bits) - 1u;
  if (value < prefix_max) {
    out.push_back(static_cast<char>(prefix_mask | static_cast<std::uint8_t>(value)));
    return;
  }
  out.push_back(static_cast<char>(prefix_mask | static_cast<std::uint8_t>(prefix_max)));
  value -= prefix_max;
  while (value >= 128) {
    out.push_back(static_cast<char>((value % 128) + 128));
    value /= 128;
  }
  out.push_back(static_cast<char>(value));
}

Result<std::uint32_t, std::string> decode_prefixed_int(std::string_view data,
                                                       std::size_t& cursor,
                                                       std::uint8_t prefix_bits) {
  if (cursor >= data.size()) {
    return err_result<std::uint32_t>("truncated HPACK integer");
  }
  const std::uint8_t byte = static_cast<std::uint8_t>(data[cursor]);
  const std::uint32_t prefix_max = (1u << prefix_bits) - 1u;
  std::uint32_t value = byte & prefix_max;
  cursor += 1;
  if (value < prefix_max) {
    return nebula::rt::ok_result(value);
  }
  std::uint32_t shift = 0;
  while (true) {
    if (cursor >= data.size()) {
      return err_result<std::uint32_t>("truncated HPACK integer");
    }
    const std::uint8_t next = static_cast<std::uint8_t>(data[cursor]);
    cursor += 1;
    value += static_cast<std::uint32_t>(next & 0x7f) << shift;
    if ((next & 0x80) == 0) {
      return nebula::rt::ok_result(value);
    }
    shift += 7;
    if (shift > 28) {
      return err_result<std::uint32_t>("HPACK integer is too large");
    }
  }
}

void append_hpack_string(std::string& out, std::string_view text) {
  append_prefixed_int(out, 7, 0x00, static_cast<std::uint32_t>(text.size()));
  out.append(text);
}

Result<std::string, std::string> decode_hpack_string(std::string_view data, std::size_t& cursor) {
  if (cursor >= data.size()) {
    return err_result<std::string>("truncated HPACK string");
  }
  const bool huffman = (static_cast<std::uint8_t>(data[cursor]) & 0x80) != 0;
  auto length = decode_prefixed_int(data, cursor, 7);
  if (result_is_err(length)) {
    return err_result<std::string>(
        result_err_move(length));
  }
  const auto size =
      result_ok_move(length);
  if (huffman) {
    return err_result<std::string>("HPACK Huffman strings are unsupported");
  }
  if (cursor + size > data.size()) {
    return err_result<std::string>("truncated HPACK string");
  }
  std::string out(data.substr(cursor, size));
  cursor += size;
  return nebula::rt::ok_result(std::move(out));
}

Result<HpackStaticEntry, std::string> static_entry(std::uint32_t index) {
  if (index == 0 || index > kHpackStaticTable.size()) {
    return err_result<HpackStaticEntry>("HPACK dynamic table indices are unsupported");
  }
  return nebula::rt::ok_result(kHpackStaticTable[index - 1]);
}

std::uint32_t find_exact_index(std::string_view name, std::string_view value) {
  for (std::uint32_t i = 0; i < kHpackStaticTable.size(); ++i) {
    if (kHpackStaticTable[i].name == name && kHpackStaticTable[i].value == value) {
      return i + 1;
    }
  }
  return 0;
}

std::uint32_t find_name_index(std::string_view name) {
  for (std::uint32_t i = 0; i < kHpackStaticTable.size(); ++i) {
    if (kHpackStaticTable[i].name == name) {
      return i + 1;
    }
  }
  return 0;
}

void append_indexed_field(std::string& out, std::uint32_t index) {
  append_prefixed_int(out, 7, 0x80, index);
}

void append_literal_without_indexing(std::string& out,
                                     std::string_view name,
                                     std::string_view value) {
  const auto name_index = find_name_index(name);
  if (name_index != 0) {
    append_prefixed_int(out, 4, 0x00, name_index);
  } else {
    append_prefixed_int(out, 4, 0x00, 0);
    append_hpack_string(out, name);
  }
  append_hpack_string(out, value);
}

Result<std::vector<HeaderField>, std::string> decode_header_block(std::string_view block) {
  std::vector<HeaderField> out;
  std::size_t cursor = 0;
  while (cursor < block.size()) {
    const std::uint8_t byte = static_cast<std::uint8_t>(block[cursor]);
    if ((byte & 0x80) != 0) {
      auto index = decode_prefixed_int(block, cursor, 7);
      if (result_is_err(index)) {
        return err_result<std::vector<HeaderField>>(
            result_err_move(index));
      }
      auto entry = static_entry(
          result_ok_move(index));
      if (result_is_err(entry)) {
        return err_result<std::vector<HeaderField>>(
            result_err_move(entry));
      }
      const auto value =
          result_ok_move(entry);
      out.push_back(HeaderField{std::string(value.name), std::string(value.value)});
      continue;
    }

    if ((byte & 0x20) != 0) {
      auto size = decode_prefixed_int(block, cursor, 5);
      if (result_is_err(size)) {
        return err_result<std::vector<HeaderField>>(
            result_err_move(size));
      }
      const auto value =
          result_ok_move(size);
      if (value != 0) {
        return err_result<std::vector<HeaderField>>("HPACK dynamic table size updates are unsupported");
      }
      continue;
    }

    const auto prefix_bits = (byte & 0x40) != 0 ? 6u : 4u;
    auto name_index = decode_prefixed_int(block, cursor, prefix_bits);
    if (result_is_err(name_index)) {
      return err_result<std::vector<HeaderField>>(
          result_err_move(name_index));
    }
    const auto index =
        result_ok_move(name_index);
    std::string name;
    if (index == 0) {
      auto decoded_name = decode_hpack_string(block, cursor);
      if (result_is_err(decoded_name)) {
        return err_result<std::vector<HeaderField>>(
            result_err_move(decoded_name));
      }
      name = result_ok_move(decoded_name);
    } else {
      auto entry = static_entry(index);
      if (result_is_err(entry)) {
        return err_result<std::vector<HeaderField>>(
            result_err_move(entry));
      }
      name =
          std::string(result_ok_move(entry).name);
    }

    auto value = decode_hpack_string(block, cursor);
    if (result_is_err(value)) {
      return err_result<std::vector<HeaderField>>(
          result_err_move(value));
    }
    out.push_back(HeaderField{
        std::move(name),
        result_ok_move(value),
    });
  }
  return nebula::rt::ok_result(std::move(out));
}

std::string encode_header_block(const std::vector<HeaderField>& headers) {
  std::string out;
  for (const auto& header : headers) {
    const auto exact_index = find_exact_index(header.name, header.value);
    if (exact_index != 0) {
      append_indexed_field(out, exact_index);
    } else {
      append_literal_without_indexing(out, header.name, header.value);
    }
  }
  return out;
}

bool is_connection_specific_header(std::string_view name) {
  return nebula::rt::ascii_ieq(name, "connection") ||
         nebula::rt::ascii_ieq(name, "proxy-connection") ||
         nebula::rt::ascii_ieq(name, "keep-alive") ||
         nebula::rt::ascii_ieq(name, "upgrade") ||
         nebula::rt::ascii_ieq(name, "transfer-encoding");
}

Result<std::vector<HeaderField>, std::string> parse_raw_headers(std::string_view raw_headers) {
  std::vector<HeaderField> out;
  std::size_t cursor = 0;
  while (cursor < raw_headers.size()) {
    const auto line_end = raw_headers.find("\r\n", cursor);
    const auto line = line_end == std::string::npos ? raw_headers.substr(cursor)
                                                    : raw_headers.substr(cursor, line_end - cursor);
    if (!line.empty()) {
      const auto colon = line.find(':');
      if (colon == std::string::npos) {
        return err_result<std::vector<HeaderField>>("malformed HTTP header line");
      }
      const auto name = nebula::rt::trim_http_token_view(line.substr(0, colon));
      if (!nebula::rt::http_header_name_is_valid(name)) {
        return err_result<std::vector<HeaderField>>("invalid HTTP header name");
      }
      std::string lowered = nebula::rt::ascii_lower_copy(name);
      std::string value = nebula::rt::trim_http_token(line.substr(colon + 1));
      if (is_connection_specific_header(lowered)) {
        return err_result<std::vector<HeaderField>>(
            "HTTP/2 forbids connection-specific header: " + lowered);
      }
      if (lowered == "te" && !nebula::rt::ascii_ieq(value, "trailers")) {
        return err_result<std::vector<HeaderField>>("HTTP/2 only permits TE: trailers");
      }
      out.push_back(HeaderField{std::move(lowered), std::move(value)});
    }
    if (line_end == std::string::npos) {
      break;
    }
    cursor = line_end + 2;
  }
  return nebula::rt::ok_result(std::move(out));
}

bool header_list_has(std::string_view headers, std::string_view name) {
  return nebula::rt::parse_http_header_value(headers, name).has_value();
}

std::string build_headers_text(const std::vector<HeaderField>& headers) {
  std::string out;
  for (const auto& header : headers) {
    out += header.name;
    out += ": ";
    out += header.value;
    out += "\r\n";
  }
  return out;
}

Result<DecodedRequestHead, std::string> decode_request_head(std::string_view block) {
  auto decoded = decode_header_block(block);
  if (result_is_err(decoded)) {
    return err_result<DecodedRequestHead>(
        result_err_move(decoded));
  }
  auto fields =
      result_ok_move(decoded);

  bool saw_regular = false;
  std::optional<std::string> method_text;
  std::optional<std::string> path;
  std::optional<std::string> authority;
  for (const auto& field : fields) {
    if (!field.name.empty() && field.name.front() == ':') {
      if (saw_regular) {
        return err_result<DecodedRequestHead>("HTTP/2 pseudo-headers must precede regular headers");
      }
      if (field.name == ":method") {
        method_text = field.value;
      } else if (field.name == ":path") {
        path = field.value;
      } else if (field.name == ":authority") {
        authority = field.value;
      } else if (field.name == ":scheme") {
        if (!field.value.empty() && !nebula::rt::ascii_ieq(field.value, "https")) {
          return err_result<DecodedRequestHead>("unsupported HTTP/2 request scheme");
        }
      } else {
        return err_result<DecodedRequestHead>("unsupported HTTP/2 request pseudo-header");
      }
      continue;
    }
    saw_regular = true;
  }

  if (!method_text.has_value() || !path.has_value()) {
    return err_result<DecodedRequestHead>("HTTP/2 request missing required pseudo-headers");
  }
  auto method = nebula::rt::parse_http_method(*method_text);
  if (result_is_err(method)) {
    return err_result<DecodedRequestHead>(
        result_err_move(method));
  }
  if (!nebula::rt::http_request_path_is_valid(*path)) {
    return err_result<DecodedRequestHead>("invalid HTTP/2 request path");
  }

  DecodedRequestHead out;
  out.method = result_ok_move(method);
  out.path = *path;
  out.authority = authority.value_or("");
  for (const auto& field : fields) {
    if (!field.name.empty() && field.name.front() == ':') {
      continue;
    }
    if (is_connection_specific_header(field.name)) {
      return err_result<DecodedRequestHead>("HTTP/2 request contains connection-specific headers");
    }
    out.headers.push_back(field);
  }
  return nebula::rt::ok_result(std::move(out));
}

Result<DecodedResponseHead, std::string> decode_response_head(std::string_view block) {
  auto decoded = decode_header_block(block);
  if (result_is_err(decoded)) {
    return err_result<DecodedResponseHead>(
        result_err_move(decoded));
  }
  auto fields =
      result_ok_move(decoded);

  bool saw_regular = false;
  std::optional<std::string> status_text;
  DecodedResponseHead out;
  for (const auto& field : fields) {
    if (!field.name.empty() && field.name.front() == ':') {
      if (saw_regular) {
        return err_result<DecodedResponseHead>("HTTP/2 pseudo-headers must precede regular headers");
      }
      if (field.name != ":status") {
        return err_result<DecodedResponseHead>("unsupported HTTP/2 response pseudo-header");
      }
      status_text = field.value;
      continue;
    }
    saw_regular = true;
    if (is_connection_specific_header(field.name)) {
      return err_result<DecodedResponseHead>("HTTP/2 response contains connection-specific headers");
    }
    out.headers.push_back(field);
  }

  if (!status_text.has_value()) {
    return err_result<DecodedResponseHead>("HTTP/2 response missing :status pseudo-header");
  }
  std::int64_t status = 0;
  const auto* begin = status_text->data();
  const auto* end = begin + status_text->size();
  const auto parsed = std::from_chars(begin, end, status);
  if (parsed.ec != std::errc{} || parsed.ptr != end) {
    return err_result<DecodedResponseHead>("invalid HTTP/2 response status");
  }
  out.status = status;
  return nebula::rt::ok_result(std::move(out));
}

Result<std::string, std::string> request_headers_text(const DecodedRequestHead& head, std::string_view body) {
  std::string headers = build_headers_text(head.headers);
  if (!head.authority.empty() && !header_list_has(headers, "host")) {
    headers = "host: " + head.authority + "\r\n" + headers;
  }
  if (!header_list_has(headers, "content-length")) {
    headers += "content-length: " + std::to_string(body.size()) + "\r\n";
  }
  return nebula::rt::ok_result(std::move(headers));
}

Result<std::string, std::string> response_headers_text(const DecodedResponseHead& head, std::string_view body) {
  std::string headers = build_headers_text(head.headers);
  if (!header_list_has(headers, "content-length")) {
    headers += "content-length: " + std::to_string(body.size()) + "\r\n";
  }
  return nebula::rt::ok_result(std::move(headers));
}

Result<std::vector<HeaderField>, std::string> encode_request_headers(const HttpClientRequest& request) {
  if (!nebula::rt::http_request_authority_is_valid(request.authority)) {
    return err_result<std::vector<HeaderField>>("invalid HTTP authority");
  }
  if (!nebula::rt::http_request_path_is_valid(request.path)) {
    return err_result<std::vector<HeaderField>>("invalid HTTP request path");
  }
  if (!request.content_type.empty() && !nebula::rt::http_content_type_is_valid(request.content_type)) {
    return err_result<std::vector<HeaderField>>("invalid Content-Type header");
  }
  auto extras = parse_raw_headers(request.headers);
  if (result_is_err(extras)) {
    return err_result<std::vector<HeaderField>>(
        result_err_move(extras));
  }

  std::vector<HeaderField> out;
  out.push_back(HeaderField{":method", nebula::rt::render_http_method(request.method)});
  out.push_back(HeaderField{":scheme", "https"});
  out.push_back(HeaderField{":path", request.path});
  out.push_back(HeaderField{":authority", request.authority});
  if (!request.content_type.empty()) {
    out.push_back(HeaderField{"content-type", request.content_type});
  }
  out.push_back(HeaderField{"content-length", std::to_string(request.body.data.size())});

  for (auto& header :
       result_ok_move(extras)) {
    if (header.name == "host") {
      if (!nebula::rt::ascii_ieq(header.value, request.authority)) {
        return err_result<std::vector<HeaderField>>("Host header does not match request authority");
      }
      continue;
    }
    if (header.name == "content-length") {
      if (header.value != std::to_string(request.body.data.size())) {
        return err_result<std::vector<HeaderField>>("Content-Length header does not match request body");
      }
      continue;
    }
    if (header.name == "content-type" && !request.content_type.empty()) {
      if (header.value != request.content_type) {
        return err_result<std::vector<HeaderField>>("Content-Type header does not match request content_type");
      }
      continue;
    }
    out.push_back(std::move(header));
  }
  return nebula::rt::ok_result(std::move(out));
}

Result<std::vector<HeaderField>, std::string> encode_response_headers(const HttpResponse& response) {
  auto extras = parse_raw_headers(response.headers);
  if (result_is_err(extras)) {
    return err_result<std::vector<HeaderField>>(
        result_err_move(extras));
  }

  std::vector<HeaderField> out;
  out.push_back(HeaderField{":status", std::to_string(response.status)});
  if (!response.content_type.empty()) {
    out.push_back(HeaderField{"content-type", response.content_type});
  }
  out.push_back(HeaderField{"content-length", std::to_string(response.body.data.size())});

  for (auto& header :
       result_ok_move(extras)) {
    if (header.name == "content-length") {
      if (header.value != std::to_string(response.body.data.size())) {
        return err_result<std::vector<HeaderField>>("Content-Length header does not match response body");
      }
      continue;
    }
    if (header.name == "content-type" && !response.content_type.empty()) {
      if (header.value != response.content_type) {
        return err_result<std::vector<HeaderField>>("Content-Type header does not match response content_type");
      }
      continue;
    }
    out.push_back(std::move(header));
  }
  return nebula::rt::ok_result(std::move(out));
}

template <typename StatePtr>
Future<Result<std::string, std::string>> read_header_block_async(StatePtr state,
                                                                 nebula::rt::Http2SessionState& session,
                                                                 std::uint32_t stream_id,
                                                                 Frame first,
                                                                 const char* closed_message) {
  if (first.type != FrameType::Headers) {
    co_return err_result<std::string>("expected HTTP/2 HEADERS frame");
  }
  std::string block = std::move(first.payload);
  while ((first.flags & kFlagEndHeaders) == 0) {
    auto next = co_await read_frame_async(state, session, closed_message);
    if (result_is_err(next)) {
      co_return err_result<std::string>(
          result_err_move(next));
    }
    first = result_ok_move(next);
    if (first.type != FrameType::Continuation || first.stream_id != stream_id) {
      co_return err_result<std::string>("invalid HTTP/2 CONTINUATION sequence");
    }
    block.append(first.payload);
  }
  co_return nebula::rt::ok_result(std::move(block));
}

Result<void, std::string> apply_setting(nebula::rt::Http2SessionState& session,
                                        SettingsId id,
                                        std::uint32_t value) {
  switch (id) {
    case SettingsId::EnablePush:
      if (value != 0) {
        return nebula::rt::err_void_result("HTTP/2 server push is unsupported");
      }
      return nebula::rt::ok_void_result();
    case SettingsId::InitialWindowSize: {
      if (value > 0x7fffffffU) {
        return nebula::rt::err_void_result("invalid HTTP/2 initial window size");
      }
      const auto delta = static_cast<std::int64_t>(value) - session.peer_initial_stream_window;
      session.peer_initial_stream_window = static_cast<std::int64_t>(value);
      session.peer_stream_window += delta;
      return nebula::rt::ok_void_result();
    }
    case SettingsId::MaxConcurrentStreams:
      session.peer_max_concurrent_streams = value;
      return nebula::rt::ok_void_result();
    case SettingsId::MaxFrameSize:
      if (value < 16384 || value > 16777215) {
        return nebula::rt::err_void_result("invalid HTTP/2 max frame size");
      }
      session.peer_max_frame_size = value;
      return nebula::rt::ok_void_result();
    default:
      return nebula::rt::ok_void_result();
  }
}

template <typename StatePtr>
Future<Result<bool, std::string>> process_control_frame_async(StatePtr state,
                                                              nebula::rt::Http2SessionState& session,
                                                              const Frame& frame,
                                                              const char* closed_message) {
  switch (frame.type) {
    case FrameType::Settings: {
      if (frame.stream_id != 0) {
        co_return err_result<bool>("HTTP/2 SETTINGS frame must use stream 0");
      }
      if ((frame.flags & kFlagAck) != 0) {
        if (!frame.payload.empty()) {
          co_return err_result<bool>("HTTP/2 SETTINGS ack must have an empty payload");
        }
        session.local_settings_acknowledged = true;
        co_return nebula::rt::ok_result(true);
      }
      if (frame.payload.size() % 6 != 0) {
        co_return err_result<bool>("malformed HTTP/2 SETTINGS payload");
      }
      for (std::size_t i = 0; i < frame.payload.size(); i += 6) {
        const auto id =
            static_cast<SettingsId>((static_cast<std::uint16_t>(static_cast<unsigned char>(frame.payload[i])) << 8) |
                                    static_cast<std::uint16_t>(static_cast<unsigned char>(frame.payload[i + 1])));
        const auto value =
            (static_cast<std::uint32_t>(static_cast<unsigned char>(frame.payload[i + 2])) << 24) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(frame.payload[i + 3])) << 16) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(frame.payload[i + 4])) << 8) |
            static_cast<std::uint32_t>(static_cast<unsigned char>(frame.payload[i + 5]));
        auto applied = apply_setting(session, id, value);
        if (result_is_err(applied)) {
          co_return err_result<bool>(
              result_err_move(applied));
        }
      }
      session.remote_settings_seen = true;
      auto ack = co_await write_frame_async(state, session, FrameType::Settings, kFlagAck, 0, "", closed_message);
      if (result_is_err(ack)) {
        co_return err_result<bool>(
            result_err_move(ack));
      }
      co_return nebula::rt::ok_result(true);
    }
    case FrameType::Ping: {
      if (frame.stream_id != 0 || frame.payload.size() != 8) {
        co_return err_result<bool>("malformed HTTP/2 PING frame");
      }
      if ((frame.flags & kFlagAck) == 0) {
        auto ack = co_await write_frame_async(state,
                                              session,
                                              FrameType::Ping,
                                              kFlagAck,
                                              0,
                                              frame.payload,
                                              closed_message);
        if (result_is_err(ack)) {
          co_return err_result<bool>(
              result_err_move(ack));
        }
      }
      co_return nebula::rt::ok_result(true);
    }
    case FrameType::WindowUpdate: {
      if (frame.payload.size() != 4) {
        co_return err_result<bool>("malformed HTTP/2 WINDOW_UPDATE frame");
      }
      const auto increment =
          ((static_cast<std::uint32_t>(static_cast<unsigned char>(frame.payload[0])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(frame.payload[1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(frame.payload[2])) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(frame.payload[3]))) &
          0x7fffffffU;
      if (increment == 0) {
        co_return err_result<bool>("HTTP/2 WINDOW_UPDATE increment must be positive");
      }
      if (frame.stream_id == 0) {
        session.peer_connection_window += static_cast<std::int64_t>(increment);
      } else if (frame.stream_id == session.active_stream_id) {
        session.peer_stream_window += static_cast<std::int64_t>(increment);
      } else {
        co_return err_result<bool>("HTTP/2 WINDOW_UPDATE for an inactive stream is unsupported: active_stream_id=" +
                                   std::to_string(session.active_stream_id) +
                                   " updated_stream_id=" + std::to_string(frame.stream_id));
      }
      co_return nebula::rt::ok_result(true);
    }
    case FrameType::Goaway:
      if (frame.stream_id != 0 || frame.payload.size() < 8) {
        co_return err_result<bool>("malformed HTTP/2 GOAWAY frame");
      }
      session.goaway_received = true;
      session.goaway_last_stream_id =
          ((static_cast<std::uint32_t>(static_cast<unsigned char>(frame.payload[0])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(frame.payload[1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(frame.payload[2])) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(frame.payload[3]))) &
          0x7fffffffU;
      session.goaway_error_code =
          (static_cast<std::uint32_t>(static_cast<unsigned char>(frame.payload[4])) << 24) |
          (static_cast<std::uint32_t>(static_cast<unsigned char>(frame.payload[5])) << 16) |
          (static_cast<std::uint32_t>(static_cast<unsigned char>(frame.payload[6])) << 8) |
          static_cast<std::uint32_t>(static_cast<unsigned char>(frame.payload[7]));
      append_http2_phase_event(session,
                               "goaway_observed",
                               "inbound",
                               "GOAWAY",
                               0,
                               "shutdown",
                               session.goaway_error_code == 0 ? "peer_goaway_no_error"
                                                              : "peer_goaway_error",
                               session.goaway_error_code);
      co_return nebula::rt::ok_result(true);
    case FrameType::RstStream:
      if (frame.payload.size() != 4) {
        co_return err_result<bool>("malformed HTTP/2 RST_STREAM frame");
      }
      session.last_reset_stream_id = frame.stream_id;
      session.last_reset_error_code =
          (static_cast<std::uint32_t>(static_cast<unsigned char>(frame.payload[0])) << 24) |
          (static_cast<std::uint32_t>(static_cast<unsigned char>(frame.payload[1])) << 16) |
          (static_cast<std::uint32_t>(static_cast<unsigned char>(frame.payload[2])) << 8) |
          static_cast<std::uint32_t>(static_cast<unsigned char>(frame.payload[3]));
      if (frame.stream_id == session.active_stream_id) {
        append_http2_phase_event(session,
                                 "stream_closed",
                                 "inbound",
                                 "RST_STREAM",
                                 frame.stream_id,
                                 "reset",
                                 "peer_rst_stream",
                                 session.last_reset_error_code);
        co_return err_result<bool>("HTTP/2 stream reset by peer: stream_id=" +
                                   std::to_string(frame.stream_id) +
                                   " error_code=" + std::to_string(session.last_reset_error_code) +
                                   " error_name=" + http2_error_code_name(session.last_reset_error_code));
      }
      co_return nebula::rt::ok_result(true);
    case FrameType::Priority:
      co_return nebula::rt::ok_result(true);
    case FrameType::PushPromise:
      co_return err_result<bool>("HTTP/2 server push is unsupported");
    default:
      co_return nebula::rt::ok_result(false);
  }
}

template <typename StatePtr>
Future<Result<void, std::string>> ensure_client_started_async(StatePtr state,
                                                              nebula::rt::Http2SessionState& session,
                                                              const char* closed_message) {
  if (!session.client_preface_sent) {
    std::string payload;
    payload.reserve(kClientConnectionPreface.size() + kFrameHeaderBytes);
    payload.append(kClientConnectionPreface);
    payload.append(build_frame(FrameType::Settings, 0, 0, ""));
    auto wrote = co_await tls_write_http_payload_async(state, payload, closed_message);
    if (result_is_err(wrote)) {
      co_return nebula::rt::err_void_result(
          result_err_move(wrote));
    }
    session.client_preface_sent = true;
    session.local_settings_sent = true;
    append_http2_debug_event(session, "preface", "outbound", "PREFACE", 0, 0);
    append_http2_debug_event(session, "frame", "outbound", "SETTINGS", 0, 0);
  }
  co_return nebula::rt::ok_void_result();
}

template <typename StatePtr>
Future<Result<void, std::string>> ensure_server_started_async(StatePtr state,
                                                              nebula::rt::Http2SessionState& session,
                                                              const char* closed_message) {
  if (!session.client_preface_received) {
    while (session.read_buffer.size() < kClientConnectionPreface.size()) {
      auto read = co_await read_more_async(state, session.read_buffer, closed_message);
      if (result_is_err(read)) {
        co_return nebula::rt::err_void_result(
            result_err_move(read));
      }
      const auto bytes =
          result_ok_move(read);
      if (bytes == 0) {
        co_return nebula::rt::err_void_result("unexpected EOF while reading HTTP/2 connection preface");
      }
    }
    if (std::string_view(session.read_buffer).substr(0, kClientConnectionPreface.size()) !=
        kClientConnectionPreface) {
      co_return nebula::rt::err_void_result("invalid HTTP/2 client connection preface");
    }
    session.read_buffer.erase(0, kClientConnectionPreface.size());
    session.client_preface_received = true;
    append_http2_debug_event(session, "preface", "inbound", "PREFACE", 0, 0);
  }
  if (!session.local_settings_sent) {
    auto wrote = co_await write_frame_async(state, session, FrameType::Settings, 0, 0, "", closed_message);
    if (result_is_err(wrote)) {
      co_return nebula::rt::err_void_result(
          result_err_move(wrote));
    }
    session.local_settings_sent = true;
  }
  co_return nebula::rt::ok_void_result();
}

template <typename StatePtr>
Future<Result<void, std::string>> write_window_update_async(StatePtr state,
                                                            nebula::rt::Http2SessionState& session,
                                                            std::uint32_t stream_id,
                                                            std::uint32_t increment,
                                                            const char* closed_message) {
  if (increment == 0) {
    co_return nebula::rt::ok_void_result();
  }
  std::string payload;
  payload.reserve(4);
  const auto wire = increment & 0x7fffffffU;
  payload.push_back(static_cast<char>((wire >> 24) & 0x7f));
  payload.push_back(static_cast<char>((wire >> 16) & 0xff));
  payload.push_back(static_cast<char>((wire >> 8) & 0xff));
  payload.push_back(static_cast<char>(wire & 0xff));
  co_return co_await write_frame_async(state, session, FrameType::WindowUpdate, 0, stream_id, payload, closed_message);
}

template <typename StatePtr>
Future<Result<void, std::string>> write_header_frames_async(StatePtr state,
                                                            nebula::rt::Http2SessionState& session,
                                                            std::uint32_t stream_id,
                                                            std::string_view block,
                                                            bool end_stream,
                                                            const char* closed_message) {
  const std::size_t frame_size =
      std::max<std::size_t>(16384, static_cast<std::size_t>(session.peer_max_frame_size));
  std::size_t offset = 0;
  const std::size_t first_size = std::min(frame_size, block.size());
  std::uint8_t first_flags = 0;
  if (first_size == block.size()) {
    first_flags |= kFlagEndHeaders;
  }
  if (end_stream) {
    first_flags |= kFlagEndStream;
  }
  auto first = co_await write_frame_async(state,
                                          session,
                                          FrameType::Headers,
                                          first_flags,
                                          stream_id,
                                          block.substr(0, first_size),
                                          closed_message);
  if (result_is_err(first)) {
    co_return nebula::rt::err_void_result(
        result_err_move(first));
  }
  offset = first_size;
  while (offset < block.size()) {
    const std::size_t chunk = std::min(frame_size, block.size() - offset);
    const std::uint8_t flags = offset + chunk == block.size() ? kFlagEndHeaders : 0;
    auto next = co_await write_frame_async(state,
                                           session,
                                           FrameType::Continuation,
                                           flags,
                                           stream_id,
                                           block.substr(offset, chunk),
                                           closed_message);
    if (result_is_err(next)) {
      co_return nebula::rt::err_void_result(
          result_err_move(next));
    }
    offset += chunk;
  }
  co_return nebula::rt::ok_void_result();
}

template <typename StatePtr>
Future<Result<void, std::string>> await_send_window_async(StatePtr state,
                                                          nebula::rt::Http2SessionState& session,
                                                          const char* closed_message) {
  while (session.peer_connection_window <= 0 || session.peer_stream_window <= 0) {
    auto frame = co_await read_frame_async(state, session, closed_message);
    if (result_is_err(frame)) {
      co_return nebula::rt::err_void_result(
          result_err_move(frame));
    }
    auto current = result_ok_move(frame);
    auto handled = co_await process_control_frame_async(state, session, current, closed_message);
    if (result_is_err(handled)) {
      co_return nebula::rt::err_void_result(
          result_err_move(handled));
    }
    if (!result_ok_move(handled)) {
      co_return nebula::rt::err_void_result(flow_control_wait_error(session, current));
    }
  }
  co_return nebula::rt::ok_void_result();
}

template <typename StatePtr>
Future<Result<void, std::string>> await_remote_settings_async(StatePtr state,
                                                              nebula::rt::Http2SessionState& session,
                                                              const char* closed_message) {
  while (!session.remote_settings_seen) {
    auto frame = co_await read_frame_async(state, session, closed_message);
    if (result_is_err(frame)) {
      co_return nebula::rt::err_void_result(
          result_err_move(frame));
    }
    auto current = result_ok_move(frame);
    auto handled = co_await process_control_frame_async(state, session, current, closed_message);
    if (result_is_err(handled)) {
      co_return nebula::rt::err_void_result(
          result_err_move(handled));
    }
    if (!result_ok_move(handled)) {
      co_return nebula::rt::err_void_result("expected HTTP/2 SETTINGS before request, got frame_type=" +
                                            frame_type_name(current.type) +
                                            " stream_id=" + std::to_string(current.stream_id));
    }
  }
  co_return nebula::rt::ok_void_result();
}

template <typename StatePtr>
Future<Result<void, std::string>> write_data_frames_async(StatePtr state,
                                                          nebula::rt::Http2SessionState& session,
                                                          std::uint32_t stream_id,
                                                          std::string_view body,
                                                          const char* closed_message) {
  std::size_t offset = 0;
  while (offset < body.size()) {
    auto ready = co_await await_send_window_async(state, session, closed_message);
    if (result_is_err(ready)) {
      co_return ready;
    }
    std::size_t chunk = body.size() - offset;
    chunk = std::min(chunk, static_cast<std::size_t>(session.peer_max_frame_size));
    chunk = std::min(chunk, static_cast<std::size_t>(session.peer_connection_window));
    chunk = std::min(chunk, static_cast<std::size_t>(session.peer_stream_window));
    const std::uint8_t flags = offset + chunk == body.size() ? kFlagEndStream : 0;
    auto wrote = co_await write_frame_async(state,
                                            session,
                                            FrameType::Data,
                                            flags,
                                            stream_id,
                                            body.substr(offset, chunk),
                                            closed_message);
    if (result_is_err(wrote)) {
      co_return nebula::rt::err_void_result(
          result_err_move(wrote));
    }
    session.peer_connection_window -= static_cast<std::int64_t>(chunk);
    session.peer_stream_window -= static_cast<std::int64_t>(chunk);
    offset += chunk;
  }
  co_return nebula::rt::ok_void_result();
}

Result<HttpRequest, std::string> build_request(const DecodedRequestHead& head, std::string body) {
  auto headers = request_headers_text(head, body);
  if (result_is_err(headers)) {
    return err_result<HttpRequest>(
        result_err_move(headers));
  }
  std::string header_text =
      result_ok_move(headers);
  return nebula::rt::ok_result(HttpRequest{
      head.method,
      head.path,
      Bytes{std::move(body)},
      std::move(header_text),
      false,
  });
}

Result<HttpClientResponse, std::string> build_response(const DecodedResponseHead& head, std::string body) {
  auto headers = response_headers_text(head, body);
  if (result_is_err(headers)) {
    return err_result<HttpClientResponse>(
        result_err_move(headers));
  }
  std::string header_text =
      result_ok_move(headers);
  std::string content_type;
  if (auto value = nebula::rt::parse_http_header_value(header_text, "content-type"); value.has_value()) {
    content_type = *value;
  }
  return nebula::rt::ok_result(HttpClientResponse{
      head.status,
      std::move(content_type),
      Bytes{std::move(body)},
      std::move(header_text),
  });
}

template <typename StatePtr>
Future<Result<void, std::string>> write_goaway_and_close_async(StatePtr state,
                                                               std::uint32_t last_stream_id,
                                                               const char* closed_message) {
  std::string payload;
  payload.reserve(8);
  const auto sid = last_stream_id & 0x7fffffffU;
  payload.push_back(static_cast<char>((sid >> 24) & 0x7f));
  payload.push_back(static_cast<char>((sid >> 16) & 0xff));
  payload.push_back(static_cast<char>((sid >> 8) & 0xff));
  payload.push_back(static_cast<char>(sid & 0xff));
  payload.append(4, '\0');
  auto wrote = co_await write_frame_async(state, state->http2, FrameType::Goaway, 0, 0, payload, closed_message);
  if (result_is_err(wrote)) {
    co_return nebula::rt::err_void_result(
        result_err_move(wrote));
  }
  state->http2.goaway_sent = true;
  append_http2_phase_event(
      state->http2, "goaway_sent", "outbound", "GOAWAY", 0, "shutdown", "local_close_after_response");
  co_return co_await tls_stream_close_async(state, closed_message);
}

Future<Result<void, std::string>> client_write_request_async(TlsClientStream self,
                                                             HttpClientRequest request) {
  if (self.state == nullptr || self.state->handle == nullptr || !self.state->handle->valid()) {
    co_return nebula::rt::err_void_result("tls stream is closed");
  }
  const auto protocol_error = h2_protocol_error(self.state->transport.alpn_protocol);
  if (!protocol_error.empty()) {
    co_return nebula::rt::err_void_result(protocol_error);
  }
  auto started = co_await ensure_client_started_async(self.state, self.state->http2, "tls stream is closed");
  if (result_is_err(started)) {
    co_return result_err_variant_move(started);
  }
  auto settings = co_await await_remote_settings_async(self.state, self.state->http2, "tls stream is closed");
  if (result_is_err(settings)) {
    co_return result_err_variant_move(settings);
  }
  if (self.state->http2.goaway_received) {
    co_return nebula::rt::err_void_result("HTTP/2 peer sent GOAWAY: last_stream_id=" +
                                          std::to_string(self.state->http2.goaway_last_stream_id) +
                                          " error_code=" + std::to_string(self.state->http2.goaway_error_code) +
                                          " error_name=" +
                                          http2_error_code_name(self.state->http2.goaway_error_code));
  }
  if (self.state->http2.active_stream_id != 0) {
    co_return nebula::rt::err_void_result(
        "HTTP/2 client already has an active stream in the current single-stream preview: stream_id=" +
        std::to_string(self.state->http2.active_stream_id));
  }
  auto headers = encode_request_headers(request);
  if (result_is_err(headers)) {
    co_return nebula::rt::err_void_result(
        result_err_move(headers));
  }
  const std::string block =
      encode_header_block(result_ok_move(headers));
  const auto stream_id = self.state->http2.next_local_stream_id;
  self.state->http2.next_local_stream_id += 2;
  self.state->http2.active_stream_id = stream_id;
  self.state->http2.active_request_method = request.method;
  self.state->http2.peer_stream_window = self.state->http2.peer_initial_stream_window;

  const bool end_stream = request.body.data.empty();
  auto wrote_headers = co_await write_header_frames_async(self.state,
                                                          self.state->http2,
                                                          stream_id,
                                                          block,
                                                          end_stream,
                                                          "tls stream is closed");
  if (result_is_err(wrote_headers)) {
    co_return result_err_variant_move(wrote_headers);
  }
  append_http2_phase_event(self.state->http2,
                           "stream_open",
                           "outbound",
                           "HEADERS",
                           stream_id,
                           "request_start",
                           "local_request_headers_sent");
  if (!request.body.data.empty()) {
    co_return co_await write_data_frames_async(self.state,
                                               self.state->http2,
                                               stream_id,
                                               request.body.data,
                                               "tls stream is closed");
  }
  co_return nebula::rt::ok_void_result();
}

Future<Result<HttpClientResponse, std::string>> client_read_response_async(
    TlsClientStream self,
    const std::optional<HttpMethod>& request_method,
    std::int64_t max_header_bytes,
    std::int64_t max_body_bytes) {
  if (self.state == nullptr || self.state->handle == nullptr || !self.state->handle->valid()) {
    co_return err_result<HttpClientResponse>("tls stream is closed");
  }
  if (max_header_bytes <= 0 || max_body_bytes < 0) {
    co_return err_result<HttpClientResponse>("HTTP/2 limits must be positive");
  }
  const auto protocol_error = h2_protocol_error(self.state->transport.alpn_protocol);
  if (!protocol_error.empty()) {
    co_return err_result<HttpClientResponse>(protocol_error);
  }
  if (self.state->http2.active_stream_id == 0) {
    co_return err_result<HttpClientResponse>("HTTP/2 client has no active request stream");
  }
  const auto active_stream_id = self.state->http2.active_stream_id;
  const auto active_method =
      request_method.has_value() ? request_method : self.state->http2.active_request_method;

  bool head_seen = false;
  DecodedResponseHead response_head;
  std::string body;
  while (true) {
    auto next = co_await read_frame_async(self.state, self.state->http2, "tls stream is closed");
    if (result_is_err(next)) {
      co_return err_result<HttpClientResponse>(
          result_err_move(next));
    }
    auto frame = result_ok_move(next);
    auto handled = co_await process_control_frame_async(self.state, self.state->http2, frame, "tls stream is closed");
    if (result_is_err(handled)) {
      co_return err_result<HttpClientResponse>(
          result_err_move(handled));
    }
    if (result_ok_move(handled)) {
      continue;
    }

    if (frame.type == FrameType::Headers) {
      if (frame.stream_id != active_stream_id) {
        append_http2_phase_event(self.state->http2,
                                 "stream_closed",
                                 "inbound",
                                 "HEADERS",
                                 active_stream_id,
                                 "preview_limit",
                                 "concurrent_response_headers");
        co_return err_result<HttpClientResponse>(concurrent_stream_error("response_headers",
                                                                        active_stream_id,
                                                                        frame));
      }
      auto block =
          co_await read_header_block_async(self.state, self.state->http2, active_stream_id, frame, "tls stream is closed");
      if (result_is_err(block)) {
        co_return err_result<HttpClientResponse>(
            result_err_move(block));
      }
      const auto header_block =
          result_ok_move(block);
      if (header_block.size() > static_cast<std::size_t>(max_header_bytes)) {
        co_return err_result<HttpClientResponse>("HTTP/2 headers exceed configured limit");
      }
      auto decoded = decode_response_head(header_block);
      if (result_is_err(decoded)) {
        co_return err_result<HttpClientResponse>(
            result_err_move(decoded));
      }
      auto current_head =
          result_ok_move(decoded);
      if (current_head.status == 101) {
        co_return err_result<HttpClientResponse>("HTTP/2 upgrade responses are unsupported");
      }
      if (current_head.status >= 100 && current_head.status < 200) {
        continue;
      }
      if (head_seen) {
        co_return err_result<HttpClientResponse>("duplicate HTTP/2 response headers");
      }
      head_seen = true;
      response_head = std::move(current_head);
      append_http2_phase_event(self.state->http2,
                               "response_headers_seen",
                               "inbound",
                               "HEADERS",
                               active_stream_id,
                               "response_start",
                               "peer_final_response_headers_seen");
      const bool forbids_body =
          nebula::rt::http_response_status_forbids_body(response_head.status) ||
          (active_method.has_value() && nebula::rt::http_method_is_head(*active_method));
      if (forbids_body) {
        if ((frame.flags & kFlagEndStream) == 0) {
          co_return err_result<HttpClientResponse>("HTTP/2 response forbids a body but did not end the stream");
        }
        append_http2_phase_event(self.state->http2,
                                 "stream_closed",
                                 "inbound",
                                 "HEADERS",
                                 active_stream_id,
                                 "response_complete",
                                 "headers_end_stream_no_body");
        self.state->http2.active_stream_id = 0;
        self.state->http2.active_request_method.reset();
        self.state->http2.peer_stream_window = self.state->http2.peer_initial_stream_window;
        co_return build_response(response_head, "");
      }
      if ((frame.flags & kFlagEndStream) != 0) {
        append_http2_phase_event(self.state->http2,
                                 "stream_closed",
                                 "inbound",
                                 "HEADERS",
                                 active_stream_id,
                                 "response_complete",
                                 "headers_end_stream");
        self.state->http2.active_stream_id = 0;
        self.state->http2.active_request_method.reset();
        self.state->http2.peer_stream_window = self.state->http2.peer_initial_stream_window;
        co_return build_response(response_head, "");
      }
      continue;
    }

    if (frame.type == FrameType::Data) {
      if (frame.stream_id != active_stream_id) {
        append_http2_phase_event(self.state->http2,
                                 "stream_closed",
                                 "inbound",
                                 frame_type_name(frame.type),
                                 active_stream_id,
                                 "preview_limit",
                                 "concurrent_response_data");
        co_return err_result<HttpClientResponse>(concurrent_stream_error("response_body",
                                                                        active_stream_id,
                                                                        frame));
      }
      if (!head_seen) {
        co_return err_result<HttpClientResponse>("HTTP/2 response DATA arrived before HEADERS");
      }
      if (body.size() + frame.payload.size() > static_cast<std::size_t>(max_body_bytes)) {
        co_return err_result<HttpClientResponse>("HTTP/2 response body exceeds configured limit");
      }
      body.append(frame.payload);
      auto conn_window = co_await write_window_update_async(
          self.state, self.state->http2, 0, static_cast<std::uint32_t>(frame.payload.size()), "tls stream is closed");
      if (result_is_err(conn_window)) {
        co_return err_result<HttpClientResponse>(
            result_err_move(conn_window));
      }
      auto stream_window = co_await write_window_update_async(
          self.state, self.state->http2, active_stream_id, static_cast<std::uint32_t>(frame.payload.size()), "tls stream is closed");
      if (result_is_err(stream_window)) {
        co_return err_result<HttpClientResponse>(
            result_err_move(stream_window));
      }
      if ((frame.flags & kFlagEndStream) != 0) {
        append_http2_phase_event(self.state->http2,
                                 "stream_closed",
                                 "inbound",
                                 "DATA",
                                 active_stream_id,
                                 "response_complete",
                                 "data_end_stream");
        self.state->http2.active_stream_id = 0;
        self.state->http2.active_request_method.reset();
        self.state->http2.peer_stream_window = self.state->http2.peer_initial_stream_window;
        co_return build_response(response_head, std::move(body));
      }
      continue;
    }

    append_http2_phase_event(self.state->http2,
                             "stream_closed",
                             "inbound",
                             frame_type_name(frame.type),
                             active_stream_id,
                             "protocol",
                             "unsupported_response_frame");
    co_return err_result<HttpClientResponse>("unsupported HTTP/2 frame on response stream: active_stream_id=" +
                                             std::to_string(active_stream_id) +
                                             " incoming_stream_id=" + std::to_string(frame.stream_id) +
                                             " frame_type=" + frame_type_name(frame.type));
  }
}

Future<Result<HttpRequest, std::string>> server_read_request_async(TlsServerStream self,
                                                                   std::int64_t max_header_bytes,
                                                                   std::int64_t max_body_bytes) {
  if (self.state == nullptr || self.state->handle == nullptr || !self.state->handle->valid()) {
    co_return err_result<HttpRequest>("tls stream is closed");
  }
  if (max_header_bytes <= 0 || max_body_bytes < 0) {
    co_return err_result<HttpRequest>("HTTP/2 limits must be positive");
  }
  const auto protocol_error = h2_protocol_error(self.state->transport.alpn_protocol);
  if (!protocol_error.empty()) {
    co_return err_result<HttpRequest>(protocol_error);
  }
  auto started = co_await ensure_server_started_async(self.state, self.state->http2, "tls stream is closed");
  if (result_is_err(started)) {
    co_return err_result<HttpRequest>(
        result_err_move(started));
  }
  if (self.state->http2.active_stream_id != 0) {
    co_return err_result<HttpRequest>(
        "HTTP/2 cannot read the next request before the active stream is answered in the current single-stream preview: active_stream_id=" +
        std::to_string(self.state->http2.active_stream_id));
  }

  while (true) {
    auto next = co_await read_frame_async(self.state, self.state->http2, "tls stream is closed");
    if (result_is_err(next)) {
      co_return err_result<HttpRequest>(
          result_err_move(next));
    }
    auto frame = result_ok_move(next);
    auto handled = co_await process_control_frame_async(self.state, self.state->http2, frame, "tls stream is closed");
    if (result_is_err(handled)) {
      co_return err_result<HttpRequest>(
          result_err_move(handled));
    }
    if (result_ok_move(handled)) {
      continue;
    }

    if (frame.type != FrameType::Headers) {
      co_return err_result<HttpRequest>("unexpected HTTP/2 frame before request headers: frame_type=" +
                                        frame_type_name(frame.type) +
                                        " stream_id=" + std::to_string(frame.stream_id));
    }
    if (frame.stream_id == 0 || (frame.stream_id % 2) == 0) {
      co_return err_result<HttpRequest>("invalid HTTP/2 request stream id");
    }
    if (frame.stream_id <= self.state->http2.last_peer_stream_id) {
      co_return err_result<HttpRequest>("HTTP/2 request stream id did not increase");
    }
    auto block =
        co_await read_header_block_async(self.state, self.state->http2, frame.stream_id, frame, "tls stream is closed");
    if (result_is_err(block)) {
      co_return err_result<HttpRequest>(
          result_err_move(block));
    }
    const auto header_block =
        result_ok_move(block);
    if (header_block.size() > static_cast<std::size_t>(max_header_bytes)) {
      co_return err_result<HttpRequest>("HTTP/2 headers exceed configured limit");
    }
    auto decoded = decode_request_head(header_block);
    if (result_is_err(decoded)) {
      co_return err_result<HttpRequest>(
          result_err_move(decoded));
    }
    auto request_head =
        result_ok_move(decoded);

    std::string body;
    bool end_stream = (frame.flags & kFlagEndStream) != 0;
    while (!end_stream) {
      auto body_frame = co_await read_frame_async(self.state, self.state->http2, "tls stream is closed");
      if (result_is_err(body_frame)) {
        co_return err_result<HttpRequest>(
            result_err_move(body_frame));
      }
      auto current =
          result_ok_move(body_frame);
      auto body_handled =
          co_await process_control_frame_async(self.state, self.state->http2, current, "tls stream is closed");
      if (result_is_err(body_handled)) {
        co_return err_result<HttpRequest>(
            result_err_move(body_handled));
      }
      if (result_ok_move(body_handled)) {
        continue;
      }
      if (current.type != FrameType::Data || current.stream_id != frame.stream_id) {
        co_return err_result<HttpRequest>(concurrent_stream_error("request_body",
                                                                 frame.stream_id,
                                                                 current));
      }
      if (body.size() + current.payload.size() > static_cast<std::size_t>(max_body_bytes)) {
        co_return err_result<HttpRequest>("HTTP/2 request body exceeds configured limit");
      }
      body.append(current.payload);
      auto conn_window = co_await write_window_update_async(
          self.state, self.state->http2, 0, static_cast<std::uint32_t>(current.payload.size()), "tls stream is closed");
      if (result_is_err(conn_window)) {
        co_return err_result<HttpRequest>(
            result_err_move(conn_window));
      }
      auto stream_window = co_await write_window_update_async(
          self.state, self.state->http2, frame.stream_id, static_cast<std::uint32_t>(current.payload.size()), "tls stream is closed");
      if (result_is_err(stream_window)) {
        co_return err_result<HttpRequest>(
            result_err_move(stream_window));
      }
      end_stream = (current.flags & kFlagEndStream) != 0;
    }

    auto built = build_request(request_head, std::move(body));
    if (result_is_err(built)) {
      co_return built;
    }
    self.state->http2.active_stream_id = frame.stream_id;
    self.state->http2.last_peer_stream_id = frame.stream_id;
    self.state->http2.peer_stream_window = self.state->http2.peer_initial_stream_window;
    append_http2_phase_event(self.state->http2,
                             "stream_open",
                             "inbound",
                             "HEADERS",
                             frame.stream_id,
                             "request_start",
                             "peer_request_ready");
    co_return result_ok_variant_move(built);
  }
}

Future<Result<void, std::string>> server_write_response_async(TlsServerStream self,
                                                              HttpResponse response,
                                                              bool close_connection) {
  if (self.state == nullptr || self.state->handle == nullptr || !self.state->handle->valid()) {
    co_return nebula::rt::err_void_result("tls stream is closed");
  }
  const auto protocol_error = h2_protocol_error(self.state->transport.alpn_protocol);
  if (!protocol_error.empty()) {
    co_return nebula::rt::err_void_result(protocol_error);
  }
  if (self.state->http2.active_stream_id == 0) {
    co_return nebula::rt::err_void_result("HTTP/2 server has no active request stream");
  }
  if (!response.content_type.empty() && !nebula::rt::http_content_type_is_valid(response.content_type)) {
    co_return nebula::rt::err_void_result("invalid Content-Type header");
  }
  auto headers = encode_response_headers(response);
  if (result_is_err(headers)) {
    co_return nebula::rt::err_void_result(
        result_err_move(headers));
  }
  const std::string block =
      encode_header_block(result_ok_move(headers));
  const auto stream_id = self.state->http2.active_stream_id;
  const std::string_view body =
      nebula::rt::http_response_status_forbids_body(response.status) ? std::string_view{} : response.body.data;

  auto wrote_headers = co_await write_header_frames_async(self.state,
                                                          self.state->http2,
                                                          stream_id,
                                                          block,
                                                          body.empty(),
                                                          "tls stream is closed");
  if (result_is_err(wrote_headers)) {
    co_return result_err_variant_move(wrote_headers);
  }
  append_http2_phase_event(self.state->http2,
                           "response_headers_sent",
                           "outbound",
                           "HEADERS",
                           stream_id,
                           "response_start",
                           "local_response_headers_sent");
  if (!body.empty()) {
    auto wrote_body =
        co_await write_data_frames_async(self.state, self.state->http2, stream_id, body, "tls stream is closed");
    if (result_is_err(wrote_body)) {
      co_return wrote_body;
    }
  }

  append_http2_phase_event(self.state->http2,
                           "stream_closed",
                           "outbound",
                           body.empty() ? "HEADERS" : "DATA",
                           stream_id,
                           "response_complete",
                           body.empty() ? "headers_end_stream" : "data_end_stream");
  self.state->http2.active_stream_id = 0;
  self.state->http2.peer_stream_window = self.state->http2.peer_initial_stream_window;
  if (close_connection) {
    co_return co_await write_goaway_and_close_async(self.state, stream_id, "tls stream is closed");
  }
  co_return nebula::rt::ok_void_result();
}

} // namespace http2

} // namespace

Result<TlsTrustStore, std::string> __nebula_tls_trust_store_from_ca_pem(Bytes ca_pem) {
  return make_trust_store_from_pem(ca_pem.data, "provided CA bundle");
}

Result<TlsTrustStore, std::string> __nebula_tls_trust_store_default_roots() {
  std::string_view pem(nebula::rt::kNebulaTlsDefaultRootsPem, nebula::rt::kNebulaTlsDefaultRootsPemLen);
  return make_trust_store_from_pem(pem, "bundled default roots");
}

Result<TlsServerName, std::string> __nebula_tls_server_name(std::string name) {
  return make_server_name(std::move(name));
}

Result<TlsClientIdentity, std::string> __nebula_tls_client_identity_from_pem(Bytes certificate_pem,
                                                                              Bytes private_key_pem) {
  return make_client_identity_from_pem(certificate_pem.data, private_key_pem.data);
}

TlsVersionPolicy __nebula_tls_tls12_only() {
  auto state = std::make_shared<TlsVersionPolicyState>();
  state->min_tls_version = MBEDTLS_SSL_VERSION_TLS1_2;
  state->max_tls_version = MBEDTLS_SSL_VERSION_TLS1_2;
  return TlsVersionPolicy{std::move(state)};
}

TlsVersionPolicy __nebula_tls_tls12_or_13() {
  return TlsVersionPolicy{default_version_policy_state()};
}

TlsVersionPolicy __nebula_tls_tls13_only() {
  auto state = std::make_shared<TlsVersionPolicyState>();
  state->min_tls_version = MBEDTLS_SSL_VERSION_TLS1_3;
  state->max_tls_version = MBEDTLS_SSL_VERSION_TLS1_3;
  return TlsVersionPolicy{std::move(state)};
}

TlsAlpnPolicy __nebula_tls_alpn_none() {
  return TlsAlpnPolicy{default_alpn_policy_state()};
}

TlsAlpnPolicy __nebula_tls_alpn_http11_only() {
  auto state = std::make_shared<TlsAlpnPolicyState>();
  state->protocols = {"http/1.1"};
  state->rebuild();
  return TlsAlpnPolicy{std::move(state)};
}

TlsAlpnPolicy __nebula_tls_alpn_http11_or_http2() {
  auto state = std::make_shared<TlsAlpnPolicyState>();
  state->protocols = {"h2", "http/1.1"};
  state->rebuild();
  return TlsAlpnPolicy{std::move(state)};
}

TlsAlpnPolicy __nebula_tls_alpn_http2_only() {
  auto state = std::make_shared<TlsAlpnPolicyState>();
  state->protocols = {"h2"};
  state->rebuild();
  return TlsAlpnPolicy{std::move(state)};
}

TlsClientConfig __nebula_tls_client_config(TlsTrustStore trust_store) {
  auto config_state = make_client_config_state(trust_store.state);
  if (result_is_err(config_state)) {
    return TlsClientConfig{};
  }
  return TlsClientConfig{
      result_ok_move(config_state)};
}

TlsClientConfig __nebula_tls_client_config_with_identity(TlsClientConfig self,
                                                         TlsClientIdentity identity) {
  auto cloned = clone_client_config_state(std::move(self));
  if (result_is_err(cloned)) {
    return TlsClientConfig{};
  }
  auto state =
      result_ok_move(cloned);
  state->client_identity = std::move(identity.state);
  return TlsClientConfig{std::move(state)};
}

TlsClientConfig __nebula_tls_client_config_with_version_policy(TlsClientConfig self,
                                                               TlsVersionPolicy policy) {
  auto cloned = clone_client_config_state(std::move(self));
  if (result_is_err(cloned)) {
    return TlsClientConfig{};
  }
  auto state =
      result_ok_move(cloned);
  state->version_policy = std::move(policy.state);
  return TlsClientConfig{std::move(state)};
}

TlsClientConfig __nebula_tls_client_config_with_alpn_policy(TlsClientConfig self,
                                                            TlsAlpnPolicy policy) {
  auto cloned = clone_client_config_state(std::move(self));
  if (result_is_err(cloned)) {
    return TlsClientConfig{};
  }
  auto state =
      result_ok_move(cloned);
  state->alpn_policy = std::move(policy.state);
  return TlsClientConfig{std::move(state)};
}

Future<Result<TlsClientStream, std::string>> __nebula_tls_handshake_named(TcpStream stream,
                                                                          TlsServerName server_name,
                                                                          TlsClientConfig config) {
  co_return co_await tls_handshake_client_async(std::move(stream), std::move(server_name), std::move(config));
}

Future<Result<Bytes, std::string>> __nebula_tls_stream_read(TlsClientStream self, std::int64_t max_bytes) {
  co_return co_await tls_stream_read_async(self.state, max_bytes, "tls stream is closed");
}

Future<Result<std::int64_t, std::string>> __nebula_tls_stream_write(TlsClientStream self, Bytes bytes) {
  co_return co_await tls_stream_write_async(self.state, std::move(bytes), "tls stream is closed");
}

Future<Result<void, std::string>> __nebula_tls_stream_write_all(TlsClientStream self, Bytes bytes) {
  co_return co_await tls_stream_write_all_async(self.state, std::move(bytes), "tls stream is closed");
}

Future<Result<void, std::string>> __nebula_tls_stream_close(TlsClientStream self) {
  co_return co_await tls_stream_close_async(self.state, "tls stream is closed");
}

Result<void, std::string> __nebula_tls_stream_abort(TlsClientStream self) {
  return tls_stream_abort_result(self.state);
}

Result<SocketAddr, std::string> __nebula_tls_stream_peer_addr(TlsClientStream self) {
  return tls_stream_peer_addr_result(self.state, "tls stream is closed");
}

Result<SocketAddr, std::string> __nebula_tls_stream_local_addr(TlsClientStream self) {
  return tls_stream_local_addr_result(self.state, "tls stream is closed");
}

Result<std::string, std::string> __nebula_tls_stream_peer_subject(TlsClientStream self) {
  return tls_stream_peer_subject_result(self.state, "tls stream is closed");
}

Result<nebula::rt::JsonValue, std::string> __nebula_tls_stream_peer_identity_debug(TlsClientStream self) {
  return tls_stream_peer_identity_debug_result(self.state, "tls stream is closed");
}

Result<nebula::rt::JsonValue, std::string> __nebula_tls_stream_http2_debug_state(TlsClientStream self) {
  return tls_stream_http2_debug_state_result(self.state, "tls stream is closed");
}

bool __nebula_tls_stream_peer_verified(TlsClientStream self) {
  return self.state != nullptr && self.state->transport.peer_verified;
}

std::string __nebula_tls_stream_tls_version(TlsClientStream self) {
  if (self.state == nullptr) return "";
  return self.state->transport.tls_version;
}

std::string __nebula_tls_stream_alpn_protocol(TlsClientStream self) {
  if (self.state == nullptr) return "";
  return self.state->transport.alpn_protocol;
}

Future<Result<void, std::string>> __nebula_tls_http_write_request(TlsClientStream self,
                                                                  HttpClientRequest request) {
  if (self.state == nullptr || self.state->handle == nullptr || !self.state->handle->valid()) {
    co_return nebula::rt::err_void_result("tls stream is closed");
  }
  const auto protocol_error = http_over_tls_protocol_error(self.state->transport.alpn_protocol);
  if (!protocol_error.empty()) {
    co_return nebula::rt::err_void_result(protocol_error);
  }
  auto encoded = nebula::rt::build_http_request_message(request);
  if (result_is_err(encoded)) {
    co_return nebula::rt::err_void_result(
        result_err_move(encoded));
  }
  std::string payload =
      result_ok_move(encoded);
  co_return co_await tls_write_http_payload_async(self.state, payload, "tls stream is closed");
}

Future<Result<HttpClientResponse, std::string>> __nebula_tls_http_read_response(TlsClientStream self,
                                                                                std::int64_t max_header_bytes,
                                                                                std::int64_t max_body_bytes) {
  co_return co_await tls_read_http_response_async(std::move(self), std::nullopt, max_header_bytes, max_body_bytes);
}

Future<Result<HttpClientResponse, std::string>> __nebula_tls_http_read_response_for(TlsClientStream self,
                                                                                    HttpMethod method,
                                                                                    std::int64_t max_header_bytes,
                                                                                    std::int64_t max_body_bytes) {
  co_return co_await tls_read_http_response_async(std::move(self),
                                                  std::optional<HttpMethod>{std::move(method)},
                                                  max_header_bytes,
                                                  max_body_bytes);
}

Future<Result<void, std::string>> __nebula_tls_http2_write_request(TlsClientStream self,
                                                                   HttpClientRequest request) {
  co_return co_await http2::client_write_request_async(std::move(self), std::move(request));
}

Future<Result<HttpClientResponse, std::string>> __nebula_tls_http2_read_response(TlsClientStream self,
                                                                                 std::int64_t max_header_bytes,
                                                                                 std::int64_t max_body_bytes) {
  co_return co_await http2::client_read_response_async(std::move(self),
                                                       std::nullopt,
                                                       max_header_bytes,
                                                       max_body_bytes);
}

Future<Result<HttpClientResponse, std::string>> __nebula_tls_http2_read_response_for(TlsClientStream self,
                                                                                     HttpMethod method,
                                                                                     std::int64_t max_header_bytes,
                                                                                     std::int64_t max_body_bytes) {
  co_return co_await http2::client_read_response_async(std::move(self),
                                                       std::optional<HttpMethod>{std::move(method)},
                                                       max_header_bytes,
                                                       max_body_bytes);
}

Result<TlsServerIdentity, std::string> __nebula_tls_server_identity_from_pem(Bytes certificate_pem,
                                                                              Bytes private_key_pem) {
  return make_server_identity_from_pem(certificate_pem.data, private_key_pem.data);
}

TlsServerConfig __nebula_tls_server_config(TlsServerIdentity identity) {
  auto config_state = make_server_config_state(identity.state);
  if (result_is_err(config_state)) {
    return TlsServerConfig{};
  }
  return TlsServerConfig{
      result_ok_move(config_state)};
}

TlsServerConfig __nebula_tls_server_config_with_version_policy(TlsServerConfig self,
                                                               TlsVersionPolicy policy) {
  auto cloned = clone_server_config_state(std::move(self));
  if (result_is_err(cloned)) {
    return TlsServerConfig{};
  }
  auto state =
      result_ok_move(cloned);
  state->version_policy = std::move(policy.state);
  return TlsServerConfig{std::move(state)};
}

TlsServerConfig __nebula_tls_server_config_with_alpn_policy(TlsServerConfig self,
                                                            TlsAlpnPolicy policy) {
  auto cloned = clone_server_config_state(std::move(self));
  if (result_is_err(cloned)) {
    return TlsServerConfig{};
  }
  auto state =
      result_ok_move(cloned);
  state->alpn_policy = std::move(policy.state);
  return TlsServerConfig{std::move(state)};
}

TlsServerConfig __nebula_tls_server_config_client_auth_disabled(TlsServerConfig self) {
  auto cloned = clone_server_config_state(std::move(self));
  if (result_is_err(cloned)) {
    return TlsServerConfig{};
  }
  auto state =
      result_ok_move(cloned);
  state->client_auth_mode = TlsServerClientAuthMode::Disabled;
  state->client_trust_store.reset();
  return TlsServerConfig{std::move(state)};
}

TlsServerConfig __nebula_tls_server_config_client_auth_optional(TlsServerConfig self,
                                                                TlsTrustStore trust_store) {
  auto cloned = clone_server_config_state(std::move(self));
  if (result_is_err(cloned)) {
    return TlsServerConfig{};
  }
  auto state =
      result_ok_move(cloned);
  state->client_auth_mode = TlsServerClientAuthMode::Optional;
  state->client_trust_store = std::move(trust_store.state);
  return TlsServerConfig{std::move(state)};
}

TlsServerConfig __nebula_tls_server_config_client_auth_required(TlsServerConfig self,
                                                                TlsTrustStore trust_store) {
  auto cloned = clone_server_config_state(std::move(self));
  if (result_is_err(cloned)) {
    return TlsServerConfig{};
  }
  auto state =
      result_ok_move(cloned);
  state->client_auth_mode = TlsServerClientAuthMode::Required;
  state->client_trust_store = std::move(trust_store.state);
  return TlsServerConfig{std::move(state)};
}

Future<Result<TlsServerListener, std::string>> __nebula_tls_bind(SocketAddr addr, TlsServerConfig config) {
  if (config.state == nullptr) {
    co_return err_result<TlsServerListener>("tls server config is uninitialized");
  }
  if (config.state->server_identity == nullptr) {
    co_return err_result<TlsServerListener>("tls server identity is uninitialized");
  }
  if (config.state->client_auth_mode != TlsServerClientAuthMode::Disabled &&
      config.state->client_trust_store == nullptr) {
    co_return err_result<TlsServerListener>("tls server client auth requires a trust store");
  }

  auto bound = co_await nebula::rt::bind_listener(addr);
  if (result_is_err(bound)) {
    co_return err_result<TlsServerListener>(
        result_err_move(bound));
  }
  auto listener =
      result_ok_move(bound);

  auto state = std::make_shared<TlsServerListenerState>();
  state->handle = listener.handle;
  state->config = std::move(config.state);

  int rc = mbedtls_ssl_config_defaults(&state->ssl_config,
                                       MBEDTLS_SSL_IS_SERVER,
                                       MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT);
  if (rc != 0) {
    co_return err_result<TlsServerListener>("mbedtls_ssl_config_defaults failed: " +
                                            mbedtls_error_string(rc));
  }

  apply_version_policy(state->ssl_config, state->config->version_policy);
  auto alpn = apply_alpn_policy(state->ssl_config, state->config->alpn_policy);
  if (result_is_err(alpn)) {
    co_return err_result<TlsServerListener>(
        result_err_move(alpn));
  }

  mbedtls_ssl_conf_rng(&state->ssl_config,
                       mbedtls_ctr_drbg_random,
                       &state->config->ctr_drbg);
  maybe_enable_tls_debug(state->ssl_config);

  switch (state->config->client_auth_mode) {
    case TlsServerClientAuthMode::Disabled:
      mbedtls_ssl_conf_authmode(&state->ssl_config, MBEDTLS_SSL_VERIFY_NONE);
      break;
    case TlsServerClientAuthMode::Optional:
      mbedtls_ssl_conf_authmode(&state->ssl_config, MBEDTLS_SSL_VERIFY_OPTIONAL);
      mbedtls_ssl_conf_ca_chain(&state->ssl_config, &state->config->client_trust_store->ca_chain, nullptr);
      break;
    case TlsServerClientAuthMode::Required:
      mbedtls_ssl_conf_authmode(&state->ssl_config, MBEDTLS_SSL_VERIFY_REQUIRED);
      mbedtls_ssl_conf_ca_chain(&state->ssl_config, &state->config->client_trust_store->ca_chain, nullptr);
      break;
  }

  rc = mbedtls_ssl_conf_own_cert(&state->ssl_config,
                                 &state->config->server_identity->certificate_chain,
                                 &state->config->server_identity->private_key);
  if (rc != 0) {
    co_return err_result<TlsServerListener>("mbedtls_ssl_conf_own_cert failed: " +
                                            mbedtls_error_string(rc));
  }
  state->setup_complete = true;
  co_return nebula::rt::ok_result(TlsServerListener{std::move(state)});
}

Future<Result<TlsServerStream, std::string>> __nebula_tls_listener_accept(TlsServerListener listener) {
  if (listener.state == nullptr || listener.state->handle == nullptr || !listener.state->handle->valid()) {
    co_return err_result<TlsServerStream>("tls listener is closed");
  }
  while (true) {
    auto accepted = co_await nebula::rt::accept_stream(TcpListener{listener.state->handle});
    if (result_is_err(accepted)) {
      co_return err_result<TlsServerStream>(
          result_err_move(accepted));
    }
    auto stream =
        result_ok_move(accepted);
    auto outcome = co_await tls_handshake_server_async(listener, std::move(stream));
    if (outcome.kind == TlsServerHandshakeOutcomeKind::Established) {
      co_return nebula::rt::ok_result(std::move(outcome.stream));
    }
    if (outcome.kind == TlsServerHandshakeOutcomeKind::Fatal) {
      co_return err_result<TlsServerStream>(std::move(outcome.message));
    }
  }
}

Future<Result<TlsServerStream, std::string>> __nebula_tls_listener_accept_timeout(TlsServerListener listener,
                                                                                  std::int64_t timeout_ms) {
  if (listener.state == nullptr || listener.state->handle == nullptr || !listener.state->handle->valid()) {
    co_return err_result<TlsServerStream>("tls listener is closed");
  }
  while (true) {
    auto accepted =
        co_await nebula::rt::accept_stream_timeout(TcpListener{listener.state->handle}, timeout_ms);
    if (result_is_err(accepted)) {
      co_return err_result<TlsServerStream>(
          result_err_move(accepted));
    }
    auto stream =
        result_ok_move(accepted);
    auto outcome = co_await tls_handshake_server_async(listener, std::move(stream));
    if (outcome.kind == TlsServerHandshakeOutcomeKind::Established) {
      co_return nebula::rt::ok_result(std::move(outcome.stream));
    }
    if (outcome.kind == TlsServerHandshakeOutcomeKind::Fatal) {
      co_return err_result<TlsServerStream>(std::move(outcome.message));
    }
  }
}

Result<SocketAddr, std::string> __nebula_tls_listener_local_addr(TlsServerListener listener) {
  if (listener.state == nullptr || listener.state->handle == nullptr || !listener.state->handle->valid()) {
    return err_result<SocketAddr>("tls listener is closed");
  }
  return nebula::rt::socket_name_of(listener.state->handle->fd, false);
}

Result<void, std::string> __nebula_tls_listener_close(TlsServerListener listener) {
  if (listener.state == nullptr) return nebula::rt::ok_void_result();
  return nebula::rt::close_socket_handle(listener.state->handle);
}

Future<Result<Bytes, std::string>> __nebula_tls_server_stream_read(TlsServerStream self,
                                                                   std::int64_t max_bytes) {
  co_return co_await tls_stream_read_async(self.state, max_bytes, "tls stream is closed");
}

Future<Result<std::int64_t, std::string>> __nebula_tls_server_stream_write(TlsServerStream self,
                                                                           Bytes bytes) {
  co_return co_await tls_stream_write_async(self.state, std::move(bytes), "tls stream is closed");
}

Future<Result<void, std::string>> __nebula_tls_server_stream_write_all(TlsServerStream self, Bytes bytes) {
  co_return co_await tls_stream_write_all_async(self.state, std::move(bytes), "tls stream is closed");
}

Future<Result<void, std::string>> __nebula_tls_server_stream_close(TlsServerStream self) {
  co_return co_await tls_stream_close_async(self.state, "tls stream is closed");
}

Result<void, std::string> __nebula_tls_server_stream_abort(TlsServerStream self) {
  return tls_stream_abort_result(self.state);
}

Result<SocketAddr, std::string> __nebula_tls_server_stream_peer_addr(TlsServerStream self) {
  return tls_stream_peer_addr_result(self.state, "tls stream is closed");
}

Result<SocketAddr, std::string> __nebula_tls_server_stream_local_addr(TlsServerStream self) {
  return tls_stream_local_addr_result(self.state, "tls stream is closed");
}

Result<std::string, std::string> __nebula_tls_server_stream_peer_subject(TlsServerStream self) {
  return tls_stream_peer_subject_result(self.state, "tls stream is closed");
}

Result<std::string, std::string> __nebula_tls_server_stream_peer_fingerprint_sha256(TlsServerStream self) {
  return tls_stream_peer_fingerprint_sha256_result(self.state, "tls stream is closed");
}

Result<nebula::rt::JsonValue, std::string> __nebula_tls_server_stream_peer_san_claims(TlsServerStream self) {
  return tls_stream_peer_san_claims_result(self.state, "tls stream is closed");
}

Result<nebula::rt::JsonValue, std::string> __nebula_tls_server_stream_peer_identity_debug(TlsServerStream self) {
  return tls_stream_peer_identity_debug_result(self.state, "tls stream is closed");
}

Result<nebula::rt::JsonValue, std::string> __nebula_tls_server_stream_http2_debug_state(TlsServerStream self) {
  return tls_stream_http2_debug_state_result(self.state, "tls stream is closed");
}

bool __nebula_tls_server_stream_peer_present(TlsServerStream self) {
  return self.state != nullptr && self.state->transport.peer_certificate_present;
}

bool __nebula_tls_server_stream_peer_verified(TlsServerStream self) {
  return self.state != nullptr && self.state->transport.peer_verified;
}

std::string __nebula_tls_server_stream_tls_version(TlsServerStream self) {
  if (self.state == nullptr) return "";
  return self.state->transport.tls_version;
}

std::string __nebula_tls_server_stream_alpn_protocol(TlsServerStream self) {
  if (self.state == nullptr) return "";
  return self.state->transport.alpn_protocol;
}

Future<Result<HttpRequest, std::string>> __nebula_tls_server_http_read_request(TlsServerStream self,
                                                                               std::int64_t max_header_bytes,
                                                                               std::int64_t max_body_bytes) {
  co_return co_await tls_read_http_request_async(std::move(self), max_header_bytes, max_body_bytes);
}

Future<Result<void, std::string>> __nebula_tls_server_http_write_response(TlsServerStream self,
                                                                          HttpResponse response) {
  if (self.state == nullptr || self.state->handle == nullptr || !self.state->handle->valid()) {
    co_return nebula::rt::err_void_result("tls stream is closed");
  }
  const auto protocol_error = http_over_tls_protocol_error(self.state->transport.alpn_protocol);
  if (!protocol_error.empty()) {
    co_return nebula::rt::err_void_result(protocol_error);
  }
  const std::string payload = nebula::rt::build_http_response_message(response, true);
  co_return co_await tls_write_http_payload_async(self.state, payload, "tls stream is closed");
}

Future<Result<void, std::string>> __nebula_tls_server_http_write_response_with_connection(
    TlsServerStream self,
    HttpResponse response,
    bool close_connection) {
  if (self.state == nullptr || self.state->handle == nullptr || !self.state->handle->valid()) {
    co_return nebula::rt::err_void_result("tls stream is closed");
  }
  const auto protocol_error = http_over_tls_protocol_error(self.state->transport.alpn_protocol);
  if (!protocol_error.empty()) {
    co_return nebula::rt::err_void_result(protocol_error);
  }
  const std::string payload = nebula::rt::build_http_response_message(response, close_connection);
  co_return co_await tls_write_http_payload_async(self.state, payload, "tls stream is closed");
}

Future<Result<HttpRequest, std::string>> __nebula_tls_server_http2_read_request(TlsServerStream self,
                                                                                std::int64_t max_header_bytes,
                                                                                std::int64_t max_body_bytes) {
  co_return co_await http2::server_read_request_async(std::move(self), max_header_bytes, max_body_bytes);
}

Future<Result<void, std::string>> __nebula_tls_server_http2_write_response(TlsServerStream self,
                                                                           HttpResponse response) {
  co_return co_await http2::server_write_response_async(std::move(self), std::move(response), true);
}

Future<Result<void, std::string>> __nebula_tls_server_http2_write_response_with_connection(
    TlsServerStream self,
    HttpResponse response,
    bool close_connection) {
  co_return co_await http2::server_write_response_async(std::move(self),
                                                        std::move(response),
                                                        close_connection);
}
