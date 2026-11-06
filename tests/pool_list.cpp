// Pool invariants node (S5.0): list and text by base offsets; runs check.sh.
#include "vish/pool.hpp"
#include <cassert>
#include <cstdio>
using namespace vish;
int main() {
  node_pool a, b;
  pool_scope sa(&a);
  static_assert(std::is_trivially_copyable_v<list<uint32_t>> && std::is_trivially_copyable_v<node_text>);
  static_assert(sizeof(list<uint32_t>) == 8 && sizeof(node_text) == 8);
  list<uint32_t> xs;
  for(uint32_t i = 0; i < 1000; ++i) xs.push_back(i);
  assert(xs.size() == 1000 && xs[999] == 999 && xs.back() == 999 && xs.front() == 0);
  list<uint32_t> alias = xs; // copy of the descriptor - same list
  xs.erase(xs.begin() + 10);
  assert(xs.size() == 999 && xs[10] == 11);
  xs.insert(xs.begin() + 10, 10u);
  assert(xs.size() == 1000 && xs[10] == 10 && xs[11] == 11);
  uint32_t ys[] = {7, 8, 9};
  xs.insert(xs.begin() + 1, ys, ys + 3);
  assert(xs.size() == 1003 && xs[1] == 7 && xs[3] == 9 && xs[4] == 1);
  list<uint32_t> copy = xs.clone();
  xs.clear();
  assert(xs.empty() && copy.size() == 1003 && copy[1] == 7);
  list<uint32_t> init = {1, 2, 3};
  std::vector<uint32_t> v = init.vec();
  assert(v.size() == 3 && v[2] == 3 && init == list<uint32_t>(v));
  init.resize(5, 4u);
  assert(init.size() == 5 && init[4] == 4);
  // Free pieces are reused: the growth of one list does not inflate the pool unnecessarily
  uint32_t before = a.words();
  for(int k = 0; k < 100; ++k) {
    list<uint32_t> t;
    for(uint32_t i = 0; i < 40; ++i) t.push_back(i);
    for(uint32_t i = 0; i < 40; ++i) t.pop_back();
    t.release();
  }
  assert(a.words() - before < 2048);
  node_text s = "привет";
  assert(s.view() == "привет" && s.size() == 12);
  s = std::string(5000, 'x');
  assert(s.size() == 5000 && s[4999] == 'x');
  s = "ab";
  assert(s.view() == "ab");
  node_text t2 = s; // same-word
  assert(t2 == s);
  { // two bases: the descriptor is read through the current
    pool_scope sb(&b);
    list<uint32_t> other = {5, 6};
    assert(other[1] == 6);
  }
  assert(xs.empty() && copy.size() == 1003);
  std::puts("pool ok");
}
