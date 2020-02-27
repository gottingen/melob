//

#ifndef ABEL_CONTAINER_INTERNAL_CONTAINER_MEMORY_H_
#define ABEL_CONTAINER_INTERNAL_CONTAINER_MEMORY_H_

#ifdef ADDRESS_SANITIZER
#include <sanitizer/asan_interface.h>
#endif

#ifdef MEMORY_SANITIZER
#include <sanitizer/msan_interface.h>
#endif

#include <cassert>
#include <cstddef>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

#include <abel/memory/memory.h>
#include <abel/utility/utility.h>

namespace abel {

    namespace container_internal {

// Allocates at least n bytes aligned to the specified alignment.
// Alignment must be a power of 2. It must be positive.
//
// Note that many allocators don't honor alignment requirements above certain
// threshold (usually either alignof(std::max_align_t) or alignof(void*)).
// Allocate() doesn't apply alignment corrections. If the underlying allocator
// returns insufficiently alignment pointer, that's what you are going to get.
        template<size_t Alignment, class Alloc>
        void *Allocate(Alloc *alloc, size_t n) {
            static_assert(Alignment > 0, "");
            assert(n && "n must be positive");
            struct alignas(Alignment) M {
            };
            using A = typename abel::allocator_traits<Alloc>::template rebind_alloc<M>;
            using AT = typename abel::allocator_traits<Alloc>::template rebind_traits<M>;
            A mem_alloc(*alloc);
            void *p = AT::allocate(mem_alloc, (n + sizeof(M) - 1) / sizeof(M));
            assert(reinterpret_cast<uintptr_t>(p) % Alignment == 0 &&
                   "allocator does not respect alignment");
            return p;
        }

// The pointer must have been previously obtained by calling
// Allocate<Alignment>(alloc, n).
        template<size_t Alignment, class Alloc>
        void Deallocate(Alloc *alloc, void *p, size_t n) {
            static_assert(Alignment > 0, "");
            assert(n && "n must be positive");
            struct alignas(Alignment) M {
            };
            using A = typename abel::allocator_traits<Alloc>::template rebind_alloc<M>;
            using AT = typename abel::allocator_traits<Alloc>::template rebind_traits<M>;
            A mem_alloc(*alloc);
            AT::deallocate(mem_alloc, static_cast<M *>(p),
                           (n + sizeof(M) - 1) / sizeof(M));
        }

        namespace memory_internal {

// Constructs T into uninitialized storage pointed by `ptr` using the args
// specified in the tuple.
            template<class Alloc, class T, class Tuple, size_t... I>
            void ConstructFromTupleImpl(Alloc *alloc, T *ptr, Tuple &&t,
                                        abel::index_sequence<I...>) {
                abel::allocator_traits<Alloc>::construct(
                        *alloc, ptr, std::get<I>(std::forward<Tuple>(t))...);
            }

            template<class T, class F>
            struct WithConstructedImplF {
                template<class... Args>
                decltype(std::declval<F>()(std::declval<T>())) operator()(
                        Args &&... args) const {
                    return std::forward<F>(f)(T(std::forward<Args>(args)...));
                }

                F &&f;
            };

            template<class T, class Tuple, size_t... Is, class F>
            decltype(std::declval<F>()(std::declval<T>())) WithConstructedImpl(
                    Tuple &&t, abel::index_sequence<Is...>, F &&f) {
                return WithConstructedImplF<T, F>{std::forward<F>(f)}(
                        std::get<Is>(std::forward<Tuple>(t))...);
            }

            template<class T, size_t... Is>
            auto TupleRefImpl(T &&t, abel::index_sequence<Is...>)
            -> decltype(std::forward_as_tuple(std::get<Is>(std::forward<T>(t))...)) {
                return std::forward_as_tuple(std::get<Is>(std::forward<T>(t))...);
            }

// Returns a tuple of references to the elements of the input tuple. T must be a
// tuple.
            template<class T>
            auto TupleRef(T &&t) -> decltype(
            TupleRefImpl(std::forward<T>(t),
                         abel::make_index_sequence<
                                 std::tuple_size<typename std::decay<T>::type>::value>())) {
                return TupleRefImpl(
                        std::forward<T>(t),
                        abel::make_index_sequence<
                                std::tuple_size<typename std::decay<T>::type>::value>());
            }

