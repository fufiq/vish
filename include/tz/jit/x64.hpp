// x86-64 assembler for JIT (§10.3 - 10.4, D8): own, without LLVM and without C.
//
// Only that you need code generator operations. Code memory - W^X: write to RW pages,
// after finalization switch them to RX.
#pragma once

#include "../base.hpp"

#include <sys/mman.h>

namespace tz::x64 {

enum reg : u8 { rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi, r8, r9, r10, r11, r12, r13, r14, r15 };
enum xmm : u8 { xmm0, xmm1, xmm2, xmm3, xmm4, xmm5, xmm6, xmm7 };

// Transition conditions/setcc.
enum cond : u8 { o = 0, no, b, ae, e, ne, be, a, s, ns, p, np, l, ge, le, g };

class label {
public:
    bool bound() const { return pos_ >= 0; }

private:
    friend class assembler;
    i64              pos_ = -1;
    std::vector<u32> fixups_; // rel32 locations waiting for addresses
};

// Executable Code: Owns pages, releases them.
class code_block {
public:
    code_block() = default;
    code_block(void* mem, std::size_t size) : mem_(mem), size_(size) {}
    code_block(code_block&& o) noexcept : mem_(std::exchange(o.mem_, nullptr)), size_(o.size_) {}
    code_block& operator=(code_block&& o) noexcept {
        std::swap(mem_, o.mem_);
        std::swap(size_, o.size_);
        return *this;
    }
    ~code_block() {
        if (mem_) munmap(mem_, size_);
    }

    void*       entry() const { return mem_; }
    std::size_t size() const { return size_; }

private:
    void*       mem_  = nullptr;
    std::size_t size_ = 0;
};

class assembler {
public:
    // --- whole ---

    void mov(reg dst, reg src) { op_rr(0x89, src, dst); }
    void mov(reg dst, u64 imm) {
        rex(true, 0, 0, dst);
        byte(0xB8 + (dst & 7));
        u64le(imm);
    }
    void load(reg dst, reg base, i32 disp) { op_rm(0x8B, dst, base, disp); }
    void store(reg base, i32 disp, reg src) { op_rm(0x89, src, base, disp); }
    void load_idx(reg dst, reg base, reg index) { op_sib(0x8B, dst, base, index); }
    void store_idx(reg base, reg index, reg src) { op_sib(0x89, src, base, index); }
    void lea(reg dst, reg base, i32 disp) { op_rm(0x8D, dst, base, disp); }
    void store_imm32(reg base, i32 disp, i32 imm) { // mov qword [base+disp], imm32 (signal expands)
        op_rm(0xC7, reg(0), base, disp);
        i32le(imm);
    }
    void cmp_mem_imm8(reg base, i32 disp, i8 imm) {
        op_rm(0x83, reg(7), base, disp);
        byte(u8(imm));
    }

    void add(reg dst, reg src) { op_rr(0x01, src, dst); }
    void sub(reg dst, reg src) { op_rr(0x29, src, dst); }
    void cmp(reg a, reg b_) { op_rr(0x39, b_, a); }
    void and_(reg dst, reg src) { op_rr(0x21, src, dst); }
    void or_(reg dst, reg src) { op_rr(0x09, src, dst); }
    void xor_(reg dst, reg src) { op_rr(0x31, src, dst); }
    void test(reg a, reg b_) { op_rr(0x85, b_, a); }
    void imul(reg dst, reg src) {
        rex(true, dst, 0, src);
        byte(0x0F);
        byte(0xAF);
        modrm(3, dst, src);
    }
    void cqo() { byte(0x48); byte(0x99); }
    void idiv(reg src) { unary_f7(7, src); }
    void neg(reg r) { unary_f7(3, r); }
    void xor_imm8(reg r, i8 imm) {
        rex(true, 0, 0, r);
        byte(0x83);
        modrm(3, 6, r);
        byte(u8(imm));
    }
    void cmp_imm8(reg r, i8 imm) {
        rex(true, 0, 0, r);
        byte(0x83);
        modrm(3, 7, r);
        byte(u8(imm));
    }
    void setcc(cond c, reg r8) { // only al/cl/dl/bl
        byte(0x0F);
        byte(0x90 + c);
        modrm(3, 0, r8);
    }
    void movzx_rax_al() { byte(0x48); byte(0x0F); byte(0xB6); byte(0xC0); }
    void and_al_cl() { byte(0x20); byte(0xC8); }
    void or_al_cl() { byte(0x08); byte(0xC8); }

