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


#include <pthread.h>
#include <melon/utility/macros.h>
#include <atomic>
#include <melon/var/passive_status.h>
#include <melon/fiber/errno.h>                       // EAGAIN
#include <melon/fiber/task_group.h>                  // TaskGroup
#include <melon/fiber/key.h>

// Implement fiber_key_t related functions

namespace fiber {

    class KeyTable;

    // defined in task_group.cpp
    extern __thread TaskGroup *tls_task_group;
    extern __thread LocalStorage tls_bls;
    static __thread bool tls_ever_created_keytable = false;

    // We keep thread specific data in a two-level array. The top-level array
    // contains at most KEY_1STLEVEL_SIZE pointers to dynamically allocated
    // arrays of at most KEY_2NDLEVEL_SIZE data pointers. Many applications
    // may just occupy one or two second level array, thus this machanism keeps
    // memory footprint smaller and we can change KEY_1STLEVEL_SIZE to a
    // bigger number more freely. The tradeoff is an additional memory indirection:
    // negligible at most time.
    static const uint32_t KEY_2NDLEVEL_SIZE = 32;

    // Notice that we're trying to make the memory of second level and first
    // level both 256 bytes to make memory allocator happier.
    static const uint32_t KEY_1STLEVEL_SIZE = 31;

    static const uint32_t KEYS_MAX = KEY_2NDLEVEL_SIZE * KEY_1STLEVEL_SIZE;

    // destructors/version of TLS.
    struct KeyInfo {
        uint32_t version;

        void (*dtor)(void *, const void *);

        const void *dtor_args;
    };

    static KeyInfo s_key_info[KEYS_MAX] = {};

    // For allocating keys.
    static pthread_mutex_t s_key_mutex = PTHREAD_MUTEX_INITIALIZER;
    static size_t nfreekey = 0;
    static size_t nkey = 0;
    static uint32_t s_free_keys[KEYS_MAX];

    // Stats.
    static std::atomic<size_t> nkeytable = MUTIL_STATIC_ATOMIC_INIT(0);
    static std::atomic<size_t> nsubkeytable = MUTIL_STATIC_ATOMIC_INIT(0);

    // The second-level array.
    // Align with cacheline to avoid false sharing.
    class MELON_CACHELINE_ALIGNMENT SubKeyTable {
    public:
        SubKeyTable() {
            memset(_data, 0, sizeof(_data));
            nsubkeytable.fetch_add(1, std::memory_order_relaxed);
        }

        // NOTE: Call clear first.
        ~SubKeyTable() {
            nsubkeytable.fetch_sub(1, std::memory_order_relaxed);
        }

        void clear(uint32_t offset) {
            for (uint32_t i = 0; i < KEY_2NDLEVEL_SIZE; ++i) {
                void *p = _data[i].ptr;
                if (p) {
                    // Set the position to NULL before calling dtor which may set
                    // the position again.
                    _data[i].ptr = NULL;

                    KeyInfo info = fiber::s_key_info[offset + i];
                    if (info.dtor && _data[i].version == info.version) {
                        info.dtor(p, info.dtor_args);
                    }
                }
            }
        }

        bool cleared() const {
            // We need to iterate again to check if every slot is empty. An
            // alternative is remember if set_data() was called during clear.
            for (uint32_t i = 0; i < KEY_2NDLEVEL_SIZE; ++i) {
                if (_data[i].ptr) {
                    return false;
                }
            }
            return true;
        }

        inline void *get_data(uint32_t index, uint32_t version) const {
            if (_data[index].version == version) {
                return _data[index].ptr;
            }
            return NULL;
        }

        inline void set_data(uint32_t index, uint32_t version, void *data) {
            _data[index].version = version;
            _data[index].ptr = data;
        }

    private:
        struct Data {
            uint32_t version;
            void *ptr;
        };
        Data _data[KEY_2NDLEVEL_SIZE];
    };

    // The first-level array.
    // Align with cacheline to avoid false sharing.
    class MELON_CACHELINE_ALIGNMENT KeyTable {
    public:
        KeyTable() : next(NULL) {
            memset(_subs, 0, sizeof(_subs));
            nkeytable.fetch_add(1, std::memory_order_relaxed);
        }

