// Binary serialization is one implementation for all aggregates through reflection.
//
// : on the disk - binar, text - projection when reading (tz log).
//
#pragma once

#include "base.hpp"

#include <optional>
#include <variant>

namespace tz {

template <class T> struct is_vector : std::false_type {};
template <class T> struct is_vector<std::vector<T>> : std::true_type {};
template <class T> struct is_variant : std::false_type {};
template <class... T> struct is_variant<std::variant<T...>> : std::true_type {};
template <class T> struct is_id : std::false_type {};
template <class T> struct is_id<id<T>> : std::true_type {};

class writer {
public:
    template <class T>
    void put(const T& x) {
        if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>) {
            raw(&x, sizeof x);
        } else if constexpr (is_id<T>::value) {
            put(x.value());
        } else if constexpr (std::is_same_v<T, std::string>) {
            put(u32(x.size()));
            raw(x.data(), x.size());
        } else if constexpr (is_vector<T>::value) {
            put(u32(x.size()));
            for (const auto& e : x) put(e);
        } else if constexpr (is_variant<T>::value) {
            put(u32(x.index()));
            std::visit([&](const auto& alt) { put(alt); }, x);
        } else {
            static_assert(std::is_aggregate_v<T>, "сериализуются агрегаты");
            template for (constexpr auto m : fields_of<T>()) put(x.[:m:]);
        }
    }

    const std::string& bytes() const { return out_; }

private:
    void raw(const void* p, std::size_t n) { out_.append(static_cast<const char*>(p), n); }

    std::string out_;
};

class reader {
public:
    explicit reader(std::string_view in) : in_(in) {}

    template <class T>
    T get() {
        if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>) {
            T x;
            raw(&x, sizeof x);
            return x;
        } else if constexpr (is_id<T>::value) {
            return T(get<u32>());
        } else if constexpr (std::is_same_v<T, std::string>) {
            u32 n = get<u32>();
            need(n);
            std::string s(in_.substr(pos_, n));
            pos_ += n;
            return s;
        } else if constexpr (is_vector<T>::value) {
            u32 n = get<u32>();
            T   v;
            v.reserve(n);
            for (u32 i = 0; i < n; ++i) v.push_back(get<typename T::value_type>());
            return v;
        } else if constexpr (is_variant<T>::value) {
            return get_variant<T>(get<u32>(), std::make_index_sequence<std::variant_size_v<T>>{});
        } else {
            static_assert(std::is_aggregate_v<T>, "сериализуются агрегаты");
            T x{};
            template for (constexpr auto m : fields_of<T>()) x.[:m:] = get<std::remove_cvref_t<decltype(x.[:m:])>>();
            return x;
        }
    }

    bool at_end() const { return pos_ >= in_.size(); }

private:
    template <class V, std::size_t... I>
    V get_variant(u32 index, std::index_sequence<I...>) {
        std::optional<V> out;
        ((index == I ? (out.emplace(std::in_place_index<I>, get<std::variant_alternative_t<I, V>>()), 0) : 0), ...);
        if (!out) throw diagnostic(std::format("журнал повреждён: вид {} неизвестен", index), {});
        return std::move(*out);
    }

    void need(std::size_t n) const {
        if (pos_ + n > in_.size()) throw diagnostic("журнал повреждён: неожиданный конец", {});
    }

    void raw(void* p, std::size_t n) {
        need(n);
        std::memcpy(p, in_.data() + pos_, n);
        pos_ += n;
    }

    std::string_view in_;
    std::size_t      pos_ = 0;
};

} // namespace tz
