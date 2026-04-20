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

#include "bvar/collector.h"
#include "bthread/rwlock.h"
#include "bthread/mutex.h"
#include "bthread/butex.h"

namespace bthread {


int rwlock_rdlock(bthread_rwlock_t* rwlock, bool try_lock,
                  const struct timespec* abstime) {
    auto whole = (butil::atomic<unsigned>*)rwlock->state;
    auto w_wait_count = (butil::atomic<unsigned>*)rwlock->w_wait_count;
    while (true) {
        // Write priority.
        unsigned w = w_wait_count->load(butil::memory_order_acquire);
        if (w > 0) {
            if (try_lock) {
                return EBUSY;
            } else if (butex_wait(w_wait_count, w, abstime) < 0 &&
                errno != EWOULDBLOCK && errno != EINTR) {
                return errno;
            }
            continue;
        }

        // 2^31 should be enough.
        unsigned r = whole->load(butil::memory_order_relaxed);
        if ((r >> 31) == 0) {
            if (whole->compare_exchange_weak(r, r + 1,
                                             butil::memory_order_acquire,
                                             butil::memory_order_relaxed)) {
                return 0;
            }
        } else if (try_lock) {
            // Write exists.
            return EBUSY;
        }
    }
}

int rwlock_unrdlock(bthread_rwlock_t* rwlock) {
    auto whole = (butil::atomic<unsigned>*)rwlock->state;
    auto w_wait_count = (butil::atomic<unsigned>*)rwlock->w_wait_count;

    while (true) {
        unsigned r = whole->load(butil::memory_order_relaxed);
        if (r == 0 || (r >> 31) != 0) {
            LOG(ERROR) << "Invalid unrlock!";
            return -1;
        }
        if(!(whole->compare_exchange_weak(r, r - 1,
                                         butil::memory_order_release,
                                         butil::memory_order_relaxed))) {
            continue;
        }
        // Only wake up write waiters if there are actually write waiters.
        // Check if this is the last reader and there are pending writers.
        if (r == 1) {
            butex_wake(whole);
        }
        return 0;
    }

}

static BUTIL_FORCE_INLINE void rwlock_wrlock_cleanup(bthread_rwlock_t* rwlock, bool write_queue_locked) {
    if (write_queue_locked) {
        bthread_mutex_unlock(&rwlock->write_queue_mutex);
    }
    auto w_wait_count = (butil::atomic<unsigned>*)rwlock->w_wait_count;
    // Fail to acquire the wrlock.
    auto w = w_wait_count->fetch_sub(1, butil::memory_order_relaxed);
    // Only wake read waiters if there might be any (w_wait_count > 0 means readers are waiting).
    if (w == 1) {
        butex_wake_all(w_wait_count);
    }
}

int rwlock_wrlock(bthread_rwlock_t* rwlock, bool try_lock,
                  const struct timespec* abstime) {
    auto w_wait_count = (butil::atomic<unsigned>*)rwlock->w_wait_count;
    // 2^31 should be enough.
    w_wait_count->fetch_add(1, butil::memory_order_relaxed);

    // First, resolve competition with other writers.
    int rc = bthread_mutex_trylock(&rwlock->write_queue_mutex);
    if (0 != rc) {
        if (try_lock) {
            // Fail to acquire the wrlock.
            rwlock_wrlock_cleanup(rwlock, false);
            return rc;
        }
        rc = bthread_mutex_timedlock(&rwlock->write_queue_mutex, abstime);
        if (0 != rc) {
            // Fail to acquire the wrlock.
            rwlock_wrlock_cleanup(rwlock, false);
            return rc;
        }
    }

    auto whole = (butil::atomic<unsigned>*)rwlock->state;
    while (true) {
        unsigned r = whole->load(butil::memory_order_relaxed);
        if (r != 0) {
            if (try_lock) {
                errno = EBUSY;
                break;
            }
            if (butex_wait(whole, r, abstime) < 0 &&
                errno != EWOULDBLOCK && errno != EINTR) {
                break;
            }
            continue;
        }
        if (whole->compare_exchange_weak(r, (unsigned)(1 << 31),
                                         butil::memory_order_acquire,
                                         butil::memory_order_relaxed)) {
            return 0;
        }
    }

    rwlock_wrlock_cleanup(rwlock, true);
    return errno;
}

int rwlock_unwrlock(bthread_rwlock_t* rwlock) {
    auto whole = (butil::atomic<unsigned>*)rwlock->state;
    auto w_wait_count = (butil::atomic<unsigned>*)rwlock->w_wait_count;

    while (true) {
        unsigned r = whole->load(butil::memory_order_relaxed);
        if (r != (unsigned)(1 << 31) ) {
            LOG(ERROR) << "Invalid unwrlock!";
            return EINVAL;
        }
        if (!whole->compare_exchange_weak(r, 0,
                                          butil::memory_order_release,
                                          butil::memory_order_relaxed)) {
            continue;
        }

        // Wake up one write waiter first if there are more write waiters.
        // w > 1 means there are other write waiters (w was the count before decrement).
        // if (w > 1) {
        //     butex_wake(whole);
        // }
        // Unlock `write_queue_mutex' to wake up other write waiters (if any).
        bthread_mutex_unlock(&rwlock->write_queue_mutex);

        // Decrement write waiter count.
        unsigned w = w_wait_count->fetch_sub(1, butil::memory_order_relaxed);
        // Wake up all read waiters if no more write waiters.
        if (w == 1) {
            butex_wake_all(w_wait_count);
        }
        return 0;
    }
}

int rwlock_unlock(bthread_rwlock_t* rwlock) {
    auto whole = (butil::atomic<unsigned>*)rwlock->state;
    if ((whole->load(butil::memory_order_relaxed) >> 31) != 0) {
        return rwlock_unwrlock(rwlock);
    } else {
        return rwlock_unrdlock(rwlock);
    }
}

} // namespace bthread

