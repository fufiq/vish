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

#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <new>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace vish {

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
  std::recursive_mutex lock;

  // Non-volatile layer (program state, §12.2): The pages of the file that the VM holds are at the
  // same addresses each time you run.
// 
// Metadata in its pages at the beginning of the file.
  bool persistent = false;
  bool banded = false; // one lo range (program state, machine process level)
  int fd = -1;
  std::byte * lo = nullptr; // beginning of the level range (header)
  std::byte * big_next = nullptr; // next-range
  size_t file_size = 0;
  // connected parts of addresses → places in the file: the file is dense (grows as you
  // connect), addresses are sparse
  struct piece {
    uint64_t addr, file, len;
  };
  std::vector<piece> pieces;
  static constexpr size_t state_range = size_t(1) << 40; // 1 TB addresses per level
  static constexpr size_t state_meta = size_t(1) << 22; // 4 MiB: title and metadata
  static constexpr size_t state_small = size_t(1) << 37; // 128 GB of small
  static constexpr size_t state_big = size_t(1) << 32; // 4 GB per growing vector

  static std::byte * reserve(size_t n) {
    void * p = mmap(nullptr, n, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if(p == MAP_FAILED) throw std::bad_alloc();
    return static_cast<std::byte *>(p);
  }
  void commit(void * at, size_t n) {
    lane_guard g(lock); // Stripes (step 12.3b): the piece is given and taken from different streams
    if(!persistent) {
      if(mprotect(at, n, PROT_READ | PROT_WRITE) != 0) throw std::bad_alloc();
      if(banded) { // for the dump; adjacent parts - one
        uint64_t off = uint64_t(static_cast<std::byte *>(at) - lo);
        if(!pieces.empty() && pieces.back().addr + pieces.back().len == off) pieces.back().len += n;
        else pieces.push_back({off, 0, n});
      }
      return;
    }
    size_t off = size_t(static_cast<std::byte *>(at) - lo);
    if(ftruncate(fd, off_t(file_size + n)) != 0) throw std::bad_alloc();
    if(mmap(at, n, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, off_t(file_size)) == MAP_FAILED) throw std::bad_alloc();
    pieces.push_back({off, file_size, n});
    file_size += n;
  }

  static uint32_t class_of(uint32_t words) {
    uint32_t k = 1;
    while((uint32_t(1) << k) < words) ++k;
    return k;
  }
  // Live level allocation: a region is liberated only when nothing lives in it.
  //  
  uint64_t live = 0;
  uint32_t holders = 0; // region() values that keep the level
  bool dead = false;
  std::vector<uint64_t *> big_reserved; // Ranges of growing vectors (level release)
  uint64_t * take_small(uint32_t k) {
    lane_guard g(lock); // Stripes (step 12.3b): the piece is given and taken from different streams
    if(dead) throw std::bad_alloc();
    ++live;
    if(!small_free[k].empty()) {
      uint64_t * p = small_free[k].back();
      small_free[k].pop_back();
      return p;
    }
    if(!small_base) small_base = reserve(small_range); // non-volatile - set at opening
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
  void give_small(uint64_t * p, uint32_t k) {
    lane_guard g(lock); // Stripes (step 12.3b): the piece is given and taken from different streams
    --live;
    small_free[k].push_back(p);
  }

  uint64_t * take_big() {
    lane_guard g(lock); // Stripes (step 12.3b): the piece is given and taken from different streams
    if(dead) throw std::bad_alloc();
    ++live;
    if(!big_free.empty()) {
      uint64_t * p = big_free.back();
      big_free.pop_back();
      return p;
    }
    if(banded) {
      if(big_next + state_big > lo + state_range) throw std::bad_alloc();
      auto * p = reinterpret_cast<uint64_t *>(big_next);
      big_next += state_big;
      return p;
    }
    auto * p = reinterpret_cast<uint64_t *>(reserve(big_range));
    big_reserved.push_back(p);
    return p;
  }
  // level as a whole - system (no live allocations): addresses and pages
  void destroy() {
    if(small_base) munmap(small_base, small_range);
    for(uint64_t * p: big_reserved) munmap(p, big_range);
    small_base = nullptr;
    big_reserved.clear();
    big_free.clear();
    for(auto & f: small_free) f.clear();
    dead = true;
  }
  size_t big_limit() const { return banded ? state_big : big_range; }
  // pages to the system, addresses remain behind VM: the range will go to a new vector
  void give_big(uint64_t * p, size_t committed) {
    lane_guard g(lock); // Stripes (step 12.3b): the piece is given and taken from different streams
    --live;
    if(!persistent && !banded) { // Pages remain connected: the range goes as follows
      madvise(p, committed, MADV_DONTNEED);
      mprotect(p, committed, PROT_NONE);
    }
    big_free.push_back(p);
  }
  // lane: how much is connected to each growing range (repeated - only the tail)
  std::vector<std::pair<std::byte *, size_t>> big_have;
  void commit_big(std::byte * p, size_t need) {
    lane_guard g(lock); // Stripes (step 12.3b): the piece is given and taken from different streams
    for(auto & [at, have]: big_have)
      if(at == p) {
        if(need > have) {
          commit(p + have, need - have);
          have = need;
        }
        return;
      }
    commit(p, need);
    big_have.push_back({p, need});
  }
  bool holds(void const * p) const { return persistent && p >= lo && p < lo + state_range; }
  bool in_band(void const * p) const { return banded && p >= lo && p < lo + state_range; }
};

// not destroyed: values in global tables give away memory and after leaving the main
address_space & default_space() {
  alignas(address_space) static std::byte place[sizeof(address_space)];
  static address_space * s = new(place) address_space;
  return *s;
}
// The memory level in which values are now created: the context with_alloc(r) { … } (§12.2),
// without it - the level of the caller; fiber - its own (VM changes when switching).
thread_local address_space * current_space = nullptr; // on the lane - its own (step 12.3b)
address_space & space() { return current_space ? *current_space : default_space(); }

// The title of the program status file: allocator metadata and roots - in their pages at the
// beginning of the level, not in the data Type numbers and field names of the VM change from start
// to run - tables for repair when opened.
//
struct state_header {
  struct named {
    uint64_t id;
    char name[64];
  };
  char magic[8];
  uint64_t base, small_used, small_committed, big_next, file_size;
  uint64_t nfree[address_space::classes];
  uint64_t free_off[address_space::classes][4096];
  uint64_t nbig;
  uint64_t big_off[4096];
  uint64_t nroots;
  named roots[256]; // id - the displacement of the root cell from the beginning of the level
  uint64_t ntypes;
  named types[1024];
  uint64_t nsyms;
  named syms[4096];
  uint64_t npieces;
  address_space::piece pieces[65536];
};
static_assert(sizeof(state_header) <= address_space::state_meta);

// Open the status level: the path file; new - addresses in its area (slot on the hash path), existing
// - the same as those recorded in the file. Empty and text - it is impossible (occupied with another
// execution, the address is occupied).
std::vector<std::byte *> & free_bands();

// Level addresses - in their areas, away from what the kernel lays out (libraries, heaps, stacks): the
// address chosen by the kernel at the first run could be occupied in the next one (state and dump open
// at the same addresses). States are slots [16, 48] TiB, process bands are [48, 80] TiB; the first is
// the slot from which to look (*mine*).
void * reserve_level(uint64_t area_tib, uint64_t first) {
  for(uint64_t k = 0; k < 32; ++k) {
    auto * hint = reinterpret_cast<void *>((area_tib + (first + k) % 32) << 40);
    void * r = mmap(hint, address_space::state_range, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if(r == hint) return r;
    if(r != MAP_FAILED) munmap(r, address_space::state_range);
  }
  return mmap(nullptr, address_space::state_range, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
}

std::string open_state(address_space & a, std::string const & path) {
  uint64_t base = 0;
  int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if(fd < 0) return "не открыть файл состояния";
  if(flock(fd, LOCK_EX | LOCK_NB) != 0) {
    ::close(fd);
    return "состояние программы занято другим исполнением";
  }
  struct stat st {};
  fstat(fd, &st);
  bool fresh = size_t(st.st_size) < sizeof(state_header);
  if(!fresh) {
    state_header h;
    if(pread(fd, &h, sizeof h, 0) != ssize_t(sizeof h) || (std::memcmp(h.magic, "VISHST01", 8) != 0 && std::memcmp(h.magic, "TZSTATE1", 8) != 0)) {
      ::close(fd);
      return "файл состояния повреждён";
    }
    base = h.base;
  }
  void * r = MAP_FAILED;
  auto & pool = free_bands(); // the same addresses that were at the closed level - for VM (pool)
  if(auto it = std::ranges::find(pool, reinterpret_cast<std::byte *>(base)); base && it != pool.end()) {
    r = *it;
    pool.erase(it);
  } else {
    r = base ? mmap(reinterpret_cast<void *>(base), address_space::state_range, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0)
             : reserve_level(16, std::hash<std::string>{}(path));
    if(r == MAP_FAILED || (base && r != reinterpret_cast<void *>(base))) {
      if(r != MAP_FAILED) munmap(r, address_space::state_range);
      ::close(fd);
      return "адреса состояния программы заняты";
    }
  }
  base = uint64_t(r);
  a.persistent = true;
  a.banded = true;
  a.fd = fd;
  a.lo = static_cast<std::byte *>(r);
  a.small_base = a.lo + address_space::state_meta;
  a.big_next = a.small_base + address_space::state_small;
  if(fresh) {
    a.file_size = 0;
    a.commit(a.lo, address_space::state_meta);
    auto * h = reinterpret_cast<state_header *>(a.lo);
    std::memcpy(h->magic, "VISHST01", 8);
    h->base = base;
    return "";
  }
  a.file_size = size_t(st.st_size);
  if(mmap(a.lo, address_space::state_meta, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED) return "не отобразить состояние";
  auto * h = reinterpret_cast<state_header *>(a.lo);
  for(uint64_t i = 0; i < h->npieces; ++i) {
    auto const & p = h->pieces[i];
    a.pieces.push_back(p);
    if(p.addr == 0) continue; // headline - already
    if(mmap(a.lo + p.addr, p.len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, off_t(p.file)) == MAP_FAILED) return "не отобразить состояние";
  }
  a.small_used = h->small_used;
  a.small_committed = h->small_committed;
  a.big_next = a.lo + h->big_next;
  for(uint32_t k = 0; k < address_space::classes; ++k)
    for(uint64_t i = 0; i < h->nfree[k]; ++i) a.small_free[k].push_back(reinterpret_cast<uint64_t *>(a.lo + h->free_off[k][i]));
  for(uint64_t i = 0; i < h->nbig; ++i) a.big_free.push_back(reinterpret_cast<uint64_t *>(a.lo + h->big_off[i]));
  return "";
}

// Write allocator metadata into a header and reset pages to disk.
void save_state(address_space & a) {
  auto * h = reinterpret_cast<state_header *>(a.lo);
  h->small_used = a.small_used;
  h->small_committed = a.small_committed;
  h->big_next = uint64_t(a.big_next - a.lo);
  h->file_size = a.file_size;
  for(uint32_t k = 0; k < address_space::classes; ++k) {
    h->nfree[k] = std::min<uint64_t>(a.small_free[k].size(), 4096); // over - not overused
    for(uint64_t i = 0; i < h->nfree[k]; ++i) h->free_off[k][i] = uint64_t(reinterpret_cast<std::byte *>(a.small_free[k][i]) - a.lo);
  }
  if(a.pieces.size() > 65536) throw std::bad_alloc();
  h->npieces = a.pieces.size();
  for(uint64_t i = 0; i < h->npieces; ++i) h->pieces[i] = a.pieces[i];
  h->nbig = std::min<uint64_t>(a.big_free.size(), 4096);
  for(uint64_t i = 0; i < h->nbig; ++i) h->big_off[i] = uint64_t(reinterpret_cast<std::byte *>(a.big_free[i]) - a.lo);
  msync(a.lo, a.file_size, MS_SYNC);
}

// Close the level: pages to the system, addresses remain behind the VM (pool, like process lanes:
// reopening in the same process - at the same addresses, they will not be occupied by anyone else),
// the file - release (saves save_state).
void close_state(address_space & a) {
  mmap(a.lo, address_space::state_range, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
  free_bands().push_back(a.lo);
  ::close(a.fd);
  a.fd = -1;
  a.persistent = false;
  a.banded = false;
}

// Machine process address band (§12.2: dump - pages as is): level on base (0 - new: addresses
// are selected by the kernel). Lies - addresses are taken.
std::vector<address_space *> & bands() {
  alignas(std::vector<address_space *>) static std::byte place[sizeof(std::vector<address_space *>)];
  static auto * v = new(place) std::vector<address_space *>;
  return *v;
}
// closed bands remain behind VM (reserve without pages): their addresses will not be taken by
// anyone else - the recovery of the dump in the same process will receive the same
std::vector<std::byte *> & free_bands() {
  alignas(std::vector<std::byte *>) static std::byte place[sizeof(std::vector<std::byte *>)];
  static auto * v = new(place) std::vector<std::byte *>;
  return *v;
}
bool open_band(address_space & a, uint64_t base) {
  void * r = MAP_FAILED;
  auto & pool = free_bands();
  auto it = base ? std::ranges::find(pool, reinterpret_cast<std::byte *>(base)) : pool.begin();
  if(it != pool.end()) { // backstreet
    r = *it;
    pool.erase(it);
  } else {
    r = base ? mmap(reinterpret_cast<void *>(base), address_space::state_range, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0)
             : reserve_level(48, 0);
    if(r == MAP_FAILED || (base && r != reinterpret_cast<void *>(base))) {
      if(r != MAP_FAILED) munmap(r, address_space::state_range);
      return false;
    }
  }
  a.banded = true;
  a.lo = static_cast<std::byte *>(r);
  a.small_base = a.lo + address_space::state_meta;
  a.big_next = a.small_base + address_space::state_small;
  bands().push_back(&a);
  return true;
}
void close_band(address_space & a) {
  // pages to the system, addresses to the VM pool
  mmap(a.lo, address_space::state_range, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
  free_bands().push_back(a.lo);
  std::erase(bands(), &a);
  a.banded = false;
}
address_space * band_of(void const * p) {
  for(address_space * a: bands())
    if(a->in_band(p)) return a;
  return nullptr;
}

// Memory of containers by class of pieces: the size knows the caller - no metadata in the data
void * space_alloc(address_space & a, size_t bytes) {
  uint64_t w = std::max<uint64_t>((bytes + 7) / 8, 2);
  if(w <= address_space::small_max) return a.take_small(address_space::class_of(uint32_t(w)));
  uint64_t * p = a.take_big();
  a.commit_big(reinterpret_cast<std::byte *>(p), (bytes + address_space::page - 1) / address_space::page * address_space::page);
  return p;
}
void space_free(void * p, size_t bytes) {
  address_space * a = band_of(p);
  if(!a) return std::free(p);
  uint64_t w = std::max<uint64_t>((bytes + 7) / 8, 2);
  if(w <= address_space::small_max) a->give_small(static_cast<uint64_t *>(p), address_space::class_of(uint32_t(w)));
  else a->give_big(static_cast<uint64_t *>(p), (bytes + address_space::page - 1) / address_space::page * address_space::page);
}

// The level of the process the machine is currently building (the containers of the machine are its pages)
thread_local address_space * machine_space = nullptr;
// Distributor of process machine containers: pages of its level, dump - they are also. Template - because
// that's how std containers are arranged (*mine*: an exception to "auto instead of template<>").
template<typename T>
struct pages_alloc {
  using value_type = T;
  pages_alloc() = default;
  pages_alloc(auto const &) {}
  T * allocate(size_t n) {
    if(!machine_space) return static_cast<T *>(std::malloc(n * sizeof(T)));
    return static_cast<T *>(space_alloc(*machine_space, n * sizeof(T)));
  }
  void deallocate(T * p, size_t n) { space_free(p, n * sizeof(T)); }
  bool operator==(pages_alloc const &) const { return true; }
};

// 8-byte word vector (i64, f64 - bits): size and capacity - out of data.
struct words {
  uint64_t * data = nullptr;
  address_space * home = nullptr; // Data Memory Level: Growth and Return
  uint64_t n = 0; // word: machine code reads size from memory
  uint64_t cap = 0; // words: small - 2^k, growing - on connected pages
  bool big = false;

  words() = default;
  words(words const & o) { *this = o; }
  words(words && o) noexcept { steal(o); }
  words & operator=(words const & o) { // copy - at the level where it is made (and empty: growth - there)
    if(this == &o) return *this;
    clear();
    if(!data) home = &space();
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

  uint32_t size() const { return uint32_t(n); }
  bool empty() const { return n == 0; }
  uint64_t & operator[](uint32_t i) { return data[i]; }
  uint64_t operator[](uint32_t i) const { return data[i]; }
  void clear() { n = 0; }
  void push_back(uint64_t w) {
    if(n == cap) reserve(n + 1);
    data[n++] = w;
  }

  // your range of addresses at once: further growth - pages in place, data does not move
  // 
  void pin() {
    if(!big) reserve(std::max<uint64_t>(cap, address_space::small_max) + 1);
  }
  // place under want words: small grows in classes, then - his range (no longer moves)
  void reserve(uint64_t want) {
    if(want <= cap) return;
    if(big) {
      size_t have = size_t(cap) * 8, need = (size_t(want) * 8 + address_space::page - 1) / address_space::page * address_space::page;
      need = std::max(need, have * 2); // pages - with a margin of twice (without moving)
      need = std::min(need, home->big_limit());
      home->commit(reinterpret_cast<std::byte *>(data) + have, need - have);
      cap = need / 8;
      return;
    }
    uint64_t * old = data;
    uint64_t old_cap = cap;
    uint64_t grow = std::max(want, cap * 2); // petty
    if(!home) home = &space();
    if(grow <= address_space::small_max) {
      uint32_t k = address_space::class_of(uint32_t(grow));
      data = home->take_small(k);
      cap = uint64_t(1) << k;
    } else {
      data = home->take_big();
      big = true;
      size_t need = (size_t(want) * 8 + address_space::page - 1) / address_space::page * address_space::page;
      home->commit(data, need);
      cap = need / 8;
    }
    if(old) {
      if(n) std::memcpy(data, old, size_t(n) * 8);
      home->give_small(old, address_space::class_of(uint32_t(old_cap)));
    }
  }

  void steal(words & o) {
    data = o.data;
    home = o.home;
    n = o.n;
    cap = o.cap;
    big = o.big;
    o.data = nullptr;
    o.n = o.cap = 0;
    o.big = false;
  }
  void release() {
    if(!data) return;
    if(big) home->give_big(data, size_t(cap) * 8);
    else home->give_small(data, address_space::class_of(uint32_t(cap)));
    data = nullptr;
    home = nullptr;
    n = cap = 0;
    big = false;
  }
};

// VM string - bytes in the same pages (line - bytes, §4.0): length - out of data.
struct str_v {
  words w;
  uint32_t len = 0;

  str_v() = default;
  str_v(std::string_view s) { append(s); }
  str_v(std::string const & s) : str_v(std::string_view(s)) {}
  str_v(char const * s) : str_v(std::string_view(s)) {}

  std::string_view view() const { return {reinterpret_cast<char const *>(w.data), len}; }
  operator std::string_view() const { return view(); }
  std::string str() const { return std::string(view()); }
  uint32_t size() const { return len; }
  bool empty() const { return len == 0; }
  char operator[](uint32_t i) const { return reinterpret_cast<char const *>(w.data)[i]; }
  void append(std::string_view s) {
    if(s.empty()) return;
    uint64_t need = (len + s.size() + 7) / 8;
    w.reserve(need);
    std::memcpy(reinterpret_cast<char *>(w.data) + len, s.data(), s.size());
    len += uint32_t(s.size());
    w.n = need;
  }
  bool operator==(str_v const & o) const { return view() == o.view(); }
  str_v operator+(str_v const & o) const {
    str_v r = *this;
    r.append(o.view());
    return r;
  }
};

} // namespace vish
