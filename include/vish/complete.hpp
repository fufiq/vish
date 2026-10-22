// Addition: list option (§13.1, M5). Collection and selection - in the language (complete editor module,
// lib/editor.vish); here is the option as it stores the presentation and sends the protocol.
//
// 
// 
// 
// 
// Therefore, the context is the path from the cursor's place up the AST: the block
// gives the declared to the place, the function and the block of construction -
// parameters, module and prelude - their entity.
//
// The list is alive: nothing is
// inserted by itself.
//
//
#pragma once

#include "ast.hpp"
#include "interp.hpp"

#include <algorithm>

namespace vish {

struct completion_item {
  std::string name;
  std::string kind; // variable, parameter, function, type
  std::string detail;
  uint32_t depth = 0; // closer to place, less
};

} // namespace vish
