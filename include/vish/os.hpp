// host VM Layer: All accesses to the OS are here (files, stdin, stdout).
//
// Vish programs do not go directly to the OS: their I/O are VM services on top of
// this layer (§9.5, M3): Files are not “read” but are displayed: No iostream.
//
//
//
#pragma once

#include "base.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sched.h>
#include <sys/eventfd.h>
#include <pthread.h>
#include <liburing.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <time.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <atomic>
#include <sys/wait.h>
#include <unistd.h>

namespace vish::os {

bool file_exists(std::string const & path) {
  struct stat info;
  if(::stat(path.c_str(), &info) == 0) return true;
  if(errno == ENOENT || errno == ENOTDIR) return false;
  throw diagnostic(cat("не могу проверить файл ", path), {});
}

// A separate inode lock survives an atomic rename data file. Verification of the
// expected version and recording are performed under one lock.
struct file_lock {
  int fd = -1;
  explicit file_lock(std::string const & path) {
    fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if(fd < 0) throw diagnostic(cat("не могу открыть блокировку ", path), {});
    int rc;
    do rc = ::flock(fd, LOCK_EX); while(rc < 0 && errno == EINTR);
    if(rc < 0) { ::close(fd); fd = -1; throw diagnostic(cat("не могу заблокировать ", path), {}); }
  }
  file_lock(file_lock const &) = delete;
  file_lock & operator=(file_lock const &) = delete;
  ~file_lock() { if(fd >= 0) ::close(fd); }
};

// Display a read-only file; text is string_view on file pages.
struct mapped_file {
  char const * data = nullptr;
  std::size_t size = 0;

  explicit mapped_file(std::string const & path) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if(fd < 0) throw diagnostic(cat("не могу открыть ", path), {});
    struct stat st{};
    if(::fstat(fd, &st) != 0) {
      ::close(fd);
      throw diagnostic(cat("не могу прочитать ", path), {});
    }
    size = std::size_t(st.st_size);
    if(size) {
      void * m = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
      if(m == MAP_FAILED) {
        ::close(fd);
        throw diagnostic(cat("не могу отобразить ", path), {});
      }
      data = static_cast<char const *>(m);
    }
    ::close(fd); // mapping lives without a descriptor
  }
  mapped_file(mapped_file && o) noexcept : data(std::exchange(o.data, nullptr)), size(std::exchange(o.size, 0)) {}
  mapped_file & operator=(mapped_file &&) = delete;
  ~mapped_file() {
    if(data) ::munmap(const_cast<char *>(data), size);
  }

  std::string_view text() const { return {data ? data : "", size}; }
};

// Write bytes into the entire file (create/rewrite).
void write_file(std::string const & path, std::string_view bytes) {
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if(fd < 0) throw diagnostic(cat("не могу записать ", path), {});
  while(!bytes.empty()) {
    auto n = ::write(fd, bytes.data(), bytes.size());
    if(n <= 0) {
      ::close(fd);
      throw diagnostic(cat("не могу записать ", path), {});
    }
    bytes.remove_prefix(std::size_t(n));
  }
  ::close(fd);
}

// Atomic Replacement: A temporary file is created in the same directory.
void write_file_atomic(std::string const & path, std::string_view bytes) {
  auto slash = path.rfind('/');
  std::string dir = slash == std::string::npos ? "." : slash == 0 ? "/" : path.substr(0, slash);
  std::string temp = dir + "/.vish-tmp-XXXXXX";
  int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if(dfd < 0) throw diagnostic(cat("не могу открыть каталог ", dir), {});
  int fd = ::mkstemp(temp.data());
  if(fd < 0) {
    ::close(dfd);
    throw diagnostic(cat("не могу записать ", path), {});
  }
  bool renamed = false;
  auto fail = [&] {
    if(fd >= 0) ::close(fd);
    if(!renamed) ::unlink(temp.c_str());
    ::close(dfd);
    throw diagnostic(cat("не могу записать ", path), {});
  };
  while(!bytes.empty()) {
    auto n = ::write(fd, bytes.data(), bytes.size());
    if(n < 0 && errno == EINTR) continue;
    if(n <= 0) fail();
    bytes.remove_prefix(std::size_t(n));
  }
  int rc;
  do rc = ::fsync(fd); while(rc < 0 && errno == EINTR);
  if(rc < 0) fail();
  rc = ::close(fd);
  fd = -1;
  if(rc < 0) fail();
  if(::rename(temp.c_str(), path.c_str()) < 0) fail();
  renamed = true;
  do rc = ::fsync(dfd); while(rc < 0 && errno == EINTR);
  if(rc < 0) fail();
  if(::close(dfd) < 0) throw diagnostic(cat("не могу закрыть каталог ", dir), {});
}

