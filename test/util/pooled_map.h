//
// Copyright (C) 2024 EA group inc.
// Author: Jeff.li lijippy@163.com
// All rights reserved.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
//


// Date: Sat Dec  3 13:11:32 CST 2016

#ifndef MUTIL_POOLED_MAP_H
#define MUTIL_POOLED_MAP_H

#include "melon/utility/single_threaded_pool.h"
#include <new>
#include <map>

namespace mutil {
namespace details {
template <class T1, size_t BLOCK_SIZE> class PooledAllocator;
}

// A drop-in replacement for std::map to improve insert/erase performance slightly
//
// When do use PooledMap?
//   A std::map with 10~100 elements. insert/erase performance will be slightly
//   improved. Performance of find() is unaffected.
// When do NOT use PooledMap?
//   When the std::map has less that 10 elements, PooledMap is probably slower
//   because it allocates BLOCK_SIZE memory at least. When the std::map has more than
//   100 elements, you should use mutil::FlatMap instead.

// insert/erase comparisons between several maps:
// [ value = 8 bytes ]
// Sequentially inserting 100 into FlatMap/std::map/mutil::PooledMap/mutil::hash_map takes 15/114/54/60
// Sequentially erasing 100 from FlatMap/std::map/mutil::PooledMap/mutil::hash_map takes 6/123/56/37
// Sequentially inserting 1000 into FlatMap/std::map/mutil::PooledMap/mutil::hash_map takes 9/92/56/54
// Sequentially erasing 1000 from FlatMap/std::map/mutil::PooledMap/mutil::hash_map takes 3/68/51/35
// Sequentially inserting 10000 into FlatMap/std::map/mutil::PooledMap/mutil::hash_map takes 10/99/63/54
// Sequentially erasing 10000 from FlatMap/std::map/mutil::PooledMap/mutil::hash_map takes 4/73/54/35
// [ value = 32 bytes ]
// Sequentially inserting 100 into FlatMap/std::map/mutil::PooledMap/mutil::hash_map takes 14/107/57/57
// Sequentially erasing 100 from FlatMap/std::map/mutil::PooledMap/mutil::hash_map takes 5/75/53/37
// Sequentially inserting 1000 into FlatMap/std::map/mutil::PooledMap/mutil::hash_map takes 13/94/55/53
// Sequentially erasing 1000 from FlatMap/std::map/mutil::PooledMap/mutil::hash_map takes 4/67/50/37
// Sequentially inserting 10000 into FlatMap/std::map/mutil::PooledMap/mutil::hash_map takes 13/102/63/54
// Sequentially erasing 10000 from FlatMap/std::map/mutil::PooledMap/mutil::hash_map takes 4/69/53/36
// [ value = 128 bytes ]
// Sequentially inserting 100 into FlatMap/std::map/mutil::PooledMap/mutil::hash_map takes 35/160/96/98
// Sequentially erasing 100 from FlatMap/std::map/mutil::PooledMap/mutil::hash_map takes 7/96/53/42
// Sequentially inserting 1000 into FlatMap/std::map/mutil::PooledMap/mutil::hash_map takes 30/159/98/98
// Sequentially erasing 1000 from FlatMap/std::map/mutil::PooledMap/mutil::hash_map takes 6/82/49/43
// Sequentially inserting 10000 into FlatMap/std::map/mutil::PooledMap/mutil::hash_map takes 29/155/114/116
// Sequentially erasing 10000 from FlatMap/std::map/mutil::PooledMap/mutil::hash_map takes 6/81/53/43

// [ value = 8 bytes ]
// Randomly inserting 100 into FlatMap/std::map/mutil::PooledMap/mutil::hash_map takes 13/168/103/59
// Randomly erasing 100 from FlatMap/std::map/mutil::PooledMap/mutil::hash_map takes 6/159/125/37
// Randomly inserting 1000 into FlatMap/std::map/mutil::PooledMap/mutil::hash_map takes 10/157/115/54
// Randomly erasing 1000 from FlatMap/std::map/mutil::PooledMap/mutil::hash_map takes 4/175/138/36
// Randomly inserting 10000 into FlatMap/std::map/mutil::PooledMap/mutil::hash_map takes 11/219/177/56
// Randomly erasing 10000 from FlatMap/std::map/mutil::PooledMap/mutil::hash_map takes 4/229/207/47
// [ value = 32 bytes ]
// Randomly inserting 100 into FlatMap/std::map/mutil::PooledMap/mutil::hash_map takes 17/178/112/57
// Randomly erasing 100 from FlatMap/std::map/mutil::PooledMap/mutil::hash_map takes 6/149/117/38
// Randomly inserting 1000 into FlatMap/std::map/mutil::PooledMap/mutil::hash_map takes 15/169/135/54
// Randomly erasing 1000 from FlatMap/std::map/mutil::PooledMap/mutil::hash_map takes 5/157/129/39
// Randomly inserting 10000 into FlatMap/std::map/mutil::PooledMap/mutil::hash_map takes 19/242/203/55
// Randomly erasing 10000 from FlatMap/std::map/mutil::PooledMap/mutil::hash_map takes 5/233/218/54
// [ value = 128 bytes ]
// Randomly inserting 100 into FlatMap/std::map/mutil::PooledMap/mutil::hash_map takes 36/214/145/96
// Randomly erasing 100 from FlatMap/std::map/mutil::PooledMap/mutil::hash_map takes 7/166/122/53
// Randomly inserting 1000 into FlatMap/std::map/mutil::PooledMap/mutil::hash_map takes 36/230/174/100
// Randomly erasing 1000 from FlatMap/std::map/mutil::PooledMap/mutil::hash_map takes 6/193/153/65
// Randomly inserting 10000 into FlatMap/std::map/mutil::PooledMap/mutil::hash_map takes 45/304/270/115
// Randomly erasing 10000 from FlatMap/std::map/mutil::PooledMap/mutil::hash_map takes 7/299/246/88

template <typename K, typename V, size_t BLOCK_SIZE = 512,
          typename C = std::less<K> >
class PooledMap
    : public std::map<K, V, C, details::PooledAllocator<std::pair<const K, V>, BLOCK_SIZE> > {
    
};

namespace details {
// Specialize for void
template <size_t BLOCK_SIZE>
class PooledAllocator<void, BLOCK_SIZE> {
public:
    typedef void * pointer;
    typedef const void* const_pointer;
    typedef void value_type;
    template <class U1> struct rebind {
        typedef PooledAllocator<U1, BLOCK_SIZE> other;
    };
};

template <class T1, size_t BLOCK_SIZE>
class PooledAllocator {
public:
    typedef T1 value_type;
    typedef size_t size_type;
    typedef ptrdiff_t difference_type;
    typedef T1* pointer;
    typedef const T1* const_pointer;
    typedef T1& reference;
    typedef const T1& const_reference;
    template <class U1> struct rebind {
        typedef PooledAllocator<U1, BLOCK_SIZE> other;
    };
    
public:
    PooledAllocator() {}
    PooledAllocator(const PooledAllocator&) {}
    template <typename U1, size_t BS2>
    PooledAllocator(const PooledAllocator<U1, BS2>&) {}
    void operator=(const PooledAllocator&) {}
    template <typename U1, size_t BS2>
    void operator=(const PooledAllocator<U1, BS2>&) {}

