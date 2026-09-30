// Verification of AArch64 code in parallel with execution in host (M5, D8).
//
// 
// The standard here is the host execution (interpreter and x64, verified by tests). Each
// instance call that goes into the host machine code is compiled for AArch64, the code is
// collected into a bare static ELF (without libc: arguments, call, output, error tags and
// arrays by system call) and executed in qemu-aarch64; the result is compared with host.
// *My:* emulator is only the performer for correctness; the speed under it is not measured.
//
//
//
#pragma once

#include "a64.hpp"

#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

namespace tz {

// Ready code of someone else's target: bytes and addresses of host - in the image.
void finish(jit_fn & out, a64::assembler & a) {
  auto img = a.finalize();
  out.image = std::move(img.bytes);
  out.relocs = std::move(img.relocs);
  out.entry = nullptr; // host is not executed
}

struct a64_check {
  a64_check(ast_db & db, node_id module) : eng(db, module) {}

  // The result of the call in qemu: result bits, error marks, arrays after the call.
  struct outcome {
    bool ran = false;
    std::string why; // why not tested
    uint64_t bits = 0, fault = 0, node = 0, thrown = 0, thrown_type = 0;
    std::vector<std::vector<uint64_t>> arrays;
  };

  // Argument: scalar (bits) or array (bits of elements).
  struct arg {
    uint64_t bits = 0;
    bool array = false;
    std::vector<uint64_t> items;
    int same_as = -1; // the same array as the same_as argument (alias: f(w, w))
  };

  outcome run(node_id f, std::vector<jt> const & types, std::vector<arg> const & args) {
    outcome r;
    for(jt t: types)
      if(is_stream(t)) {
        r.why = "поток — операция VM";
        return r;
      }
    jit_fn * code = eng.get(f, types);
    if(!code) {
      r.why = "AArch64 не скомпилировал";
      return r;
    }
    std::string path = cat("/tmp/tz-a64-", getpid(), ".elf");
    std::vector<uint8_t> elf = build(*code, args);
    if(FILE * out = fopen(path.c_str(), "wb")) {
      fwrite(elf.data(), 1, elf.size(), out);
      fclose(out);
    } else {
      r.why = "не записать образ";
      return r;
    }
    chmod(path.c_str(), 0755);
    FILE * p = popen(cat("qemu-aarch64 ", path).c_str(), "r");
    if(!p) {
      r.why = "нет qemu-aarch64";
      return r;
    }
    std::vector<uint64_t> words;
    uint64_t w;
    while(fread(&w, 8, 1, p) == 1) words.push_back(w);
    int status = pclose(p);
    unlink(path.c_str());
    std::size_t need = 5;
    for(auto const & a: args) need += a.array && a.same_as < 0 ? a.items.size() : 0;
    if(status != 0 || words.size() != need) {
      r.why = cat("образ не отработал (код ", status, ", слов ", words.size(), " из ", need, ")");
      return r;
    }
    r.ran = true;
    r.bits = words[0];
    r.fault = words[1];
    r.node = words[2];
    r.thrown = words[3];
    r.thrown_type = words[4];
    std::size_t at = 5;
    for(auto const & a: args) {
      if(!a.array || a.same_as >= 0) {
        r.arrays.emplace_back(); // alias is the array of its first
        continue;
      }
      r.arrays.emplace_back(words.begin() + std::ptrdiff_t(at), words.begin() + std::ptrdiff_t(at + a.items.size()));
      at += a.items.size();
    }
    for(std::size_t i = 0; i < args.size(); ++i)
      if(args[i].same_as >= 0) r.arrays[i] = r.arrays[std::size_t(args[i].same_as)];
    return r;
  }