    void push(reg r) {
        if (r >= 8) byte(0x41);
        byte(0x50 + (r & 7));
    }
    void pop(reg r) {
        if (r >= 8) byte(0x41);
        byte(0x58 + (r & 7));
    }
    void sub_rsp(i32 n) { byte(0x48); byte(0x81); byte(0xEC); i32le(n); }
    void add_rsp(i32 n) { byte(0x48); byte(0x81); byte(0xC4); i32le(n); }
    void call(reg r) {
        if (r >= 8) byte(0x41);
        byte(0xFF);
        modrm(3, 2, r);
    }
    void call_mem(reg base, i32 disp) { op_rm_nw(0xFF, reg(2), base, disp); } // call qword [base+disp]
    void ret() { byte(0xC3); }

    // --- SSE2 (f64) ---------------------------------------------------------------------

    void movsd_load(xmm dst, reg base, i32 disp) { sse_rm(0xF2, 0x10, dst, base, disp); }
    void movsd_store(reg base, i32 disp, xmm src) { sse_rm(0xF2, 0x11, src, base, disp); }
    void movsd_load_idx(xmm dst, reg base, reg index) { sse_sib(0xF2, 0x10, dst, base, index); }
    void movsd_store_idx(reg base, reg index, xmm src) { sse_sib(0xF2, 0x11, src, base, index); }
    void movsd(xmm dst, xmm src) { sse_rr(0xF2, 0x10, dst, src); }
    void addsd(xmm dst, xmm src) { sse_rr(0xF2, 0x58, dst, src); }
    void mulsd(xmm dst, xmm src) { sse_rr(0xF2, 0x59, dst, src); }
    void subsd(xmm dst, xmm src) { sse_rr(0xF2, 0x5C, dst, src); }
    void divsd(xmm dst, xmm src) { sse_rr(0xF2, 0x5E, dst, src); }
    void ucomisd(xmm a_, xmm b_) { sse_rr(0x66, 0x2E, a_, b_); }
    void xorpd(xmm dst, xmm src) { sse_rr(0x66, 0x57, dst, src); }
    void movq(xmm dst, reg src) { // 66 REX.W 0F 6E /r
        byte(0x66);
        rex(true, dst, 0, src);
        byte(0x0F);
        byte(0x6E);
        modrm(3, dst, src);
    }
    void movq(reg dst, xmm src) { // 66 REX.W 0F 7E /r
        byte(0x66);
        rex(true, src, 0, dst);
        byte(0x0F);
        byte(0x7E);
        modrm(3, src, dst);
    }
    void cvtsi2sd(xmm dst, reg src) {
        byte(0xF2);
        rex(true, dst, 0, src);
        byte(0x0F);
        byte(0x2A);
        modrm(3, dst, src);
    }
    void cvttsd2si(reg dst, xmm src) {
        byte(0xF2);
        rex(true, dst, 0, src);
        byte(0x0F);
        byte(0x2C);
        modrm(3, dst, src);
    }

    // --- crossings ---

    void jmp(label& l) {
        byte(0xE9);
        rel32(l);
    }
    void jcc(cond c, label& l) {
        byte(0x0F);
        byte(0x80 + c);
        rel32(l);
    }
    void bind(label& l) {
        l.pos_ = i64(buf_.size());
        for (u32 at : l.fixups_) patch32(at, i32(l.pos_ - i64(at + 4)));
        l.fixups_.clear();
    }

