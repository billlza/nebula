#include "runtime/nebula_runtime.hpp"

#include <iostream>
#include <string>
#include <variant>

namespace {

using VoidResult = nebula::rt::Result<void, std::string>;

template <typename Action> bool throws_bad_variant_access(Action action) {
  try {
    action();
  } catch (const std::bad_variant_access &) {
    return true;
  } catch (...) {
    return false;
  }
  return false;
}

} // namespace

int main() {
  VoidResult ok = VoidResult::Ok{};
  nebula::rt::result_ok_move(ok);
  nebula::rt::result_ok_ref(ok);
  const VoidResult &const_ok = ok;
  nebula::rt::result_ok_ref(const_ok);

  VoidResult error = VoidResult::Err{"expected failure"};
  if (!throws_bad_variant_access([&error] { nebula::rt::result_ok_move(error); }) ||
      !throws_bad_variant_access([&error] { nebula::rt::result_ok_ref(error); })) {
    std::cerr << "mutable Result<void, E> helpers did not reject Err\n";
    return 1;
  }

  const VoidResult &const_error = error;
  if (!throws_bad_variant_access([&const_error] { nebula::rt::result_ok_ref(const_error); })) {
    std::cerr << "const Result<void, E> helper did not reject Err\n";
    return 1;
  }

  return 0;
}
