#include "../common/thin_host_model.hpp"

int main() {
  return app_platform_cpp::run_benchmark(app_platform_cpp::thin_host::run_payload_command_roundtrip);
}
