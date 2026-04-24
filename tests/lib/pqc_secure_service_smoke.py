from __future__ import annotations

import http.client
import os
import shutil
import subprocess
import time
from pathlib import Path

from service_probe import wait_for_observe_event_in_file


def rewrite_server_manifest(server_project: Path, repo_root: Path) -> None:
    server_project.joinpath("nebula.toml").write_text(
        f"""schema_version = 1

[package]
name = "pqc-secure-service"
version = "0.1.0"
entry = "src/main.nb"
src_dir = "src"

[dependencies]
crypto = {{ path = "{(repo_root / 'official' / 'nebula-crypto').resolve()}" }}
pqc = {{ path = "{(repo_root / 'official' / 'nebula-pqc-protocols').resolve()}" }}
service = {{ path = "{(repo_root / 'official' / 'nebula-service').resolve()}" }}
""",
        encoding="utf-8",
    )


def write_keygen_project(root: Path, repo_root: Path) -> None:
    keygen_root = root / "keygen"
    keygen_root.joinpath("nebula.toml").write_text(
        f"""schema_version = 1

[package]
name = "example-pqc-keygen"
version = "0.1.0"
entry = "src/main.nb"
src_dir = "src"

[dependencies]
crypto = {{ path = "{(repo_root / 'official' / 'nebula-crypto').resolve()}" }}
""",
        encoding="utf-8",
    )
    keygen_root.joinpath("src", "main.nb").write_text(
        """module main
import crypto::hash
import crypto::pqc.kem
import crypto::pqc.sign

fn main() -> Void {
  match ml_kem_768_keypair() {
    Ok(kem_pair) => {
      match ml_dsa_65_keypair() {
        Ok(sign_pair) => {
          print(hex(kem_pair.public_key().to_bytes()))
          print(hex(kem_pair.secret_key().to_bytes()))
          print(hex(sign_pair.public_key().to_bytes()))
          print(hex(sign_pair.secret_key().to_bytes()))
        }
        Err(msg) => { print(msg) }
      }
    }
    Err(msg) => { print(msg) }
  }
}
""",
        encoding="utf-8",
    )


def write_client_project(root: Path, repo_root: Path, port: int) -> None:
    client_root = root / "client"
    client_root.joinpath("nebula.toml").write_text(
        f"""schema_version = 1

[package]
name = "example-pqc-client"
version = "0.1.0"
entry = "src/main.nb"
src_dir = "src"

[dependencies]
crypto = {{ path = "{(repo_root / 'official' / 'nebula-crypto').resolve()}" }}
pqc = {{ path = "{(repo_root / 'official' / 'nebula-pqc-protocols').resolve()}" }}
""",
        encoding="utf-8",
    )
    client_root.joinpath("src", "main.nb").write_text(
        CLIENT_SOURCE.replace("__PQC_SECURE_TEST_PORT__", str(port)),
        encoding="utf-8",
    )


def extract_key_lines(run: subprocess.CompletedProcess[str]) -> tuple[str, str, str, str]:
    key_lines = [
        line.strip()
        for line in run.stdout.splitlines()
        if line.strip()
        and not line.startswith("[")
        and not line.startswith("wrote:")
        and not line.startswith("wrote artifact:")
    ]
    if len(key_lines) < 4:
        raise SystemExit(f"expected 4 key lines, got: {key_lines!r}")
    return tuple(key_lines[-4:])  # type: ignore[return-value]


def extract_client_lines(run: subprocess.CompletedProcess[str]) -> tuple[str, str, str]:
    app_lines = [
        line.strip()
        for line in run.stdout.splitlines()
        if line.strip()
        and not line.startswith("[")
        and not line.startswith("wrote:")
        and not line.startswith("wrote artifact:")
    ]
    if len(app_lines) < 3:
        raise SystemExit(f"expected 3 application output lines, got: {app_lines!r}")
    return tuple(app_lines[-3:])  # type: ignore[return-value]


