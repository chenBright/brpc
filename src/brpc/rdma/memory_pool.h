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

#ifndef BRPC_RDMA_MEMORY_POOL_H
#define BRPC_RDMA_MEMORY_POOL_H

#include <cstddef>
#include <cstdint>
#include <ostream>

#include "butil/atomicops.h"
#include "butil/macros.h"
#include "butil/synchronization/lock.h"

namespace brpc {
namespace rdma {

// A pool of registered memory blocks, with the *source* of the bytes left to
// a pluggable backend.
//
// RDMA can only send from memory that ibv_reg_mr() has pinned, and pinning
// costs milliseconds -- far too much to pay per RPC. So memory is taken from
// the allocator in big regions, each registered once, and carved into blocks
// that keep their registration for the lifetime of the process; freeing a
// block only returns it to its size class's free list.
//
// That mechanism says nothing about where the bytes come from. It was written
// twice -- once in block_pool.cpp over tcmalloc for IOBuf, once again for the
// GDR second channel over cudaMalloc -- and the two copies differ only in
// four calls. Those four are MemoryBackend; everything else (regions, size
// classes, the lock-free address lookup, the thread cache) lives here once
// and serves host and GPU memory alike.
//
// Thread-safe. The steady state -- allocate and free the same size class on
// one thread -- takes no lock at all.

// Where the bytes come from and how they get registered. Every method may be
// called concurrently; Alloc/Register may block for milliseconds.
class MemoryBackend {
public:
    virtual ~MemoryBackend() { }

    // For logs and DumpInfo(), e.g. "host" or "cuda".
    virtual const char* name() const = 0;

    // nullptr on failure. `size' is always a multiple of a block size, and the
    // returned address must be at least 4096-aligned: DeallocBlock() tells a
    // block start from an interior pointer by the offset's alignment.
    virtual void* Alloc(size_t size) = 0;
    virtual void Free(void* ptr) = 0;

    // Returns the lkey, or 0 on failure. A backend with no RDMA context of
    // its own may return any non-zero token; the pool only stores it.
    virtual uint32_t Register(void* ptr, size_t size) = 0;
    virtual void Deregister(void* ptr) = 0;
};

struct MemoryPoolOptions {
    // log2 of each block size, ascending and distinct, e.g. {13, 16, 21} for
    // 8KB / 64KB / 2MB. Points at storage the caller keeps alive.
    const int* size_class_shifts{nullptr};
    int num_size_classes{0};

    // Bound on the region table. GetLKey() resolves an address by scanning
    // it without a lock, so keeping this small is what makes that legal.
    int max_regions{16};

    // How big one region is, i.e. one Alloc() + one Register(). When zero, a
    // region holds `blocks_per_region' blocks, clamped to `max_region_size'.
    // Either way the result is rounded down to a whole number of blocks.
    size_t region_size{0};
    size_t blocks_per_region{0};
    size_t max_region_size{0};

    // Hard cap on the bytes handed to the backend, counting whole regions.
    // 0 means unlimited.
    int64_t max_bytes{0};

    // Free blocks cached per thread, per size class and in total. 0 disables
    // the thread cache.
    int tls_cache_num{0};
    int64_t tls_cache_bytes{0};

    // When true the pool never calls Alloc() by itself: it only ever hands
    // out what AddBlockRegion() gave it, and running out is an error rather
    // than a reason to grow.
    bool user_specified_memory{false};
};

// A run of free memory: a whole fresh region, what is left of one after some
// blocks were carved off, or a single block that came back. Namespace-scope
// rather than nested because butil::get_object<> needs it accessible.
struct MemoryPoolIdleNode {
    void* start;
    size_t len;
    MemoryPoolIdleNode* next;
};

class MemoryPool {
public:
    // Bound of the region table; --*_max_regions is validated against it.
    static const int MAX_REGIONS = 64;
    static const int MAX_SIZE_CLASSES = 24;
    // Pools sharing the per-thread cache array. One slot is taken per
    // constructed MemoryPool and never given back, so pools are meant to be
    // process-lifetime singletons that Init()/Destroy() in place.
    static const int MAX_POOLS = 4;

    MemoryPool();
    ~MemoryPool();

    // `backend' must outlive the pool; ownership is not taken. Returns 0, or
    // -1 with errno set (EINVAL for a malformed options).
    int Init(MemoryBackend* backend, const MemoryPoolOptions& options);

    // Deregister and free every region the pool owns, and invalidate every
    // thread cache. Safe to call on a pool that was never Init()ed.
    void Destroy();

    bool initialized() const { return _backend != nullptr; }

    // A block of at least `size' bytes, or nullptr with errno set (E2BIG when
    // `size' exceeds the largest class, ENOMEM when the pool cannot grow).
    // *`lkey' (may be nullptr) gets the lkey of the containing region -- pass
    // nullptr to skip the region lookup on the allocation path.
    void* AllocBlock(size_t size, uint32_t* lkey = nullptr);

