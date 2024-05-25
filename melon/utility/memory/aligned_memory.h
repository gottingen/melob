// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// AlignedMemory is a POD type that gives you a portable way to specify static
// or local stack data of a given alignment and size. For example, if you need
// static storage for a class, but you want manual control over when the object
// is constructed and destructed (you don't want static initialization and
// destruction), use AlignedMemory:
//
//   static AlignedMemory<sizeof(MyClass), ALIGNOF(MyClass)> my_class;
//
//   // ... at runtime:
//   new(my_class.void_data()) MyClass();
//
//   // ... use it:
//   MyClass* mc = my_class.data_as<MyClass>();
//
//   // ... later, to destruct my_class:
//   my_class.data_as<MyClass>()->MyClass::~MyClass();
//
// Alternatively, a runtime sized aligned allocation can be created:
//
//   float* my_array = static_cast<float*>(AlignedAlloc(size, alignment));
//
//   // ... later, to release the memory:
//   AlignedFree(my_array);
//
// Or using scoped_ptr:
//
//   scoped_ptr<float, AlignedFreeDeleter> my_array(
//       static_cast<float*>(AlignedAlloc(size, alignment)));

#ifndef MUTIL_MEMORY_ALIGNED_MEMORY_H_
#define MUTIL_MEMORY_ALIGNED_MEMORY_H_

#include "melon/utility/base_export.h"
#include "melon/utility/basictypes.h"
#include <melon/utility/compiler_specific.h>

#if defined(COMPILER_MSVC)
#include <malloc.h>
#else
#include <stdlib.h>
#endif

namespace mutil {

// AlignedMemory is specialized for all supported alignments.
// Make sure we get a compiler error if someone uses an unsupported alignment.
template <size_t Size, size_t ByteAlignment>
struct AlignedMemory {};

#define MUTIL_DECL_ALIGNED_MEMORY(byte_alignment) \
    template <size_t Size> \
    class AlignedMemory<Size, byte_alignment> { \
     public: \
      ALIGNAS(byte_alignment) uint8_t data_[Size]; \
      void* void_data() { return static_cast<void*>(data_); } \
      const void* void_data() const { \
        return static_cast<const void*>(data_); \
      } \
      template<typename Type> \
      Type* data_as() { return static_cast<Type*>(void_data()); } \
      template<typename Type> \
      const Type* data_as() const { \
        return static_cast<const Type*>(void_data()); \
      } \
     private: \
      void* operator new(size_t); \
      void operator delete(void*); \
    }

// Specialization for all alignments is required because MSVC (as of VS 2008)
// does not understand ALIGNAS(ALIGNOF(Type)) or ALIGNAS(template_param).
// Greater than 4096 alignment is not supported by some compilers, so 4096 is
// the maximum specified here.
MUTIL_DECL_ALIGNED_MEMORY(1);
MUTIL_DECL_ALIGNED_MEMORY(2);
MUTIL_DECL_ALIGNED_MEMORY(4);
MUTIL_DECL_ALIGNED_MEMORY(8);
MUTIL_DECL_ALIGNED_MEMORY(16);
MUTIL_DECL_ALIGNED_MEMORY(32);
MUTIL_DECL_ALIGNED_MEMORY(64);
MUTIL_DECL_ALIGNED_MEMORY(128);
MUTIL_DECL_ALIGNED_MEMORY(256);
MUTIL_DECL_ALIGNED_MEMORY(512);
MUTIL_DECL_ALIGNED_MEMORY(1024);
MUTIL_DECL_ALIGNED_MEMORY(2048);
MUTIL_DECL_ALIGNED_MEMORY(4096);

#undef MUTIL_DECL_ALIGNED_MEMORY

MUTIL_EXPORT void* AlignedAlloc(size_t size, size_t alignment);

inline void AlignedFree(void* ptr) {
#if defined(COMPILER_MSVC)
  _aligned_free(ptr);
#else
  free(ptr);
#endif
}

// Deleter for use with scoped_ptr. E.g., use as
//   scoped_ptr<Foo, mutil::AlignedFreeDeleter> foo;
struct AlignedFreeDeleter {
  inline void operator()(void* ptr) const {
    AlignedFree(ptr);
  }
};

}  // namespace mutil

#endif  // MUTIL_MEMORY_ALIGNED_MEMORY_H_
