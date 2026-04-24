#include "runtime/nebula_runtime.hpp"

#include <exception>
#include <string>

namespace {

using HttpResponse = nebula::rt::HttpResponse;
using ResponseResult = nebula::rt::Result<HttpResponse, std::string>;
using CaughtResponse = nebula::rt::Result<HttpResponse, std::string>;
using CaughtResponseResult = nebula::rt::Result<ResponseResult, std::string>;

inline std::string describe_std_exception(const std::exception& ex) {
  return std::string(ex.what());
}

} // namespace

nebula::rt::Future<CaughtResponse> __nebula_service_catch_response_panic(
    nebula::rt::Future<HttpResponse> future) {
  try {
    co_return nebula::rt::ok_result<HttpResponse>(co_await std::move(future));
  } catch (const nebula::rt::UserPanic& ex) {
    co_return nebula::rt::err_result<HttpResponse>(ex.what());
  } catch (const std::exception& ex) {
    co_return nebula::rt::err_result<HttpResponse>(describe_std_exception(ex));
  } catch (...) {
    co_return nebula::rt::err_result<HttpResponse>("unknown handler exception");
  }
}

nebula::rt::Future<CaughtResponseResult> __nebula_service_catch_result_response_panic(
    nebula::rt::Future<ResponseResult> future) {
  try {
    co_return nebula::rt::ok_result<ResponseResult>(co_await std::move(future));
  } catch (const nebula::rt::UserPanic& ex) {
    co_return nebula::rt::err_result<ResponseResult>(ex.what());
  } catch (const std::exception& ex) {
    co_return nebula::rt::err_result<ResponseResult>(describe_std_exception(ex));
  } catch (...) {
    co_return nebula::rt::err_result<ResponseResult>("unknown handler exception");
  }
}