    u32  here() const { return u32(buf_.size()); }
    void patch32(u32 at, i32 v) { std::memcpy(buf_.data() + at, &v, 4); }
    void i32le(i32 v) { for (int k = 0; k < 4; ++k) byte(u8(u32(v) >> (8 * k))); }

    // The finished executable block (W^X).
    code_block finalize() {
        std::size_t size = (buf_.size() + 4095) & ~std::size_t(4095);
        void*       mem  = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED) throw diagnostic("JIT: не удалось выделить память кода", {});
        std::memcpy(mem, buf_.data(), buf_.size());
        if (mprotect(mem, size, PROT_READ | PROT_EXEC) != 0) {
            munmap(mem, size);
            throw diagnostic("JIT: не удалось сделать код исполняемым", {});
        }
        return code_block(mem, size);
    }

    std::size_t size() const { return buf_.size(); }

private:
    void byte(u8 b) { buf_.push_back(b); }
    void u64le(u64 v) { for (int k = 0; k < 8; ++k) byte(u8(v >> (8 * k))); }

    void rex(bool w, u8 r, u8 x, u8 b_) {
        u8 v = 0x40 | (w ? 8 : 0) | ((r >> 3) << 2) | ((x >> 3) << 1) | (b_ >> 3);
        if (v != 0x40) byte(v);
    }
    void modrm(u8 mod, u8 r, u8 rm) { byte(u8((mod << 6) | ((r & 7) << 3) | (rm & 7))); }

    // [base + disp32]; rsp/r12 requires SIB
    void mem(u8 r, reg base, i32 disp) {
        modrm(2, r, base);
        if ((base & 7) == 4) byte(0x24);
        i32le(disp);
    }
    void op_rr(u8 op, reg r, reg rm) {
        rex(true, r, 0, rm);
        byte(op);
        modrm(3, r, rm);
    }
    void op_rm(u8 op, reg r, reg base, i32 disp) {
        rex(true, r, 0, base);
        byte(op);
        mem(r, base, disp);
    }
    void op_rm_nw(u8 op, reg r, reg base, i32 disp) {
        rex(false, r, 0, base);
        byte(op);
        mem(r, base, disp);
    }
    // [base + index*8]
    void op_sib(u8 op, reg r, reg base, reg index) {
        rex(true, r, index, base);
        byte(op);
        modrm(0, r, 4);
        byte(u8((3 << 6) | ((index & 7) << 3) | (base & 7)));
        if ((base & 7) == 5) throw diagnostic("JIT: rbp/r13 как база SIB без смещения не поддержан", {});
    }
    void unary_f7(u8 ext, reg r) {
        rex(true, 0, 0, r);
        byte(0xF7);
        modrm(3, ext, r);
    }
    void sse_rr(u8 prefix, u8 op, u8 r, u8 rm) {
        byte(prefix);
        rex(false, r, 0, rm);
        byte(0x0F);
        byte(op);
        modrm(3, r, rm);
    }
    void sse_rm(u8 prefix, u8 op, u8 r, reg base, i32 disp) {
        byte(prefix);
        rex(false, r, 0, base);
        byte(0x0F);
        byte(op);
        mem(r, base, disp);
    }
    void sse_sib(u8 prefix, u8 op, u8 r, reg base, reg index) {
        byte(prefix);
        rex(false, r, index, base);
        byte(0x0F);
        byte(op);
        modrm(0, r, 4);
        byte(u8((3 << 6) | ((index & 7) << 3) | (base & 7)));
        if ((base & 7) == 5) throw diagnostic("JIT: rbp/r13 как база SIB без смещения не поддержан", {});
    }
    void rel32(label& l) {
        u32 at = here();
        i32le(0);
        if (l.bound()) patch32(at, i32(l.pos_ - i64(at + 4)));
        else l.fixups_.push_back(at);
    }

    std::vector<u8> buf_;
};

} // namespace tz::x64