            template<class F, class K, class V>
            decltype(std::declval<F>()(std::declval<const K &>(), std::piecewise_construct,
                                       std::declval<std::tuple<K>>(), std::declval<V>()))
            DecomposePairImpl(F &&f, std::pair<std::tuple<K>, V> p) {
                const auto &key = std::get<0>(p.first);
                return std::forward<F>(f)(key, std::piecewise_construct, std::move(p.first),
                                          std::move(p.second));
            }

        }  // namespace memory_internal

// Constructs T into uninitialized storage pointed by `ptr` using the args
// specified in the tuple.
        template<class Alloc, class T, class Tuple>
        void ConstructFromTuple(Alloc *alloc, T *ptr, Tuple &&t) {
            memory_internal::ConstructFromTupleImpl(
                    alloc, ptr, std::forward<Tuple>(t),
                    abel::make_index_sequence<
                            std::tuple_size<typename std::decay<Tuple>::type>::value>());
        }

// Constructs T using the args specified in the tuple and calls F with the
// constructed value.
        template<class T, class Tuple, class F>
        decltype(std::declval<F>()(std::declval<T>())) WithConstructed(
                Tuple &&t, F &&f) {
            return memory_internal::WithConstructedImpl<T>(
                    std::forward<Tuple>(t),
                    abel::make_index_sequence<
                            std::tuple_size<typename std::decay<Tuple>::type>::value>(),
                    std::forward<F>(f));
        }

// Given arguments of an std::pair's consructor, PairArgs() returns a pair of
// tuples with references to the passed arguments. The tuples contain
// constructor arguments for the first and the second elements of the pair.
//
// The following two snippets are equivalent.
//
// 1. std::pair<F, S> p(args...);
//
// 2. auto a = PairArgs(args...);
//    std::pair<F, S> p(std::piecewise_construct,
//                      std::move(p.first), std::move(p.second));
        ABEL_FORCE_INLINE std::pair<std::tuple<>, std::tuple<>> PairArgs() { return {}; }

        template<class F, class S>
        std::pair<std::tuple<F &&>, std::tuple<S &&>> PairArgs(F &&f, S &&s) {
            return {std::piecewise_construct, std::forward_as_tuple(std::forward<F>(f)),
                    std::forward_as_tuple(std::forward<S>(s))};
        }

        template<class F, class S>
        std::pair<std::tuple<const F &>, std::tuple<const S &>> PairArgs(
                const std::pair<F, S> &p) {
            return PairArgs(p.first, p.second);
        }

        template<class F, class S>
        std::pair<std::tuple<F &&>, std::tuple<S &&>> PairArgs(std::pair<F, S> &&p) {
            return PairArgs(std::forward<F>(p.first), std::forward<S>(p.second));
        }

        template<class F, class S>
        auto PairArgs(std::piecewise_construct_t, F &&f, S &&s)
        -> decltype(std::make_pair(memory_internal::TupleRef(std::forward<F>(f)),
                                   memory_internal::TupleRef(std::forward<S>(s)))) {
            return std::make_pair(memory_internal::TupleRef(std::forward<F>(f)),
                                  memory_internal::TupleRef(std::forward<S>(s)));
        }

// A helper function for implementing apply() in map policies.
        template<class F, class... Args>
        auto DecomposePair(F &&f, Args &&... args)
        -> decltype(memory_internal::DecomposePairImpl(
                std::forward<F>(f), PairArgs(std::forward<Args>(args)...))) {
            return memory_internal::DecomposePairImpl(
                    std::forward<F>(f), PairArgs(std::forward<Args>(args)...));
        }

// A helper function for implementing apply() in set policies.
        template<class F, class Arg>
        decltype(std::declval<F>()(std::declval<const Arg &>(), std::declval<Arg>()))
        DecomposeValue(F &&f, Arg &&arg) {
            const auto &key = arg;
            return std::forward<F>(f)(key, std::forward<Arg>(arg));
        }

// Helper functions for asan and msan.
        ABEL_FORCE_INLINE void SanitizerPoisonMemoryRegion(const void *m, size_t s) {
#ifdef ADDRESS_SANITIZER
            ASAN_POISON_MEMORY_REGION(m, s);
#endif
#ifdef MEMORY_SANITIZER
            __msan_poison(m, s);
#endif
            (void) m;
            (void) s;
        }

