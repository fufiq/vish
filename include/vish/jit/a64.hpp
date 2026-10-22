// AArch64 (§10.3 steak): Bytes of compiler code in the qemu validation language and word verification
// image. Encoding instructions - at the target in the language (arm64 prelude context):
// A goal is a context that redefines operations.
//
//
// Code for someone else's purpose is not executed on host: finalize gives bytes of the image.
#pragma once

#include "x64.hpp"

namespace vish::a64 {

struct image {
  std::vector<uint8_t> bytes;
  std::vector<std::pair<uint32_t, void const *>> relocs; // movz/movk ×4 ← address in image
};

struct assembler {
  static constexpr std::string_view target = "arm64"; // code generation context in language (§10.4)

  uint32_t here() const { return uint32_t(buf.size()); }
  std::size_t size() const { return buf.size(); }
  image finalize() { return {std::move(buf), std::move(relocs)}; }
  void byte(uint8_t b) { buf.push_back(b); } // byte
  void word(uint32_t w) {
    for(int k = 0; k < 4; ++k) byte(uint8_t(w >> (8 * k)));
  }

  std::vector<uint8_t> buf;
  std::vector<std::pair<uint32_t, void const *>> relocs;
};

} // namespace vish::a64
