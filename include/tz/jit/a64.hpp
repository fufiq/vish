// AArch64 assembler for JIT (§10.3, D8): own, without LLVM and without C.
//
// code generation is one for all purposes: the goal is the context that redefines operations.
// Therefore, here are the same operations as x64::assembler, on the same register model code
// generator (battery rax, second operand rcx, call arguments rdi/rsi/rdx/rcx, variables in
// rbx/r12 - r15 and xmm8 - xmm15, frame rbp), displayed on AArch64 registers:
//
//   rax → x9, rdi/rsi/rdx/rcx → x0–x3, r8–r11 → x4–x7, rbx/r12–r15 → x19–x23
// (callee-saved: code generator saves them itself), rbp → x29, rsp → sp
// xmm0 - xmm7 → d0 - d7, xmm8 - xmm15 → d16 - d23 (caller-saved - as on x64)
// x16/x17 are internal temporal assemblers. Differences of purpose are hidden here: the result
// of the call comes in x0 (transferred to x9), temporary on the stack - 16 bytes (sp is always
// aligned, offsets from rsp double), flags after fcmp are read differently than after cmp
// (remember whose they are).
//
// Code for someone else's purpose is not executed on host: finalize gives bytes and
// places of absolute addresses host (mov_addr) - they are put by the image linker.
#pragma once

#include "x64.hpp"

namespace tz::a64 {

using x64::cond;
using x64::label;
using x64::reg;
using x64::xmm;

struct image {
  std::vector<uint8_t> bytes;
  std::vector<std::pair<uint32_t, void const *>> relocs; // movz/movk ×4 ← address in image
};

struct assembler {
  static constexpr std::string_view target = "arm64"; // code generation context in language (§10.4)

  // --- registers and conditions ---

  static uint32_t x(reg r) {
    static constexpr uint8_t map[] = {9, 3, 2, 19, 31, 29, 1, 0, 4, 5, 6, 7, 20, 21, 22, 23};
    return map[r];
  }
  static uint32_t d(xmm r) { return r < 8 ? uint32_t(r) : uint32_t(r) + 8; }
  static constexpr uint32_t ip0 = 16, ip1 = 17, zr = 31;

  // x64-condition → condition AArch64; after fcmp - real reading of flags
  uint32_t cc(cond c) const {
    enum : uint32_t { eq, ne, hs, lo, mi, pl, vs, vc, hi, ls, ge, lt, gt, le };
    if(float_flags) switch(c) {
        case x64::a: return gt;
        case x64::ae: return ge;
        case x64::b: return lt;
        case x64::be: return le;
        case x64::e: return eq; // x64 e = equal to or NaN; code generator is always together with np
        case x64::ne: return ne;
        case x64::p: return vs;
        case x64::np: return vc;
        default: break;
      }
    switch(c) {
      case x64::o: return vs;
      case x64::no: return vc;
      case x64::b: return lo;
      case x64::ae: return hs;
      case x64::e: return eq;
      case x64::ne: return ne;
      case x64::be: return ls;
      case x64::a: return hi;
      case x64::s: return mi;
      case x64::ns: return pl;
      case x64::l: return lt;
      case x64::ge: return ge;
      case x64::le: return le;
      case x64::g: return gt;
      default: throw diagnostic("AArch64: условие чётности после целого сравнения", {});
    }
  }

  // --- whole ---

