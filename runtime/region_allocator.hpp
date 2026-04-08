#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

// v0.1 runtime: bump/arena region allocator.
//
// Properties:
// - O(1) amortized allocation (no per-object malloc in the common case)
// - Frees all memory at end of scope
// - Runs non-trivial destructors in reverse allocation order
struct Region {
  struct Block {
    std::uint8_t* data = nullptr;
    std::size_t cap = 0;
    std::size_t off = 0;
  };
  struct DtorRecord {
    void* ptr = nullptr;
    void (*destroy)(void*) = nullptr;
  };

  std::vector<Block> blocks;
  std::vector<DtorRecord> dtors;
  std::size_t default_block_size = 64 * 1024; // 64KiB

  Region() = default;
  explicit Region(std::size_t block_size) : default_block_size(block_size) {}

  Region(const Region&) = delete;
  Region& operator=(const Region&) = delete;
  Region(Region&&) = default;
  Region& operator=(Region&&) = default;

  void* alloc(std::size_t size, std::size_t align = alignof(std::max_align_t)) {
    if (size == 0) size = 1;
    if (align == 0) align = 1;

    auto alloc_from = [&](Block& b) -> void* {
      const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(b.data) + b.off;
      const std::uintptr_t aligned =
          (base + (align - 1)) & ~(static_cast<std::uintptr_t>(align - 1));
      const std::size_t new_off =
          (aligned - reinterpret_cast<std::uintptr_t>(b.data)) + size;
      if (new_off > b.cap) return nullptr;
      b.off = new_off;
      return reinterpret_cast<void*>(aligned);
    };

    if (!blocks.empty()) {
      if (void* p = alloc_from(blocks.back())) return p;
    }

    const std::size_t cap = std::max(default_block_size, size + align);
    void* mem = std::malloc(cap);
    if (!mem) throw std::bad_alloc();
    Block b;
    b.data = reinterpret_cast<std::uint8_t*>(mem);
    b.cap = cap;
    b.off = 0;
    blocks.push_back(b);
    void* p = alloc_from(blocks.back());
    if (!p) throw std::bad_alloc();
    return p;
  }

  void register_dtor(void* ptr, void (*destroy)(void*)) {
    if (!ptr || !destroy) return;
    dtors.push_back(DtorRecord{ptr, destroy});
  }

  template <typename T, typename... Args>
  T* make(Args&&... args) {
    void* mem = alloc(sizeof(T), alignof(T));
    T* obj = new (mem) T(std::forward<Args>(args)...);
    if constexpr (!std::is_trivially_destructible_v<T>) {
      register_dtor(static_cast<void*>(obj), [](void* p) {
        static_cast<T*>(p)->~T();
      });
    }
    return obj;
  }

  ~Region() {
    for (auto it = dtors.rbegin(); it != dtors.rend(); ++it) {
      if (it->destroy) it->destroy(it->ptr);
    }
    for (auto& b : blocks) {
      std::free(b.data);
    }
  }
};

#define NEBULA_REGION(name) Region name

#define NEBULA_ALLOC(region, T, ...) (region).make<T>(__VA_ARGS__)