    // Return a block to its size class. Returns 0, or -1 with errno set
    // (ERANGE when `buf' is in no region, in a user region, or is not the
    // start of a block).
    int DeallocBlock(void* buf);

    // Add one region for `size_class' up front. `region_size' overrides
    // options.region_size when non-zero. Returns 0, or -1 with errno set.
    int Reserve(int size_class, size_t region_size = 0);

    // Hand the pool a caller-allocated region to carve into blocks of
    // `size_class'. The pool registers it, and frees it in Destroy() when
    // `owned'. Returns 0, or -1 with errno set.
    int AddBlockRegion(void* base, size_t size, int size_class, bool owned);

    // Record an already-registered, caller-owned buffer so GetLKey() can
    // resolve addresses inside it. Never carved into blocks, never freed.
    // Returns 0, or -1 with errno set (ENOSPC when the table is full, which
    // the caller may want to handle rather than propagate).
    int AddUserRegion(void* base, size_t size, uint32_t lkey);

    // Drop the region starting exactly at `base', deregistering it. Regions
    // added by AddUserRegion() only. Returns 0, or -1 with errno ERANGE.
    int RemoveRegion(const void* base);

    // lkey of the region containing `buf', 0 if there is none. Takes no lock.
    //
    // *`is_block_region' (may be nullptr, written only on a hit) says whether
    // the region is one the pool carves into blocks or one the caller
    // registered. The table holds both and the lkey cannot tell them apart,
    // which matters to anyone deducing a property of the memory -- the pool's
    // blocks are made of whatever the backend allocates, a user region is
    // whatever the user had.
    uint32_t GetLKey(const void* buf, bool* is_block_region = nullptr) const;

    // Smallest class fitting `size', or -1 if there is none.
    int SizeClassOf(size_t size) const;
    size_t SizeOfClass(int size_class) const {
        return (size_t)1 << _options.size_class_shifts[size_class];
    }
    int num_size_classes() const { return _options.num_size_classes; }

    // Size class of the block containing `buf', or -1 when `buf' is in no
    // region or in a user region.
    int BlockSizeClassOf(const void* buf) const;

    int num_regions() const {
        return _region_num.load(butil::memory_order_acquire);
    }
    // Whole regions, so this includes the part not handed out yet -- which is
    // also what options.max_bytes limits.
    int64_t reserved_bytes() const;
    int64_t in_use_bytes() const {
        return _in_use_bytes.load(butil::memory_order_relaxed);
    }
    // Bytes on the global free list of one class. Excludes thread caches.
    size_t idle_bytes(int size_class) const;

    void DumpInfo(std::ostream& os) const;

private:
    DISALLOW_COPY_AND_ASSIGN(MemoryPool);

    // Slots are appended and never reused, so the fields of a published slot
    // are immutable apart from `start' going to 0. That is what makes
    // FindRegion() safe with no lock on the read side.
    struct Region {
        butil::atomic<uintptr_t> start;
        size_t size;
        uint32_t lkey;
        // -1 for a buffer passed to AddUserRegion().
        int size_class;
        bool owned;
    };

    struct ClassInfo {
        mutable butil::Mutex mutex;
        MemoryPoolIdleNode* idle_list;
        size_t idle_bytes;

        ClassInfo() : idle_list(nullptr), idle_bytes(0) { }
    };

    const Region* FindRegion(const void* buf) const;
    // Both require _extend_mutex. AddRegionLocked() publishes a slot;
    // ExtendLocked() also needs `size_class''s mutex and only runs when that
    // class has nothing left.
    int AddRegionLocked(void* base, size_t size, int size_class,
                        uint32_t lkey, bool owned);
    bool ExtendLocked(int size_class, size_t region_size);
    void* AllocFromClass(int size_class);
    void SyncTlsCache();
    void DropTlsCache();
    void RecycleTlsCache();
    static void RecycleTlsCacheThunk(void* arg);

    MemoryBackend* _backend;
    MemoryPoolOptions _options;
    int _tls_slot;

    Region _regions[MAX_REGIONS];
    butil::atomic<int> _region_num;

    ClassInfo _classes[MAX_SIZE_CLASSES];
    // Taken while already holding a class mutex, never the other way round.
    mutable butil::Mutex _extend_mutex;
    int64_t _reserved_bytes;  // guarded by _extend_mutex
    butil::atomic<int64_t> _in_use_bytes;

    // Bumped by Destroy(). A thread cache filled before the bump points into
    // regions that have been freed, so it has to be dropped rather than used
    // or returned. Only unit tests tear a pool down and build it again, but a
    // use-after-free there is just as loud.
    butil::atomic<uint64_t> _generation;
};

}  // namespace rdma
}  // namespace brpc

#endif  // BRPC_RDMA_MEMORY_POOL_H
