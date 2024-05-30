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


#ifndef MELON_FIBER_LIST_OF_ABAFREE_ID_H_
#define MELON_FIBER_LIST_OF_ABAFREE_ID_H_

#include <vector>
#include <deque>

namespace fiber {

// A container for storing identifiers that may be invalidated.

// [Basic Idea]
// identifiers are remembered for error notifications. While insertions are 
// easy, removals are hard to be done in O(1) time. More importantly, 
// insertions are often done in one thread, while removals come from many
// threads simultaneously. Think about the usage in melon::Socket, most
// fiber_session_t are inserted by one thread (the thread calling non-contended
// Write or the KeepWrite thread), but removals are from many threads 
// processing responses simultaneously.

// [The approach]
// Don't remove old identifiers eagerly, replace them when new ones are inserted.

// IdTraits MUST have {
//   // #identifiers in each block
//   static const size_t BLOCK_SIZE = 63;
//
//   // Max #entries. Often has close relationship with concurrency, 65536
//   // is "huge" for most apps.
//   static const size_t MAX_ENTRIES = 65536;
//
//   // Initial value of id. Id with the value is treated as invalid.
//   static const Id ID_INIT = ...;
//
//   // Returns true if the id is valid. The "validness" must be permanent or
//   // stable for a very long period (to make the id ABA-free).
//   static bool exists(Id id);
// }

// This container is NOT thread-safe right now, and shouldn't be
// an issue in current usages throughout melon.
template <typename Id, typename IdTraits> 
class ListOfABAFreeId {
public:
    ListOfABAFreeId();
    ~ListOfABAFreeId();
    
    // Add an identifier into the list.
    int add(Id id);
    
    // Apply fn(id) to all identifiers.
    template <typename Fn> void apply(const Fn& fn);
    
    // Put #entries of each level into `counts'
    // Returns #levels.
    size_t get_sizes(size_t* counts, size_t n);

private:
    DISALLOW_COPY_AND_ASSIGN(ListOfABAFreeId);
    struct IdBlock {
        Id ids[IdTraits::BLOCK_SIZE];
        IdBlock* next;
    };
    void forward_index();
    IdBlock* _cur_block;
    uint32_t _cur_index;
    uint32_t _nblock;
    IdBlock _head_block;
};

// [impl.]

template <typename Id, typename IdTraits> 
ListOfABAFreeId<Id, IdTraits>::ListOfABAFreeId()
    : _cur_block(&_head_block)
    , _cur_index(0)
    , _nblock(1) {
    for (size_t i = 0; i < IdTraits::BLOCK_SIZE; ++i) {
        _head_block.ids[i] = IdTraits::ID_INIT;
    }
    _head_block.next = NULL;
}

template <typename Id, typename IdTraits> 
ListOfABAFreeId<Id, IdTraits>::~ListOfABAFreeId() {
    _cur_block = NULL;
    _cur_index = 0;
    _nblock = 0;
    for (IdBlock* p = _head_block.next; p != NULL;) {
        IdBlock* saved_next = p->next;
        delete p;
        p = saved_next;
    }
    _head_block.next = NULL;
}

template <typename Id, typename IdTraits> 
inline void ListOfABAFreeId<Id, IdTraits>::forward_index() {
    if (++_cur_index >= IdTraits::BLOCK_SIZE) {
        _cur_index = 0;
        if (_cur_block->next) {
            _cur_block = _cur_block->next;
        } else {
            _cur_block = &_head_block;
        }
    }
}

template <typename Id, typename IdTraits> 
int ListOfABAFreeId<Id, IdTraits>::add(Id id) {
    // Scan for at most 4 positions, if any of them is empty, use the position.
    Id* saved_pos[4];
    for (size_t i = 0; i < arraysize(saved_pos); ++i) {
        Id* const pos = _cur_block->ids + _cur_index;
        forward_index();
        // The position is not used.
        if (*pos == IdTraits::ID_INIT || !IdTraits::exists(*pos)) {
            *pos = id;
            return 0;
        }
        saved_pos[i] = pos;
    }
    // The list is considered to be "crowded", add a new block and scatter
    // the conflict identifiers by inserting an empty entry after each of
    // them, so that even if the identifiers are still valid when we walk
    // through the area again, we can find an empty entry.

    // The new block is inserted as if it's inserted between xxxx and yyyy,
    // where xxxx are the 4 conflict identifiers.
    //  [..xxxxyyyy] -> [..........]
    //    block A        block B
    //
    //  [..xxxx....] -> [......yyyy] -> [..........]
    //    block A        new block      block B
    if (_nblock * IdTraits::BLOCK_SIZE > IdTraits::MAX_ENTRIES) {
        return EAGAIN;
    }
    IdBlock* new_block = new (std::nothrow) IdBlock;
    if (NULL == new_block) {
        return ENOMEM;
    }
    ++_nblock;
    for (size_t i = 0; i < _cur_index; ++i) {
        new_block->ids[i] = IdTraits::ID_INIT;
    }
    for (size_t i = _cur_index; i < IdTraits::BLOCK_SIZE; ++i) {
        new_block->ids[i] = _cur_block->ids[i];
        _cur_block->ids[i] = IdTraits::ID_INIT;
    }
    new_block->next = _cur_block->next;
    _cur_block->next = new_block;
    // Scatter the conflict identifiers.
    //  [..xxxx....] -> [......yyyy] -> [..........]
    //    block A        new block      block B
    //
    //  [..x.x.x.x.] -> [......yyyy] -> [..........]
    //    block A        new block      block B
    _cur_block->ids[_cur_index] = *saved_pos[2];
    *saved_pos[2] = *saved_pos[1];
    *saved_pos[1] = IdTraits::ID_INIT;
    forward_index();
    forward_index();
    _cur_block->ids[_cur_index] = *saved_pos[3];
    *saved_pos[3] = IdTraits::ID_INIT;
    forward_index();
    _cur_block->ids[_cur_index] = id;
    forward_index();
    return 0;
}

template <typename Id, typename IdTraits>
template <typename Fn>
void ListOfABAFreeId<Id, IdTraits>::apply(const Fn& fn) {
    for (IdBlock* p = &_head_block; p != NULL; p = p->next) {
        for (size_t i = 0; i < IdTraits::BLOCK_SIZE; ++i) {
            if (p->ids[i] != IdTraits::ID_INIT && IdTraits::exists(p->ids[i])) {
                fn(p->ids[i]);
            }
        }
    }
}

template <typename Id, typename IdTraits>
size_t ListOfABAFreeId<Id, IdTraits>::get_sizes(size_t* cnts, size_t n) {
    if (n == 0) {
        return 0;
    }
    // Current impl. only has one level.
    cnts[0] = _nblock * IdTraits::BLOCK_SIZE;
    return 1;
}

}  // namespace fiber

#endif  // MELON_FIBER_LIST_OF_ABAFREE_ID_H_