        ~KeyTable() {
            nkeytable.fetch_sub(1, std::memory_order_relaxed);
            for (int ntry = 0; ntry < PTHREAD_DESTRUCTOR_ITERATIONS; ++ntry) {
                for (uint32_t i = 0; i < KEY_1STLEVEL_SIZE; ++i) {
                    if (_subs[i]) {
                        _subs[i]->clear(i * KEY_2NDLEVEL_SIZE);
                    }
                }
                bool all_cleared = true;
                for (uint32_t i = 0; i < KEY_1STLEVEL_SIZE; ++i) {
                    if (_subs[i] != NULL && !_subs[i]->cleared()) {
                        all_cleared = false;
                        break;
                    }
                }
                if (all_cleared) {
                    for (uint32_t i = 0; i < KEY_1STLEVEL_SIZE; ++i) {
                        delete _subs[i];
                    }
                    return;
                }
            }
            LOG(ERROR) << "Fail to destroy all objects in KeyTable[" << this << ']';
        }

        inline void *get_data(fiber_key_t key) const {
            const uint32_t subidx = key.index / KEY_2NDLEVEL_SIZE;
            if (subidx < KEY_1STLEVEL_SIZE) {
                const SubKeyTable *sub_kt = _subs[subidx];
                if (sub_kt) {
                    return sub_kt->get_data(
                            key.index - subidx * KEY_2NDLEVEL_SIZE, key.version);
                }
            }
            return NULL;
        }

        inline int set_data(fiber_key_t key, void *data) {
            const uint32_t subidx = key.index / KEY_2NDLEVEL_SIZE;
            if (subidx < KEY_1STLEVEL_SIZE &&
                key.version == s_key_info[key.index].version) {
                SubKeyTable *sub_kt = _subs[subidx];
                if (sub_kt == NULL) {
                    sub_kt = new(std::nothrow) SubKeyTable;
                    if (NULL == sub_kt) {
                        return ENOMEM;
                    }
                    _subs[subidx] = sub_kt;
                }
                sub_kt->set_data(key.index - subidx * KEY_2NDLEVEL_SIZE,
                                 key.version, data);
                return 0;
            }
            CHECK(false) << "fiber_setspecific is called on invalid " << key;
            return EINVAL;
        }

    public:
        KeyTable *next;
    private:
        SubKeyTable *_subs[KEY_1STLEVEL_SIZE];
    };

    static KeyTable *borrow_keytable(fiber_keytable_pool_t *pool) {
        if (pool != NULL && pool->free_keytables) {
            MELON_SCOPED_LOCK(pool->mutex);
            KeyTable *p = (KeyTable *) pool->free_keytables;
            if (p) {
                pool->free_keytables = p->next;
                return p;
            }
        }
        return NULL;
    }

    // Referenced in task_group.cpp, must be extern.
    // Caller of this function must hold the KeyTable
    void return_keytable(fiber_keytable_pool_t *pool, KeyTable *kt) {
        if (NULL == kt) {
            return;
        }
        if (pool == NULL) {
            delete kt;
            return;
        }
        std::unique_lock<pthread_mutex_t> mu(pool->mutex);
        if (pool->destroyed) {
            mu.unlock();
            delete kt;
            return;
        }
        kt->next = (KeyTable *) pool->free_keytables;
        pool->free_keytables = kt;
    }

    static void cleanup_pthread(void *arg) {
        KeyTable *kt = static_cast<KeyTable *>(arg);
        if (kt) {
            delete kt;
            // After deletion: tls may be set during deletion.
            tls_bls.keytable = NULL;
        }
    }

    static void arg_as_dtor(void *data, const void *arg) {
        typedef void (*KeyDtor)(void *);
        return ((KeyDtor) arg)(data);
    }

    static int get_key_count(void *) {
        MELON_SCOPED_LOCK(fiber::s_key_mutex);
        return (int) nkey - (int) nfreekey;
    }

    static size_t get_keytable_count(void *) {
        return nkeytable.load(std::memory_order_relaxed);
    }

