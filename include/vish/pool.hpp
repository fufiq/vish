// Pool node base - lists and text node offsets from the general base (S5.0, plan S5).
//
// 
// 
//;
// 
// 
//.
//
// *My* (decision 2026-11-06): base - NA ast_db (copy for AI samples, diff, restore_nodes and repository
// live in parallel), current - thread_local, as current_space in pages.hpp; page size host is not
// included in the format. List descriptor - {смещение, длина|класс}: 8 bytes instead of 24 in
// std::vector; copy of the descriptor - the same list (node trivially copied), deep copy - only
// explicitly (clone). Memory pool - one range of addresses, does not move; chunks in classes of 2^k
// words, free - lists off-data; pool grows and does not shrink (compact - later, when the storage is
// fixed).
#pragma once

#include "base.hpp"

#include <sys/mman.h>

#include <bit>
#include <compare>
#include <functional>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <map>
#include <span>
#include <string_view>
#include <vector>

namespace vish {

struct node_pool {
  static constexpr size_t words_max = size_t(1) << 30; // 4 GB addresses per base: growth limit
  uint64_t cap = words_max; // limit of this pool, words
  static constexpr uint32_t len_bits = 27; // Length - in the lower bits, class capacity - in the higher

  uint32_t * base = nullptr;
  uint32_t used = 1; // The word 0 is not given: offset 0 is an empty list
  std::map<uint32_t, std::vector<uint32_t>> free_runs; // size in words → free pieces (sizes: ew · 2^k)
  std::recursive_mutex lock;
  // Area in storage (S5.1): Pool memory - its display; ready - words are available without reference to the
  // area (it grows the file), the first change after fixing marks the area "dirty".
  uint64_t ready = words_max; // (Anonymous: All available)
  uint64_t garbage = 0; // words in thrown chunks (rewriting the list with a new chunk): assessment for compaction
  bool external = false;
  bool dirty = true; // Anonymous pool is always “dirty”: there is nothing to fix
  std::function<void(uint64_t)> grow_to; // At least as many words are available (the area grows the file)
  std::function<void()> mark_dirty;

  explicit node_pool(uint64_t capacity_words = words_max) : cap(capacity_words) {
    void * p = ::mmap(nullptr, cap * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if(p == MAP_FAILED) throw std::bad_alloc();
    base = static_cast<uint32_t *>(p);
  }
  // Pool in someone else's memory: at - word 0, used_words - occupied (≥ 1), ready_words - available words
  node_pool(uint32_t * at, uint64_t capacity_words, uint32_t used_words, uint64_t ready_words, std::function<void(uint64_t)> grow, std::function<void()> dirtied)
      : cap(capacity_words), base(at), used(used_words), ready(ready_words), external(true), dirty(false), grow_to(std::move(grow)), mark_dirty(std::move(dirtied)) {}
  node_pool(node_pool const &) = delete;
  node_pool & operator=(node_pool const &) = delete;
  ~node_pool() {
    if(base && !external) ::munmap(base, cap * 4);
  }

  void touch() {
    if(!dirty) {
      dirty = true;
      if(mark_dirty) mark_dirty();
    }
  }
  // a piece in words of words (list sizes - ew · 2^k, text - 2^k)
  uint32_t take(uint64_t words) {
    lane_guard g(lock);
    touch();
    if(auto it = free_runs.find(uint32_t(words)); it != free_runs.end() && !it->second.empty()) {
      uint32_t at = it->second.back();
      it->second.pop_back();
      return at;
    }
    // chunks - with an even word: elements with 8-byte fields (uint64 t, stored_entry, reuse_snap ...) require alignment of 8 (word - 4 bytes; on
    // AArch64 and under UBSan unaligned access - an error, not just slowly); the extra word disappears before compaction
    uint32_t at = (used + 1) & ~uint32_t{1};
    if(uint64_t(at) + words > cap) throw std::bad_alloc();
    if(uint64_t(at) + words > ready) {
      grow_to(uint64_t(at) + words);
      ready = std::min<uint64_t>(cap, std::max<uint64_t>(ready, uint64_t(at) + words));
    }
    used = at + uint32_t(words);
    return at;
  }
  void give(uint32_t at, uint64_t words) {
    lane_guard g(lock);
    touch();
    free_runs[uint32_t(words)].push_back(at);
    if(words >= 16384 && base) { // large abandoned piece (journal increase by doubling): pages to the system; contents are garbage, not zeros
      uintptr_t b = reinterpret_cast<uintptr_t>(base + at), from = (b + 4095) & ~uintptr_t(4095), to = (b + words * 4) & ~uintptr_t(4095);
      if(to > from) ::madvise(reinterpret_cast<void *>(from), to - from, MADV_DONTNEED);
    }
  }
  uint64_t free_words() const {
    uint64_t w = 0;
    for(auto const & [size, runs]: free_runs) w += uint64_t(size) * runs.size();
    return w;
  }
  // How many words are used (including free pieces)
  uint32_t words() const { return used; }
};

thread_local node_pool * current_pool = nullptr;

// The node array is one continuous address range (S5.1): the element i lies on base + i * sizeof(T), does not
// move, is read without a lock while the other band adds (like arena, but without a catalog of pieces: "vector
// in mmap" - the same array can be displayed on the file, the index remains the address).
template<typename T, typename tag>
struct node_array {
  using handle = id<tag>;
  static_assert(std::is_trivially_copyable_v<T>);
  T * base = nullptr;
  uint64_t cap = 0; // range
  uint64_t ready = 0; // Elements available without reference to the area
  std::atomic<uint32_t> count{0};
  std::recursive_mutex grow;
  bool external = false;
  bool dirty = true;
  std::function<void(uint64_t)> grow_to;
  std::function<void()> mark_dirty;

