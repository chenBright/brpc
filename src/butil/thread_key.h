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

#ifndef  BUTIL_THREAD_KEY_H
#define  BUTIL_THREAD_KEY_H

#include <pthread.h>
#include <stdlib.h>
#include <limits>
#include <vector>
#include <functional>
#include <memory>

#include "butil/scoped_lock.h"
#include "butil/type_traits.h"
#include "butil/shared_object.h"
#include "butil/synchronization/lock.h"

namespace butil {

using DtorFunction = std::function<void(void*)>;

class ThreadKey {
public:
    friend int thread_key_create(ThreadKey& thread_key, DtorFunction dtor);
    friend int thread_key_delete(ThreadKey& thread_key);
    friend int thread_setspecific(ThreadKey& thread_key, void* data);
    friend void* thread_getspecific(ThreadKey& thread_key);

    static constexpr size_t InvalidID = std::numeric_limits<size_t>::max();
    static constexpr size_t InitSeq = 0;

    constexpr ThreadKey() : _id(InvalidID), _seq(InitSeq) {}

    ~ThreadKey() {
        Reset();
    }

    ThreadKey(ThreadKey&& other) noexcept
        : _id(other._id)
        , _seq(other._seq) {
        other.Reset();
    }

    ThreadKey& operator=(ThreadKey&& other) noexcept;

    ThreadKey(const ThreadKey& other) = delete;
    ThreadKey& operator=(const ThreadKey& other) = delete;

    bool Valid() const;

    void Reset() {
        _id = InvalidID;
        _seq = InitSeq;
    }

private:
    size_t _id; // Key id.
    // Sequence number form g_thread_keys set in thread_key_create.
    size_t _seq;
};

struct ThreadKeyInfo {
    ThreadKeyInfo() : seq(0), dtor(NULL) {}

    size_t seq; // Already allocated?
    DtorFunction dtor; // Destruction routine.
};

struct ThreadKeyTLS {
    ThreadKeyTLS() : seq(0), data(NULL) {}

    // Sequence number form ThreadKey,
    // set in `thread_setspecific',
    // used to check if the key is valid in `thread_getspecific'.
    size_t seq;
    void* data; // User data.
};

// pthread_key_xxx implication without num limit of key.
int thread_key_create(ThreadKey& thread_key, DtorFunction dtor);
int thread_key_delete(ThreadKey& thread_key);
int thread_setspecific(ThreadKey& thread_key, void* data);
void* thread_getspecific(ThreadKey& thread_key);


template <typename T>
class ThreadLocal {
    class InternalObject {
    public:
        template <typename Callback>
        void for_each(Callback&& callback) {
            BAIDU_SCOPED_LOCK(mutex);
            for (auto ptr : ptrs) {
                callback(ptr);
            }
        }

        void push(T* ptr) {
            BAIDU_SCOPED_LOCK(mutex);
            ptrs.push_back(ptr);
        }

        void replace(T* old_ptr, T* new_ptr) {
            BAIDU_SCOPED_LOCK(mutex);
            auto it = std::find(ptrs.begin(), ptrs.end(), old_ptr);
            CHECK_NE(it, ptrs.end());
            *it = new_ptr;
        }

        void remove(T* ptr) {
            // Remove and delete old_ptr.
            if (NULL == ptr) {
                return;
            }
            BAIDU_SCOPED_LOCK(mutex);
            auto iter = std::find(ptrs.begin(), ptrs.end(), ptr);
            if (iter != ptrs.end()) {
                ptrs.erase(iter, ptrs.end());
                delete ptr;
            }
        }

        void remove_all() {
            BAIDU_SCOPED_LOCK(mutex);
            for (auto ptr : ptrs) {
                delete ptr;
            }
        }

    private:
        Mutex mutex;
        // All pointers of data allocated by the ThreadLocal.
        std::vector<T*> ptrs;
    };

public:
    ThreadLocal() : ThreadLocal(false) {}

    explicit ThreadLocal(bool delete_at_thread_exit)
        : _object(std::make_shared<InternalObject>())
        , _delete_at_thread_exit(delete_at_thread_exit) {
        DtorFunction dtor;
        if (_delete_at_thread_exit) {
            // Remove and delete the thread local object at thread exit.
            std::weak_ptr<InternalObject> weak_object(_object);
            dtor = [weak_object](void* ptr) {
                auto object = weak_object.lock();
                if (NULL != object) {
                    object->remove(static_cast<T*>(ptr));
                } // else the ThreadLocal is destructed, do nothing.
            };
        }
        CHECK_EQ(0, thread_key_create(_key, dtor));
    }

    ~ThreadLocal() {
        thread_key_delete(_key);
        _object->remove_all();
    }

    DISALLOW_COPY(ThreadLocal);

    // Returns the thread local object if existed, NULL otherwise.
    T* get_or_null() {
        return static_cast<T*>(thread_getspecific(_key));
    }

    // Returns the thread local object, create one if not exist.
    // Args are passed to T's constructor.
    template<typename... Args>
    T* get(Args&&... args) {
        T* ptr = static_cast<T*>(thread_getspecific(_key));
        if (NULL == ptr) {
            ptr = new T(std::forward<Args>(args)...);
            CHECK_EQ(0, thread_setspecific(_key, ptr));
            _object->push(ptr);
        }
        return ptr;
    }

    T* operator->() { return get(); }

    T& operator*() { return *get(); }

    // Iterate through all thread local objects.
    // Callback, which must accept Args params and return void,
    // will be called under a thread lock.
    template <typename Callback>
    void for_each(Callback&& callback) {
        BAIDU_CASSERT((is_result_void<Callback, T*>::value),
                      "Callback must accept Args params and return void");
        _object->for_each(std::forward<Callback>(callback));
    }

    // Set the thread local object to `ptr', and delete the old one if any.
    // If `ptr' is same as the old one, do nothing.
    void reset(T* ptr) {
        T* old_ptr = get();
        if (ptr == old_ptr) {
            return;
        }
        if (thread_setspecific(_key, ptr) != 0) {
            return;
        }
        _object->replace(old_ptr, ptr);
    }

    void reset() {
        reset(NULL);
    }

private:
    ThreadKey _key;
    std::shared_ptr<InternalObject> _object;
    // Delete data on thread exit or destructor of ThreadLocal.
    bool _delete_at_thread_exit;
};

} // namespace butil


#endif // BUTIL_THREAD_KEY_H
