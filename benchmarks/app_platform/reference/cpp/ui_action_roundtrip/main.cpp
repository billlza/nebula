#include "../common/ui_model.hpp"

int main() {
  return app_platform_cpp::run_benchmark(app_platform_cpp::ui::run_action_roundtrip);
}