  explicit node_array(uint64_t elements) : cap(elements), ready(elements) {
    void * p = ::mmap(nullptr, cap * sizeof(T), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if(p == MAP_FAILED) throw std::bad_alloc();
    base = static_cast<T *>(p);
  }
  node_array(T * at, uint64_t elements, uint32_t have, uint64_t ready_elements, std::function<void(uint64_t)> grow_fn, std::function<void()> dirtied)
      : base(at), cap(elements), ready(ready_elements), external(true), dirty(false), grow_to(std::move(grow_fn)), mark_dirty(std::move(dirtied)) {
    count.store(have, std::memory_order_relaxed);
  }
  node_array(node_array const &) = delete;
  ~node_array() {
    if(base && !external) ::munmap(base, cap * sizeof(T));
  }

  handle add(T value) {
    lane_guard g(grow);
    uint32_t n = count.load(std::memory_order_relaxed);
    if(n >= cap) throw diagnostic(cat("база: больше ", cap, " узлов"), {});
    if(!dirty) {
      dirty = true;
      if(mark_dirty) mark_dirty();
    }
    if(n >= ready) {
      grow_to(uint64_t(n) + 1);
      ready = std::min<uint64_t>(cap, std::max<uint64_t>(ready, uint64_t(n) + 1));
    }
    new(base + n) T(std::move(value));
    count.store(n + 1, std::memory_order_release);
    return handle(n);
  }
  // Any reading of node exposes the base whose lists it holds: bypassing ast_db accessories, too.
  node_pool * owner = nullptr;
  T & operator[](handle h) pre(h.value < size()) {
    if(__builtin_expect(current_pool != owner, 0)) current_pool = owner;
    return base[h.value];
  }
  T const & operator[](handle h) const pre(h.value < size()) {
    if(__builtin_expect(current_pool != owner, 0)) current_pool = owner;
    return base[h.value];
  }
  uint32_t size() const { return count.load(std::memory_order_acquire); }
};

// Current Stream Base: The list reads and grows its memory here. Expose ast_db on each call to node
// (as, operator[], add...) and pool_scope where the base is known in advance.
struct pool_scope {
  node_pool * saved;
  explicit pool_scope(node_pool * p) : saved(current_pool) { current_pool = p; }
  ~pool_scope() { current_pool = saved; }
  pool_scope(pool_scope const &) = delete;
};

constexpr uint32_t pool_class_of(uint64_t words) { return words <= 1 ? 0 : uint32_t(std::bit_width(words - 1)); }

// List of 4-byte values in the pool: {смещение, длина | класс << 27}.
template<typename T>
struct list {
  static_assert(sizeof(T) % 4 == 0 && std::is_trivially_copyable_v<T>, "список — тривиальные значения, кратные слову");
  static constexpr uint32_t ew = sizeof(T) / 4; // element
  using value_type = T;
  using iterator = T *;
  using const_iterator = T const *;

  uint32_t off = 0;
  uint32_t meta = 0;

