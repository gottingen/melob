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


// fiber - An M:N threading library to make applications more concurrent.

// Date: Sun Jul 13 15:04:18 CST 2014

#ifndef MUTIL_OBJECT_POOL_INL_H
#define MUTIL_OBJECT_POOL_INL_H

#include <iostream>                      // std::ostream
#include <pthread.h>                     // pthread_mutex_t
#include <algorithm>                     // std::max, std::min
#include <melon/utility/atomicops.h>              // mutil::atomic
#include <melon/utility/macros.h>                 // MELON_CACHELINE_ALIGNMENT
#include <melon/utility/scoped_lock.h>            // MELON_SCOPED_LOCK
#include <melon/utility/thread_local.h>           // MELON_THREAD_LOCAL
#include <vector>

#ifdef MUTIL_OBJECT_POOL_NEED_FREE_ITEM_NUM
#define MELON_OBJECT_POOL_FREE_ITEM_NUM_ADD1                    \
    (_global_nfree.fetch_add(1, mutil::memory_order_relaxed))
#define MELON_OBJECT_POOL_FREE_ITEM_NUM_SUB1                    \
    (_global_nfree.fetch_sub(1, mutil::memory_order_relaxed))
#else
#define MELON_OBJECT_POOL_FREE_ITEM_NUM_ADD1
#define MELON_OBJECT_POOL_FREE_ITEM_NUM_SUB1
#endif

namespace mutil {

template <typename T, size_t NITEM>
struct ObjectPoolFreeChunk {
    size_t nfree;
    T* ptrs[NITEM];
}; 
// for gcc 3.4.5
template <typename T>
struct ObjectPoolFreeChunk<T, 0> {
    size_t nfree;
    T* ptrs[0];
}; 

struct ObjectPoolInfo {
    size_t local_pool_num;
    size_t block_group_num;
    size_t block_num;
    size_t item_num;
    size_t block_item_num;
    size_t free_chunk_item_num;
    size_t total_size;
#ifdef MUTIL_OBJECT_POOL_NEED_FREE_ITEM_NUM
    size_t free_item_num;
#endif
};

static const size_t OP_MAX_BLOCK_NGROUP = 65536;
static const size_t OP_GROUP_NBLOCK_NBIT = 16;
static const size_t OP_GROUP_NBLOCK = (1UL << OP_GROUP_NBLOCK_NBIT);
static const size_t OP_INITIAL_FREE_LIST_SIZE = 1024;

template <typename T>
class ObjectPoolBlockItemNum {
    static const size_t N1 = ObjectPoolBlockMaxSize<T>::value / sizeof(T);
    static const size_t N2 = (N1 < 1 ? 1 : N1);
public:
    static const size_t value = (N2 > ObjectPoolBlockMaxItem<T>::value ?
                                 ObjectPoolBlockMaxItem<T>::value : N2);
};

template <typename T>
class MELON_CACHELINE_ALIGNMENT ObjectPool {
public:
    static const size_t BLOCK_NITEM = ObjectPoolBlockItemNum<T>::value;
    static const size_t FREE_CHUNK_NITEM = BLOCK_NITEM;

    // Free objects are batched in a FreeChunk before they're added to
    // global list(_free_chunks).
    typedef ObjectPoolFreeChunk<T, FREE_CHUNK_NITEM>    FreeChunk;
    typedef ObjectPoolFreeChunk<T, 0> DynamicFreeChunk;

    // When a thread needs memory, it allocates a Block. To improve locality,
    // items in the Block are only used by the thread.
    // To support cache-aligned objects, align Block.items by cacheline.
    struct MELON_CACHELINE_ALIGNMENT Block {
        char items[sizeof(T) * BLOCK_NITEM];
        size_t nitem;

        Block() : nitem(0) {}
    };

    // An Object addresses at most OP_MAX_BLOCK_NGROUP BlockGroups,
    // each BlockGroup addresses at most OP_GROUP_NBLOCK blocks. So an
    // object addresses at most OP_MAX_BLOCK_NGROUP * OP_GROUP_NBLOCK Blocks.
    struct BlockGroup {
        mutil::atomic<size_t> nblock;
        mutil::atomic<Block*> blocks[OP_GROUP_NBLOCK];