        ABEL_FORCE_INLINE void SanitizerUnpoisonMemoryRegion(const void *m, size_t s) {
#ifdef ADDRESS_SANITIZER
            ASAN_UNPOISON_MEMORY_REGION(m, s);
#endif
#ifdef MEMORY_SANITIZER
            __msan_unpoison(m, s);
#endif
            (void) m;
            (void) s;
        }

        template<typename T>
        ABEL_FORCE_INLINE void SanitizerPoisonObject(const T *object) {
            SanitizerPoisonMemoryRegion(object, sizeof(T));
        }

        template<typename T>
        ABEL_FORCE_INLINE void SanitizerUnpoisonObject(const T *object) {
            SanitizerUnpoisonMemoryRegion(object, sizeof(T));
        }

        namespace memory_internal {

// If Pair is a standard-layout type, OffsetOf<Pair>::kFirst and
// OffsetOf<Pair>::kSecond are equivalent to offsetof(Pair, first) and
// offsetof(Pair, second) respectively. Otherwise they are -1.
//
// The purpose of OffsetOf is to avoid calling offsetof() on non-standard-layout
// type, which is non-portable.
            template<class Pair, class = std::true_type>
            struct OffsetOf {
                static constexpr size_t kFirst = -1;
                static constexpr size_t kSecond = -1;
            };

            template<class Pair>
            struct OffsetOf<Pair, typename std::is_standard_layout<Pair>::type> {
                static constexpr size_t kFirst = offsetof(Pair, first);
                static constexpr size_t kSecond = offsetof(Pair, second);
            };

            template<class K, class V>
            struct IsLayoutCompatible {
            private:
                struct Pair {
                    K first;
                    V second;
                };

                // Is P layout-compatible with Pair?
                template<class P>
                static constexpr bool LayoutCompatible() {
                    return std::is_standard_layout<P>() && sizeof(P) == sizeof(Pair) &&
                           alignof(P) == alignof(Pair) &&
                           memory_internal::OffsetOf<P>::kFirst ==
                           memory_internal::OffsetOf<Pair>::kFirst &&
                           memory_internal::OffsetOf<P>::kSecond ==
                           memory_internal::OffsetOf<Pair>::kSecond;
                }

            public:
                // Whether pair<const K, V> and pair<K, V> are layout-compatible. If they are,
                // then it is safe to store them in a union and read from either.
                static constexpr bool value = std::is_standard_layout<K>() &&
                                              std::is_standard_layout<Pair>() &&
                                              memory_internal::OffsetOf<Pair>::kFirst == 0 &&
                                              LayoutCompatible<std::pair<K, V>>() &&
                                              LayoutCompatible<std::pair<const K, V>>();
            };

        }  // namespace memory_internal

// The internal storage type for key-value containers like flat_hash_map.
//
// It is convenient for the value_type of a flat_hash_map<K, V> to be
// pair<const K, V>; the "const K" prevents accidental modification of the key
// when dealing with the reference returned from find() and similar methods.
// However, this creates other problems; we want to be able to emplace(K, V)
// efficiently with move operations, and similarly be able to move a
// pair<K, V> in insert().
//
// The solution is this union, which aliases the const and non-const versions
// of the pair. This also allows flat_hash_map<const K, V> to work, even though
// that has the same efficiency issues with move in emplace() and insert() -
// but people do it anyway.
//
// If kMutableKeys is false, only the value member can be accessed.
//
// If kMutableKeys is true, key can be accessed through all slots while value
// and mutable_value must be accessed only via INITIALIZED slots. Slots are
// created and destroyed via mutable_value so that the key can be moved later.
//
// Accessing one of the union fields while the other is active is safe as
// long as they are layout-compatible, which is guaranteed by the definition of
// kMutableKeys. For C++11, the relevant section of the standard is
// https://timsong-cpp.github.io/cppwp/n3337/class.mem#19 (9.2.19)
        template<class K, class V>
        union map_slot_type {
            map_slot_type() {}

            ~map_slot_type() = delete;

            using value_type = std::pair<const K, V>;
            using mutable_value_type = std::pair<K, V>;

            value_type value;
            mutable_value_type mutable_value;
            K key;
        };

