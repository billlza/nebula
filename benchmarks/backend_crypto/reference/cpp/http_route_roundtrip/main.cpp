#include "bench_runtime.hpp"

#include <cstdlib>
#include <string>

namespace {

struct Response {
  int status;
  std::string content_type;
  std::string body;
};

void workload() {
  const std::string path = "/hello/nebula";
  const std::string prefix = "/hello/";
  if (path.rfind(prefix, 0) != 0) {
    std::abort();
  }
  const std::string name = path.substr(prefix.size());
  Response response{200, "text/plain; charset=utf-8", name};
  if (response.status != 200 || response.content_type != "text/plain; charset=utf-8" ||
      response.body != "nebula") {
    std::abort();
  }
}

}  // namespace

int main() {
  const auto metrics = backend_crypto_bench::run_bench(workload);
  backend_crypto_bench::emit_metrics_json(metrics);
  return 0;
}