        BlockGroup() : nblock(0) {
            // We fetch_add nblock in add_block() before setting the entry,
            // thus address_resource() may sees the unset entry. Initialize
            // all entries to NULL makes such address_resource() return NULL.
            memset(static_cast<void*>(blocks), 0, sizeof(mutil::atomic<Block*>) * OP_GROUP_NBLOCK);
        }
    };

    // Each thread has an instance of this class.
    class MELON_CACHELINE_ALIGNMENT LocalPool {
    public:
        explicit LocalPool(ObjectPool* pool)
            : _pool(pool)
            , _cur_block(NULL)
            , _cur_block_index(0) {
            _cur_free.nfree = 0;
        }

        ~LocalPool() {
            // Add to global _free if there're some free objects
            if (_cur_free.nfree) {
                _pool->push_free_chunk(_cur_free);
            }

            _pool->clear_from_destructor_of_local_pool();
        }

        static void delete_local_pool(void* arg) {
            delete (LocalPool*)arg;
        }

        // We need following macro to construct T with different CTOR_ARGS
        // which may include parenthesis because when T is POD, "new T()"
        // and "new T" are different: former one sets all fields to 0 which
        // we don't want.
#define MELON_OBJECT_POOL_GET(CTOR_ARGS)                                \
        /* Fetch local free ptr */                                      \
        if (_cur_free.nfree) {                                          \
            MELON_OBJECT_POOL_FREE_ITEM_NUM_SUB1;                       \
            return _cur_free.ptrs[--_cur_free.nfree];                   \
        }                                                               \
        /* Fetch a FreeChunk from global.                               \
           TODO: Popping from _free needs to copy a FreeChunk which is  \
           costly, but hardly impacts amortized performance. */         \
        if (_pool->pop_free_chunk(_cur_free)) {                         \
            MELON_OBJECT_POOL_FREE_ITEM_NUM_SUB1;                       \
            return _cur_free.ptrs[--_cur_free.nfree];                   \
        }                                                               \
        /* Fetch memory from local block */                             \
        if (_cur_block && _cur_block->nitem < BLOCK_NITEM) {            \
            T* obj = new ((T*)_cur_block->items + _cur_block->nitem) T CTOR_ARGS; \
            if (!ObjectPoolValidator<T>::validate(obj)) {               \
                obj->~T();                                              \
                return NULL;                                            \
            }                                                           \
            ++_cur_block->nitem;                                        \
            return obj;                                                 \
        }                                                               \
        /* Fetch a Block from global */                                 \
        _cur_block = add_block(&_cur_block_index);                      \
        if (_cur_block != NULL) {                                       \
            T* obj = new ((T*)_cur_block->items + _cur_block->nitem) T CTOR_ARGS; \
            if (!ObjectPoolValidator<T>::validate(obj)) {               \
                obj->~T();                                              \
                return NULL;                                            \
            }                                                           \
            ++_cur_block->nitem;                                        \
            return obj;                                                 \
        }                                                               \
        return NULL;                                                    \
 

        inline T* get() {
            MELON_OBJECT_POOL_GET();
        }

        template <typename A1>
        inline T* get(const A1& a1) {
            MELON_OBJECT_POOL_GET((a1));
        }

        template <typename A1, typename A2>
        inline T* get(const A1& a1, const A2& a2) {
            MELON_OBJECT_POOL_GET((a1, a2));
        }

#undef MELON_OBJECT_POOL_GET

        inline int return_object(T* ptr) {
            // Return to local free list
            if (_cur_free.nfree < ObjectPool::free_chunk_nitem()) {
                _cur_free.ptrs[_cur_free.nfree++] = ptr;
                MELON_OBJECT_POOL_FREE_ITEM_NUM_ADD1;
                return 0;
            }
            // Local free list is full, return it to global.
            // For copying issue, check comment in upper get()
            if (_pool->push_free_chunk(_cur_free)) {
                _cur_free.nfree = 1;
                _cur_free.ptrs[0] = ptr;
                MELON_OBJECT_POOL_FREE_ITEM_NUM_ADD1;
                return 0;
            }
            return -1;
        }

    private:
        ObjectPool* _pool;
        Block* _cur_block;
        size_t _cur_block_index;
        FreeChunk _cur_free;
    };