    static size_t get_keytable_memory(void *) {
        const size_t n = nkeytable.load(std::memory_order_relaxed);
        const size_t nsub = nsubkeytable.load(std::memory_order_relaxed);
        return n * sizeof(KeyTable) + nsub * sizeof(SubKeyTable);
    }

    static melon::var::PassiveStatus<int> s_fiber_key_count(
            "fiber_key_count", get_key_count, NULL);
    static melon::var::PassiveStatus<size_t> s_fiber_keytable_count(
            "fiber_keytable_count", get_keytable_count, NULL);
    static melon::var::PassiveStatus<size_t> s_fiber_keytable_memory(
            "fiber_keytable_memory", get_keytable_memory, NULL);

}  // namespace fiber

extern "C" {

int fiber_keytable_pool_init(fiber_keytable_pool_t *pool) {
    if (pool == NULL) {
        LOG(ERROR) << "Param[pool] is NULL";
        return EINVAL;
    }
    pthread_mutex_init(&pool->mutex, NULL);
    pool->free_keytables = NULL;
    pool->destroyed = 0;
    return 0;
}

int fiber_keytable_pool_destroy(fiber_keytable_pool_t *pool) {
    if (pool == NULL) {
        LOG(ERROR) << "Param[pool] is NULL";
        return EINVAL;
    }
    fiber::KeyTable *saved_free_keytables = NULL;
    {
        MELON_SCOPED_LOCK(pool->mutex);
        if (pool->free_keytables) {
            saved_free_keytables = (fiber::KeyTable *) pool->free_keytables;
            pool->free_keytables = NULL;
        }
        pool->destroyed = 1;
    }
    // Cheat get/setspecific and destroy the keytables.
    fiber::TaskGroup *const g = fiber::tls_task_group;
    fiber::KeyTable *old_kt = fiber::tls_bls.keytable;
    while (saved_free_keytables) {
        fiber::KeyTable *kt = saved_free_keytables;
        saved_free_keytables = kt->next;
        fiber::tls_bls.keytable = kt;
        if (g) {
            g->current_task()->local_storage.keytable = kt;
        }
        delete kt;
        if (old_kt == kt) {
            old_kt = NULL;
        }
    }
    fiber::tls_bls.keytable = old_kt;
    if (g) {
        g->current_task()->local_storage.keytable = old_kt;
    }
    // TODO: return_keytable may race with this function, we don't destroy
    // the mutex right now.
    // pthread_mutex_destroy(&pool->mutex);
    return 0;
}

int fiber_keytable_pool_getstat(fiber_keytable_pool_t *pool,
                                fiber_keytable_pool_stat_t *stat) {
    if (pool == NULL || stat == NULL) {
        LOG(ERROR) << "Param[pool] or Param[stat] is NULL";
        return EINVAL;
    }
    std::unique_lock<pthread_mutex_t> mu(pool->mutex);
    size_t count = 0;
    fiber::KeyTable *p = (fiber::KeyTable *) pool->free_keytables;
    for (; p; p = p->next, ++count) {}
    stat->nfree = count;
    return 0;
}

// TODO: this is not strict `reserve' because we only check #free.
// Currently there's no way to track KeyTables that may be returned
// to the pool in future.
void fiber_keytable_pool_reserve(fiber_keytable_pool_t *pool,
                                 size_t nfree,
                                 fiber_key_t key,
                                 void *ctor(const void *),
                                 const void *ctor_args) {
    if (pool == NULL) {
        LOG(ERROR) << "Param[pool] is NULL";
        return;
    }
    fiber_keytable_pool_stat_t stat;
    if (fiber_keytable_pool_getstat(pool, &stat) != 0) {
        LOG(ERROR) << "Fail to getstat of pool=" << pool;
        return;
    }
    for (size_t i = stat.nfree; i < nfree; ++i) {
        fiber::KeyTable *kt = new(std::nothrow) fiber::KeyTable;
        if (kt == NULL) {
            break;
        }
        void *data = ctor(ctor_args);
        if (data) {
            kt->set_data(key, data);
        }  // else append kt w/o data.

        std::unique_lock<pthread_mutex_t> mu(pool->mutex);
        if (pool->destroyed) {
            mu.unlock();
            delete kt;
            break;
        }
        kt->next = (fiber::KeyTable *) pool->free_keytables;
        pool->free_keytables = kt;
        if (data == NULL) {
            break;
        }
    }
}

int fiber_key_create2(fiber_key_t *key,
                      void (*dtor)(void *, const void *),
                      const void *dtor_args) {
    uint32_t index = 0;
    {
        MELON_SCOPED_LOCK(fiber::s_key_mutex);
        if (fiber::nfreekey > 0) {
            index = fiber::s_free_keys[--fiber::nfreekey];
        } else if (fiber::nkey < fiber::KEYS_MAX) {
            index = fiber::nkey++;
        } else {
            return EAGAIN;  // what pthread_key_create returns in this case.
        }
    }
    fiber::s_key_info[index].dtor = dtor;
    fiber::s_key_info[index].dtor_args = dtor_args;
    key->index = index;
    key->version = fiber::s_key_info[index].version;
    if (key->version == 0) {
        ++fiber::s_key_info[index].version;
        ++key->version;
    }
    return 0;
}

int fiber_key_create(fiber_key_t *key, void (*dtor)(void *)) {
    if (dtor == NULL) {
        return fiber_key_create2(key, NULL, NULL);
    } else {
        return fiber_key_create2(key, fiber::arg_as_dtor, (const void *) dtor);
    }
}

int fiber_key_delete(fiber_key_t key) {
    if (key.index < fiber::KEYS_MAX &&
        key.version == fiber::s_key_info[key.index].version) {
        MELON_SCOPED_LOCK(fiber::s_key_mutex);
        if (key.version == fiber::s_key_info[key.index].version) {
            if (++fiber::s_key_info[key.index].version == 0) {
                ++fiber::s_key_info[key.index].version;
            }
            fiber::s_key_info[key.index].dtor = NULL;
            fiber::s_key_info[key.index].dtor_args = NULL;
            fiber::s_free_keys[fiber::nfreekey++] = key.index;
            return 0;
        }
    }
    CHECK(false) << "fiber_key_delete is called on invalid " << key;
    return EINVAL;
}

// NOTE: Can't borrow_keytable in fiber_setspecific, otherwise following
// memory leak may occur:
//  -> fiber_getspecific fails to borrow_keytable and returns NULL.
//  -> fiber_setspecific succeeds to borrow_keytable and overwrites old data
//     at the position with newly created data, the old data is leaked.
int fiber_setspecific(fiber_key_t key, void *data) {
    fiber::KeyTable *kt = fiber::tls_bls.keytable;
    if (NULL == kt) {
        kt = new(std::nothrow) fiber::KeyTable;
        if (NULL == kt) {
            return ENOMEM;
        }
        fiber::tls_bls.keytable = kt;
        fiber::TaskGroup *const g = fiber::tls_task_group;
        if (g) {
            g->current_task()->local_storage.keytable = kt;
        } else {
            // Only cleanup keytable created by pthread.
            // keytable created by fiber will be deleted
            // in `return_keytable' or `fiber_keytable_pool_destroy'.
            if (!fiber::tls_ever_created_keytable) {
                fiber::tls_ever_created_keytable = true;
                CHECK_EQ(0, mutil::thread_atexit(fiber::cleanup_pthread, kt));
            }
        }
    }
    return kt->set_data(key, data);
}

void *fiber_getspecific(fiber_key_t key) {
    fiber::KeyTable *kt = fiber::tls_bls.keytable;
    if (kt) {
        return kt->get_data(key);
    }
    fiber::TaskGroup *const g = fiber::tls_task_group;
    if (g) {
        fiber::TaskMeta *const task = g->current_task();
        kt = fiber::borrow_keytable(task->attr.keytable_pool);
        if (kt) {
            g->current_task()->local_storage.keytable = kt;
            fiber::tls_bls.keytable = kt;
            return kt->get_data(key);
        }
    }
    return NULL;
}

void fiber_assign_data(void *data) {
    fiber::tls_bls.assigned_data = data;
}

void *fiber_get_assigned_data() {
    return fiber::tls_bls.assigned_data;
}

}  // extern "C"
