// x86-64: исполняемые страницы для кода компилятора на языке и переключатель стеков
// fiber (speak §10.3 - 10.4, D8). Encoding program instructions at targets in language (context
// amd64 prelude): Code memory - W^X: write to RW pages, after finalization - RX.
//
#pragma once

#include "../base.hpp"

#include <sys/mman.h>

namespace vish::x64 {

// clang-format off
enum reg : uint8_t { rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi, r8, r9, r10, r11, r12, r13, r14, r15 };
// clang-format on

// Executable Code: Owns pages, releases them.
struct code_block {
  code_block() = default;
  code_block(void * mem, std::size_t size) : entry(mem), size(size) {}
  code_block(code_block && o) noexcept : entry(std::exchange(o.entry, nullptr)), size(o.size) {}
  code_block & operator=(code_block && o) noexcept {
    std::swap(entry, o.entry);
    std::swap(size, o.size);
    return *this;
  }
  ~code_block() {
    if(entry) munmap(entry, size);
  }

  void * entry = nullptr;
  std::size_t size = 0;
};

struct assembler {
  static constexpr std::string_view target = "amd64"; // code generation context in language (§10.4)

  // fiber stack switch (fiber.hpp) - several instructions
  void mov(reg dst, reg src) { op_rr(0x89, src, dst); }
  void store(reg base, int32_t disp, reg src) {
    rex(true, src, 0, base);
    byte(0x89);
    modrm(2, src, base);
    if((base & 7) == 4) byte(0x24);
    i32le(disp);
  }
  void push(reg r) {
    if(r >= 8) byte(0x41);
    byte(0x50 + (r & 7));
  }
  void pop(reg r) {
    if(r >= 8) byte(0x41);
    byte(0x58 + (r & 7));
  }
  void call(reg r) {
    if(r >= 8) byte(0x41);
    byte(0xFF);
    modrm(3, 2, r);
  }
  void ret() { byte(0xC3); }

  // The finished executable block (W^X).
  code_block finalize() {
    std::size_t size = (buf.size() + 4095) & ~std::size_t(4095);
    void * mem = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if(mem == MAP_FAILED) throw diagnostic("JIT: не удалось выделить память кода", {});
    std::memcpy(mem, buf.data(), buf.size());
    if(mprotect(mem, size, PROT_READ | PROT_EXEC) != 0) {
      munmap(mem, size);
      throw diagnostic("JIT: не удалось сделать код исполняемым", {});
    }
    return code_block(mem, size);
  }

  std::size_t size() const { return buf.size(); }
  void byte(uint8_t b) { buf.push_back(b); }
  void i32le(int32_t v) {
    for(int k = 0; k < 4; ++k) byte(uint8_t(uint32_t(v) >> (8 * k)));
  }
  void rex(bool w, uint8_t r, uint8_t x, uint8_t b_) {
    uint8_t v = 0x40 | (w ? 8 : 0) | ((r >> 3) << 2) | ((x >> 3) << 1) | (b_ >> 3);
    if(v != 0x40) byte(v);
  }
  void modrm(uint8_t mod, uint8_t r, uint8_t rm) { byte(uint8_t((mod << 6) | ((r & 7) << 3) | (rm & 7))); }
  void op_rr(uint8_t op, reg r, reg rm) {
    rex(true, r, 0, rm);
    byte(op);
    modrm(3, r, rm);
  }

  std::vector<uint8_t> buf;
};

} // namespace vish::x64