    inline T* get_object() {
        LocalPool* lp = get_or_new_local_pool();
        if (MELON_LIKELY(lp != NULL)) {
            return lp->get();
        }
        return NULL;
    }

    template <typename A1>
    inline T* get_object(const A1& arg1) {
        LocalPool* lp = get_or_new_local_pool();
        if (MELON_LIKELY(lp != NULL)) {
            return lp->get(arg1);
        }
        return NULL;
    }

    template <typename A1, typename A2>
    inline T* get_object(const A1& arg1, const A2& arg2) {
        LocalPool* lp = get_or_new_local_pool();
        if (MELON_LIKELY(lp != NULL)) {
            return lp->get(arg1, arg2);
        }
        return NULL;
    }

    inline int return_object(T* ptr) {
        LocalPool* lp = get_or_new_local_pool();
        if (MELON_LIKELY(lp != NULL)) {
            return lp->return_object(ptr);
        }
        return -1;
    }

    void clear_objects() {
        LocalPool* lp = _local_pool;
        if (lp) {
            _local_pool = NULL;
            mutil::thread_atexit_cancel(LocalPool::delete_local_pool, lp);
            delete lp;
        }
    }

    inline static size_t free_chunk_nitem() {
        const size_t n = ObjectPoolFreeChunkMaxItem<T>::value();
        return (n < FREE_CHUNK_NITEM ? n : FREE_CHUNK_NITEM);
    }

    // Number of all allocated objects, including being used and free.
    ObjectPoolInfo describe_objects() const {
        ObjectPoolInfo info;
        info.local_pool_num = _nlocal.load(mutil::memory_order_relaxed);
        info.block_group_num = _ngroup.load(mutil::memory_order_acquire);
        info.block_num = 0;
        info.item_num = 0;
        info.free_chunk_item_num = free_chunk_nitem();
        info.block_item_num = BLOCK_NITEM;
#ifdef MUTIL_OBJECT_POOL_NEED_FREE_ITEM_NUM
        info.free_item_num = _global_nfree.load(mutil::memory_order_relaxed);
#endif

        for (size_t i = 0; i < info.block_group_num; ++i) {
            BlockGroup* bg = _block_groups[i].load(mutil::memory_order_consume);
            if (NULL == bg) {
                break;
            }
            size_t nblock = std::min(bg->nblock.load(mutil::memory_order_relaxed),
                                     OP_GROUP_NBLOCK);
            info.block_num += nblock;
            for (size_t j = 0; j < nblock; ++j) {
                Block* b = bg->blocks[j].load(mutil::memory_order_consume);
                if (NULL != b) {
                    info.item_num += b->nitem;
                }
            }
        }
        info.total_size = info.block_num * info.block_item_num * sizeof(T);
        return info;
    }

    static inline ObjectPool* singleton() {
        ObjectPool* p = _singleton.load(mutil::memory_order_consume);
        if (p) {
            return p;
        }
        pthread_mutex_lock(&_singleton_mutex);
        p = _singleton.load(mutil::memory_order_consume);
        if (!p) {
            p = new ObjectPool();
            _singleton.store(p, mutil::memory_order_release);
        }
        pthread_mutex_unlock(&_singleton_mutex);
        return p;
    }

private:
    ObjectPool() {
        _free_chunks.reserve(OP_INITIAL_FREE_LIST_SIZE);
        pthread_mutex_init(&_free_chunks_mutex, NULL);
    }

    ~ObjectPool() {
        pthread_mutex_destroy(&_free_chunks_mutex);
    }

    // Create a Block and append it to right-most BlockGroup.
    static Block* add_block(size_t* index) {
        Block* const new_block = new(std::nothrow) Block;
        if (NULL == new_block) {
            return NULL;
        }
        size_t ngroup;
        do {
            ngroup = _ngroup.load(mutil::memory_order_acquire);
            if (ngroup >= 1) {
                BlockGroup* const g =
                    _block_groups[ngroup - 1].load(mutil::memory_order_consume);
                const size_t block_index =
                    g->nblock.fetch_add(1, mutil::memory_order_relaxed);
                if (block_index < OP_GROUP_NBLOCK) {
                    g->blocks[block_index].store(
                        new_block, mutil::memory_order_release);
                    *index = (ngroup - 1) * OP_GROUP_NBLOCK + block_index;
                    return new_block;
                }
                g->nblock.fetch_sub(1, mutil::memory_order_relaxed);
            }
        } while (add_block_group(ngroup));

        // Fail to add_block_group.
        delete new_block;
        return NULL;
    }

