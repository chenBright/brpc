// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

// bthread - An M:N threading library to make applications more concurrent.

// Date: Sun Jul 13 15:04:18 CST 2014

#ifndef BUTIL_OBJECT_POOL_H
#define BUTIL_OBJECT_POOL_H

#include <cstddef>                       // size_t
#include "butil/type_traits.h"

// ObjectPool is a derivative class of ResourcePool to allocate and
// reuse fixed-size objects without identifiers.

namespace butil {

// Specialize following classes to override default parameters for type T.
//   namespace butil {
//     template <> struct ObjectPoolBlockMaxSize<Foo> {
//       static const size_t value = 1024;
//     };
//   }

// Memory is allocated in blocks, memory size of a block will not exceed:
//   min(ObjectPoolBlockMaxSize<T>::value,
//       ObjectPoolBlockMaxItem<T>::value * sizeof(T))
template <typename T> struct ObjectPoolBlockMaxSize {
    static const size_t value = 64 * 1024; // bytes
};
template <typename T> struct ObjectPoolBlockMaxItem {
    static const size_t value = 256;
};

// Free objects of each thread are grouped into a chunk before they are merged
// to the global list. Memory size of objects in one free chunk will not exceed:
//   min(ObjectPoolFreeChunkMaxItem<T>::value() * sizeof(T),
//       ObjectPoolBlockMaxSize<T>::value,
//       ObjectPoolBlockMaxItem<T>::value * sizeof(T))
template <typename T> struct ObjectPoolFreeChunkMaxItem {
    static size_t value() { return 256; }
};

// ObjectPool calls this function on newly constructed objects. If this
// function returns false, the object is destructed immediately and
// get_object() shall return NULL. This is useful when the constructor
// failed internally(namely ENOMEM).
template <typename T> struct ObjectPoolValidator {
    static bool validate(const T*) { return true; }
};

//
template <typename T>
struct ObjectPoolWithASanPoison : true_type {};

}  // namespace butil

#include "butil/object_pool_inl.h"

namespace butil {

// Whether if FreeChunk of LocalPool is empty.
template <typename T> inline bool local_pool_free_empty() {
    return ObjectPool<T>::singleton()->local_free_empty();
}

// Get an object typed |T|. The object should be cleared before usage.
// NOTE: If there are no arguments, T must be default-constructible.
template <typename T, typename... Args>
inline T* get_object(Args&&... args) {
    return ObjectPool<T>::singleton()->get_object(std::forward<Args>(args)...);
}

// Return the object |ptr| back. The object is NOT destructed and will be
// returned by later get_object<T>. Similar with free/delete, validity of
// the object is not checked, user shall not return a not-yet-allocated or
// already-returned object otherwise behavior is undefined.
// Returns 0 when successful, -1 otherwise.
template <typename T> inline int return_object(T* ptr) {
    return ObjectPool<T>::singleton()->return_object(ptr);
}

// Reclaim all allocated objects typed T if caller is the last thread called
// this function, otherwise do nothing. You rarely need to call this function
// manually because it's called automatically when each thread quits.
template <typename T> inline void clear_objects() {
    ObjectPool<T>::singleton()->clear_objects();
}

// Get description of objects typed T.
// This function is possibly slow because it iterates internal structures.
// Don't use it frequently like a "getter" function.
template <typename T> ObjectPoolInfo describe_objects() {
    return ObjectPool<T>::singleton()->describe_objects();
}

}  // namespace butil

#endif  // BUTIL_OBJECT_POOL_H
