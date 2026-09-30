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

#if BRPC_WITH_RDMA

#include <errno.h>
#include <stdlib.h>
#include <gflags/gflags.h>
#include "butil/iobuf.h"
#include "butil/logging.h"
#include "brpc/rdma/block_pool.h"
#include "brpc/rdma/memory_pool.h"

namespace brpc {
namespace rdma {

DEFINE_int32(rdma_memory_pool_initial_size_mb, 1024,
             "Initial size of memory pool for RDMA (MB)");
DEFINE_int32(rdma_memory_pool_increase_size_mb, 1024,
             "Increased size of memory pool for RDMA (MB)");
DEFINE_int32(rdma_memory_pool_max_regions, 3, "Max number of regions");
DEFINE_int32(rdma_memory_pool_buckets, 4,
             "Deprecated and ignored. The pool used to sharpen its idle "
             "lists into buckets to cut contention; that job now belongs to "
             "the per-thread block cache, which takes no lock at all.");
DEFINE_int32(rdma_memory_pool_tls_cache_num, 128, "Number of cached block in tls");
DEFINE_bool(rdma_memory_pool_user_specified_memory, false,
            "If true, the user must call UserExtendBlockPool() to extend "
            "memory. bRPC will not handle memory extension.");
DEFINE_string(rdma_recv_block_type, "default", "Default size type for recv WR: "
              "default(8KB - 32B)/large(64KB - 32B)/huge(2MB - 32B)");

// Number of bytes in 1MB
static const size_t BYTES_IN_MB = 1048576;

static const int BLOCK_DEFAULT = 0; // 8KB
static const int BLOCK_LARGE = 1;  // 64KB
static const int BLOCK_HUGE = 2;  // 2MB
static const int BLOCK_SIZE_COUNT = 3;
// log2 of the three block sizes above. This is the whole of what makes this
// pool different from the GDR one in device_memory.cpp -- everything else
// lives in MemoryPool.
static const int BLOCK_SIZE_SHIFT[BLOCK_SIZE_COUNT] = { 13, 16, 21 };
static size_t g_block_size[BLOCK_SIZE_COUNT] = { 8192, 65536, 2 * BYTES_IN_MB };

static const int32_t RDMA_MEMORY_POOL_MIN_REGIONS = 1;
static const int32_t RDMA_MEMORY_POOL_MAX_REGIONS = 16;

static const int32_t RDMA_MEMORY_POOL_MIN_SIZE = 32;  // 16MB
static const int32_t RDMA_MEMORY_POOL_MAX_SIZE = 1048576;  // 1TB

static RegisterCallback g_cb = nullptr;

// Ordinary host memory, registered through whatever callback the caller gave
// InitBlockPool() -- in brpc that is ibv_reg_mr() via rdma_helper.cpp.
//
// Deregistration is a no-op because block_pool has never had a callback for
// it: regions live until the process does, and the one path that frees them
// (DestroyBlockPool, for unit tests) never had a PD to deregister against.
class HostBlockBackend : public MemoryBackend {
public:
    const char* name() const override { return "host"; }

    void* Alloc(size_t size) override {
        void* ptr = nullptr;
        if (posix_memalign(&ptr, 4096, size) != 0) {
            PLOG_EVERY_SECOND(ERROR) << "Memory not enough";
            return nullptr;
        }
        return ptr;
    }

    void Free(void* ptr) override { free(ptr); }

    uint32_t Register(void* ptr, size_t size) override {
        return g_cb ? g_cb(ptr, size) : 0;
    }

