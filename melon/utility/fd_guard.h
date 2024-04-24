// Copyright 2023 The Elastic-AI Authors.
// part of Elastic AI Search
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//


// Date: Mon. Nov 7 14:47:36 CST 2011

#ifndef MUTIL_FD_GUARD_H
#define MUTIL_FD_GUARD_H

#include <unistd.h>                                  // close()

namespace mutil {

// RAII file descriptor.
//
// Example:
//    fd_guard fd1(open(...));
//    if (fd1 < 0) {
//        printf("Fail to open\n");
//        return -1;
//    }
//    if (another-error-happened) {
//        printf("Fail to do sth\n");
//        return -1;   // *** closing fd1 automatically ***
//    }
class fd_guard {
public:
    fd_guard() : _fd(-1) {}
    explicit fd_guard(int fd) : _fd(fd) {}
    
    ~fd_guard() {
        if (_fd >= 0) {
            ::close(_fd);
            _fd = -1;
        }
    }

    // Close current fd and replace with another fd
    void reset(int fd) {
        if (_fd >= 0) {
            ::close(_fd);
            _fd = -1;
        }
        _fd = fd;
    }

    // Set internal fd to -1 and return the value before set.
    int release() {
        const int prev_fd = _fd;
        _fd = -1;
        return prev_fd;
    }
    
    operator int() const { return _fd; }
    
private:
    // Copying this makes no sense.
    fd_guard(const fd_guard&);
    void operator=(const fd_guard&);
    
    int _fd;
};

}  // namespace mutil

#endif  // MUTIL_FD_GUARD_H