// --- state storage (VM service, plan C1) ---
//
// 
// 
// 
// 
// 
// ; The program sees the name (key in the VM environment) and the offset, paths, and descriptors of host are not.
// *My:* Linux implementation is a file in the VM repository directory, displayed entirely in its address range (64
// GB): pages after the end of the file become available after ftruncate, the data does not move; fixation is
// msync+fsync; the file holds one execution (flock). General display storage (MAP_SHARED) opened in this process.
// After fork, the descendant sees the same pages of the file, and his record would spoil the parent (clone sample,
// run the test): the handler of the descendant replays each on top of itself as MAP_PRIVATE - the record of the
// descendant remains with him, the file does not change. The descendant reads the pages of the file until he wrote
// down his own: the parent’s edits after fork are visible to him before his first entry into the page (parent
// fork-host is waiting for a descendant). VISH_NO_ATFORK = 1 without it (for verification).
//
struct storage_area;
std::vector<storage_area *> & shared_areas() {
  static std::vector<storage_area *> v;
  return v;
}
void remap_private_after_fork();
void register_shared_area(storage_area * a) {
  static bool installed = false;
  if(!installed) {
    installed = true;
    if(!std::getenv("VISH_NO_ATFORK")) ::pthread_atfork(nullptr, nullptr, remap_private_after_fork);
  }
  shared_areas().push_back(a);
}

struct storage_area {
  static constexpr uint64_t range = uint64_t(1) << 36; // Storage Addresses: The Growth Limit
  int fd = -1;
  std::byte * at = nullptr;
  uint64_t size = 0;
  bool shared = true; // display general (MAP_SHARED); after fork in the descendant - private

  storage_area() = default;
  storage_area(storage_area const &) = delete;
  storage_area & operator=(storage_area const &) = delete;
  ~storage_area() { close(); }

  bool alive() const { return fd >= 0; }
  // The name is the key, not the path: without the '/', the NUL and the leading point
  static bool good_name(std::string_view name) {
    if(name.empty() || name.size() > 200 || name[0] == '.') return false;
    for(char c: name)
      if(c == '/' || c == '\0') return false;
    return true;
  }
  // 'Open'; otherwise lock cause = false: no flock (readers and private copies - file they don't write) shared = false: display
  // MAP_PRIVATE - changes do not enter the file and are not visible after fork (check databases in the area)
  //
  std::string open(std::string const & dir, std::string_view name, bool share = true, bool lock = true) {
    if(!good_name(name)) return "имя хранилища — ключ без '/' и ведущей точки";
    if(::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) return cat("каталог хранилищ VM недоступен: ", std::strerror(errno));
    std::string path = cat(dir, "/", name);
    bool fresh = !file_exists(path);
    int f = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if(f < 0) return std::strerror(errno);
    if(lock && ::flock(f, LOCK_EX | LOCK_NB) != 0) {
      ::close(f);
      return "хранилище занято другим исполнением";
    }
    struct stat st{};
    void * m = ::fstat(f, &st) == 0 ? ::mmap(nullptr, range, PROT_READ | PROT_WRITE, (share ? MAP_SHARED : MAP_PRIVATE) | MAP_NORESERVE, f, 0) : MAP_FAILED;
    if(m == MAP_FAILED) {
      ::close(f);
      return cat("не отобразить: ", std::strerror(errno));
    }
    if(fresh) { // the name of the new storage - on the disk before the first fixation
      int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      if(dfd >= 0) {
        ::fsync(dfd);
        ::close(dfd);
      }
    }
    fd = f;
    at = static_cast<std::byte *>(m);
    size = uint64_t(st.st_size);
    shared = share;
    if(share) register_shared_area(this);
    return "";
  }
  // +n zero bytes at the end: range pages behind the previous end become available
  std::string grow(uint64_t n) {
    if(n > range - size) return "больше 64 ГиБ";
    int rc;
    do rc = ::ftruncate(fd, off_t(size + n)); while(rc < 0 && errno == EINTR);
    if(rc < 0) return std::strerror(errno);
    size += n;
    return "";
  }
  // Fix: Pages and sizes on disk
  std::string commit() {
    if(size && ::msync(at, size, MS_SYNC) != 0) return std::strerror(errno);
    int rc;
    do rc = ::fsync(fd); while(rc < 0 && errno == EINTR);
    return rc < 0 ? std::string(std::strerror(errno)) : std::string();
  }
  // Close without fixing: addresses and file - system
  void close() {
    if(fd < 0) return;
    std::erase(shared_areas(), this);
    ::munmap(at, range);
    ::close(fd);
    fd = -1;
    at = nullptr;
    size = 0;
  }
};

// Fork descendant: shared storage displays become private (the record does not go to the file and to the parent)
void remap_private_after_fork() {
  for(storage_area * a: shared_areas()) {
    if(a->fd < 0 || !a->shared) continue;
    ::mmap(a->at, storage_area::range, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED | MAP_NORESERVE, a->fd, 0);
    a->shared = false;
  }
}