    // Create a BlockGroup and append it to _block_groups.
    // Shall be called infrequently because a BlockGroup is pretty big.
    static bool add_block_group(size_t old_ngroup) {
        BlockGroup* bg = NULL;
        MELON_SCOPED_LOCK(_block_group_mutex);
        const size_t ngroup = _ngroup.load(mutil::memory_order_acquire);
        if (ngroup != old_ngroup) {
            // Other thread got lock and added group before this thread.
            return true;
        }
        if (ngroup < OP_MAX_BLOCK_NGROUP) {
            bg = new(std::nothrow) BlockGroup;
            if (NULL != bg) {
                // Release fence is paired with consume fence in add_block()
                // to avoid un-constructed bg to be seen by other threads.
                _block_groups[ngroup].store(bg, mutil::memory_order_release);
                _ngroup.store(ngroup + 1, mutil::memory_order_release);
            }
        }
        return bg != NULL;
    }

    inline LocalPool* get_or_new_local_pool() {
        LocalPool* lp = _local_pool;
        if (MELON_LIKELY(lp != NULL)) {
            return lp;
        }
        lp = new(std::nothrow) LocalPool(this);
        if (NULL == lp) {
            return NULL;
        }
        MELON_SCOPED_LOCK(_change_thread_mutex); //avoid race with clear()
        _local_pool = lp;
        mutil::thread_atexit(LocalPool::delete_local_pool, lp);
        _nlocal.fetch_add(1, mutil::memory_order_relaxed);
        return lp;
    }

    void clear_from_destructor_of_local_pool() {
        // Remove tls
        _local_pool = NULL;

        // Do nothing if there're active threads.
        if (_nlocal.fetch_sub(1, mutil::memory_order_relaxed) != 1) {
            return;
        }

        // Can't delete global even if all threads(called ObjectPool
        // functions) quit because the memory may still be referenced by 
        // other threads. But we need to validate that all memory can
        // be deallocated correctly in tests, so wrap the function with 
        // a macro which is only defined in unittests.
#ifdef MELON_CLEAR_OBJECT_POOL_AFTER_ALL_THREADS_QUIT
        MELON_SCOPED_LOCK(_change_thread_mutex);  // including acquire fence.
        // Do nothing if there're active threads.
        if (_nlocal.load(mutil::memory_order_relaxed) != 0) {
            return;
        }
        // All threads exited and we're holding _change_thread_mutex to avoid
        // racing with new threads calling get_object().

        // Clear global free list.
        FreeChunk dummy;
        while (pop_free_chunk(dummy));

        // Delete all memory
        const size_t ngroup = _ngroup.exchange(0, mutil::memory_order_relaxed);
        for (size_t i = 0; i < ngroup; ++i) {
            BlockGroup* bg = _block_groups[i].load(mutil::memory_order_relaxed);
            if (NULL == bg) {
                break;
            }
            size_t nblock = std::min(bg->nblock.load(mutil::memory_order_relaxed),
                                     OP_GROUP_NBLOCK);
            for (size_t j = 0; j < nblock; ++j) {
                Block* b = bg->blocks[j].load(mutil::memory_order_relaxed);
                if (NULL == b) {
                    continue;
                }
                for (size_t k = 0; k < b->nitem; ++k) {
                    T* const objs = (T*)b->items;
                    objs[k].~T();
                }
                delete b;
            }
            delete bg;
        }

        memset(_block_groups, 0, sizeof(BlockGroup*) * OP_MAX_BLOCK_NGROUP);
#endif
    }

private:
    bool pop_free_chunk(FreeChunk& c) {
        // Critical for the case that most return_object are called in
        // different threads of get_object.
        if (_free_chunks.empty()) {
            return false;
        }
        pthread_mutex_lock(&_free_chunks_mutex);
        if (_free_chunks.empty()) {
            pthread_mutex_unlock(&_free_chunks_mutex);
            return false;
        }
        DynamicFreeChunk* p = _free_chunks.back();
        _free_chunks.pop_back();
        pthread_mutex_unlock(&_free_chunks_mutex);
        c.nfree = p->nfree;
        memcpy(c.ptrs, p->ptrs, sizeof(*p->ptrs) * p->nfree);
        free(p);
        return true;
    }

