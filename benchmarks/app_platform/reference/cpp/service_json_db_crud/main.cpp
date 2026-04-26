#include "../common/service_json_db_crud_model.hpp"

int main() {
  return app_platform_cpp::run_benchmark(app_platform_cpp::service_json_db_crud::run_workload_once);
}