  // Image: ELF64 AArch64, one RWX segment from base address. Code: start, VM assistant plugs,
  // instance; data: error tags, entry cells, arguments, arrays.
  std::vector<uint8_t> build(jit_fn const & main_fn, std::vector<arg> const & args) {
    constexpr uint64_t base = 0x400000, code_at = 0x100;
    a64::assembler c; // start and stubs - the same operations
    std::vector<uint8_t> data;
    auto data_word = [&](uint64_t v) {
      uint64_t off = data.size();
      for(int k = 0; k < 8; ++k) data.push_back(uint8_t(v >> (8 * k)));
      return off;
    };
    // Data: results and error tags in a row - they are displayed by one write
    uint64_t d_result = data_word(0), d_fault = data_word(0), d_node = data_word(0), d_thrown = data_word(0), d_thrown_type = data_word(0);
    std::vector<uint64_t> d_cells;
    for(std::size_t i = 0; i < eng.fns.size(); ++i) d_cells.push_back(data_word(0));
    std::vector<uint64_t> d_args, d_arrays; // for array: header {данные, размер} and data
    // Compiler code in language: array in arguments - {данные, размер}, behind them - error slots
    bool lang = main_fn.by_lang;
    uint64_t d_lang_fault = 0;
    for(auto const & a: args) {
      d_args.push_back(data_word(0));
      if(lang && a.array) data_word(0);
    }
    if(lang) { // type of error, node, thrown value, its type
      d_lang_fault = data_word(0);
      for(int k = 0; k < 3; ++k) data_word(0);
    }
    std::vector<uint64_t> d_headers;
    for(auto const & a: args) {
      if(!a.array || a.same_as >= 0) {
        d_headers.push_back(a.array ? d_headers[std::size_t(a.same_as)] : 0);
        d_arrays.push_back(a.array ? d_arrays[std::size_t(a.same_as)] : 0);
        continue;
      }
      d_headers.push_back(data_word(0));
      data_word(a.items.size());
      d_arrays.push_back(data.size());
      for(uint64_t v: a.items) data_word(v);
    }

    // code: first dimensions, then addresses (two-pass layout)
    auto layout = [&](uint64_t data_addr, std::vector<uint64_t> & fn_addr, uint64_t & stubs) {
      c = a64::assembler{};
      // Addresses - always movz + 3 movk: code size does not depend on layout
      auto imm = [&](uint32_t r, uint64_t v) {
        for(uint32_t k = 0; k < 4; ++k) c.word((k ? 0xF2800000 : 0xD2800000) | k << 21 | uint32_t(v >> (16 * k) & 0xFFFF) << 5 | r);
      };
      auto at = [&](uint64_t off) { return base + data_addr + off; };
      // start: x0 - Array of arguments; cell call; output; output
      imm(0, at(d_args.empty() ? 0 : d_args[0]));
      imm(16, at(d_cells[index_of(main_fn)]));
      c.word(0xF8400210); // ldr x16, [x16]
      c.word(0xD63F0200); // blr x16
      imm(17, at(d_result));
      c.word(0xF8000220); // stur x0, [x17]
      if(lang) // Error slots from the field of arguments - in image tags (they are compared with host)
        for(uint64_t k = 0; k < 4; ++k) {
          imm(16, at(d_lang_fault + 8 * k));
          c.word(0xF8400211); // ldur x17, [x16]
          imm(16, at(k == 0 ? d_fault : k == 1 ? d_node : k == 2 ? d_thrown : d_thrown_type));
          c.word(0xF8000211); // stur x17, [x16]
        }
      auto write = [&](uint64_t addr, uint64_t bytes) {
        imm(0, 1);
        imm(1, addr);
        imm(2, bytes);
        imm(8, 64);
        c.word(0xD4000001); // svc #0
      };
      write(at(d_result), 40);
      for(std::size_t i = 0; i < args.size(); ++i)
        if(args[i].array && args[i].same_as < 0 && !args[i].items.empty()) write(at(d_arrays[i]), 8 * args[i].items.size());
      imm(0, 0);
      imm(8, 93);
      c.word(0xD4000001); // exit(0)
      stubs = c.here();
      c.word(0xF8400000); // jit_array_data: ldur x0, [x0]
      c.word(0xD65F03C0);
      c.word(0xF8408000); // jit_array_size: ldur x0, [x0, #8]
      c.word(0xD65F03C0);
      // jit_stream_call: new (op 0) и close (op 4) следа не оставляют (свёрнутый
      // generator: stream created and closed, number does not go anywhere) - nothing;
      // other - tag vm_error (unchecked)
      c.word(0xF100101F); // cmp x0, #4
      c.word(0x540001A0); // b.eq ok (+13)
      c.word(0xB4000180); // cbz x0, ok (+12)
      imm(16, at(d_fault));
      imm(17, uint64_t(jit_fault_kind::vm_error));
      c.word(0xF8000211); // stur x17, [x16]
      c.word(0xD2800000); // mov x0, #0
      c.word(0xD65F03C0); // ret
      c.word(0xD2800000); // ok: mov x0, #0
      c.word(0xD65F03C0); // ret
      fn_addr.clear();
      uint64_t pos = code_at + c.size();
      for(auto const & f: eng.fns) {
        pos = (pos + 15) & ~uint64_t(15);
        fn_addr.push_back(pos);
        pos += f.image.size();
      }
      return (pos + 15) & ~uint64_t(15); // beginning
    };
    std::vector<uint64_t> fn_addr;
    uint64_t stubs = 0;
    uint64_t data_addr = layout(0, fn_addr, stubs);
    data_addr = layout(data_addr, fn_addr, stubs); // start with correct addresses (same size)

    auto addr_of = [&](void const * p) -> uint64_t {
      auto at = [&](uint64_t off) { return base + data_addr + off; };
      if(p == &jit_fault) return at(d_fault);
      if(p == &jit_fault_node) return at(d_node);
      if(p == &jit_thrown) return at(d_thrown);
      if(p == &jit_thrown_type) return at(d_thrown_type);
      if(p == reinterpret_cast<void const *>(&jit_array_data)) return base + code_at + stubs;
      if(p == reinterpret_cast<void const *>(&jit_array_size)) return base + code_at + stubs + 8;
      if(p == reinterpret_cast<void const *>(&jit_stream_call)) return base + code_at + stubs + 16;
      for(std::size_t i = 0; i < eng.fns.size(); ++i)
        if(p == &eng.fns[i].entry) return at(d_cells[i]);
      throw diagnostic("AArch64: адрес хоста без места в образе", {});
    };
    // entry cells, arguments, array headings
    auto put = [&](uint64_t off, uint64_t v) {
      for(int k = 0; k < 8; ++k) data[off + k] = uint8_t(v >> (8 * k));
    };
    for(std::size_t i = 0; i < eng.fns.size(); ++i) put(d_cells[i], base + fn_addr[i]);
    for(std::size_t i = 0; i < args.size(); ++i) {
      if(!args[i].array) {
        put(d_args[i], args[i].bits);
        continue;
      }
      if(lang) { // {data, size}
        put(d_args[i], base + data_addr + d_arrays[i]);
        put(d_args[i] + 8, args[i].items.size());
      } else put(d_args[i], base + data_addr + d_headers[i]);
      if(args[i].same_as < 0) put(d_headers[i], args[i].items.empty() ? 8 : base + data_addr + d_arrays[i]);
    }

    std::vector<uint8_t> out(code_at, 0);
    out.insert(out.end(), c.buf.begin(), c.buf.end());
    for(std::size_t i = 0; i < eng.fns.size(); ++i) {
      out.resize(fn_addr[i], 0);
      std::size_t start = out.size();
      out.insert(out.end(), eng.fns[i].image.begin(), eng.fns[i].image.end());
      for(auto const & [pos, host]: eng.fns[i].relocs) { // Moviz / Movik x4 - Image address
        uint64_t v = addr_of(host);
        for(uint32_t k = 0; k < 4; ++k) {
          std::size_t w = start + pos + 4 * k;
          uint32_t ins = uint32_t(out[w]) | uint32_t(out[w + 1]) << 8 | uint32_t(out[w + 2]) << 16 | uint32_t(out[w + 3]) << 24;
          ins |= uint32_t(v >> (16 * k) & 0xFFFF) << 5;
          for(int b = 0; b < 4; ++b) out[w + b] = uint8_t(ins >> (8 * b));
        }
      }
    }
    out.resize(data_addr, 0);
    out.insert(out.end(), data.begin(), data.end());

    // headlines
    auto le = [&](std::size_t off, uint64_t v, int n) {
      for(int k = 0; k < n; ++k) out[off + k] = uint8_t(v >> (8 * k));
    };
    uint8_t ident[16] = {0x7F, 'E', 'L', 'F', 2, 1, 1};
    std::memcpy(out.data(), ident, 16);
    le(16, 2, 2); // ET_EXEC
    le(18, 183, 2); // EM_AARCH64
    le(20, 1, 4);
    le(24, base + code_at, 8); // entrance - start
    le(32, 64, 8); // program headers
    le(52, 64, 2);
    le(54, 56, 2);
    le(56, 1, 2);
    le(64, 1, 4); // PT_LOAD
    le(68, 7, 4); // RWX
    le(72, 0, 8);
    le(80, base, 8);
    le(88, base, 8);
    le(96, out.size(), 8);
    le(104, out.size(), 8);
    le(112, 0x1000, 8);
    return out;
  }

