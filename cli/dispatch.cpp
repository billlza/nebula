#include "cli_shared.hpp"

#include <iostream>
#include <optional>
#include <cstdlib>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace {

std::optional<fs::path> self_executable_path(const char* argv0) {
#if defined(_WIN32)
  std::wstring buffer(MAX_PATH, L'\0');
  while (true) {
    const DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (len == 0) break;
    if (len < buffer.size()) {
      buffer.resize(len);
      return fs::weakly_canonical(fs::path(buffer));
    }
    buffer.resize(buffer.size() * 2);
  }
#elif defined(__APPLE__)
  uint32_t size = 0;
  (void)_NSGetExecutablePath(nullptr, &size);
  if (size > 0) {
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
      return fs::weakly_canonical(fs::path(buffer.c_str()));
    }
  }
#else
  std::vector<char> buffer(1024, '\0');
  while (true) {
    const ssize_t len = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (len < 0) break;
    if (static_cast<std::size_t>(len) < buffer.size() - 1) {
      buffer[static_cast<std::size_t>(len)] = '\0';
      return fs::weakly_canonical(fs::path(buffer.data()));
    }
    buffer.resize(buffer.size() * 2, '\0');
  }
#endif

  if (argv0 != nullptr && *argv0 != '\0') {
    return fs::absolute(fs::path(argv0)).lexically_normal();
  }
  return std::nullopt;
}

} // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args;
  args.reserve(static_cast<std::size_t>(argc));
  for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);

  if (args.size() < 2) {
    print_usage();
    return 2;
  }

  const std::string cmd = args[1];
  if (cmd == "--help" || cmd == "-h" || cmd == "-help") {
    print_usage();
    return 0;
  }
  if (cmd == "--version") {
    print_version(std::cout);
    return 0;
  }

  const bool is_file_cmd = (cmd == "check" || cmd == "build" || cmd == "run");
  const bool is_dir_cmd = (cmd == "test" || cmd == "bench");
  const bool is_tool_cmd =
      (cmd == "new" || cmd == "add" || cmd == "publish" || cmd == "fetch" || cmd == "update" || cmd == "fmt" ||
       cmd == "explain" || cmd == "lsp");
  if (!is_file_cmd && !is_dir_cmd && !is_tool_cmd) {
    print_usage();
    return 2;
  }
  if (is_file_cmd && args.size() < 3) {
    print_usage();
    return 2;
  }

  CliOptions opt;

  // Resolve the runtime/include root from the actual executable location so PATH-based
  // invocation and installed binaries behave the same way.
  try {
    const auto exe = self_executable_path(argc > 0 ? argv[0] : nullptr);
    fs::path resolved_exe = exe.value_or(fs::absolute(fs::path(args[0])));
    resolved_exe = resolved_exe.lexically_normal();
    opt.self_executable = resolved_exe;
    const fs::path exe_dir = resolved_exe.parent_path();
    const fs::path install_root = exe_dir.parent_path();
    const fs::path build_include = exe_dir / "generated" / "include";
    const fs::path build_std = exe_dir / "generated" / "std";
    const fs::path install_include = install_root / "include";
    const fs::path install_std = install_root / "share" / "nebula" / "std";
    const fs::path build_backend_sdk = install_root / "official";
    const fs::path install_backend_sdk = install_root / "share" / "nebula" / "sdk" / "backend";
    if (const char* backend_root = std::getenv("NEBULA_BACKEND_SDK_ROOT");
        backend_root != nullptr && *backend_root != '\0') {
      opt.backend_sdk_root = fs::absolute(fs::path(backend_root)).lexically_normal();
      if (!fs::exists(opt.backend_sdk_root / "nebula-service" / "nebula.toml")) {
        opt.backend_sdk_root_error =
            "nebula backend SDK root is missing nebula-service: expected "
            + (opt.backend_sdk_root / "nebula-service" / "nebula.toml").string();
      }
    }

    if (fs::exists(build_include / "runtime" / "nebula_runtime.hpp")) {
      opt.repo_root = install_root;
      opt.include_root = build_include;
      if (fs::exists(build_std / "task.nb")) {
        opt.std_root = build_std;
      } else {
        opt.std_root.clear();
        opt.std_root_error =
            "nebula bundled std sources were not found next to the executable: expected "
            + (build_std / "task.nb").string() + " for a build-tree binary";
      }
      if (opt.backend_sdk_root.empty()) {
        if (fs::exists(build_backend_sdk / "nebula-service" / "nebula.toml")) {
          opt.backend_sdk_root = build_backend_sdk;
        } else {
          opt.backend_sdk_root_error =
              "nebula backend SDK sources were not found next to the build-tree binary: expected "
              + (build_backend_sdk / "nebula-service" / "nebula.toml").string();
        }
      }
    } else if (fs::exists(install_include / "runtime" / "nebula_runtime.hpp")) {
      opt.repo_root = install_root;
      opt.include_root = install_include;
      if (fs::exists(install_std / "task.nb")) {
        opt.std_root = install_std;
      } else {
        opt.std_root.clear();
        opt.std_root_error =
            "nebula bundled std sources were not found next to the executable: expected "
            + (install_std / "task.nb").string() + " for an installed binary";
      }
      if (opt.backend_sdk_root.empty()) {
        if (fs::exists(install_backend_sdk / "nebula-service" / "nebula.toml")) {
          opt.backend_sdk_root = install_backend_sdk;
        } else {
          opt.backend_sdk_root_error =
              "nebula backend SDK was not found next to the executable: expected "
              + (install_backend_sdk / "nebula-service" / "nebula.toml").string()
              + " for an installed binary";
        }
      }
    } else {
      opt.repo_root = install_root;
      opt.include_root.clear();
      opt.include_root_error =
          "nebula runtime headers were not found next to the executable: expected "
          + (install_include / "runtime" / "nebula_runtime.hpp").string()
          + " for an installed binary, or "
          + (build_include / "runtime" / "nebula_runtime.hpp").string()
          + " for a build-tree binary";
      opt.std_root.clear();
      opt.std_root_error =
          "nebula bundled std sources were not found next to the executable: expected "
          + (install_std / "task.nb").string() + " for an installed binary, or "
          + (build_std / "task.nb").string() + " for a build-tree binary";
      if (opt.backend_sdk_root.empty()) {
        opt.backend_sdk_root.clear();
        opt.backend_sdk_root_error =
            "nebula backend SDK was not found next to the executable: expected "
            + (install_backend_sdk / "nebula-service" / "nebula.toml").string()
            + " for an installed binary, or "
            + (build_backend_sdk / "nebula-service" / "nebula.toml").string()
            + " for a build-tree binary";
      }
    }
  } catch (...) {
    opt.repo_root.clear();
    opt.include_root.clear();
    opt.include_root_error = "nebula could not resolve its own install layout";
    opt.std_root.clear();
    opt.std_root_error = "nebula could not resolve the bundled std install layout";
    opt.backend_sdk_root.clear();
    opt.backend_sdk_root_error = "nebula could not resolve the backend SDK install layout";
  }

  if (is_tool_cmd) {
    if (cmd == "new") return cmd_new(args, opt);
    if (cmd == "add") return cmd_add(args, opt);
    if (cmd == "publish") return cmd_publish(args, opt);
    if (cmd == "fetch") return cmd_fetch(args, opt);
    if (cmd == "update") return cmd_update(args, opt);
    if (cmd == "fmt") return cmd_fmt(args, opt);
    if (cmd == "explain") return cmd_explain(args, opt);
    return cmd_lsp(args, opt);
  }

  std::string parse_err;
  if (!parse_cli_options(args, cmd, opt, parse_err)) {
    std::cerr << "error: " << parse_err << "\n";
    return 2;
  }

  // Root-cause compression v2 default policy:
  // auto => on for check/run, off elsewhere.
  opt.root_cause_v2_default_on = (cmd == "check" || cmd == "run");

  if (is_file_cmd) {
    const fs::path file = args[2];
    if (cmd == "check") return cmd_check(file, opt);
    if (cmd == "build") return cmd_build(file, opt);
    return cmd_run(file, opt);
  }

  if (cmd == "test") return cmd_test(opt);
  return cmd_bench(opt);
}