  list() = default;
  list(std::initializer_list<T> xs) { assign(xs.begin(), xs.end()); }
  list(std::vector<T> const & xs) { assign(xs.begin(), xs.end()); }
  list(std::span<T const> xs) { assign(xs.begin(), xs.end()); }
  template<typename It>
  list(It first, It last) { assign(first, last); }
  list & operator=(std::vector<T> const & xs) {
    assign(xs.begin(), xs.end());
    return *this;
  }
  list & operator=(std::initializer_list<T> xs) {
    assign(xs.begin(), xs.end());
    return *this;
  }

  uint32_t size() const { return meta & ((uint32_t(1) << node_pool::len_bits) - 1); }
  bool empty() const { return size() == 0; }
  uint32_t cap_class() const { return meta >> node_pool::len_bits; }
  uint64_t capacity() const { return off ? uint64_t(1) << cap_class() : 0; } // component
  uint64_t capacity_words() const { return capacity() * ew; }
  T * data() const { return off ? reinterpret_cast<T *>(current_pool->base + off) : nullptr; }
  T * begin() const { return data(); }
  T * end() const { return data() + size(); }
  T & operator[](size_t i) const pre(i < size()) { return data()[i]; }
  T & front() const pre(!empty()) { return data()[0]; }
  T & back() const pre(!empty()) { return data()[size() - 1]; }
  operator std::span<T const>() const { return {data(), size()}; }
  std::vector<T> vec() const { return std::vector<T>(begin(), end()); }
  operator std::vector<T>() const { return vec(); } // Copy: for temporary lists (parsing, rounds)
  std::reverse_iterator<T *> rbegin() const { return std::reverse_iterator<T *>(end()); }
  std::reverse_iterator<T *> rend() const { return std::reverse_iterator<T *>(begin()); }
  // Deep copy is your list; the usual assignment of a descriptor divides the list
  list clone() const { return list(begin(), end()); }
  // return a piece of pool (descriptor no longer used); without it, the piece lives to the end of the base
  void release() {
    if(off) current_pool->give(off, capacity_words());
    off = 0;
    meta = 0;
  }

  void set_size(uint32_t n) { meta = (meta & ~((uint32_t(1) << node_pool::len_bits) - 1)) | n; }
  void reserve(uint64_t n) {
    if(n <= capacity()) return;
    if(n >= (uint64_t(1) << node_pool::len_bits)) throw std::bad_alloc();
    uint32_t cls = pool_class_of(n);
    uint32_t at = current_pool->take(uint64_t(ew) << cls);
    uint32_t old_off = off, old_cls = cap_class(), len = size();
    if(len) std::memcpy(current_pool->base + at, current_pool->base + old_off, size_t(len) * sizeof(T));
    if(old_off) current_pool->give(old_off, uint64_t(ew) << old_cls);
    off = at;
    meta = len | (cls << node_pool::len_bits);
  }
  void push_back(T v) {
    uint32_t n = size();
    if(n == capacity()) reserve(uint64_t(n) + 1);
    data()[n] = v;
    set_size(n + 1);
  }
  template<typename... A>
  T & emplace_back(A &&... a) {
    push_back(T(std::forward<A>(a)...));
    return back();
  }
  void pop_back() { set_size(size() - 1); }
  void clear() { set_size(0); }
  void resize(size_t n, T v = T{}) {
    size_t old = size();
    reserve(n);
    for(size_t i = old; i < n; ++i) data()[i] = v;
    set_size(uint32_t(n));
  }
  // Rewriting, pasting, and erasing are always in a new piece: a copy of the descriptor (node) divides the piece, and
  // editing the copy should not change the original; the old piece remains (it can be held by another copy), the pool grows.
  // The addition to the end (push_back) goes in place behind the length - other people's copies see the former elements.
  template<typename It>
  void assign(It first, It last) {
    std::vector<T> tmp(first, last); // The source may be on the same list.
    if(off) current_pool->garbage += capacity_words(); // abandoned piece: it can be held by a copy of the descriptor - the compaction will release
    off = 0;
    meta = 0;
    reserve(tmp.size());
    if(!tmp.empty()) std::memcpy(data(), tmp.data(), tmp.size() * sizeof(T));
    set_size(uint32_t(tmp.size()));
  }
  iterator insert(const_iterator pos, T v) { return insert(pos, &v, &v + 1); }
  iterator insert(const_iterator pos, size_t count, T v) {
    std::vector<T> tmp(count, v);
    return insert(pos, tmp.begin(), tmp.end());
  }
  template<typename It>
  iterator insert(const_iterator pos, It first, It last) {
    size_t at = size_t(pos - begin());
    std::vector<T> tmp(first, last);
    std::vector<T> all = vec();
    all.insert(all.begin() + std::ptrdiff_t(at), tmp.begin(), tmp.end());
    assign(all.begin(), all.end());
    return data() + at;
  }
  iterator erase(const_iterator pos) { return erase(pos, pos + 1); }
  iterator erase(const_iterator first, const_iterator last) {
    size_t at = size_t(first - begin());
    std::vector<T> all = vec();
    all.erase(all.begin() + std::ptrdiff_t(at), all.begin() + std::ptrdiff_t(at + size_t(last - first)));
    assign(all.begin(), all.end());
    return data() + at;
  }
  // Remove all equals v (as std::erase); a new piece
  size_t erase_value(T v) {
    std::vector<T> all = vec();
    size_t n = std::erase(all, v);
    assign(all.begin(), all.end());
    return n;
  }
  friend bool operator==(list const & a, list const & b) { return a.size() == b.size() && (a.size() == 0 || std::memcmp(a.data(), b.data(), size_t(a.size()) * sizeof(T)) == 0); }
};

template<typename T>
struct is_pool_list : std::false_type {};
template<typename T>
struct is_pool_list<list<T>> : std::true_type {};

// Text in the pool: bytes, length - in bytes; piece - words of 4 bytes.
struct node_text {
  uint32_t off = 0;
  uint32_t meta = 0; // length in bytes | class (in words) << 27