// IPC between VMs on one machine (plan "VM OS", author's decision 2026-11-09): shared memory + signal.
// 
// ; - VM service, common to instances. Everything else is **mine**: channel is a named file in the IPC
// machine directory displayed by MAP_SHARED in both VMs; messages are a ring of bytes (length u32 +
// bytes), one writer and one reader (role holds flock of individual files: the process dies - the lock is
// removed itself); signal is futex in common memory (not sockets and not pipes). The channel lives as long
// as at least one end is held: having opened both ends free, the ring begins anew (obsolete messages from
// the previous launch are not read).
//
struct ipc_header {
  char magic[8];
  uint32_t version;
  uint32_t cap; // byte ring, degree two
  alignas(64) std::atomic<uint64_t> head; // Read byte (moves the reader)
  alignas(64) std::atomic<uint64_t> tail; // byte (moves the writer)
  alignas(64) std::atomic<uint32_t> data_seq; // Reader’s futex: Increases by each entry and closing
  alignas(64) std::atomic<uint32_t> space_seq; // Writer's futex: grows by every reading and closing
};

struct ipc_channel {
  static constexpr uint32_t header_bytes = 4096;
  static constexpr uint32_t default_cap = 1u << 16;
  static constexpr uint32_t max_cap = 1u << 28;
  // kernel response statuses: 0 yes, 4 host error (cause), 5 time out, 6 other end closed, 7 message larger than ring
  int fd = -1, lock_fd = -1;
  ipc_header * h = nullptr;
  std::byte * ring = nullptr;
  uint64_t map_bytes = 0;
  bool writer = false;

  ipc_channel() = default;
  ipc_channel(ipc_channel const &) = delete;
  ipc_channel & operator=(ipc_channel const &) = delete;
  ~ipc_channel() { close(); }