    void Deregister(void*) override { }
};

static HostBlockBackend g_backend;

// Leaked on purpose: threads register a MemoryPool::RecycleTlsCache() handler
// with thread_atexit(), and those outlive any static destructor order we
// could rely on. DestroyBlockPool() tears the *contents* down instead.
static MemoryPool& GetPool() {
    static MemoryPool* pool = new MemoryPool;
    return *pool;
}

bool InitBlockPool(RegisterCallback cb) {
    if (!cb) {
        errno = EINVAL;
        return false;
    }
    if (g_cb) {
        LOG(WARNING) << "Do not initialize block pool repeatedly";
        errno = EINVAL;
        return false;
    }
    if (FLAGS_rdma_memory_pool_max_regions < RDMA_MEMORY_POOL_MIN_REGIONS ||
        FLAGS_rdma_memory_pool_max_regions > RDMA_MEMORY_POOL_MAX_REGIONS) {
        LOG(WARNING) << "rdma_memory_pool_max_regions("
                     << FLAGS_rdma_memory_pool_max_regions << ") not in ["
                     << RDMA_MEMORY_POOL_MIN_REGIONS << ","
                     << RDMA_MEMORY_POOL_MAX_REGIONS << "]!";
        errno = EINVAL;
        return false;
    }
    if (FLAGS_rdma_memory_pool_initial_size_mb < RDMA_MEMORY_POOL_MIN_SIZE ||
        FLAGS_rdma_memory_pool_initial_size_mb > RDMA_MEMORY_POOL_MAX_SIZE) {
        LOG(WARNING) << "rdma_memory_pool_initial_size_mb("
                     << FLAGS_rdma_memory_pool_initial_size_mb << ") not in ["
                     << RDMA_MEMORY_POOL_MIN_SIZE << ","
                     << RDMA_MEMORY_POOL_MAX_SIZE << "]!";
        errno = EINVAL;
        return false;
    }
    if (FLAGS_rdma_memory_pool_increase_size_mb < RDMA_MEMORY_POOL_MIN_SIZE ||
        FLAGS_rdma_memory_pool_increase_size_mb > RDMA_MEMORY_POOL_MAX_SIZE) {
        LOG(WARNING) << "rdma_memory_pool_increase_size_mb("
                     << FLAGS_rdma_memory_pool_increase_size_mb << ") not in ["
                     << RDMA_MEMORY_POOL_MIN_SIZE << ","
                     << RDMA_MEMORY_POOL_MAX_SIZE << "]!";
        errno = EINVAL;
        return false;
    }

    MemoryPoolOptions options;
    options.size_class_shifts = BLOCK_SIZE_SHIFT;
    options.num_size_classes = BLOCK_SIZE_COUNT;
    options.max_regions = FLAGS_rdma_memory_pool_max_regions;
    options.region_size =
        (size_t)FLAGS_rdma_memory_pool_increase_size_mb * BYTES_IN_MB;
    options.tls_cache_num = FLAGS_rdma_memory_pool_tls_cache_num;
    // The old pool cached default-size blocks only, so a thread could hold
    // at most tls_cache_num * 8KB. Keeping that as the byte budget keeps the
    // per-thread footprint where it was while letting 64KB blocks -- which
    // IOBuf does use -- into the cache as well. A 2MB block never fits, and
    // a thread churning those is not the case the cache is for.
    options.tls_cache_bytes =
        (int64_t)FLAGS_rdma_memory_pool_tls_cache_num * g_block_size[0];
    options.user_specified_memory = FLAGS_rdma_memory_pool_user_specified_memory;

    g_cb = cb;
    if (GetPool().Init(&g_backend, options) != 0) {
        g_cb = nullptr;
        return false;
    }

    if (FLAGS_rdma_memory_pool_user_specified_memory) {
        return true;
    }
    const int block_type = GetRdmaBlockType();
    if (block_type < 0) {
        GetPool().Destroy();
        g_cb = nullptr;
        errno = EINVAL;
        return false;
    }
    LOG(INFO) << "Start extend rdma memory "
              << FLAGS_rdma_memory_pool_initial_size_mb << "MB";
    if (GetPool().Reserve(
            block_type,
            (size_t)FLAGS_rdma_memory_pool_initial_size_mb * BYTES_IN_MB) != 0) {
        GetPool().Destroy();
        g_cb = nullptr;
        return false;
    }
    return true;
}

void* ExtendBlockPoolByUser(void* region_base, size_t region_size,
                            int block_type) {
    if (!FLAGS_rdma_memory_pool_user_specified_memory) {
        LOG_EVERY_SECOND(ERROR) << "User extend memory is disabled";
        free(region_base);
        return nullptr;
    }
    if (reinterpret_cast<uintptr_t>(region_base) % 4096 != 0) {
        LOG_EVERY_SECOND(ERROR) << "region_base must be 4096 aligned";
        errno = EINVAL;
        free(region_base);
        return nullptr;
    }
    // `region_size' is in MB, as it has always been.
    if (GetPool().AddBlockRegion(region_base, region_size * BYTES_IN_MB,
                                 block_type, true) != 0) {
        free(region_base);
        return nullptr;
    }
    return region_base;
}

void* AllocBlock(size_t size) {
    if (size == 0 || size > g_block_size[BLOCK_SIZE_COUNT - 1]) {
        errno = EINVAL;
        return nullptr;
    }
    return GetPool().AllocBlock(size);
}

int DeallocBlock(void* buf) {
    return GetPool().DeallocBlock(buf);
}

uint32_t GetRegionId(const void* buf) {
    return GetPool().GetLKey(buf);
}

size_t GetBlockSize(int type) {
    return g_block_size[type];
}

size_t GetRdmaBlockSize() {
    if (FLAGS_rdma_recv_block_type == "default") {
        return GetBlockSize(0);
    } else if (FLAGS_rdma_recv_block_type == "large") {
        return GetBlockSize(1);
    } else if (FLAGS_rdma_recv_block_type == "huge") {
        return GetBlockSize(2);
    } else {
        LOG(ERROR) << "rdma_recv_block_type incorrect "
                   << "(valid value: default/large/huge)";
        return 0;
    }
}

int GetRdmaBlockType() {
    if (FLAGS_rdma_recv_block_type == "default") {
        return BLOCK_DEFAULT;
    } else if (FLAGS_rdma_recv_block_type == "large") {
        return BLOCK_LARGE;
    } else if (FLAGS_rdma_recv_block_type == "huge") {
        return BLOCK_HUGE;
    } else {
        LOG(ERROR) << "rdma_recv_block_type incorrect "
                   << "(valid value: default/large/huge)";
        return -1;
    }
}

void DumpMemoryPoolInfo(std::ostream& os) {
    GetPool().DumpInfo(os);
}

// Just for UT
void DestroyBlockPool() {
    GetPool().Destroy();
    g_cb = nullptr;
}

// Just for UT
int GetBlockType(void* buf) {
    return GetPool().BlockSizeClassOf(buf);
}

// Just for UT
size_t GetGlobalLen(int block_type) {
    return GetPool().idle_bytes(block_type);
}

// Just for UT
size_t GetRegionNum() {
    return GetPool().num_regions();
}

}  // namespace rdma
}  // namespace brpc

#endif  // if BRPC_WITH_RDMA