    bool push_free_chunk(const FreeChunk& c) {
        DynamicFreeChunk* p = (DynamicFreeChunk*)malloc(
            offsetof(DynamicFreeChunk, ptrs) + sizeof(*c.ptrs) * c.nfree);
        if (!p) {
            return false;
        }
        p->nfree = c.nfree;
        memcpy(p->ptrs, c.ptrs, sizeof(*c.ptrs) * c.nfree);
        pthread_mutex_lock(&_free_chunks_mutex);
        _free_chunks.push_back(p);
        pthread_mutex_unlock(&_free_chunks_mutex);
        return true;
    }
    
    static mutil::static_atomic<ObjectPool*> _singleton;
    static pthread_mutex_t _singleton_mutex;
    static MELON_THREAD_LOCAL LocalPool* _local_pool;
    static mutil::static_atomic<long> _nlocal;
    static mutil::static_atomic<size_t> _ngroup;
    static pthread_mutex_t _block_group_mutex;
    static pthread_mutex_t _change_thread_mutex;
    static mutil::static_atomic<BlockGroup*> _block_groups[OP_MAX_BLOCK_NGROUP];

    std::vector<DynamicFreeChunk*> _free_chunks;
    pthread_mutex_t _free_chunks_mutex;

#ifdef MUTIL_OBJECT_POOL_NEED_FREE_ITEM_NUM
    static mutil::static_atomic<size_t> _global_nfree;
#endif
};

// Declare template static variables:

template <typename T>
const size_t ObjectPool<T>::FREE_CHUNK_NITEM;

template <typename T>
MELON_THREAD_LOCAL typename ObjectPool<T>::LocalPool*
ObjectPool<T>::_local_pool = NULL;

template <typename T>
mutil::static_atomic<ObjectPool<T>*> ObjectPool<T>::_singleton = MUTIL_STATIC_ATOMIC_INIT(NULL);

template <typename T>
pthread_mutex_t ObjectPool<T>::_singleton_mutex = PTHREAD_MUTEX_INITIALIZER;

template <typename T>
static_atomic<long> ObjectPool<T>::_nlocal = MUTIL_STATIC_ATOMIC_INIT(0);

template <typename T>
mutil::static_atomic<size_t> ObjectPool<T>::_ngroup = MUTIL_STATIC_ATOMIC_INIT(0);

template <typename T>
pthread_mutex_t ObjectPool<T>::_block_group_mutex = PTHREAD_MUTEX_INITIALIZER;

template <typename T>
pthread_mutex_t ObjectPool<T>::_change_thread_mutex = PTHREAD_MUTEX_INITIALIZER;

template <typename T>
mutil::static_atomic<typename ObjectPool<T>::BlockGroup*>
ObjectPool<T>::_block_groups[OP_MAX_BLOCK_NGROUP] = {};

#ifdef MUTIL_OBJECT_POOL_NEED_FREE_ITEM_NUM
template <typename T>
mutil::static_atomic<size_t> ObjectPool<T>::_global_nfree = MUTIL_STATIC_ATOMIC_INIT(0);
#endif

inline std::ostream& operator<<(std::ostream& os,
                                ObjectPoolInfo const& info) {
    return os << "local_pool_num: " << info.local_pool_num
              << "\nblock_group_num: " << info.block_group_num
              << "\nblock_num: " << info.block_num
              << "\nitem_num: " << info.item_num
              << "\nblock_item_num: " << info.block_item_num
              << "\nfree_chunk_item_num: " << info.free_chunk_item_num
              << "\ntotal_size: " << info.total_size
#ifdef MUTIL_OBJECT_POOL_NEED_FREE_ITEM_NUM
              << "\nfree_num: " << info.free_item_num
#endif
        ;
}
}  // namespace mutil

#endif  // MUTIL_OBJECT_POOL_INL_H