  bool alive() const { return h != nullptr; }
  // IPC machine directory: VISH_IPC_DIR, otherwise $ XDG_RUNTIME_DIR (or /tmp)/vish-ipc-<uid >>
  static std::string dir() {
    if(char const * d = std::getenv("VISH_IPC_DIR"); d && *d) return d;
    char const * run = std::getenv("XDG_RUNTIME_DIR");
    return cat(run && *run ? run : "/tmp", "/vish-ipc-", ::getuid());
  }
  static bool good_name(std::string_view name) { return storage_area::good_name(name); }
  static uint64_t now_ms() {
    timespec t;
    ::clock_gettime(CLOCK_MONOTONIC, &t);
    return uint64_t(t.tv_sec) * 1000 + uint64_t(t.tv_nsec) / 1000000;
  }
  static int open_lock(std::string const & path) {
    int f = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    return f;
  }
  // The end is occupied (the process holds it): the role lock is not taken
  static bool end_held(std::string const & base, char role) {
    int f = open_lock(cat(base, ".", role));
    if(f < 0) return false;
    bool held = ::flock(f, LOCK_EX | LOCK_NB) != 0;
    ::close(f); // locks off
    return held;
  }
  // 'is open; otherwise the cause. role - 'w' writer, 'r' reader; cap - for new channel (0 - default)
  std::string open(std::string_view name, char role, uint32_t want_cap = 0) {
    if(alive()) close();
    if(!good_name(name)) return "имя канала — ключ без '/' и ведущей точки";
    if(role != 'w' && role != 'r') return "роль — w или r";
    std::string d = dir();
    if(::mkdir(d.c_str(), 0700) != 0 && errno != EEXIST) return cat("каталог IPC недоступен: ", std::strerror(errno));
    std::string base = cat(d, "/", name);
    int lf = open_lock(cat(base, ".", role));
    if(lf < 0) return std::strerror(errno);
    if(::flock(lf, LOCK_EX | LOCK_NB) != 0) {
      ::close(lf);
      return cat("конец «", role == 'w' ? "писатель" : "читатель", "» канала занят другим процессом");
    }
    int f = ::open(base.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if(f < 0) {
      ::close(lf);
      return std::strerror(errno);
    }
    ::flock(f, LOCK_EX); // Creating and restarting the ring - one at a time
    struct stat st{};
    ::fstat(f, &st);
    uint32_t cap = want_cap ? want_cap : default_cap;
    if(cap < 64 || cap > max_cap || (cap & (cap - 1))) {
      ::close(f);
      ::close(lf);
      return "ёмкость кольца — степень двойки от 64 байт до 256 МиБ";
    }
    bool fresh = st.st_size == 0 || !end_held(base, role == 'w' ? 'r' : 'w'); // There is no other end, the last ring is not necessary.
    if(fresh) {
      uint64_t old = st.st_size > header_bytes ? uint64_t(st.st_size) - header_bytes : 0;
      if(!want_cap && old >= 64 && old <= max_cap && !(old & (old - 1))) cap = uint32_t(old); // capacitance
      if(uint64_t(st.st_size) != uint64_t(header_bytes) + cap && ::ftruncate(f, off_t(header_bytes + cap)) != 0) {
        std::string why = std::strerror(errno);
        ::close(f);
        ::close(lf);
        return why;
      }
    }
    ::fstat(f, &st);
    void * m = ::mmap(nullptr, size_t(st.st_size), PROT_READ | PROT_WRITE, MAP_SHARED, f, 0);
    if(m == MAP_FAILED) {
      std::string why = std::strerror(errno);
      ::close(f);
      ::close(lf);
      return cat("не отобразить: ", why);
    }
    auto * hh = static_cast<ipc_header *>(m);
    if(fresh) {
      std::memcpy(hh->magic, "VISHIPC1", 8);
      hh->version = 1;
      hh->cap = cap;
      hh->head.store(0);
      hh->tail.store(0);
      hh->data_seq.store(0);
      hh->space_seq.store(0);
    } else if(std::memcmp(hh->magic, "VISHIPC1", 8) != 0 || hh->version != 1 || uint64_t(st.st_size) < header_bytes + uint64_t(hh->cap)) {
      ::munmap(m, size_t(st.st_size));
      ::close(f);
      ::close(lf);
      return "не канал vish (или другой версии)";
    }
    ::flock(f, LOCK_UN);
    fd = f;
    lock_fd = lf;
    h = hh;
    ring = static_cast<std::byte *>(m) + header_bytes;
    map_bytes = uint64_t(st.st_size);
    writer = role == 'w';
    base_name = base;
    peer_seen = false;
    return "";
  }
  // The other end is alive (holds its lock)
  bool peer_alive(std::string const & base) const { return end_held(base, writer ? 'r' : 'w'); }
  std::string base_name;
  bool peer_seen = false; // The other end was alive: leaving after that is closing, not "not yet come."
  // The other end is alive now; true is remembered, false after true - channel closed
  bool peer_gone() {
    if(peer_alive(base_name)) {
      peer_seen = true;
      return false;
    }
    return peer_seen;
  }
  static long futex(std::atomic<uint32_t> * addr, int op, uint32_t val, timespec const * to) {
    return ::syscall(SYS_futex, reinterpret_cast<uint32_t *>(addr), op, val, to, nullptr, 0); // without PRIVATE: shared memory of different processes
  }
  // wait for the change *seq from the value seen to ms milliseconds (0 - do not wait, <0 - without limit); true - not out of time
  bool wait_on(std::atomic<uint32_t> * seq, uint32_t seen, int64_t ms, uint64_t until) {
    if(ms == 0) return false;
    timespec ts{}, * pts = nullptr;
    if(ms > 0) {
      uint64_t now = now_ms();
      if(now >= until) return false;
      uint64_t left = until - now;
      // Another end could die without waking up: wake up at least once in 200 ms to check his lock
      left = std::min<uint64_t>(left, 200);
      ts.tv_sec = time_t(left / 1000);
      ts.tv_nsec = long(left % 1000) * 1000000;
      pts = &ts;
    } else {
      ts.tv_nsec = 200 * 1000000;
      pts = &ts;
    }
    futex(seq, FUTEX_WAIT, seen, pts);
    return true;
  }
  uint32_t cap() const { return h->cap; }
  // put a message: 0, 4 no, 5 time out (ring full), 6 reader left, 7 more ring
  int send(std::string_view msg, int64_t ms) {
    if(!alive() || !writer) return 4;
    uint64_t need = uint64_t(msg.size()) + 4;
    if(need > h->cap) return 7;
    uint64_t until = ms > 0 ? now_ms() + uint64_t(ms) : 0;
    for(;;) {
      uint32_t seen = h->space_seq.load(std::memory_order_acquire);
      uint64_t tail = h->tail.load(std::memory_order_relaxed), head = h->head.load(std::memory_order_acquire);
      if(h->cap - (tail - head) >= need) {
        uint32_t len = uint32_t(msg.size());
        auto put = [&](uint64_t at, void const * p, uint64_t n) {
          uint64_t o = at & (h->cap - 1), first = std::min<uint64_t>(n, h->cap - o);
          std::memcpy(ring + o, p, size_t(first));
          if(first < n) std::memcpy(ring, static_cast<std::byte const *>(p) + first, size_t(n - first));
        };
        put(tail, &len, 4);
        put(tail + 4, msg.data(), msg.size());
        h->tail.store(tail + need, std::memory_order_release);
        h->data_seq.fetch_add(1, std::memory_order_release);
        futex(&h->data_seq, FUTEX_WAKE, 1, nullptr);
        return 0;
      }
      if(peer_gone()) return 6;
      if(ms == 0) return 5;
      if(ms > 0 && now_ms() >= until) return 5;
      wait_on(&h->space_seq, seen, ms, until);
    }
  }
  // Take a message: 0 (msg), 5 is empty and time is up, 6 is empty and the writer is gone.
  int recv(std::string & msg, int64_t ms) {
    if(!alive() || writer) return 4;
    uint64_t until = ms > 0 ? now_ms() + uint64_t(ms) : 0;
    for(;;) {
      uint32_t seen = h->data_seq.load(std::memory_order_acquire);
      uint64_t head = h->head.load(std::memory_order_relaxed), tail = h->tail.load(std::memory_order_acquire);
      if(tail != head) {
        auto get = [&](uint64_t at, void * p, uint64_t n) {
          uint64_t o = at & (h->cap - 1), first = std::min<uint64_t>(n, h->cap - o);
          std::memcpy(p, ring + o, size_t(first));
          if(first < n) std::memcpy(static_cast<std::byte *>(p) + first, ring, size_t(n - first));
        };
        uint32_t len = 0;
        get(head, &len, 4);
        msg.resize(len);
        get(head + 4, msg.data(), len);
        h->head.store(head + 4 + len, std::memory_order_release);
        h->space_seq.fetch_add(1, std::memory_order_release);
        futex(&h->space_seq, FUTEX_WAKE, 1, nullptr);
        return 0;
      }
      if(peer_gone()) {
        if(h->tail.load(std::memory_order_acquire) != head) continue; // I finished writing before leaving.
        return 6;
      }
      if(ms == 0) return 5;
      if(ms > 0 && now_ms() >= until) return 5;
      wait_on(&h->data_seq, seen, ms, until);
    }
  }
  void close() {
    if(!alive()) return;
    // The lock of the role is to remove the first, then wake the other end: he will wake up and see that there is no end.
    if(lock_fd >= 0) ::close(lock_fd);
    lock_fd = -1;
    std::atomic<uint32_t> * other = writer ? &h->data_seq : &h->space_seq;
    other->fetch_add(1, std::memory_order_release);
    futex(other, FUTEX_WAKE, INT32_MAX, nullptr);
    ::munmap(h, size_t(map_bytes));
    h = nullptr;
    ring = nullptr;
    if(fd >= 0) ::close(fd);
    fd = -1;
  }
};

// Descriptor lines (stdin host): your buffer, read in chunks.
struct line_reader {
  int fd = 0;
  std::string buf;
  std::size_t pos = 0;
  bool eof = false;