  node_text() = default;
  node_text(std::string_view s) { assign(s); }
  node_text(std::string const & s) { assign(s); }
  node_text(char const * s) { assign(std::string_view(s)); }
  node_text & operator=(std::string_view s) {
    assign(s);
    return *this;
  }
  node_text & operator=(std::string const & s) {
    assign(s);
    return *this;
  }
  node_text & operator=(char const * s) {
    assign(std::string_view(s));
    return *this;
  }

  uint32_t size() const { return meta & ((uint32_t(1) << node_pool::len_bits) - 1); }
  bool empty() const { return size() == 0; }
  uint32_t cap_class() const { return meta >> node_pool::len_bits; }
  uint64_t capacity() const { return off ? (uint64_t(4) << cap_class()) : 0; } // byte
  char * data() const { return off ? reinterpret_cast<char *>(current_pool->base + off) : nullptr; }
  std::string_view view() const { return {data(), size()}; }
  operator std::string_view() const { return view(); }
  std::string str() const { return std::string(view()); }
  char operator[](size_t i) const pre(i < size()) { return data()[i]; }
  char const * begin() const { return data(); }
  char const * end() const { return data() + size(); }

  void assign(std::string_view s) {
    std::string tmp(s); // The source may be in the same text.
    if(tmp.size() >= (size_t(1) << node_pool::len_bits)) throw std::bad_alloc();
    if(tmp.size() > capacity()) {
      uint32_t cls = pool_class_of((tmp.size() + 3) / 4);
      uint32_t at = current_pool->take(uint64_t(1) << cls);
      if(off) current_pool->give(off, uint64_t(1) << cap_class());
      off = at;
      meta = cls << node_pool::len_bits;
    }
    if(!tmp.empty()) std::memcpy(data(), tmp.data(), tmp.size());
    meta = (meta & ~((uint32_t(1) << node_pool::len_bits) - 1)) | uint32_t(tmp.size());
  }
  node_text clone() const { return node_text(view()); }
  void release() {
    if(off) current_pool->give(off, uint64_t(1) << cap_class());
    off = 0;
    meta = 0;
  }
  friend bool operator==(node_text const & a, node_text const & b) { return a.view() == b.view(); }
  friend bool operator==(node_text const & a, std::string_view b) { return a.view() == b; }
  friend bool operator==(node_text const & a, char const * b) { return a.view() == b; }
  friend bool operator==(node_text const & a, std::string const & b) { return a.view() == b; }
  friend std::string operator+(std::string a, node_text const & b) { return a.append(b.view()), a; }
  friend std::string operator+(node_text const & a, std::string_view b) { return std::string(a.view()) + std::string(b); }
};

// Interner: character (number) я text, text in a row pool (spool: grows, does not shrink and does not move - string_view from
// text() does not hang, as in the former std::string arena); tables - in the main pool (list), roots - in db_roots. Hash table
// with open addressing by displacement: search by string_view does not allocate memory (hot path sym()). Readers (text, find)
// do not take the lock: growth is a new piece and publication of displacement, the old piece is not released.
struct interner_roots {
  list<node_text> strs; // symbol → text (bytes - in spool)
  list<uint32_t> table; // symbol + 1; 0 - empty; length - degree of two (0 - no table)
};

struct interner {
  node_pool * spool;
  node_pool * pool;
  interner_roots * r;
  std::recursive_mutex lock;

