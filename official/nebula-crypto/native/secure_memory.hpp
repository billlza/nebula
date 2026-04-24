#pragma once

#include "runtime/nebula_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace nebula::crypto_native {

template <typename T>
using SecretResult = rt::Result<T, std::string>;

inline void secure_zeroize(void* ptr, std::size_t len) {
  if (ptr == nullptr) return;
  volatile std::uint8_t* cursor = static_cast<volatile std::uint8_t*>(ptr);
  while (len-- > 0) {
    *cursor++ = 0;
  }
}

inline void secure_zeroize(std::string& bytes) {
  if (!bytes.empty()) {
    secure_zeroize(bytes.data(), bytes.size());
  }
}

class SecretBytesOwner {
 public:
  SecretBytesOwner() = default;
  explicit SecretBytesOwner(std::string bytes) : bytes_(std::move(bytes)) {}

  SecretBytesOwner(const SecretBytesOwner&) = delete;
  auto operator=(const SecretBytesOwner&) -> SecretBytesOwner& = delete;

  SecretBytesOwner(SecretBytesOwner&& other) noexcept : bytes_(std::move(other.bytes_)) {}

  auto operator=(SecretBytesOwner&& other) noexcept -> SecretBytesOwner& {
    if (this != &other) {
      secure_zeroize(bytes_);
      bytes_ = std::move(other.bytes_);
    }
    return *this;
  }

  ~SecretBytesOwner() {
    secure_zeroize(bytes_);
  }

  static auto adopt_checked(rt::Bytes bytes,
                            std::size_t expected,
                            std::string_view label) -> SecretResult<SecretBytesOwner> {
    if (bytes.data.size() != expected) {
      secure_zeroize(bytes.data);
      return rt::err_result<SecretBytesOwner>(size_error(label, expected, bytes.data.size()));
    }
    return rt::ok_result(SecretBytesOwner(std::move(bytes.data)));
  }

  static auto copy_checked(const rt::Bytes& bytes,
                           std::size_t expected,
                           std::string_view label) -> SecretResult<SecretBytesOwner> {
    if (bytes.data.size() != expected) {
      return rt::err_result<SecretBytesOwner>(size_error(label, expected, bytes.data.size()));
    }
    try {
      return rt::ok_result(SecretBytesOwner(bytes.data));
    } catch (const std::exception& ex) {
      return rt::err_result<SecretBytesOwner>(std::string(label) + " allocation failed: " +
                                              ex.what());
    }
  }

  static auto adopt_or_panic(rt::Bytes bytes,
                             std::size_t expected,
                             std::string_view label) -> SecretBytesOwner {
    if (bytes.data.size() != expected) {
      secure_zeroize(bytes.data);
      rt::panic("corrupt " + std::string(label) + ": expected " + std::to_string(expected) +
                " bytes, got " + std::to_string(bytes.data.size()));
    }
    return SecretBytesOwner(std::move(bytes.data));
  }

  static auto copy_or_panic(const rt::Bytes& bytes,
                            std::size_t expected,
                            std::string_view label) -> SecretBytesOwner {
    if (bytes.data.size() != expected) {
      rt::panic("corrupt " + std::string(label) + ": expected " + std::to_string(expected) +
                " bytes, got " + std::to_string(bytes.data.size()));
    }
    try {
      return SecretBytesOwner(bytes.data);
    } catch (const std::exception& ex) {
      rt::panic(std::string(label) + " allocation failed: " + ex.what());
    }
  }

  static auto copy_from_raw_or_panic(const std::uint8_t* data,
                                     std::size_t len,
                                     std::string_view label) -> SecretBytesOwner {
    try {
      return SecretBytesOwner(std::string(reinterpret_cast<const char*>(data), len));
    } catch (const std::exception& ex) {
      rt::panic(std::string(label) + " allocation failed: " + ex.what());
    }
  }

  auto data() const -> const std::uint8_t* {
    return reinterpret_cast<const std::uint8_t*>(bytes_.data());
  }

  auto size() const -> std::size_t {
    return bytes_.size();
  }

  auto export_bytes() -> rt::Bytes {
    return rt::Bytes{std::move(bytes_)};
  }

  auto export_slice_or_panic(std::size_t offset,
                             std::size_t len,
                             std::string_view label) const -> rt::Bytes {
    if (offset > bytes_.size() || len > bytes_.size() - offset) {
      rt::panic("corrupt " + std::string(label) + ": slice is out of bounds");
    }
    try {
      return rt::Bytes{std::string(bytes_.data() + offset, len)};
    } catch (const std::exception& ex) {
      rt::panic(std::string(label) + " allocation failed: " + ex.what());
    }
  }

  auto constant_time_equal(const SecretBytesOwner& other) const -> bool {
    if (bytes_.size() != other.bytes_.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < bytes_.size(); ++i) {
      diff |= static_cast<unsigned char>(bytes_[i]) ^
              static_cast<unsigned char>(other.bytes_[i]);
    }
    return diff == 0;
  }

 private:
  static auto size_error(std::string_view label,
                         std::size_t expected,
                         std::size_t actual) -> std::string {
    (void)actual;
    return std::string(label) + " must be exactly " + std::to_string(expected) + " bytes";
  }

  std::string bytes_;
};

}  // namespace nebula::crypto_native
