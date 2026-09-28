// host VM Layer: All accesses to the OS are here (files, stdin, stdout).
//
// Tsar’s programs do not go directly to the OS: their I/O are VM services on top of
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
#include <liburing.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tz::os {

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
};
} // namespace tz::os

namespace tz {

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

} // namespace tz
