// host VM Layer: All accesses to the OS are here (files, stdin, stdout).
//
// Tsar’s programs do not go directly to the OS: their I/O are VM services on top of
// this layer (§9.5, M3): Files are not “read” but are displayed: No iostream.
//
//
//
#pragma once

#include "base.hpp"

#include <fcntl.h>
#include <sys/mman.h>
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