  interner(node_pool * strings_pool, node_pool * main_pool, interner_roots * roots) : spool(strings_pool), pool(main_pool), r(roots) {}
  interner(interner const &) = delete;

  static uint32_t hash(std::string_view s) {
    uint32_t h = 2166136261u;
    for(char c: s) h = (h ^ uint8_t(c)) * 16777619u;
    return h;
  }
  uint32_t size() const { return r->strs.size(); }
  node_text const & entry(uint32_t i) const { return reinterpret_cast<node_text const *>(pool->base + r->strs.off)[i]; }
  std::string_view text(symbol s) const {
    if(!s.valid()) return "<none>";
    node_text const & t = entry(s.value);
    return {reinterpret_cast<char const *>(spool->base + t.off), t.size()};
  }
  // symbol-number
  symbol lookup(std::string_view s) const {
    uint32_t slots = r->table.size();
    if(!slots) return {};
    uint32_t const * tab = reinterpret_cast<uint32_t const *>(pool->base + r->table.off);
    for(uint32_t i = hash(s) & (slots - 1);; i = (i + 1) & (slots - 1)) {
      if(!tab[i]) return {};
      if(text(symbol(tab[i] - 1)) == s) return symbol(tab[i] - 1);
    }
  }
  // Compatible with previous unordered_map: strings.index.find(ключ) / end() / contains
  struct hit {
    symbol second;
    bool ok = false;
    hit const * operator->() const { return this; }
    friend bool operator==(hit a, hit b) { return a.ok == b.ok; }
  };
  struct index_view {
    interner const * in;
    hit find(std::string_view s) const {
      symbol x = in->lookup(s);
      return {x, x.valid()};
    }
    hit end() const { return {}; }
    bool contains(std::string_view s) const { return in->lookup(s).valid(); }
  };
  index_view index{this};

  symbol intern(std::string_view s) {
    if(symbol x = lookup(s); x.valid()) return x;
    lane_guard g(lock); // insert - under the lock of stripes (reading text - without it)
    if(symbol x = lookup(s); x.valid()) return x;
    pool_scope in_pool(pool);
    uint32_t n = r->strs.size();
    node_text t;
    {
      pool_scope in_strings(spool);
      t = node_text(s);
    }
    if(n == r->strs.capacity()) { // a new piece, the old one is not released: the reader could take the displacement
      uint32_t cls = pool_class_of(uint64_t(n) + 1);
      uint32_t at = pool->take(uint64_t(list<node_text>::ew) << cls);
      if(n) std::memcpy(pool->base + at, pool->base + r->strs.off, size_t(n) * sizeof(node_text));
      r->strs.meta = n | (cls << node_pool::len_bits);
      r->strs.off = at;
    }
    r->strs.data()[n] = t;
    r->strs.set_size(n + 1);
    if(uint64_t(n + 1) * 2 > r->table.size()) { // Rebuild: twice as many slots
      uint32_t slots = std::max<uint32_t>(64, uint32_t(std::bit_ceil(uint64_t(n + 1) * 4)));
      list<uint32_t> fresh;
      fresh.resize(slots, 0u);
      uint32_t * tab = fresh.data();
      for(uint32_t k = 0; k <= n; ++k) {
        uint32_t i = hash(text(symbol(k))) & (slots - 1);
        while(tab[i]) i = (i + 1) & (slots - 1);
        tab[i] = k + 1;
      }
      r->table = fresh;
    } else {
      uint32_t slots = r->table.size();
      uint32_t * tab = r->table.data();
      uint32_t i = hash(s) & (slots - 1);
      while(tab[i]) i = (i + 1) & (slots - 1);
      tab[i] = n + 1;
    }
    return symbol(n);
  }
};

} // namespace vish