  bool next(std::string & line) {
    for(;;) {
      if(auto nl = buf.find('\n', pos); nl != std::string::npos) {
        line.assign(buf, pos, nl - pos);
        pos = nl + 1;
        return true;
      }
      if(eof) {
        if(pos >= buf.size()) return false;
        line.assign(buf, pos);
        pos = buf.size();
        return true;
      }
      buf.erase(0, pos);
      pos = 0;
      char chunk[65536];
      auto n = ::read(fd, chunk, sizeof chunk);
      if(n <= 0) eof = true;
      else buf.append(chunk, std::size_t(n));
    }
  }
};

line_reader & host_stdin() {
  static line_reader r{0};
  return r;
}

// --- host I/O via io_uring (M3b) ---
//
// The kernel delivers events and data in a stream, rather than through blocking sockets:
// 
// 
// 
// Multishot accept and recv with a ring of kernel buffers (as in): one
// application - as many events as you want. The socket program doesn't see.
//

// Listener socket at 127.0.0.1:port (0 - choose free); -1 - error.
int listen_tcp(int port) {
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if(fd < 0) return -1;
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(uint16_t(port));
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if(::bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0 || ::listen(fd, 512) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

int bound_port(int fd) {
  sockaddr_in a{};
  socklen_t n = sizeof a;
  ::getsockname(fd, reinterpret_cast<sockaddr *>(&a), &n);
  return ntohs(a.sin_port);
}

bool ipv4(std::string const & host, int port, sockaddr_in & a) {
  a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(uint16_t(port));
  return ::inet_pton(AF_INET, host.c_str(), &a.sin_addr) == 1;
}

// Listening to a Unix socket (protocol editorйVM, M5); the old socket file is removed.
int listen_unix(std::string const & path) {
  sockaddr_un a{};
  if(path.size() >= sizeof a.sun_path) {
    errno = ENAMETOOLONG; // Socket path - no longer than 107 bytes
    return -1;
  }
  a.sun_family = AF_UNIX;
  std::memcpy(a.sun_path, path.data(), path.size());
  int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if(fd < 0) return -1;
  ::unlink(path.c_str());
  if(::bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0 || ::listen(fd, 16) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

// A pair of linked sockets is a client in the same process (protocol tests).
bool socket_pair(int (&fds)[2]) { return ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0; }

void write_all(int fd, std::string_view s) {
  while(!s.empty()) {
    auto n = ::write(fd, s.data(), s.size());
    if(n < 0 && errno == EINTR) continue;
    if(n <= 0) throw diagnostic(cat("запись: ", std::strerror(errno)), {});
    s.remove_prefix(std::size_t(n));
  }
}

// Everything that has already come, without waiting.
std::string read_ready(int fd) {
  std::string out;
  char buf[16384];
  for(;;) {
    auto n = ::recv(fd, buf, sizeof buf, MSG_DONTWAIT);
    if(n <= 0) break;
    out.append(buf, std::size_t(n));
  }
  return out;
}

// --- child processes (parallel run of tests) ---
//
// The child is fork: the body is executed in it, its total (bytes) goes into the pipe, the
// output is exit, without destructors of statics. The global state (JIT, pages, forms) of the
// child has its own. The ends of the pipe - O_CLOEXEC: grandchildren (qemu) do not hold them.
struct child {
  int pid = -1;
  int fd = -1; // parent's pipe end: child's total, EOF - child finished
};

child fork_child(auto && body) {
  int fds[2];
  if(::pipe2(fds, O_CLOEXEC) != 0) throw diagnostic(cat("pipe: ", std::strerror(errno)), {});
  std::fflush(stdout); // The stdio buffer of the parent must not leave twice.
  std::fflush(stderr);
  int pid = ::fork();
  if(pid < 0) throw diagnostic(cat("fork: ", std::strerror(errno)), {});
  if(pid == 0) {
    ::close(fds[0]);
    int code = 0;
    try {
      write_all(fds[1], body());
    } catch(diagnostic const &) {
      code = 1;
    }
    ::_exit(code);
  }
  ::close(fds[1]);
  return {pid, fds[0]};
}

// Wait for one of the fds to be read or closed; the finished ones are true.
std::vector<bool> wait_readable(std::vector<int> const & fds) {
  std::vector<pollfd> ps;
  for(int fd: fds) ps.push_back({fd, POLLIN, 0});
  while(::poll(ps.data(), ps.size(), -1) < 0)
    if(errno != EINTR) throw diagnostic(cat("poll: ", std::strerror(errno)), {});
  std::vector<bool> ready;
  for(auto const & p: ps) ready.push_back(p.revents != 0);
  return ready;
}

// Finish finished (after wait_readable): False - end of flow.
bool read_some(int fd, std::string & to) {
  char buf[65536];
  for(;;) {
    auto n = ::read(fd, buf, sizeof buf);
    if(n < 0 && errno == EINTR) continue;
    if(n <= 0) return false;
    to.append(buf, std::size_t(n));
    return true;
  }
}

// The result of the child: exit code, or -signal if he was killed.
int wait_child(int pid) {
  int st = 0;
  while(::waitpid(pid, &st, 0) < 0)
    if(errno != EINTR) return -1;
  return WIFEXITED(st) ? WEXITSTATUS(st) : -WTERMSIG(st);
}

void close_fd(int fd) { ::close(fd); }

// The io_uring ring and the common ring of reception buffers (Group 0).
struct uring {
  static constexpr unsigned buf_count = 256, buf_size = 16384;
  io_uring ring{};
  io_uring_buf_ring * bufs = nullptr;
  std::vector<char> backing;
  bool ok = false;

  uring() {
    if(io_uring_queue_init(512, &ring, 0) < 0) return;
    ok = true;
  }
  ~uring() {
    if(bufs) io_uring_free_buf_ring(&ring, bufs, buf_count, 0);
    if(ok) io_uring_queue_exit(&ring);
  }

  // Reception buffers - as needed (not every program needs a network).
  void ensure_bufs() {
    if(bufs || !ok) return;
    int err = 0;
    bufs = io_uring_setup_buf_ring(&ring, buf_count, 0, 0, &err);
    if(!bufs) throw diagnostic(cat("io_uring: кольцо буферов: ", std::strerror(-err)), {});
    backing.resize(std::size_t(buf_count) * buf_size);
    for(unsigned i = 0; i < buf_count; ++i) io_uring_buf_ring_add(bufs, backing.data() + std::size_t(i) * buf_size, buf_size, uint16_t(i), io_uring_buf_ring_mask(buf_count), int(i));
    io_uring_buf_ring_advance(bufs, int(buf_count));
  }

  char const * buffer(unsigned id) const { return backing.data() + std::size_t(id) * buf_size; }
  void give_back(unsigned id) {
    io_uring_buf_ring_add(bufs, backing.data() + std::size_t(id) * buf_size, buf_size, uint16_t(id), io_uring_buf_ring_mask(buf_count), 0);
    io_uring_buf_ring_advance(bufs, 1);
  }

  io_uring_sqe * sqe() {
    io_uring_sqe * s = io_uring_get_sqe(&ring);
    if(!s) {
      io_uring_submit(&ring);
      s = io_uring_get_sqe(&ring);
    }
    if(!s) throw diagnostic("io_uring: очередь заявок полна", {});
    return s;
  }

  void accept_all(int fd, uint64_t tag) {
    io_uring_sqe * s = sqe();
    io_uring_prep_multishot_accept(s, fd, nullptr, nullptr, SOCK_CLOEXEC);
    io_uring_sqe_set_data64(s, tag);
    io_uring_submit(&ring);
  }
  void recv_all(int fd, uint64_t tag) {
    ensure_bufs();
    io_uring_sqe * s = sqe();
    io_uring_prep_recv_multishot(s, fd, nullptr, 0, 0);
    s->flags |= IOSQE_BUFFER_SELECT;
    s->buf_group = 0;
    io_uring_sqe_set_data64(s, tag);
    io_uring_submit(&ring);
  }
  void send(int fd, char const * p, std::size_t n, uint64_t tag) {
    io_uring_sqe * s = sqe();
    io_uring_prep_send(s, fd, p, n, MSG_NOSIGNAL);
    io_uring_sqe_set_data64(s, tag);
    io_uring_submit(&ring);
  }
  void connect(int fd, sockaddr_in const * a, uint64_t tag) {
    io_uring_sqe * s = sqe();
    io_uring_prep_connect(s, fd, reinterpret_cast<sockaddr const *>(a), sizeof *a);
    io_uring_sqe_set_data64(s, tag);
    io_uring_submit(&ring);
  }
  void read(int fd, char * p, std::size_t n, uint64_t off, uint64_t tag) {
    io_uring_sqe * s = sqe();
    io_uring_prep_read(s, fd, p, unsigned(n), off);
    io_uring_sqe_set_data64(s, tag);
    io_uring_submit(&ring);
  }
  void write(int fd, char const * p, std::size_t n, uint64_t off, uint64_t tag) {
    io_uring_sqe * s = sqe();
    io_uring_prep_write(s, fd, p, unsigned(n), off);
    io_uring_sqe_set_data64(s, tag);
    io_uring_submit(&ring);
  }
  // Monoton time in milliseconds (set pauses).
  static uint64_t now_ms() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return uint64_t(t.tv_sec) * 1000 + uint64_t(t.tv_nsec) / 1000000;
  }

  // Timer: End with tag in ms milliseconds (res ==-ETIME). The term lives in a ring until
  // completion - one per ring.
  __kernel_timespec timer_at{};
  void timeout(uint32_t ms, uint64_t tag) {
    timer_at.tv_sec = ms / 1000;
    timer_at.tv_nsec = long(ms % 1000) * 1000000;
    io_uring_sqe * s = sqe();
    io_uring_prep_timeout(s, &timer_at, 0, 0);
    io_uring_sqe_set_data64(s, tag);
    io_uring_submit(&ring);
  }
  // Readiness of the descriptor (PSI: POLLPRI - pressure trigger triggered), one operation
  void poll(int fd, unsigned mask, uint64_t tag) {
    io_uring_sqe * s = sqe();
    io_uring_prep_poll_add(s, fd, mask);
    io_uring_sqe_set_data64(s, tag);
    io_uring_submit(&ring);
  }
  void cancel_fd(int fd) {
    io_uring_sqe * s = sqe();
    io_uring_prep_cancel_fd(s, fd, IORING_ASYNC_CANCEL_ALL);
    io_uring_sqe_set_data64(s, 0);
    io_uring_submit(&ring);
  }

  struct completion {
    uint64_t tag;
    int res;
    unsigned flags;
  };
  // Wait for at least one finish and pick up all the finished ones.
  std::vector<completion> wait() {
    std::vector<completion> out;
    io_uring_cqe * c = nullptr;
    int r;
    do r = io_uring_wait_cqe(&ring, &c);
    while(r == -EINTR);
    if(r < 0) return out;
    unsigned head, n = 0;
    io_uring_for_each_cqe(&ring, head, c) {
      out.push_back({io_uring_cqe_get_data64(c), c->res, c->flags});
      ++n;
    }
    io_uring_cq_advance(&ring, n);
    return out;
  }
  // Completed completions without waiting (server between process quanta).
  std::vector<completion> peek() {
    std::vector<completion> out;
    io_uring_cqe * c = nullptr;
    unsigned head, n = 0;
    io_uring_for_each_cqe(&ring, head, c) {
      out.push_back({io_uring_cqe_get_data64(c), c->res, c->flags});
      ++n;
    }
    io_uring_cq_advance(&ring, n);
    return out;
  }
};
// Core Types (§9.5, step 12.3b): Intel’s hybrid processor has /sys/devices/cpu_core/cpus
// (performance) and cpu_atom/cpus (energy efficient). Not both, untethered.
enum class core_kind { performance, efficiency };
bool cpu_list(char const * path, cpu_set_t & set) {
  CPU_ZERO(&set);
  int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if(fd < 0) return false;
  char buf[256];
  ssize_t n = ::read(fd, buf, sizeof buf - 1);
  ::close(fd);
  if(n <= 0) return false;
  std::string_view t(buf, std::size_t(n));
  bool any = false;
  while(!t.empty()) { // "0-11" or "0-3.8-11"
    std::size_t comma = t.find(',');
    std::string_view part = t.substr(0, comma);
    t = comma == std::string_view::npos ? std::string_view() : t.substr(comma + 1);
    unsigned a = 0, b = 0;
    auto [p1, e1] = std::from_chars(part.data(), part.data() + part.size(), a);
    if(e1 != std::errc{}) continue;
    b = a;
    if(p1 < part.data() + part.size() && *p1 == '-') std::from_chars(p1 + 1, part.data() + part.size(), b);
    for(unsigned c = a; c <= b && c < CPU_SETSIZE; ++c) {
      CPU_SET(c, &set);
      any = true;
    }
  }
  return any;
}
// ARM (big.LITTLE): the core has cpu_capacity; the most capacious are productive, the rest are
// energy efficient. Everything is the same (or there is no information) - false.
bool cpu_capacity_split(cpu_set_t & perf, cpu_set_t & eff) {
  CPU_ZERO(&perf);
  CPU_ZERO(&eff);
  std::vector<std::pair<unsigned, unsigned>> caps; // (core, container)
  for(unsigned c = 0; c < CPU_SETSIZE; ++c) {
    int fd = ::open(cat("/sys/devices/system/cpu/cpu", c, "/cpu_capacity").c_str(), O_RDONLY | O_CLOEXEC);
    if(fd < 0) {
      if(c > 0 && caps.empty()) return false;
      if(::access(cat("/sys/devices/system/cpu/cpu", c).c_str(), F_OK) != 0) break;
      continue;
    }
    char buf[32];
    ssize_t n = ::read(fd, buf, sizeof buf);
    ::close(fd);
    unsigned v = 0;
    if(n > 0) std::from_chars(buf, buf + n, v);
    caps.push_back({c, v});
  }
  unsigned top = 0;
  for(auto [c, v]: caps) top = std::max(top, v);
  bool any_eff = false;
  for(auto [c, v]: caps) {
    if(v == top) CPU_SET(c, &perf);
    else {
      CPU_SET(c, &eff);
      any_eff = true;
    }
  }
  return any_eff;
}
void pin_to(core_kind k) {
  cpu_set_t perf, eff;
  bool intel = cpu_list("/sys/devices/cpu_core/cpus", perf) && cpu_list("/sys/devices/cpu_atom/cpus", eff);
  if(!intel && !cpu_capacity_split(perf, eff)) return; // kernel of one kind - without binding
  ::sched_setaffinity(0, sizeof(cpu_set_t), k == core_kind::performance ? &perf : &eff); // flow
}
// host stream with its own stack (VM band): starts fn(arg); waits for it pthread_join
pthread_t start_thread(size_t bytes, void (*fn)(void *), void * arg) {
  struct job {
    void (*fn)(void *);
    void * arg;
  };
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, bytes);
  pthread_t t;
  auto * j = new job{fn, arg};
  int e = pthread_create(&t, &a, [](void * p) -> void * {
    job j = *static_cast<job *>(p);
    delete static_cast<job *>(p);
    j.fn(j.arg);
    return nullptr;
  }, j);
  pthread_attr_destroy(&a);
  if(e != 0) throw diagnostic(cat("поток полосы: ", std::strerror(e)), {});
  return t;
}
// Memory pressure host (Linux PSI): trigger-descriptor triggered by POLLPRI. The window is
// a multiple of 2 s: this is possible without root rights. No PSI-1.
int memory_pressure_fd() {
  int fd = ::open("/proc/pressure/memory", O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if(fd < 0) return -1;
  char const trig[] = "some 150000 2000000"; // with zero at the end, that's what the kernel requires.
  if(::write(fd, trig, sizeof trig) < 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}
// The bottom of the execution stack (Flow or fiber): from this VM knows that the stack is running out, and gives
// a stack_overflow error instead of a drop ("no UB", R20). The stream has pthread, and fiber has its stack.
thread_local char const * stack_floor = nullptr;
thread_local char const * thread_floor_cache = nullptr;
char const * thread_floor() {
  if(thread_floor_cache) return thread_floor_cache;
  pthread_attr_t a;
  void * addr = nullptr;
  size_t size = 0;
  if(pthread_getattr_np(pthread_self(), &a) == 0) {
    pthread_attr_getstack(&a, &addr, &size);
    pthread_attr_destroy(&a);
  }
#if defined(__SANITIZE_ADDRESS__)
  // ASan, when throwing, cleans the shadow of the stack no deeper than 64 MB (google/sanitizers #189), deeper - false
  // errors after the throw: under it, the bottom of the stream is 64 MB from the top (fiber stack - 64 MB and so on).
  if(size > (size_t(64) << 20)) addr = static_cast<char *>(addr) + size - (size_t(64) << 20);
#endif
  return thread_floor_cache = static_cast<char const *>(addr);
}
// The work of VM is on its stack: the depth of parsing and executions by AST (compiler in the
// language compiles itself, VISH_JIT_SELF) does not depend on the limit of the host stack (ulimit
// -s, under asan frames three times more). Reserve - virtual, pages - as you touch.
int with_stack(size_t bytes, auto && f) {
  struct job {
    decltype(f) & run;
    int rc = 1;
  } j{f};
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, bytes);
  pthread_t t;
  auto body = [](void * p) -> void * {
    auto & x = *static_cast<job *>(p);
    x.rc = x.run();
    return nullptr;
  };
  int e = pthread_create(&t, &a, body, &j);
  pthread_attr_destroy(&a);
  if(e) return f(); // Not given flow - on the host stack
  pthread_join(t, nullptr);
  return j.rc;
}

} // namespace vish::os

namespace vish {

// Slicing string_view text without copies: lines and words.
std::vector<std::string_view> lines_of(std::string_view s) {
  std::vector<std::string_view> out;
  while(!s.empty()) {
    auto nl = s.find('\n');
    out.push_back(s.substr(0, nl));
    if(nl == std::string_view::npos) break;
    s.remove_prefix(nl + 1);
  }
  return out;
}

std::vector<std::string_view> words_of(std::string_view s) {
  std::vector<std::string_view> out;
  for(std::size_t i = 0; i < s.size();) {
    while(i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    std::size_t start = i;
    while(i < s.size() && s[i] != ' ' && s[i] != '\t') ++i;
    if(i > start) out.push_back(s.substr(start, i - start));
  }
  return out;
}

} // namespace vish
