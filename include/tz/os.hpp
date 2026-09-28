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
#include <sys/epoll.h>
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

// --- host (M3b): non-blocking sockets and epoll ---
//
// The program sees only the blocker read/write streams; the expectation of readiness is
// here at the VM scheduler.
//

void nonblocking(int fd) { ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }

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
  if(::bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0 || ::listen(fd, 128) != 0) {
    ::close(fd);
    return -1;
  }
  nonblocking(fd);
  return fd;
}

int bound_port(int fd) {
  sockaddr_in a{};
  socklen_t n = sizeof a;
  ::getsockname(fd, reinterpret_cast<sockaddr *>(&a), &n);
  return ntohs(a.sin_port);
}

// Begin connection (non-blocking); readiness - on record.
int connect_tcp(std::string const & host, int port) {
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if(fd < 0) return -1;
  nonblocking(fd);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(uint16_t(port));
  if(::inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1) {
    ::close(fd);
    return -1;
  }
  if(::connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0 && errno != EINPROGRESS) {
    ::close(fd);
    return -1;
  }
  return fd;
}

int socket_error(int fd) {
  int e = 0;
  socklen_t n = sizeof e;
  ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &n);
  return e;
}

// Waiting for descriptors: the key is the waiting fiber number.
struct poller {
  int ep = ::epoll_create1(EPOLL_CLOEXEC);
  ~poller() {
    if(ep >= 0) ::close(ep);
  }
  void wait_for(int fd, bool write, uint32_t key) {
    epoll_event ev{};
    ev.events = (write ? EPOLLOUT : EPOLLIN) | EPOLLONESHOT | EPOLLRDHUP;
    ev.data.u32 = key;
    if(::epoll_ctl(ep, EPOLL_CTL_MOD, fd, &ev) != 0) ::epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
  }
  void forget(int fd) { ::epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr); }
  std::vector<uint32_t> wait() {
    epoll_event evs[64];
    int n;
    do n = ::epoll_wait(ep, evs, 64, -1);
    while(n < 0 && errno == EINTR);
    std::vector<uint32_t> out;
    for(int i = 0; i < n; ++i) out.push_back(evs[i].data.u32);
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