  void mov(reg dst, reg src) { mov_x(x(dst), x(src)); }
  void mov(reg dst, uint64_t imm) { imm64(x(dst), imm); }
  void load(reg dst, reg base, int32_t disp) { mem(0xF8400000, 0xF8606800, x(dst), x(base), scaled(base, disp)); }
  void store(reg base, int32_t disp, reg src) { mem(0xF8000000, 0xF8206800, x(src), x(base), scaled(base, disp)); }
  void load_idx(reg dst, reg base, reg index) { word(0xF8607800 | x(index) << 16 | x(base) << 5 | x(dst)); }
  void store_idx(reg base, reg index, reg src) { word(0xF8207800 | x(index) << 16 | x(base) << 5 | x(src)); }
  void lea(reg dst, reg base, int32_t disp) { add_imm(x(dst), x(base), scaled(base, disp)); }
  void store_imm32(reg base, int32_t disp, int32_t imm) {
    imm64(ip1, uint64_t(int64_t(imm)));
    mem(0xF8000000, 0xF8206800, ip1, x(base), scaled(base, disp));
  }
  void cmp_mem_imm8(reg base, int32_t disp, int8_t imm) {
    mem(0xF8400000, 0xF8606800, ip1, x(base), scaled(base, disp));
    cmp_imm(ip1, imm);
  }

  void add(reg dst, reg src) { word(0x8B000000 | x(src) << 16 | x(dst) << 5 | x(dst)); }
  void sub(reg dst, reg src) { word(0xCB000000 | x(src) << 16 | x(dst) << 5 | x(dst)); }
  void cmp(reg a, reg b) {
    int_flags();
    word(0xEB00001F | x(b) << 16 | x(a) << 5);
  }
  void and_(reg dst, reg src) { word(0x8A000000 | x(src) << 16 | x(dst) << 5 | x(dst)); }
  void or_(reg dst, reg src) { word(0xAA000000 | x(src) << 16 | x(dst) << 5 | x(dst)); }
  void xor_(reg dst, reg src) { word(0xCA000000 | x(src) << 16 | x(dst) << 5 | x(dst)); }
  void test(reg a, reg b) {
    int_flags();
    word(0xEA00001F | x(b) << 16 | x(a) << 5);
  }
  void imul(reg dst, reg src) { word(0x9B007C00 | x(src) << 16 | x(dst) << 5 | x(dst)); }
  void cqo() {} // Divisible - rax whole; the remainder puts idiv
  // x64: rax = rax / src, rdx = rax % src
  void idiv(reg src) {
    word(0x9AC00C00 | x(src) << 16 | x(x64::rax) << 5 | ip0); // sdiv x16, x9, src
    word(0x9B008000 | x(src) << 16 | x(x64::rax) << 10 | ip0 << 5 | x(x64::rdx)); // msub x2, x16, src, x9
    mov_x(x(x64::rax), ip0);
  }
  void neg(reg r) { word(0xCB0003E0 | x(r) << 16 | x(r)); }
  void xor_imm8(reg r, int8_t imm) {
    if(imm != 1) throw diagnostic("AArch64: xor_imm8 только с 1", {});
    word(0xD2400000 | x(r) << 5 | x(r)); // eor r, r, #1
  }
  void cmp_imm8(reg r, int8_t imm) { cmp_imm(x(r), imm); }
  void setcc(cond c, reg r) { word(0x9A9F07E0 | (cc(c) ^ 1) << 12 | x(r)); } // cset: whole register 0/1
  void movzx_rax_al() {} // cset has already given 0/1 in the entire register
  void and_al_cl() { and_(x64::rax, x64::rcx); }
  void or_al_cl() { or_(x64::rax, x64::rcx); }

  // Temporary on the stack - 16 bytes; push(rbp) in the prologue - pair x29, x30
  void push(reg r) {
    if(r == x64::rbp) return word(0xA9BF7BFD); // stp x29, x30, [sp, #-16]!
    word(0xF81F0FE0 | x(r)); // str r, [sp, #-16]!
  }
  void pop(reg r) {
    if(r == x64::rbp) return word(0xA8C17BFD); // ldp x29, x30, [sp], #16
    word(0xF84107E0 | x(r)); // ldr r, [sp], #16
  }
  void sub_rsp(int32_t n) { add_imm(31, 31, -2 * n); }
  void add_rsp(int32_t n) { add_imm(31, 31, 2 * n); }
  void call(reg r) {
    word(0xD63F0000 | x(r) << 5); // blr
    mov_x(x(x64::rax), 0); // result
  }
  void call_mem(reg base, int32_t disp) {
    mem(0xF8400000, 0xF8606800, ip1, x(base), scaled(base, disp));
    word(0xD63F0000 | ip1 << 5);
    mov_x(x(x64::rax), 0);
  }
  void ret() {
    mov_x(0, x(x64::rax)); // result in x0
    word(0xD65F03C0);
  }

