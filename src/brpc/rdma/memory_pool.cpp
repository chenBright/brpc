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

#include "brpc/rdma/memory_pool.h"

#include <errno.h>

#include "butil/logging.h"
#include "butil/object_pool.h"
#include "butil/scoped_lock.h"
#include "butil/thread_local.h"

namespace brpc {
namespace rdma {

// Out-of-line definitions, needed because the bounds below are odr-used --
// CHECK_LT() and std::min() bind them to a const reference.
const int MemoryPool::MAX_REGIONS;
const int MemoryPool::MAX_SIZE_CLASSES;
const int MemoryPool::MAX_POOLS;

typedef MemoryPoolIdleNode IdleNode;

namespace {

// The per-thread free block cache, one set of lists per pool.
//
// It has to be `__thread' to be worth having -- a cache that costs an
// indirect call is not faster than the mutex it replaces -- and `__thread'
// cannot be a member. So each pool takes a slot at construction and indexes
// into a fixed array. MAX_POOLS is small because the whole array is per
// thread and brpc has exactly two of these pools.
struct TlsCache {
    IdleNode* list[MemoryPool::MAX_SIZE_CLASSES];
    int num[MemoryPool::MAX_SIZE_CLASSES];
    int64_t bytes;
    // Compared against MemoryPool::_generation; 0 means never used, which
    // never matches because a live pool's generation starts at 1.
    uint64_t generation;
    bool registered;
};

__thread TlsCache tls_caches[MemoryPool::MAX_POOLS] = {};

butil::atomic<int> g_next_tls_slot(0);

}  // namespace

MemoryPool::MemoryPool()
    : _backend(nullptr)
    , _tls_slot(g_next_tls_slot.fetch_add(1, butil::memory_order_relaxed))
    , _region_num(0)
    , _reserved_bytes(0)
    , _in_use_bytes(0)
    , _generation(1) {
    CHECK_LT(_tls_slot, MAX_POOLS)
        << "Too many MemoryPool instances, raise MemoryPool::MAX_POOLS";
    for (int i = 0; i < MAX_REGIONS; ++i) {
        _regions[i].start.store(0, butil::memory_order_relaxed);
        _regions[i].size = 0;
        _regions[i].lkey = 0;
        _regions[i].size_class = -1;
        _regions[i].owned = false;
    }
}

MemoryPool::~MemoryPool() {
    // Only reached by a pool that is not a process-lifetime singleton. The
    // thread_atexit() handler registered below outlives such a pool and would
    // dereference it, so a pool meant to be destroyed must be the only one
    // whose blocks any live thread has cached -- in practice, a unit test on
    // one thread. The slot is not given back: a later pool reusing it would
    // inherit this one's cached pointers.
    Destroy();
}

int MemoryPool::Init(MemoryBackend* backend,
                     const MemoryPoolOptions& options) {
    if (_backend) {
        LOG(ERROR) << "Do not initialize a MemoryPool repeatedly";
        errno = EINVAL;
        return -1;
    }
    if (backend == nullptr || options.size_class_shifts == nullptr ||
        options.num_size_classes <= 0 ||
        options.num_size_classes > MAX_SIZE_CLASSES) {
        LOG(ERROR) << "Invalid MemoryPoolOptions: num_size_classes="
                   << options.num_size_classes << " must be in [1, "
                   << MAX_SIZE_CLASSES << "]";
        errno = EINVAL;
        return -1;
    }
    for (int i = 0; i < options.num_size_classes; ++i) {
        const int shift = options.size_class_shifts[i];
        if (shift < 0 || shift >= 63 ||
            (i > 0 && shift <= options.size_class_shifts[i - 1])) {
            LOG(ERROR) << "Invalid MemoryPoolOptions: size_class_shifts must"
                          " be ascending and in [0, 63), got " << shift
                       << " at " << i;
            errno = EINVAL;
            return -1;
        }
    }
    if (options.max_regions < 1 || options.max_regions > MAX_REGIONS) {
        LOG(ERROR) << "Invalid MemoryPoolOptions: max_regions="
                   << options.max_regions << " must be in [1, " << MAX_REGIONS
                   << "]";
        errno = EINVAL;
        return -1;
    }
    if (options.region_size == 0 && options.blocks_per_region == 0 &&
        !options.user_specified_memory) {
        LOG(ERROR) << "Invalid MemoryPoolOptions: one of region_size and"
                      " blocks_per_region must be set";
        errno = EINVAL;
        return -1;
    }
    _options = options;
    _backend = backend;
    return 0;
}

void MemoryPool::Destroy() {
    if (_backend == nullptr) {
        return;
    }

    const int num = _region_num.load(butil::memory_order_acquire);
    for (int i = 0; i < num; ++i) {
        const uintptr_t start =
            _regions[i].start.load(butil::memory_order_relaxed);
        if (start == 0) {
            continue;
        }
        _backend->Deregister((void*)start);
        if (_regions[i].owned) {
            _backend->Free((void*)start);
        }
        _regions[i].start.store(0, butil::memory_order_relaxed);
    }
    _region_num.store(0, butil::memory_order_release);

    for (int c = 0; c < _options.num_size_classes; ++c) {
        IdleNode* node = _classes[c].idle_list;
        while (node) {
            IdleNode* const next = node->next;
            butil::return_object<IdleNode>(node);
            node = next;
        }
        _classes[c].idle_list = nullptr;
        _classes[c].idle_bytes = 0;
    }
    _reserved_bytes = 0;
    _in_use_bytes.store(0, butil::memory_order_relaxed);
    _backend = nullptr;

    // Whatever any thread still has cached points into the regions just
    // freed. Bumping the generation makes each of them notice and drop it on
    // next use instead of handing out a dangling pointer.
    _generation.fetch_add(1, butil::memory_order_relaxed);
}

int MemoryPool::SizeClassOf(size_t size) const {
    for (int i = 0; i < _options.num_size_classes; ++i) {
        if (size <= ((size_t)1 << _options.size_class_shifts[i])) {
            return i;
        }
    }
    return -1;
}

// No lock. The array is append-only and a published slot never changes
// except for `start' being zeroed, so the worst a concurrent RemoveRegion()
// can do is return the lkey of a buffer the caller is deregistering while
// still sending from it -- which is a bug the caller has to fix anyway, and a
// much bigger one than a stale lkey.
const MemoryPool::Region* MemoryPool::FindRegion(const void* buf) const {
    const uintptr_t addr = (uintptr_t)buf;
    const int num = _region_num.load(butil::memory_order_acquire);
    for (int i = 0; i < num; ++i) {
        const uintptr_t start =
            _regions[i].start.load(butil::memory_order_relaxed);
        // A tombstoned slot; keep going rather than stopping at the first
        // hole, because RemoveRegion() can leave one in the middle.
        if (start == 0) {
            continue;
        }
        if (addr >= start && addr < start + _regions[i].size) {
            return &_regions[i];
        }
    }
    return nullptr;
}

uint32_t MemoryPool::GetLKey(const void* buf, bool* is_block_region) const {
    if (_backend == nullptr || buf == nullptr) {
        return 0;
    }
    const Region* const region = FindRegion(buf);
    if (region == nullptr) {
        return 0;
    }
    if (is_block_region) {
        *is_block_region = (region->size_class >= 0);
    }
    return region->lkey;
}

int MemoryPool::BlockSizeClassOf(const void* buf) const {
    if (_backend == nullptr || buf == nullptr) {
        return -1;
    }
    const Region* const region = FindRegion(buf);
    return region ? region->size_class : -1;
}

int MemoryPool::AddRegionLocked(void* base, size_t size, int size_class,
                                uint32_t lkey, bool owned) {
    const int index = _region_num.load(butil::memory_order_relaxed);
    if (index >= _options.max_regions) {
        errno = ENOSPC;
        return -1;
    }
    Region& region = _regions[index];
    region.size = size;
    region.lkey = lkey;
    region.size_class = size_class;
    region.owned = owned;
    region.start.store((uintptr_t)base, butil::memory_order_release);
    // Publish last: a reader that sees the new count sees the fields above.
    _region_num.store(index + 1, butil::memory_order_release);
    return 0;
}

bool MemoryPool::ExtendLocked(int size_class, size_t requested_size) {
    const size_t block_size = SizeOfClass(size_class);
    size_t region_size = requested_size;
    if (region_size == 0) {
        region_size = _options.region_size;
    }
    if (region_size == 0) {
        region_size = block_size * _options.blocks_per_region;
    }
    if (_options.max_region_size && region_size > _options.max_region_size) {
        region_size = _options.max_region_size;
    }
    if (region_size < block_size) {
        // A size class bigger than the configured region.
        region_size = block_size;
    }
    if (_options.max_bytes > 0) {
        const int64_t remaining = _options.max_bytes - _reserved_bytes;
        if (remaining < (int64_t)block_size) {
            LOG_EVERY_SECOND(WARNING)
                << "Memory pool over " << _backend->name() << " is full ("
                << _reserved_bytes << " bytes reserved)";
            errno = ENOMEM;
            return false;
        }
        if (region_size > (size_t)remaining) {
            region_size = (size_t)remaining;
        }
    }
    // Whole blocks only, so the last one cannot run past the region.
    region_size -= region_size % block_size;

    if (_region_num.load(butil::memory_order_relaxed) >=
        _options.max_regions) {
        LOG_EVERY_SECOND(ERROR)
            << "Memory pool over " << _backend->name() << " reached its "
            << _options.max_regions << "-region limit, raise the max-regions "
            << "or the region-size flag";
        errno = ENOMEM;
        return false;
    }

    IdleNode* const node = butil::get_object<IdleNode>();
    if (node == nullptr) {
        PLOG_EVERY_SECOND(ERROR) << "Memory not enough";
        errno = ENOMEM;
        return false;
    }

    // Alloc() and Register() take milliseconds, and the older per-block pool
    // ran them with the lock dropped for exactly that reason. That trade
    // flips once a region serves many allocations instead of one: dropping
    // the lock lets every thread that found the class empty reserve a whole
    // region of its own, and a region here is hundreds of MB.
    void* const base = _backend->Alloc(region_size);
    if (base == nullptr) {
        butil::return_object<IdleNode>(node);
        errno = ENOMEM;
        return false;
    }
    const uint32_t lkey = _backend->Register(base, region_size);
    if (lkey == 0) {
        _backend->Free(base);
        butil::return_object<IdleNode>(node);
        errno = EINVAL;
        return false;
    }
    if (AddRegionLocked(base, region_size, size_class, lkey, true) != 0) {
        _backend->Deregister(base);
        _backend->Free(base);
        butil::return_object<IdleNode>(node);
        errno = ENOMEM;
        return false;
    }
    _reserved_bytes += (int64_t)region_size;

    // One node for the whole region, not one per block. Blocks are carved off
    // the front as they are asked for.
    node->start = base;
    node->len = region_size;
    node->next = _classes[size_class].idle_list;
    _classes[size_class].idle_list = node;
    _classes[size_class].idle_bytes += region_size;
    return true;
}

int MemoryPool::Reserve(int size_class, size_t region_size) {
    if (_backend == nullptr || size_class < 0 ||
        size_class >= _options.num_size_classes) {
        errno = EINVAL;
        return -1;
    }
    ClassInfo& ci = _classes[size_class];
    BAIDU_SCOPED_LOCK(ci.mutex);
    BAIDU_SCOPED_LOCK(_extend_mutex);
    return ExtendLocked(size_class, region_size) ? 0 : -1;
}

int MemoryPool::AddBlockRegion(void* base, size_t size, int size_class,
                               bool owned) {
    if (_backend == nullptr || base == nullptr || size_class < 0 ||
        size_class >= _options.num_size_classes) {
        errno = EINVAL;
        return -1;
    }
    const size_t block_size = SizeOfClass(size_class);
    size -= size % block_size;
    if (size == 0) {
        errno = EINVAL;
        return -1;
    }
    IdleNode* const node = butil::get_object<IdleNode>();
    if (node == nullptr) {
        PLOG_EVERY_SECOND(ERROR) << "Memory not enough";
        errno = ENOMEM;
        return -1;
    }
    const uint32_t lkey = _backend->Register(base, size);
    if (lkey == 0) {
        butil::return_object<IdleNode>(node);
        errno = EINVAL;
        return -1;
    }

    ClassInfo& ci = _classes[size_class];
    BAIDU_SCOPED_LOCK(ci.mutex);
    BAIDU_SCOPED_LOCK(_extend_mutex);
    if (AddRegionLocked(base, size, size_class, lkey, owned) != 0) {
        _backend->Deregister(base);
        butil::return_object<IdleNode>(node);
        LOG_EVERY_SECOND(ERROR)
            << "Memory pool over " << _backend->name() << " reached its "
            << _options.max_regions << "-region limit";
        errno = ENOMEM;
        return -1;
    }
    _reserved_bytes += (int64_t)size;
    node->start = base;
    node->len = size;
    node->next = ci.idle_list;
    ci.idle_list = node;
    ci.idle_bytes += size;
    return 0;
}

int MemoryPool::AddUserRegion(void* base, size_t size, uint32_t lkey) {
    if (_backend == nullptr || base == nullptr || size == 0 || lkey == 0) {
        errno = EINVAL;
        return -1;
    }
    BAIDU_SCOPED_LOCK(_extend_mutex);
    return AddRegionLocked(base, size, -1, lkey, false);
}

int MemoryPool::RemoveRegion(const void* base) {
    if (_backend == nullptr || base == nullptr) {
        errno = EINVAL;
        return -1;
    }
    BAIDU_SCOPED_LOCK(_extend_mutex);
    const int num = _region_num.load(butil::memory_order_relaxed);
    for (int i = 0; i < num; ++i) {
        if (_regions[i].size_class >= 0 ||
            _regions[i].start.load(butil::memory_order_relaxed) !=
                (uintptr_t)base) {
            continue;
        }
        // Tombstone instead of compacting: FindRegion() reads the array
        // without a lock, and it can only do that safely as long as a
        // published slot never changes its address.
        _regions[i].start.store(0, butil::memory_order_relaxed);
        _backend->Deregister((void*)base);
        return 0;
    }
    errno = ERANGE;
    return -1;
}

void MemoryPool::DropTlsCache() {
    TlsCache& tls = tls_caches[_tls_slot];
    for (int c = 0; c < MAX_SIZE_CLASSES; ++c) {
        IdleNode* node = tls.list[c];
        while (node) {
            IdleNode* const next = node->next;
            butil::return_object<IdleNode>(node);
            node = next;
        }
        tls.list[c] = nullptr;
        tls.num[c] = 0;
    }
    tls.bytes = 0;
}

// Registered with thread_atexit() so a thread that dies with a warm cache
// gives its blocks back instead of stranding them.
void MemoryPool::RecycleTlsCache() {
    TlsCache& tls = tls_caches[_tls_slot];
    if (_backend == nullptr ||
        tls.generation != _generation.load(butil::memory_order_relaxed)) {
        DropTlsCache();
        return;
    }
    for (int c = 0; c < _options.num_size_classes; ++c) {
        if (tls.list[c] == nullptr) {
            continue;
        }
        IdleNode* tail = tls.list[c];
        while (tail->next) {
            tail = tail->next;
        }
        ClassInfo& ci = _classes[c];
        BAIDU_SCOPED_LOCK(ci.mutex);
        tail->next = ci.idle_list;
        ci.idle_list = tls.list[c];
        ci.idle_bytes += (size_t)tls.num[c] * SizeOfClass(c);
        tls.list[c] = nullptr;
        tls.num[c] = 0;
    }
    tls.bytes = 0;
}

void MemoryPool::RecycleTlsCacheThunk(void* arg) {
    static_cast<MemoryPool*>(arg)->RecycleTlsCache();
}

void MemoryPool::SyncTlsCache() {
    TlsCache& tls = tls_caches[_tls_slot];
    const uint64_t gen = _generation.load(butil::memory_order_relaxed);
    if (BAIDU_UNLIKELY(tls.generation != gen)) {
        DropTlsCache();
        tls.generation = gen;
    }
    if (BAIDU_UNLIKELY(!tls.registered)) {
        tls.registered = true;
        butil::thread_atexit(RecycleTlsCacheThunk, this);
    }
}

void* MemoryPool::AllocFromClass(int size_class) {
    SyncTlsCache();
    TlsCache& tls = tls_caches[_tls_slot];
    if (tls.list[size_class]) {
        IdleNode* const node = tls.list[size_class];
        tls.list[size_class] = node->next;
        void* const ptr = node->start;
        butil::return_object<IdleNode>(node);
        --tls.num[size_class];
        tls.bytes -= (int64_t)SizeOfClass(size_class);
        return ptr;
    }

    const size_t block_size = SizeOfClass(size_class);
    ClassInfo& ci = _classes[size_class];
    BAIDU_SCOPED_LOCK(ci.mutex);
    if (ci.idle_list == nullptr) {
        if (_options.user_specified_memory) {
            LOG_EVERY_SECOND(ERROR)
                << "Memory pool over " << _backend->name() << " is out of "
                << block_size << "-byte blocks and may only be extended by "
                << "the user";
            errno = ENOMEM;
            return nullptr;
        }
        BAIDU_SCOPED_LOCK(_extend_mutex);
        if (ci.idle_list == nullptr && !ExtendLocked(size_class, 0)) {
            return nullptr;  // errno set by ExtendLocked
        }
    }

    IdleNode* node = ci.idle_list;
    void* const ptr = node->start;
    if (node->len > block_size) {
        node->start = (char*)node->start + block_size;
        node->len -= block_size;
    } else {
        ci.idle_list = node->next;
        butil::return_object<IdleNode>(node);
    }
    ci.idle_bytes -= block_size;

    // Take whatever single blocks are already on the global list back to the
    // thread cache, so a thread that keeps allocating stops touching this
    // mutex. Extents are left alone: splitting one here would only move the
    // work, and the head of the list is an extent exactly when the region is
    // fresh, i.e. when there is nothing to be gained anyway.
    const int want = _options.tls_cache_num / 2;
    while (tls.num[size_class] < want &&
           tls.bytes + (int64_t)block_size <= _options.tls_cache_bytes) {
        node = ci.idle_list;
        if (node == nullptr || node->len != block_size) {
            break;
        }
        ci.idle_list = node->next;
        ci.idle_bytes -= block_size;
        node->next = tls.list[size_class];
        tls.list[size_class] = node;
        ++tls.num[size_class];
        tls.bytes += (int64_t)block_size;
    }
    return ptr;
}

void* MemoryPool::AllocBlock(size_t size, uint32_t* lkey) {
    if (_backend == nullptr) {
        errno = ENODEV;
        return nullptr;
    }
    if (size == 0) {
        errno = EINVAL;
        return nullptr;
    }
    const int size_class = SizeClassOf(size);
    if (size_class < 0) {
        errno = E2BIG;
        return nullptr;
    }
    void* const ptr = AllocFromClass(size_class);
    if (ptr == nullptr) {
        return nullptr;  // errno set
    }
    _in_use_bytes.fetch_add((int64_t)SizeOfClass(size_class),
                            butil::memory_order_relaxed);
    if (lkey) {
        const Region* const region = FindRegion(ptr);
        if (region == nullptr) {
            LOG(FATAL) << "Block " << ptr << " belongs to no region";
            errno = ERANGE;
            return nullptr;
        }
        *lkey = region->lkey;
    }
    return ptr;
}

int MemoryPool::DeallocBlock(void* buf) {
    if (_backend == nullptr || buf == nullptr) {
        errno = EINVAL;
        return -1;
    }
    const Region* const region = FindRegion(buf);
    if (region == nullptr || region->size_class < 0) {
        errno = ERANGE;
        return -1;
    }
    const int size_class = region->size_class;
    const size_t block_size = SizeOfClass(size_class);
    const uintptr_t offset =
        (uintptr_t)buf - region->start.load(butil::memory_order_relaxed);
    if ((offset & (block_size - 1)) != 0) {
        // An interior pointer. Blocks are carved off the region base in
        // order, so a misaligned one was never handed out, and recycling it
        // would make the pool serve overlapping blocks from then on.
        errno = ERANGE;
        return -1;
    }

    IdleNode* const node = butil::get_object<IdleNode>();
    if (node == nullptr) {
        // Leak the block rather than the pool's invariants.
        PLOG_EVERY_SECOND(ERROR) << "Memory not enough";
        return 0;
    }
    node->start = buf;
    node->len = block_size;
    _in_use_bytes.fetch_sub((int64_t)block_size, butil::memory_order_relaxed);

    SyncTlsCache();
    TlsCache& tls = tls_caches[_tls_slot];
    node->next = tls.list[size_class];
    if (tls.num[size_class] < _options.tls_cache_num &&
        tls.bytes + (int64_t)block_size <= _options.tls_cache_bytes) {
        tls.list[size_class] = node;
        ++tls.num[size_class];
        tls.bytes += (int64_t)block_size;
        return 0;
    }

    // Cache full for this class (or for this thread): give the whole list
    // back in one go, so the next tls_cache_num frees are lock-free again.
    IdleNode* tail = node;
    int count = 1;
    while (tail->next) {
        tail = tail->next;
        ++count;
    }
    ClassInfo& ci = _classes[size_class];
    {
        BAIDU_SCOPED_LOCK(ci.mutex);
        tail->next = ci.idle_list;
        ci.idle_list = node;
        ci.idle_bytes += (size_t)count * block_size;
    }
    tls.bytes -= (int64_t)tls.num[size_class] * (int64_t)block_size;
    tls.list[size_class] = nullptr;
    tls.num[size_class] = 0;
    return 0;
}

int64_t MemoryPool::reserved_bytes() const {
    if (_backend == nullptr) {
        return 0;
    }
    BAIDU_SCOPED_LOCK(_extend_mutex);
    return _reserved_bytes;
}

size_t MemoryPool::idle_bytes(int size_class) const {
    if (_backend == nullptr || size_class < 0 ||
        size_class >= _options.num_size_classes) {
        return 0;
    }
    BAIDU_SCOPED_LOCK(_classes[size_class].mutex);
    return _classes[size_class].idle_bytes;
}

void MemoryPool::DumpInfo(std::ostream& os) const {
    if (_backend == nullptr) {
        os << "The memory pool is not initialized\n";
        return;
    }
    os << "***************** " << _backend->name()
       << " memory pool info *****************\n"
       << "reserved=" << reserved_bytes()
       << " in_use=" << in_use_bytes() << "\n"
       << "Region Info:\n";
    const int num = _region_num.load(butil::memory_order_acquire);
    for (int i = 0; i < num; ++i) {
        const uintptr_t start =
            _regions[i].start.load(butil::memory_order_relaxed);
        os << "\tRegion " << i << ": base=" << (void*)start
           << " size=" << _regions[i].size << " lkey=" << _regions[i].lkey;
        if (_regions[i].size_class >= 0) {
            os << " block_size=" << SizeOfClass(_regions[i].size_class);
        } else {
            os << " block_size=n/a(user)";
        }
        os << (start == 0 ? " (removed)\n" : "\n");
    }
    os << "Idle List Info:\n";
    for (int c = 0; c < _options.num_size_classes; ++c) {
        const size_t idle = idle_bytes(c);
        if (idle != 0) {
            os << "\tFor block size " << SizeOfClass(c) << ": " << idle
               << "\n";
        }
    }
    os << "*****************************************************\n";
}

}  // namespace rdma
}  // namespace brpc