  // Comparison with host immediately after its call (error marks have not yet been
  // reset, arrays after the call).
  void compare(outcome const & a, std::vector<arg> const & args, std::vector<uint64_t> const & raw, jt result, uint64_t bits, std::string const & what) {
    if(!a.ran) {
      ++skipped;
      notes.insert(cat(what, ": ", a.why));
      return;
    }
    if(a.fault == uint64_t(jit_fault_kind::vm_error)) { // operation VM (flow) - stub
      ++skipped;
      notes.insert(cat(what, ": операция VM"));
      return;
    }
    ++checked;
    std::string diff;
    if(a.fault != jit_fault) diff += cat(" ошибка ", a.fault, " против ", jit_fault, ";");
    else if(jit_fault && a.node != jit_fault_node) diff += cat(" место ошибки #", a.node, " против #", jit_fault_node, ";");
    else if(jit_fault == uint64_t(jit_fault_kind::thrown) && (a.thrown != jit_thrown || a.thrown_type != jit_thrown_type)) diff += " брошенное значение;";
    else if(!jit_fault && result != jt::unit && a.bits != bits) diff += cat(" результат ", a.bits, " против ", bits, ";");
    for(std::size_t i = 0; i < args.size(); ++i)
      if(args[i].array) {
        auto const * host = reinterpret_cast<array_v const *>(raw[i]);
        std::size_t k = i;
        for(uint32_t j = 0; j < host->size() && j < a.arrays[k].size(); ++j) {
          value e = host->get(j);
          uint64_t h = e.is<double>() ? std::bit_cast<uint64_t>(e.as<double>()) : uint64_t(e.as<int64_t>());
          if(h != a.arrays[k][j]) {
            diff += cat(" массив ", i, "[", j, "];");
            break;
          }
        }
      }
    if(!diff.empty()) mismatches.push_back(cat(what, ":", diff));
  }

  std::string report() const {
    std::string out = cat("AArch64: проверено вызовов ", checked, ", расхождений ", mismatches.size(), ", не проверено ", skipped, "\n");
    for(auto const & m: mismatches) out += cat("  расхождение  ", m, "\n");
    for(auto const & n: notes) out += cat("  не проверено  ", n, "\n");
    return out;
  }

  std::size_t index_of(jit_fn const & f) const {
    for(std::size_t i = 0; i < eng.fns.size(); ++i)
      if(&eng.fns[i] == &f) return i;
    throw diagnostic("AArch64: экземпляр не из движка проверки", {});
  }

  jit_machine<a64::assembler> eng;
  std::map<std::string, uint32_t> calls; // verified instance calls
  uint32_t checked = 0, skipped = 0;
  std::vector<std::string> mismatches;
  std::set<std::string> notes;
};

} // namespace tz
