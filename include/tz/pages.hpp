// Memory VM - address space pages (author's decision 2026-10-01).
//
// 
// 
// 
// 
//.
//
// A growing vector is its own range of addresses: pages connect as they grow, data does not
// move. Small - a piece of class 2^k words in the general range; free pieces - lists outside the
// data. My: * range size, classes, small to growing.
#pragma once

#include <sys/mman.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace tz {

struct address_space {
  static constexpr size_t page = 4096;
  static constexpr size_t big_range = size_t(1) << 36; // growing vector: 64 GB addresses
  static constexpr size_t small_range = size_t(1) << 40; // Small chunks: 1 TB of addresses
  static constexpr uint32_t classes = 10; // 2^1 ... 2^9 words: 16 B ... 4 KB
  static constexpr uint32_t small_max = uint32_t(1) << (classes - 1); // chunk

  std::byte * small_base = nullptr;
  size_t small_used = 0, small_committed = 0;
  std::vector<uint64_t *> small_free[classes];
  std::vector<uint64_t *> big_free; // ranges with given pages - for new vectors

  static std::byte * reserve(size_t n) {
    void * p = mmap(nullptr, n, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if(p == MAP_FAILED) throw std::bad_alloc();
    return static_cast<std::byte *>(p);
  }
  static void commit(void * at, size_t n) {
    if(mprotect(at, n, PROT_READ | PROT_WRITE) != 0) throw std::bad_alloc();
  }

  static uint32_t class_of(uint32_t words) {
    uint32_t k = 1;
    while((uint32_t(1) << k) < words) ++k;
    return k;
  }
  uint64_t * take_small(uint32_t k) {
    if(!small_free[k].empty()) {
      uint64_t * p = small_free[k].back();
      small_free[k].pop_back();
      return p;
    }
    if(!small_base) small_base = reserve(small_range);
    size_t bytes = size_t(8) << k;
    small_used = (small_used + bytes - 1) / bytes * bytes; // piece aligned in size
    if(small_used + bytes > small_committed) {
      size_t grow = (small_used + bytes - small_committed + page - 1) / page * page;
      commit(small_base + small_committed, grow);
      small_committed += grow;
    }
    auto * p = reinterpret_cast<uint64_t *>(small_base + small_used);
    small_used += bytes;
    return p;
  }
  void give_small(uint64_t * p, uint32_t k) { small_free[k].push_back(p); }

  uint64_t * take_big() {
    if(!big_free.empty()) {
      uint64_t * p = big_free.back();
      big_free.pop_back();
      return p;
    }
    return reinterpret_cast<uint64_t *>(reserve(big_range));
  }
  // pages to the system, addresses remain behind VM: the range will go to a new vector
  void give_big(uint64_t * p, size_t committed) {
    madvise(p, committed, MADV_DONTNEED);
    mprotect(p, committed, PROT_NONE);
    big_free.push_back(p);
  }
};

address_space & space() {
  static address_space s;
  return s;
}

// 8-byte word vector (i64, f64 - bits): size and capacity - out of data.
struct words {
  uint64_t * data = nullptr;
  uint32_t n = 0;
  uint32_t cap = 0; // words: small - 2^k, growing - on connected pages
  bool big = false;

  words() = default;
  words(words const & o) { *this = o; }
  words(words && o) noexcept { steal(o); }
  words & operator=(words const & o) {
    if(this == &o) return *this;
    clear();
    reserve(o.n);
    if(o.n) std::memcpy(data, o.data, size_t(o.n) * 8);
    n = o.n;
    return *this;
  }
  words & operator=(words && o) noexcept {
    if(this == &o) return *this;
    release();
    steal(o);
    return *this;
  }
  ~words() { release(); }

  uint32_t size() const { return n; }
  bool empty() const { return n == 0; }
  uint64_t & operator[](uint32_t i) { return data[i]; }
  uint64_t operator[](uint32_t i) const { return data[i]; }
  void clear() { n = 0; }
  void push_back(uint64_t w) {
    if(n == cap) reserve(n + 1);
    data[n++] = w;
  }

  // place under want words: small grows in classes, then - his range (no longer moves)
  void reserve(uint32_t want) {
    if(want <= cap) return;
    if(big) {
      size_t have = size_t(cap) * 8, need = (size_t(want) * 8 + address_space::page - 1) / address_space::page * address_space::page;
      need = std::max(need, have * 2); // pages - with a margin of twice (without moving)
      need = std::min(need, address_space::big_range);
      address_space::commit(reinterpret_cast<std::byte *>(data) + have, need - have);
      cap = uint32_t(need / 8);
      return;
    }
    uint64_t * old = data;
    uint32_t old_cap = cap;
    uint32_t grow = std::max(want, cap * 2); // petty
    if(grow <= address_space::small_max) {
      uint32_t k = address_space::class_of(grow);
      data = space().take_small(k);
      cap = uint32_t(1) << k;
    } else {
      data = space().take_big();
      big = true;
      size_t need = (size_t(want) * 8 + address_space::page - 1) / address_space::page * address_space::page;
      address_space::commit(data, need);
      cap = uint32_t(need / 8);
    }
    if(old) {
      if(n) std::memcpy(data, old, size_t(n) * 8);
      space().give_small(old, address_space::class_of(old_cap));
    }
  }

  void steal(words & o) {
    data = o.data;
    n = o.n;
    cap = o.cap;
    big = o.big;
    o.data = nullptr;
    o.n = o.cap = 0;
    o.big = false;
  }
  void release() {
    if(!data) return;
    if(big) space().give_big(data, size_t(cap) * 8);
    else space().give_small(data, address_space::class_of(cap));
    data = nullptr;
    n = cap = 0;
    big = false;
  }
};

} // namespace tz