        template<class K, class V>
        struct map_slot_policy {
            using slot_type = map_slot_type<K, V>;
            using value_type = std::pair<const K, V>;
            using mutable_value_type = std::pair<K, V>;

        private:
            static void emplace(slot_type *slot) {
                // The construction of union doesn't do anything at runtime but it allows us
                // to access its members without violating aliasing rules.
                new(slot) slot_type;
            }
            // If pair<const K, V> and pair<K, V> are layout-compatible, we can accept one
            // or the other via slot_type. We are also free to access the key via
            // slot_type::key in this case.
            using kMutableKeys = memory_internal::IsLayoutCompatible<K, V>;

        public:
            static value_type &element(slot_type *slot) { return slot->value; }

            static const value_type &element(const slot_type *slot) {
                return slot->value;
            }

            static const K &key(const slot_type *slot) {
                return kMutableKeys::value ? slot->key : slot->value.first;
            }

            template<class Allocator, class... Args>
            static void construct(Allocator *alloc, slot_type *slot, Args &&... args) {
                emplace(slot);
                if (kMutableKeys::value) {
                    abel::allocator_traits<Allocator>::construct(*alloc, &slot->mutable_value,
                                                                 std::forward<Args>(args)...);
                } else {
                    abel::allocator_traits<Allocator>::construct(*alloc, &slot->value,
                                                                 std::forward<Args>(args)...);
                }
            }

            // Construct this slot by moving from another slot.
            template<class Allocator>
            static void construct(Allocator *alloc, slot_type *slot, slot_type *other) {
                emplace(slot);
                if (kMutableKeys::value) {
                    abel::allocator_traits<Allocator>::construct(
                            *alloc, &slot->mutable_value, std::move(other->mutable_value));
                } else {
                    abel::allocator_traits<Allocator>::construct(*alloc, &slot->value,
                                                                 std::move(other->value));
                }
            }

            template<class Allocator>
            static void destroy(Allocator *alloc, slot_type *slot) {
                if (kMutableKeys::value) {
                    abel::allocator_traits<Allocator>::destroy(*alloc, &slot->mutable_value);
                } else {
                    abel::allocator_traits<Allocator>::destroy(*alloc, &slot->value);
                }
            }

            template<class Allocator>
            static void transfer(Allocator *alloc, slot_type *new_slot,
                                 slot_type *old_slot) {
                emplace(new_slot);
                if (kMutableKeys::value) {
                    abel::allocator_traits<Allocator>::construct(
                            *alloc, &new_slot->mutable_value, std::move(old_slot->mutable_value));
                } else {
                    abel::allocator_traits<Allocator>::construct(*alloc, &new_slot->value,
                                                                 std::move(old_slot->value));
                }
                destroy(alloc, old_slot);
            }

            template<class Allocator>
            static void swap(Allocator *alloc, slot_type *a, slot_type *b) {
                if (kMutableKeys::value) {
                    using std::swap;
                    swap(a->mutable_value, b->mutable_value);
                } else {
                    value_type tmp = std::move(a->value);
                    abel::allocator_traits<Allocator>::destroy(*alloc, &a->value);
                    abel::allocator_traits<Allocator>::construct(*alloc, &a->value,
                                                                 std::move(b->value));
                    abel::allocator_traits<Allocator>::destroy(*alloc, &b->value);
                    abel::allocator_traits<Allocator>::construct(*alloc, &b->value,
                                                                 std::move(tmp));
                }
            }

            template<class Allocator>
            static void move(Allocator *alloc, slot_type *src, slot_type *dest) {
                if (kMutableKeys::value) {
                    dest->mutable_value = std::move(src->mutable_value);
                } else {
                    abel::allocator_traits<Allocator>::destroy(*alloc, &dest->value);
                    abel::allocator_traits<Allocator>::construct(*alloc, &dest->value,
                                                                 std::move(src->value));
                }
            }

            template<class Allocator>
            static void move(Allocator *alloc, slot_type *first, slot_type *last,
                             slot_type *result) {
                for (slot_type *src = first, *dest = result; src != last; ++src, ++dest)
                    move(alloc, src, dest);
            }
        };

    }  // namespace container_internal

}  // namespace abel

#endif  // ABEL_CONTAINER_INTERNAL_CONTAINER_MEMORY_H_