__BEGIN_DECLS

int bthread_rwlock_init(bthread_rwlock_t* __restrict rwlock,
                        const bthread_rwlockattr_t* __restrict) {
    rwlock->w_wait_count = bthread::butex_create_checked<unsigned>();
    rwlock->state = bthread::butex_create_checked<unsigned>();
    if (NULL == rwlock->w_wait_count || NULL == rwlock->state) {
        LOG(ERROR) << "Out of memory";
        return ENOMEM;
    }
    *rwlock->w_wait_count = 0;
    *rwlock->state = 0;

    bthread_mutexattr_t attr;
    bthread_mutexattr_init(&attr);
    bthread_mutexattr_disable_csite(&attr);
    bthread_mutex_init(&rwlock->write_queue_mutex, &attr);
    return 0;
}

int bthread_rwlock_destroy(bthread_rwlock_t* rwlock) {
    bthread::butex_destroy(rwlock->w_wait_count);
    bthread::butex_destroy(rwlock->state);
    return 0;
}

int bthread_rwlock_rdlock(bthread_rwlock_t* rwlock) {
    return bthread::rwlock_rdlock(rwlock, false, NULL);
}

int bthread_rwlock_tryrdlock(bthread_rwlock_t* rwlock) {
    return bthread::rwlock_rdlock(rwlock, true, NULL);
}

int bthread_rwlock_timedrdlock(bthread_rwlock_t* __restrict rwlock,
                               const struct timespec* __restrict abstime) {
    return bthread::rwlock_rdlock(rwlock, false, abstime);
}

int bthread_rwlock_wrlock(bthread_rwlock_t* rwlock) {
    return bthread::rwlock_wrlock(rwlock, false, NULL);
}

int bthread_rwlock_trywrlock(bthread_rwlock_t* rwlock) {
    return bthread::rwlock_wrlock(rwlock, true, NULL);
}

int bthread_rwlock_timedwrlock(bthread_rwlock_t* __restrict rwlock,
                               const struct timespec* __restrict abstime) {
    return bthread::rwlock_wrlock(rwlock, false, abstime);
}

int bthread_rwlock_unrlock(bthread_rwlock_t* rwlock) {
    return bthread::rwlock_unrdlock(rwlock);
}

int bthread_rwlock_unwlock(bthread_rwlock_t* rwlock) {
    return bthread::rwlock_unwrlock(rwlock);
}

int bthread_rwlock_unlock(bthread_rwlock_t* rwlock) {
    return bthread::rwlock_unlock(rwlock);
}

__END_DECLS