def main() -> int:
    repo_root = Path(os.environ["NEBULA_REPO_ROOT"])
    nebula = os.environ["NEBULA_BINARY"]
    root = Path("work/example_pqc_secure_service")
    shutil.rmtree(root, ignore_errors=True)
    (root / "keygen" / "src").mkdir(parents=True, exist_ok=True)
    (root / "client" / "src").mkdir(parents=True, exist_ok=True)
    server_project = root / "server"
    shutil.copytree(repo_root / "examples" / "pqc_secure_service", server_project)
    rewrite_server_manifest(server_project, repo_root)
    write_keygen_project(root, repo_root)

    subprocess.run(
        [nebula, "fetch", str(root / "keygen")],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    keygen = subprocess.run(
        [nebula, "run", str(root / "keygen"), "--run-gate", "none"],
        check=True,
        capture_output=True,
        text=True,
    )
    (
        kem_public_key_hex,
        kem_secret_key_hex,
        sign_public_key_hex,
        sign_secret_key_hex,
    ) = extract_key_lines(keygen)

    subprocess.run(
        [nebula, "update", str(server_project)],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    server_stdout = root / "server.stdout"
    server_stderr = root / "server.stderr"
    server_env = os.environ.copy()
    server_env.update(
        {
            "NEBULA_BIND_HOST": "127.0.0.1",
            "NEBULA_PORT": "0",
            "PQC_SECURE_SERVICE_KEM_PUBLIC_KEY_HEX": kem_public_key_hex,
            "PQC_SECURE_SERVICE_KEM_SECRET_KEY_HEX": kem_secret_key_hex,
            "PQC_SECURE_SERVICE_SIGN_PUBLIC_KEY_HEX": sign_public_key_hex,
            "PQC_SECURE_SERVICE_SIGN_SECRET_KEY_HEX": sign_secret_key_hex,
        }
    )

    with server_stdout.open("w") as stdout_handle, server_stderr.open("w") as stderr_handle:
        server = subprocess.Popen(
            [nebula, "run", str(server_project), "--run-gate", "none"],
            stdout=stdout_handle,
            stderr=stderr_handle,
            env=server_env,
            text=True,
        )
        try:
            bound = wait_for_observe_event_in_file(server_stderr, "listener_bound", 20, server)
            port = bound.get("port")
            if not isinstance(port, int) or port <= 0:
                raise SystemExit(f"invalid listener_bound payload: {bound!r}")

            deadline = time.time() + 30
            ready = False
            while time.time() < deadline:
                try:
                    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=1)
                    conn.request("GET", "/healthz")
                    resp = conn.getresponse()
                    resp.read()
                    conn.close()
                    if resp.status == 200:
                        ready = True
                        break
                except Exception:
                    if server.poll() is not None:
                        raise SystemExit(
                            f"secure service example exited before ready: rc={server.returncode}"
                        )
                    time.sleep(0.2)
            if not ready:
                raise SystemExit("secure service example did not become ready")

            write_client_project(root, repo_root, port)
            subprocess.run(
                [nebula, "fetch", str(root / "client")],
                check=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            client = subprocess.run(
                [nebula, "run", str(root / "client"), "--run-gate", "none"],
                check=True,
                capture_output=True,
                text=True,
            )
            request_body_text, response_body_text, status = extract_client_lines(client)
            if status != "pqc-secure-service-ok":
                raise SystemExit(f"unexpected client status: {status!r}")
            if "hello-preview-secure-service" in request_body_text:
                raise SystemExit("plaintext leaked into request body payload")
            if "hello-preview-secure-service" in response_body_text:
                raise SystemExit("plaintext leaked into response body payload")
        finally:
            if server.poll() is None:
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait(timeout=5)
    return 0


CLIENT_SOURCE = """module main
import crypto::hash
import crypto::pqc.kem
import crypto::pqc.sign
import pqc::channel
import std::bytes
import std::http
import std::http_json
import std::json
import std::net
import std::result

struct Bootstrap {
  kem_public_key: MlKem768PublicKey
  sign_public_key: MlDsa65PublicKey
}

struct ClientOpen {
  hello: ClientHello
  session: Session
}

struct EchoCapture {
  request_body_text: String
  response_body_text: String
  reply_text: String
}

fn test_port() -> Int {
  return __PQC_SECURE_TEST_PORT__
}

async fn request_json(addr: SocketAddr, request: ClientRequest) -> Result<Json, String> {
  match await connect(addr) {
    Ok(stream) => {
      match await stream.write_request(request) {
        Ok(_) => {
          match await stream.read_response() {
            Ok(resp) => {
              return parse_json_body(resp.body)
            }
            Err(msg) => {
              return Err(msg)
            }
          }
        }
        Err(msg) => {
          return Err(msg)
        }
      }
    }
    Err(msg) => {
      return Err(msg)
    }
  }
}

async fn resolve_bootstrap(addr: SocketAddr) -> Result<Bootstrap, String> {
  match await request_json(addr, get_request("localhost", "/secure/bootstrap")) {
    Ok(value) => {
      match get_string(value, "kem_public_key_hex") {
        Ok(kem_hex) => {
          match get_string(value, "sign_public_key_hex") {
            Ok(sign_hex) => {
              match from_hex(kem_hex) {
                Ok(kem_bytes) => {
                  match ml_kem_768_public_key_from_bytes(kem_bytes) {
                    Ok(kem_public_key) => {
                      match from_hex(sign_hex) {
                        Ok(sign_bytes) => {
                          match ml_dsa_65_public_key_from_bytes(sign_bytes) {
                            Ok(sign_public_key) => {
                              return Ok(Bootstrap(kem_public_key, sign_public_key))
                            }
                            Err(msg) => { return Err(msg) }
                          }
                        }
                        Err(msg) => { return Err(msg) }
                      }
                    }
                    Err(msg) => { return Err(msg) }
                  }
                }
                Err(msg) => { return Err(msg) }
              }
            }
            Err(msg) => { return Err(msg) }
          }
        }
        Err(msg) => { return Err(msg) }
      }
    }
    Err(msg) => {
      return Err(msg)
    }
  }
}

async fn open_channel(addr: SocketAddr, bootstrap: Bootstrap) -> Result<ClientOpen, String> {
  match ml_dsa_65_keypair() {
    Ok(client_sign_keys) => {
      let signer = channel_signer(client_sign_keys)
      match initiate(bootstrap.kem_public_key, signed(signer)) {
        Ok(init) => {
          let open_body = stringify(object1(
            "hello_text",
            string_value(stringify(init.hello().as_json()))
          ))
          let req = post_request("localhost", "/secure/open", "application/json; charset=utf-8", from_string(open_body))
          match await request_json(addr, req) {
            Ok(value) => {
              match get_string(value, "accept_text") {
                Ok(accept_text) => {
                  match server_accept_message_from_json_text(accept_text) {
                    Ok(accept_msg) => {
                      match finalize(init, pinned_peer(bootstrap.sign_public_key), accept_msg) {
                        Ok(session) => {
                          return Ok(ClientOpen(init.hello(), session))
                        }
                        Err(msg) => { return Err(msg) }
                      }
                    }
                    Err(msg) => { return Err(msg) }
                  }
                }
                Err(msg) => { return Err(msg) }
              }
            }
            Err(msg) => {
              return Err(msg)
            }
          }
        }
        Err(msg) => {
          return Err(msg)
        }
      }
    }
    Err(msg) => {
      return Err(msg)
    }
  }
}

async fn roundtrip_secure_echo(addr: SocketAddr, opened: ClientOpen, cleartext: String) -> Result<EchoCapture, String> {
  match seal_next(opened.session, from_string(cleartext), from_string("nebula-http-request")) {
    Ok(sent) => {
      let request_body_text = stringify(object2(
        "hello_text",
        string_value(stringify(opened.hello.as_json())),
        "ciphertext_text",
        string_value(to_string(sent.ciphertext()))
      ))
      let req = post_request("localhost", "/secure/echo", "application/json; charset=utf-8", from_string(request_body_text))
      match await request_json(addr, req) {
        Ok(value) => {
          match get_string(value, "ciphertext_text") {
            Ok(response_body_ciphertext) => {
              let response_body_text = stringify(object1(
                "ciphertext_text",
                string_value(response_body_ciphertext)
              ))
              match open_next(sent.session(), from_string(response_body_ciphertext), from_string("nebula-http-response")) {
                Ok(reply) => {
                  return Ok(EchoCapture(request_body_text, response_body_text, to_string(reply.plaintext())))
                }
                Err(msg) => { return Err(msg) }
              }
            }
            Err(msg) => { return Err(msg) }
          }
        }
        Err(msg) => { return Err(msg) }
      }
    }
    Err(msg) => { return Err(msg) }
  }
}

async fn main() -> Void {
  let ok = true
  let cleartext = "hello-preview-secure-service"
  let port = test_port()
  match ipv4("127.0.0.1", port) {
    Ok(addr) => {
      match await resolve_bootstrap(addr) {
        Ok(bootstrap) => {
          match await open_channel(addr, bootstrap) {
            Ok(opened) => {
              match await roundtrip_secure_echo(addr, opened, cleartext) {
                Ok(capture) => {
                  print(capture.request_body_text)
                  print(capture.response_body_text)
                  if capture.reply_text != cleartext {
                    ok = false
                  }
                }
                Err(msg) => {
                  print(msg)
                  ok = false
                }
              }
            }
            Err(msg) => {
              print(msg)
              ok = false
            }
          }
        }
        Err(msg) => {
          print(msg)
          ok = false
        }
      }
    }
    Err(msg) => {
      print(msg)
      ok = false
    }
  }
  if ok {
    print("pqc-secure-service-ok")
  } else {
    print("pqc-secure-service-bad")
  }
}
"""


if __name__ == "__main__":
    raise SystemExit(main())