    void swap(PooledAllocator& other) { _pool.swap(other._pool); }

    // Convert references to pointers.
    pointer address(reference r) const { return &r; }
    const_pointer address(const_reference r) const { return &r; }

    // Allocate storage for n values of T1.
    pointer allocate(size_type n, PooledAllocator<void, 0>::const_pointer = 0) {
        if (n == 1) {
            return (pointer)_pool.get();
        } else {
            return (pointer)malloc(n * sizeof(T1));
        }
    }

    // Deallocate storage obtained by a call to allocate.
    void deallocate(pointer p, size_type n) {
        if (n == 1) {
            return _pool.back(p);
        } else {
            free(p);
        }
    }

    // Return the largest possible storage available through a call to allocate.
    size_type max_size() const { return 0xFFFFFFFF / sizeof(T1); }

    void construct(pointer ptr) { ::new (ptr) T1; }
    void construct(pointer ptr, const T1& val) { ::new (ptr) T1(val); }
    template <class U1> void construct(pointer ptr, const U1& val)
    { ::new (ptr) T1(val); }

    void destroy(pointer p) { p->T1::~T1(); }

private:
    mutil::SingleThreadedPool<sizeof(T1), BLOCK_SIZE, 1> _pool;
};

// Return true if b could be used to deallocate storage obtained through a
// and vice versa. It's clear that our allocator can't be exchanged.
template <typename T1, size_t S1, typename T2, size_t S2>
bool operator==(const PooledAllocator<T1, S1>&, const PooledAllocator<T2, S2>&)
{ return false; }
template <typename T1, size_t S1, typename T2, size_t S2>
bool operator!=(const PooledAllocator<T1, S1>& a, const PooledAllocator<T2, S2>& b)
{ return !(a == b); }

} // namespace details
} // namespace mutil

// Since this allocator can't be exchanged(check impl. of operator==) nor
// copied, specializing swap() is a must to make map.swap() work.
#if !defined(MUTIL_CXX11_ENABLED)
#include <algorithm>  // std::swap until C++11
#else
#include <utility>    // std::swap since C++11
#endif

namespace std {
template <class T1, size_t BLOCK_SIZE>
inline void swap(::mutil::details::PooledAllocator<T1, BLOCK_SIZE> &lhs,
                 ::mutil::details::PooledAllocator<T1, BLOCK_SIZE> &rhs){
    lhs.swap(rhs);
}
}  // namespace std

#endif  // MUTIL_POOLED_MAP_H
