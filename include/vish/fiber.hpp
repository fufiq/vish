// fiber is a stack coroutine VM (§8.3 stack, M3a stack).
//
//  
// 
// Stack - pages host (mmap, bottom - protective), switching - a dozen instructions of our
// assembler: save callee-saved registers and rsp, take someone else's rsp, restore, ret.
// No ucontext (it makes a system call for every switch).
//
#pragma once

#include "jit/x64.hpp"

#include <sys/mman.h>

#if defined(__SANITIZE_ADDRESS__)
extern "C" void __sanitizer_start_switch_fiber(void ** fake_stack_save, void const * bottom, std::size_t size);
extern "C" void __sanitizer_finish_switch_fiber(void * fake_stack_save, void const ** bottom_old, std::size_t * size_old);
#endif

namespace vish {

// switch_stack(&save, to): The current sequel is in *save, then the sequel to.
using switch_fn = void (*)(void ** save, void * to);
// New stack entry point: rbx is an argument, r12 is a function; no return.
using fiber_entry = void (*)(void *);

struct stack_switcher {
  x64::code_block code;
  switch_fn swap = nullptr;
  void * trampoline = nullptr;

  stack_switcher() {
    using namespace x64;
    assembler a;
    for(reg r: {rbp, rbx, r12, r13, r14, r15}) a.push(r);
    a.store(rdi, 0, rsp);
    a.mov(rsp, rsi);
    for(reg r: {r15, r14, r13, r12, rbx, rbp}) a.pop(r);
    a.ret();
    uint32_t tramp = uint32_t(a.size());
    a.mov(rdi, rbx); // First launch: f(arg), rsp aligned at 16
    a.call(r12);
    a.byte(0x0F); // ud2: not coming back here
    a.byte(0x0B);
    code = a.finalize();
    swap = reinterpret_cast<switch_fn>(code.entry);
    trampoline = static_cast<char *>(code.entry) + tramp;
  }
};

stack_switcher & switcher() {
  static stack_switcher s;
  return s;
}

// fiber stack: no commit reserve (pages as you touch), security page at the bottom.
struct fiber_stack {
  void * base = nullptr;
  std::size_t size = 0;

  explicit fiber_stack(std::size_t n) : size(n) {
    base = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if(base == MAP_FAILED) throw diagnostic("VM: не удалось выделить стек файбера", {});
    mprotect(base, 4096, PROT_NONE);
  }
  fiber_stack(fiber_stack && o) noexcept : base(std::exchange(o.base, nullptr)), size(o.size) {}
  fiber_stack & operator=(fiber_stack &&) = delete;
  ~fiber_stack() {
    if(base) munmap(base, size);
  }

  // Initial sequel: six "saved" registers and a springboard address.
  void * start(fiber_entry f, void * arg) {
    auto top = reinterpret_cast<uintptr_t>(static_cast<char *>(base) + size) & ~uintptr_t(15);
    auto * sp = reinterpret_cast<uint64_t *>(top) - 7;
    sp[0] = 0; // r15
    sp[1] = 0; // r14
    sp[2] = 0; // r13
    sp[3] = reinterpret_cast<uint64_t>(f); // r12
    sp[4] = reinterpret_cast<uint64_t>(arg); // rbx
    sp[5] = 0; // rbp
    sp[6] = reinterpret_cast<uint64_t>(switcher().trampoline);
    return sp;
  }
};

// The transition between sequels marked for ASAN (an alien stack is not a mistake).
void switch_to(void ** save, void * to, void const * to_bottom, std::size_t to_size) {
#if defined(__SANITIZE_ADDRESS__)
  void * fake = nullptr;
  __sanitizer_start_switch_fiber(&fake, to_bottom, to_size);
  switcher().swap(save, to);
  __sanitizer_finish_switch_fiber(fake, nullptr, nullptr);
#else
  (void)to_bottom;
  (void)to_size;
  switcher().swap(save, to);
#endif
}

// The first instruction of the new fiber : ASAN learns where it came from (stack boundaries).
void fiber_started(void const ** from_bottom, std::size_t * from_size) {
#if defined(__SANITIZE_ADDRESS__)
  __sanitizer_finish_switch_fiber(nullptr, from_bottom, from_size);
#else
  (void)from_bottom;
  (void)from_size;
#endif
}

} // namespace vish