  // --- f64 ------------------------------------------------------------------------------

  void movsd_load(xmm dst, reg base, int32_t disp) { mem(0xFC400000, 0xFC606800, d(dst), x(base), scaled(base, disp)); }
  void movsd_store(reg base, int32_t disp, xmm src) { mem(0xFC000000, 0xFC206800, d(src), x(base), scaled(base, disp)); }
  void movsd_load_idx(xmm dst, reg base, reg index) { word(0xFC607800 | x(index) << 16 | x(base) << 5 | d(dst)); }
  void movsd_store_idx(reg base, reg index, xmm src) { word(0xFC207800 | x(index) << 16 | x(base) << 5 | d(src)); }
  void movsd(xmm dst, xmm src) { word(0x1E604000 | d(src) << 5 | d(dst)); }
  void addsd(xmm dst, xmm src) { word(0x1E602800 | d(src) << 16 | d(dst) << 5 | d(dst)); }
  void subsd(xmm dst, xmm src) { word(0x1E603800 | d(src) << 16 | d(dst) << 5 | d(dst)); }
  void mulsd(xmm dst, xmm src) { word(0x1E600800 | d(src) << 16 | d(dst) << 5 | d(dst)); }
  void divsd(xmm dst, xmm src) { word(0x1E601800 | d(src) << 16 | d(dst) << 5 | d(dst)); }
  void ucomisd(xmm a, xmm b) {
    float_flags = true;
    word(0x1E602000 | d(b) << 16 | d(a) << 5);
  }
  void xorpd(xmm dst, xmm src) { // bitwise - through whole temporary
    word(0x9E660000 | d(dst) << 5 | ip0);
    word(0x9E660000 | d(src) << 5 | ip1);
    word(0xCA000000 | ip1 << 16 | ip0 << 5 | ip0);
    word(0x9E670000 | ip0 << 5 | d(dst));
  }
  void movq(xmm dst, reg src) { word(0x9E670000 | x(src) << 5 | d(dst)); }
  void movq(reg dst, xmm src) { word(0x9E660000 | d(src) << 5 | x(dst)); }
  void cvtsi2sd(xmm dst, reg src) { word(0x9E620000 | x(src) << 5 | d(dst)); }
  void cvttsd2si(reg dst, xmm src) { word(0x9E780000 | d(src) << 5 | x(dst)); }

  // --- crossings ---

  void jmp(label & l) { branch(l, 0x14000000); }
  void jcc(cond c, label & l) { branch(l, 0x54000000 | cc(c)); }
  void bind(label & l) {
    l.pos = int64_t(buf.size());
    for(uint32_t at: l.fixups) fix(at, l.pos);
    l.fixups.clear();
  }

  uint32_t here() const { return uint32_t(buf.size()); }
  std::size_t size() const { return buf.size(); }

  // --- target ---

  // host address: the image address (component) will replace it
  void mov_addr(reg dst, void const * p) {
    relocs.push_back({here(), p});
    for(uint32_t k = 0; k < 4; ++k) word((k ? 0xF2800000 : 0xD2800000) | k << 21 | x(dst)); // movz/movk, 0
  }
  uint32_t frame_alloc() {
    uint32_t at = here();
    word(0xD2800000 | ip0); // movz x16, #size
    word(0xCB206000 | ip0 << 16 | 31 << 5 | 31); // sub sp, sp, x16
    return at;
  }
  void set_frame(uint32_t at, uint32_t bytes) {
    if(bytes > 0xFFFF) throw diagnostic("AArch64: кадр больше 64 КБ", {});
    put(at, 0xD2800000 | bytes << 5 | ip0);
  }

