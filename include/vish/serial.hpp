// Binary serialization is one implementation for all aggregates through reflection.
//
// : on the disk - binar, text - projection when reading (vish log).
//
#pragma once

#include "base.hpp"

#include <optional>
#include <variant>

namespace vish {

template<typename T>
struct is_vector : std::false_type {};
template<typename T>
struct is_vector<std::vector<T>> : std::true_type {};
template<typename T>
struct is_variant : std::false_type {};
template<typename... T>
struct is_variant<std::variant<T...>> : std::true_type {};
template<typename T>
struct is_id : std::false_type {};
template<typename T>
struct is_id<id<T>> : std::true_type {};

struct writer {
  void put(auto const & x) {
    using T = std::remove_cvref_t<decltype(x)>;
    if constexpr(std::is_arithmetic_v<T> || std::is_enum_v<T>) {
      raw(&x, sizeof x);
    } else if constexpr(is_id<T>::value) {
      put(x.value);
    } else if constexpr(std::is_same_v<T, std::string>) {
      put(uint32_t(x.size()));
      raw(x.data(), x.size());
    } else if constexpr(is_vector<T>::value) {
      put(uint32_t(x.size()));
      for(auto const & e: x) put(e);
    } else if constexpr(is_variant<T>::value) {
      put(uint32_t(x.index()));
      std::visit([&](auto const & alt) { put(alt); }, x);
    } else {
      static_assert(std::is_aggregate_v<T>, "сериализуются агрегаты");
      template for(constexpr auto m: fields_of<T>()) put(x.[:m:]);
    }
  }

  void raw(void const * p, std::size_t n) { bytes.append(static_cast<char const *>(p), n); }

  std::string bytes;
};

struct reader {
  explicit reader(std::string_view in) : in(in) {}

  template<typename T>
  T get() {
    if constexpr(std::is_arithmetic_v<T> || std::is_enum_v<T>) {
      T x;
      raw(&x, sizeof x);
      return x;
    } else if constexpr(is_id<T>::value) {
      return T(get<uint32_t>());
    } else if constexpr(std::is_same_v<T, std::string>) {
      uint32_t n = get<uint32_t>();
      need(n);
      std::string s(in.substr(pos, n));
      pos += n;
      return s;
    } else if constexpr(is_vector<T>::value) {
      uint32_t n = get<uint32_t>();
      T v;
      v.reserve(n);
      for(uint32_t i = 0; i < n; ++i) v.push_back(get<typename T::value_type>());
      return v;
    } else if constexpr(is_variant<T>::value) {
      return get_variant<T>(get<uint32_t>(), std::make_index_sequence<std::variant_size_v<T>>{});
    } else {
      static_assert(std::is_aggregate_v<T>, "сериализуются агрегаты");
      T x{};
      template for(constexpr auto m: fields_of<T>()) x.[:m:] = get<std::remove_cvref_t<decltype(x.[:m:])>>();
      return x;
    }
  }

  bool at_end() const { return pos >= in.size(); }
  template<typename V, std::size_t... I>
  V get_variant(uint32_t index, std::index_sequence<I...>) {
    std::optional<V> out;
    ((index == I ? (out.emplace(std::in_place_index<I>, get<std::variant_alternative_t<I, V>>()), 0) : 0), ...);
    if(!out) throw diagnostic(cat("журнал повреждён: вид ", index, " неизвестен"), {});
    return std::move(*out);
  }

  void need(std::size_t n) const {
    if(pos + n > in.size()) throw diagnostic("журнал повреждён: неожиданный конец", {});
  }

  void raw(void * p, std::size_t n) {
    need(n);
    std::memcpy(p, in.data() + pos, n);
    pos += n;
  }

  std::string_view in;
  std::size_t pos = 0;
};

} // namespace vish
