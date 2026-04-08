#include "cli_shared.hpp"

#include <iostream>

int main(int argc, char** argv) {
  std::vector<std::string> args;
  args.reserve(static_cast<std::size_t>(argc));
  for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);

  if (args.size() < 2) {
    print_usage();
    return 2;
  }

  const std::string cmd = args[1];
  if (cmd == "--help" || cmd == "-h") {
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

  // Best-effort repo root discovery: assume `.../build/nebula`.
  try {
    fs::path exe = fs::absolute(fs::path(args[0]));
    fs::path guess = exe.parent_path().parent_path();
    if (fs::exists(guess / "runtime") && fs::exists(guess / "CMakeLists.txt")) {
      opt.repo_root = guess;
    } else if (fs::exists(guess / "include" / "runtime" / "nebula_runtime.hpp")) {
      opt.repo_root = guess / "include";
    } else {
      opt.repo_root = fs::current_path();
    }
  } catch (...) {
    opt.repo_root = fs::current_path();
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