  image finalize() { return {std::move(buf), std::move(relocs)}; }

  // --- coding ---

  void byte(uint8_t b) { buf.push_back(b); } // byte

  void word(uint32_t w) {
    for(int k = 0; k < 4; ++k) buf.push_back(uint8_t(w >> (8 * k)));
  }
  uint32_t get(uint32_t at) const {
    uint32_t w = 0;
    for(int k = 0; k < 4; ++k) w |= uint32_t(buf[at + k]) << (8 * k);
    return w;
  }
  void put(uint32_t at, uint32_t w) {
    for(int k = 0; k < 4; ++k) buf[at + k] = uint8_t(w >> (8 * k));
  }

  // offset from rsp - in 16-byte target slots (in x64 - 8-byte)
  static int32_t scaled(reg base, int32_t disp) { return base == x64::rsp ? 2 * disp : disp; }

  void int_flags() { float_flags = false; }

  void mov_x(uint32_t dst, uint32_t src) {
    if(dst == 31 || src == 31) return word(0x91000000 | src << 5 | dst); // add dst, src, #0 (sp)
    word(0xAA0003E0 | src << 16 | dst); // orr dst, xzr, src
  }
  void imm64(uint32_t dst, uint64_t v) {
    word(0xD2800000 | uint32_t(v & 0xFFFF) << 5 | dst);
    for(uint32_t k = 1; k < 4; ++k)
      if(uint32_t chunk = uint32_t(v >> (16 * k)) & 0xFFFF) word(0xF2800000 | k << 21 | chunk << 5 | dst);
  }
  void add_imm(uint32_t dst, uint32_t src, int64_t v) {
    if(v >= 0 && v < 4096) return word(0x91000000 | uint32_t(v) << 10 | src << 5 | dst);
    if(v < 0 && v > -4096) return word(0xD1000000 | uint32_t(-v) << 10 | src << 5 | dst);
    imm64(ip0, uint64_t(v));
    word(0x8B206000 | ip0 << 16 | src << 5 | dst); // add (extended register) - suitable for sp
  }
  void cmp_imm(uint32_t r, int64_t v) {
    int_flags();
    if(v >= 0) word(0xF100001F | uint32_t(v) << 10 | r << 5); // cmp r, #v
    else word(0xB100001F | uint32_t(-v) << 10 | r << 5); // cmn r, #-v
  }
  // ldur/stur with a sign 9-bit shift, otherwise - x16 register offset
  void mem(uint32_t unscaled, uint32_t regoff, uint32_t t, uint32_t base, int32_t disp) {
    if(disp >= -256 && disp <= 255) return word(unscaled | (uint32_t(disp) & 0x1FF) << 12 | base << 5 | t);
    imm64(ip0, uint64_t(int64_t(disp)));
    word(regoff | ip0 << 16 | base << 5 | t);
  }
  void branch(label & l, uint32_t op) {
    uint32_t at = here();
    word(op);
    if(l.bound()) fix(at, l.pos);
    else l.fixups.push_back(at);
  }
  void fix(uint32_t at, int64_t target) {
    int64_t off = (target - int64_t(at)) / 4;
    uint32_t w = get(at);
    if((w & 0xFC000000) == 0x14000000) put(at, (w & 0xFC000000) | (uint32_t(off) & 0x3FFFFFF));
    else put(at, (w & 0xFF00001F) | (uint32_t(off) & 0x7FFFF) << 5);
  }

  std::vector<uint8_t> buf;
  std::vector<std::pair<uint32_t, void const *>> relocs;
  bool float_flags = false;
};

} // namespace tz::a64
