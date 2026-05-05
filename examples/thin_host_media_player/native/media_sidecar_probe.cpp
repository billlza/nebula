#include <mpv/client.h>

#include <libtorrent/session.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/version.hpp>

#include <exception>
#include <iostream>
#include <string>

namespace {

std::string json_escape(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (const char ch : value) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += ch;
        break;
    }
  }
  return out;
}

std::string quoted(const std::string& value) {
  return std::string("\"") + json_escape(value) + "\"";
}

std::string mpv_error_text(int code) {
  const char* text = mpv_error_string(code);
  if (text == nullptr) return "unknown mpv error";
  return text;
}

int probe_mpv(std::string* error) {
  mpv_handle* handle = mpv_create();
  if (handle == nullptr) {
    *error = "mpv_create returned null";
    return 1;
  }

  int rc = mpv_set_option_string(handle, "terminal", "no");
  if (rc < 0) {
    *error = mpv_error_text(rc);
    mpv_destroy(handle);
    return 1;
  }
  rc = mpv_set_option_string(handle, "idle", "yes");
  if (rc < 0) {
    *error = mpv_error_text(rc);
    mpv_destroy(handle);
    return 1;
  }
  rc = mpv_set_option_string(handle, "vo", "null");
  if (rc < 0) {
    *error = mpv_error_text(rc);
    mpv_destroy(handle);
    return 1;
  }
  rc = mpv_set_option_string(handle, "ao", "null");
  if (rc < 0) {
    *error = mpv_error_text(rc);
    mpv_destroy(handle);
    return 1;
  }
  rc = mpv_initialize(handle);
  if (rc < 0) {
    *error = mpv_error_text(rc);
    mpv_terminate_destroy(handle);
    return 1;
  }
  mpv_terminate_destroy(handle);
  return 0;
}

int probe_libtorrent(std::string* error) {
  try {
    lt::settings_pack settings;
    settings.set_str(lt::settings_pack::listen_interfaces, "127.0.0.1:0");
    lt::session session(settings);
    const lt::session_status status = session.status();
    (void)status;
    return 0;
  } catch (const std::exception& ex) {
    *error = ex.what();
    return 1;
  } catch (...) {
    *error = "unknown libtorrent exception";
    return 1;
  }
}

}  // namespace

int main() {
  std::string mpv_error;
  std::string torrent_error;
  const int mpv_rc = probe_mpv(&mpv_error);
  const int torrent_rc = probe_libtorrent(&torrent_error);
  const bool ok = mpv_rc == 0 && torrent_rc == 0;

  std::cout << "{"
            << "\"schema\":\"media-player.native-sidecar-proof.v1\","
            << "\"status\":" << quoted(ok ? "ok" : "failed") << ","
            << "\"mpv\":{\"api\":\"libmpv-c-api\",\"status\":"
            << quoted(mpv_rc == 0 ? "ready" : "failed")
            << ",\"error\":" << quoted(mpv_error) << "},"
            << "\"torrent\":{\"api\":\"libtorrent-rasterbar\",\"status\":"
            << quoted(torrent_rc == 0 ? "ready" : "failed")
            << ",\"version\":" << quoted(LIBTORRENT_VERSION)
            << ",\"loopback\":\"session-created\","
            << "\"error\":" << quoted(torrent_error) << "}"
            << "}\n";
  return ok ? 0 : 1;
}
