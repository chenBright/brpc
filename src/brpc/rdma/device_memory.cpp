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

#include "brpc/rdma/device_memory.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <gflags/gflags.h>
#include "butil/atomicops.h"
#include "butil/logging.h"
#include "butil/scoped_lock.h"
#include "butil/synchronization/lock.h"
#include "brpc/rdma/memory_pool.h"

#if BRPC_WITH_GDR
#include <cuda_runtime.h>
#endif
#if BRPC_WITH_RDMA
#include "brpc/rdma/rdma_helper.h"
#endif

namespace brpc {
namespace rdma {

DEFINE_bool(rdma_enable_gdr, false,
            "Enable GPU Direct RDMA. Requires building with --config=gdr, a "
            "CUDA device, and a NIC with peer-memory support.");

DEFINE_string(rdma_attachment_memory, "device",
              "What this process's DeviceAttachments are made of: \"device\" "
              "for GPU memory, \"host\" for ordinary registered host memory. "
              "Governs what append_new() hands out and what the second "
              "channel's receive queue lands incoming bytes in -- not what "
              "append_user_data() accepts, which is either kind either way. "
              "Purely local: the peer is never told and does not need to be. "
              "\"device\" on a host with no usable GPU is a configuration "
              "error and the process exits rather than starting degraded.");

DEFINE_int32(rdma_gdr_recv_block_size, 1048576,
             "Size in bytes of each device block posted to the device QP's "
             "receive queue. Advertised to the peer during the v3 handshake.");

DEFINE_int64(rdma_gdr_max_device_bytes, 4L * 1024 * 1024 * 1024,
             "Upper bound on device memory reserved by the GDR block pool. "
             "Counts whole regions, including the part of a region not "
             "handed out yet. Allocation fails past it.");

DEFINE_int32(rdma_gdr_device_id, -1,
             "CUDA device ordinal all GDR memory is allocated on. -1 means "
             "the process default (device 0 unless something called "
             "cudaSetDevice first). Pick the GPU under the same PCIe switch "
             "as --rdma_device, otherwise every transfer crosses the host "
             "bridge and GDR buys you nothing.");

DEFINE_int32(rdma_gdr_region_size_mb, 256,
             "Upper bound on the size of one GDR pool region, i.e. of one "
             "cudaMalloc + ibv_reg_mr. A region is carved into blocks of a "
             "single size class; smaller classes take proportionally smaller "
             "regions.");

DEFINE_int32(rdma_gdr_max_regions, 32,
             "Max number of GDR regions, counting both pool regions and "
             "buffers passed to RegisterDeviceMemory(). Kept small on "
             "purpose: GetDeviceLKey() resolves an address by scanning this "
             "array without a lock.");

DEFINE_int32(rdma_gdr_tls_cache_num, 128,
             "Max number of free blocks of one size class cached per thread.");

DEFINE_int64(rdma_gdr_tls_cache_bytes, 8L * 1024 * 1024,
             "Max device bytes cached per thread across all size classes. "
             "The real ceiling is this times the number of threads that "
             "touch device memory, so raise it with that in mind.");

// Test hook, mirroring rdma_helper.cpp's g_skip_rdma_init: allocate host
// memory instead of calling into CUDA, and skip ibv_reg_mr(). This is what
// lets the pool, DeviceAttachment and the pending list be exercised on a CI
// machine with neither a GPU nor a RoCE card -- and it is the only way the
// pool works at all in a build without --config=gdr. Never set it outside
// unit tests: every pointer handed out is then a host pointer, and the whole
// point of the surrounding code is that those pointers are never touched.
bool g_skip_device_alloc_for_test = false;

namespace {

// 4KB .. 1GB.
const int MIN_SIZE_CLASS_SHIFT = 12;
const int MAX_SIZE_CLASS_SHIFT = 30;
const int SIZE_CLASS_COUNT = MAX_SIZE_CLASS_SHIFT - MIN_SIZE_CLASS_SHIFT + 1;
const int SIZE_CLASS_SHIFTS[SIZE_CLASS_COUNT] = {
    12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30
};

// How many blocks a region is sized for, before the byte caps clamp it. Big
// enough that cudaMalloc and ibv_reg_mr are amortized away, small enough that
// a size class used once does not strand much.
const size_t BLOCKS_PER_REGION = 64;

// A fake but non-zero lkey, so test code can tell "registered" from "not
// registered" without a card. Real lkeys come from ibv_reg_mr().
const uint32_t FAKE_LKEY = 0xdeadbeef;

// Where user regions go once the pool's region table is full. Untouched --
// not even locked -- while g_overflow_num is 0, which is the case for every
// workload that registers a bounded number of buffers.
struct UserRegion {
    uintptr_t start;
    size_t size;
    uint32_t lkey;
};
std::map<uintptr_t, UserRegion>* g_overflow = nullptr;
butil::Mutex* g_overflow_mutex = nullptr;
butil::atomic<int> g_overflow_num(0);

bool g_gdr_available = false;

// --rdma_attachment_memory, parsed once at startup. Everything above the
// backends is written against "a registered block", and this is the only
// thing that decides which backend produces one.
bool g_attachment_is_host = false;

// Whether asking CUDA about a pointer is worth doing. False in a build with
// no GDR, on a host with no CUDA device, and -- deliberately -- in `host'
// attachment mode on a host that never brought CUDA up. A caller who does
// hold device memory on such a node has to say so itself, through
// DeviceAttachment::append_user_data_with_lkey().
bool g_can_probe_cuda = false;

// The same question for a process whose pool never came up at all, answered
// lazily on first use: -1 unknown, 0 no CUDA device, 1 has one.
//
// Before the TCP fallback existed, "the pool is down" was a sufficient reason
// not to ask CUDA anything -- a device pointer could not be registered then,
// so the query only bought a slower route to the same failure. The fallback
// invalidates that: it memcpy()s from whatever the caller appended, and
// reading GPU memory that way is a segfault rather than a failure. So on a
// node that does have a GPU, the question has to be asked even with no pool.
butil::atomic<int> g_lazy_cuda_probe(-1);

int SizeClassOf(size_t size) {
    int shift = MIN_SIZE_CLASS_SHIFT;
    while (shift <= MAX_SIZE_CLASS_SHIFT && (1UL << shift) < size) {
        ++shift;
    }
    if (shift > MAX_SIZE_CLASS_SHIFT) {
        return -1;
    }
    return shift - MIN_SIZE_CLASS_SHIFT;
}

#if BRPC_WITH_GDR
// Makes --rdma_gdr_device_id current for the duration of a CUDA call, then
// puts back whatever the caller had.
//
// Needed because cudaMalloc allocates on the *calling thread's* current
// device, and pool regions are allocated from whichever bthread happens to
// run out of blocks. Without this, a process where some threads called
// cudaSetDevice() would scatter its receive blocks across GPUs and register
// them all on one NIC.
class ScopedCudaDevice {
public:
    ScopedCudaDevice() : _saved(-1) {
        if (FLAGS_rdma_gdr_device_id < 0) {
            return;
        }
        cudaError_t err = cudaGetDevice(&_saved);
        if (err != cudaSuccess) {
            LOG(ERROR) << "Fail to cudaGetDevice: " << cudaGetErrorString(err);
            _saved = -1;
            return;
        }
        if (_saved == FLAGS_rdma_gdr_device_id) {
            _saved = -1;  // nothing to restore
            return;
        }
        err = cudaSetDevice(FLAGS_rdma_gdr_device_id);
        if (err != cudaSuccess) {
            LOG(ERROR) << "Fail to cudaSetDevice(" << FLAGS_rdma_gdr_device_id
                       << "): " << cudaGetErrorString(err);
            _saved = -1;
        }
    }

    ~ScopedCudaDevice() {
        if (_saved >= 0) {
            const cudaError_t err = cudaSetDevice(_saved);
            LOG_IF(ERROR, err != cudaSuccess)
                << "Fail to restore cuda device " << _saved << ": "
                << cudaGetErrorString(err);
        }
    }

private:
    DISALLOW_COPY_AND_ASSIGN(ScopedCudaDevice);
    int _saved;
};
#endif

// Registration, shared by both backends and reachable from
// RegisterDeviceMemory() -- which registers a buffer it did not allocate and
// so has no backend to go through.
uint32_t RegisterRaw(void* ptr, size_t size) {
    if (g_skip_device_alloc_for_test) {
        return FAKE_LKEY;
    }
#if BRPC_WITH_RDMA
    return RegisterMemoryForRdma(ptr, size);
#else
    return 0;
#endif
}

void DeregisterRaw(void* ptr) {
    if (g_skip_device_alloc_for_test) {
        return;
    }
#if BRPC_WITH_RDMA
    DeregisterMemoryForRdma(ptr);
#endif
}

class GdrBackend : public MemoryBackend {
public:
    uint32_t Register(void* ptr, size_t size) override {
        return RegisterRaw(ptr, size);
    }
    void Deregister(void* ptr) override { DeregisterRaw(ptr); }
};

// `host' attachment mode, and the only backend a build without --config=gdr
// has. Page-aligning matches what cudaMalloc gives back and keeps the NIC
// off partial pages.
//
// That this mode exists at all is the point of §5.3 in
// docs/cn/gdr_design.md: the second channel carries whatever is registered,
// the peer is never told which, and a node with no GPU can still serve GDR
// traffic. It is also what the unit tests run on.
class HostAttachmentBackend : public GdrBackend {
public:
    const char* name() const override { return "gdr-host"; }

    void* Alloc(size_t size) override {
        void* ptr = nullptr;
        if (posix_memalign(&ptr, 4096, size) != 0) {
            PLOG_EVERY_SECOND(ERROR) << "Memory not enough";
            return nullptr;
        }
        return ptr;
    }

    void Free(void* ptr) override { free(ptr); }
};

class CudaAttachmentBackend : public GdrBackend {
public:
    const char* name() const override { return "gdr-cuda"; }

    void* Alloc(size_t size) override {
#if BRPC_WITH_GDR
        ScopedCudaDevice device_guard;
        void* ptr = nullptr;
        const cudaError_t err = cudaMalloc(&ptr, size);
        if (err != cudaSuccess) {
            LOG(ERROR) << "Fail to cudaMalloc " << size
                       << " bytes: " << cudaGetErrorString(err);
            return nullptr;
        }
        return ptr;
#else
        (void)size;
        return nullptr;
#endif
    }

    void Free(void* ptr) override {
#if BRPC_WITH_GDR
        ScopedCudaDevice device_guard;
        const cudaError_t err = cudaFree(ptr);
        if (err != cudaSuccess) {
            LOG(ERROR) << "Fail to cudaFree: " << cudaGetErrorString(err);
        }
#else
        (void)ptr;
#endif
    }
};

HostAttachmentBackend g_host_backend;
CudaAttachmentBackend g_cuda_backend;

// Leaked on purpose: threads register a MemoryPool::RecycleTlsCache() handler
// with thread_atexit(), and those outlive any static destructor order we
// could rely on. GlobalGdrRelease() tears the *contents* down instead.
MemoryPool& GetPool() {
    static MemoryPool* pool = new MemoryPool;
    return *pool;
}

// Whether this host has any CUDA device at all. Quiet, because `host'
// attachment mode asks purely to decide whether IsDevicePointer() may query
// CUDA, and having no GPU is a perfectly ordinary answer there. Only
// cudaGetDeviceCount(), which reports "no driver" without bringing a context
// up.
bool HasAnyCudaDevice(int* count) {
#if BRPC_WITH_GDR
    int device_count = 0;
    const cudaError_t err = cudaGetDeviceCount(&device_count);
    if (err != cudaSuccess || device_count <= 0) {
        if (count) {
            *count = 0;
        }
        return false;
    }
    if (count) {
        *count = device_count;
    }
    return true;
#else
    if (count) {
        *count = 0;
    }
    return false;
#endif
}

// Whether CUDA can be asked about a pointer in a process whose device pool
// never came up. One cudaGetDeviceCount() per process, cached; it reports
// "no driver" without bringing a CUDA context up, so a node with no GPU pays
// nothing beyond the first call.
bool CanProbeCudaLazily() {
    int v = g_lazy_cuda_probe.load(butil::memory_order_relaxed);
    if (v < 0) {
        v = HasAnyCudaDevice(nullptr) ? 1 : 0;
        g_lazy_cuda_probe.store(v, butil::memory_order_relaxed);
    }
    return v != 0;
}

// Whether a CUDA device is actually usable right now, and the one named by
// --rdma_gdr_device_id in particular. Loud: reaching here means the process
// asked for device memory, so every way of not getting it is fatal.
bool HasUsableCudaDevice() {
#if BRPC_WITH_GDR
    int device_count = 0;
    if (!HasAnyCudaDevice(&device_count)) {
        LOG(ERROR) << "Fail to enable GDR: no usable CUDA device found";
        return false;
    }
    if (FLAGS_rdma_gdr_device_id >= device_count) {
        LOG(ERROR) << "Fail to enable GDR: --rdma_gdr_device_id="
                   << FLAGS_rdma_gdr_device_id << " but this host only has "
                   << device_count << " CUDA device(s)";
        return false;
    }
    if (FLAGS_rdma_gdr_device_id >= 0) {
        // Bring the primary context up here rather than on whichever bthread
        // allocates first: it takes ~100ms and the error is much easier to
        // read at startup.
        const cudaError_t set_err = cudaSetDevice(FLAGS_rdma_gdr_device_id);
        if (set_err != cudaSuccess) {
            LOG(ERROR) << "Fail to enable GDR: cudaSetDevice("
                       << FLAGS_rdma_gdr_device_id << "): "
                       << cudaGetErrorString(set_err);
            return false;
        }
        LOG(INFO) << "GDR memory will be allocated on CUDA device "
                  << FLAGS_rdma_gdr_device_id;
    }
    return true;
#else
    LOG(ERROR) << "Fail to enable GDR: brpc was built without GDR support, "
                  "rebuild with --config=gdr (bazel) or --with-gdr";
    return false;
#endif
}

}  // namespace

bool IsGdrAvailable() {
    return g_gdr_available;
}

bool IsAttachmentMemoryDevice() {
    return g_gdr_available && !g_attachment_is_host;
}

int GlobalGdrInitialize() {
    if (!FLAGS_rdma_enable_gdr) {
        return 0;
    }
    if (g_gdr_available) {
        return 0;
    }
    if (FLAGS_rdma_attachment_memory == "host") {
        g_attachment_is_host = true;
    } else if (FLAGS_rdma_attachment_memory == "device") {
        g_attachment_is_host = false;
    } else {
        LOG(ERROR) << "Fail to enable GDR: --rdma_attachment_memory=\""
                   << FLAGS_rdma_attachment_memory
                   << "\" is neither \"host\" nor \"device\"";
        errno = EINVAL;
        return -1;
    }
    if (!g_skip_device_alloc_for_test) {
        if (g_attachment_is_host) {
            // No GPU needed, and none demanded: this mode is the reason a
            // CPU-only node can still run the second channel. Ask anyway,
            // quietly, because a node that does have a GPU can still send
            // from it and IsDevicePointer() needs to know.
            g_can_probe_cuda = HasAnyCudaDevice(nullptr);
        } else {
            if (!HasUsableCudaDevice()) {
                errno = ENODEV;
                return -1;
            }
            g_can_probe_cuda = true;
        }
    }
    if (FLAGS_rdma_gdr_recv_block_size <= 0 ||
        SizeClassOf(FLAGS_rdma_gdr_recv_block_size) < 0) {
        LOG(ERROR) << "Fail to enable GDR: --rdma_gdr_recv_block_size="
                   << FLAGS_rdma_gdr_recv_block_size << " is out of range ["
                   << (1 << MIN_SIZE_CLASS_SHIFT) << ", "
                   << (1UL << MAX_SIZE_CLASS_SHIFT) << "]";
        errno = EINVAL;
        return -1;
    }
    if (FLAGS_rdma_gdr_max_regions < 1 ||
        FLAGS_rdma_gdr_max_regions > MemoryPool::MAX_REGIONS) {
        LOG(ERROR) << "Fail to enable GDR: --rdma_gdr_max_regions="
                   << FLAGS_rdma_gdr_max_regions << " is out of range [1, "
                   << MemoryPool::MAX_REGIONS << "]";
        errno = EINVAL;
        return -1;
    }
    if (FLAGS_rdma_gdr_region_size_mb < 1) {
        LOG(ERROR) << "Fail to enable GDR: --rdma_gdr_region_size_mb="
                   << FLAGS_rdma_gdr_region_size_mb << " must be positive";
        errno = EINVAL;
        return -1;
    }

    MemoryPoolOptions options;
    options.size_class_shifts = SIZE_CLASS_SHIFTS;
    options.num_size_classes = SIZE_CLASS_COUNT;
    options.max_regions = FLAGS_rdma_gdr_max_regions;
    options.blocks_per_region = BLOCKS_PER_REGION;
    options.max_region_size = (size_t)FLAGS_rdma_gdr_region_size_mb << 20;
    options.max_bytes = FLAGS_rdma_gdr_max_device_bytes;
    options.tls_cache_num = FLAGS_rdma_gdr_tls_cache_num;
    options.tls_cache_bytes = FLAGS_rdma_gdr_tls_cache_bytes;
    // Which allocator is behind the blocks is the only thing `host' mode
    // changes. Stub mode picks host too: without CUDA there is nothing else,
    // and the surrounding code never dereferences a pool pointer anyway.
    MemoryBackend* const backend =
        (g_attachment_is_host || g_skip_device_alloc_for_test)
            ? static_cast<MemoryBackend*>(&g_host_backend)
            : static_cast<MemoryBackend*>(&g_cuda_backend);
    if (GetPool().Init(backend, options) != 0) {
        return -1;
    }

    g_overflow = new std::map<uintptr_t, UserRegion>;
    g_overflow_mutex = new butil::Mutex;
    g_gdr_available = true;
    LOG(INFO) << "The second RDMA channel is enabled, attachments land in "
              << (g_attachment_is_host ? "host" : "device") << " memory";
    return 0;
}

void GlobalGdrRelease() {
    if (!g_gdr_available) {
        return;
    }
    g_gdr_available = false;

    // Frees and deregisters every pool region, and invalidates every thread
    // cache so nothing hands out a pointer into one of them afterwards.
    GetPool().Destroy();

    for (std::map<uintptr_t, UserRegion>::iterator it = g_overflow->begin();
         it != g_overflow->end(); ++it) {
        DeregisterRaw((void*)it->second.start);
    }
    delete g_overflow;
    g_overflow = nullptr;
    g_overflow_num.store(0, butil::memory_order_relaxed);
    delete g_overflow_mutex;
    g_overflow_mutex = nullptr;

    // Back to the defaults, so a unit test that brings the pool up again with
    // a different --rdma_attachment_memory is not answered from last time.
    g_attachment_is_host = false;
    g_can_probe_cuda = false;
}

void* AllocDeviceBlock(size_t size, uint32_t* lkey) {
    if (!g_gdr_available) {
        errno = ENODEV;
        return nullptr;
    }
    if (size == 0 || lkey == nullptr) {
        errno = EINVAL;
        return nullptr;
    }
    return GetPool().AllocBlock(size, lkey);
}

int DeallocDeviceBlock(void* buf) {
    if (!g_gdr_available || buf == nullptr) {
        errno = EINVAL;
        return -1;
    }
    return GetPool().DeallocBlock(buf);
}

uint32_t GetDeviceLKey(const void* buf, bool* is_pool_block) {
    if (!g_gdr_available || buf == nullptr) {
        return 0;
    }
    const uint32_t lkey = GetPool().GetLKey(buf, is_pool_block);
    if (lkey != 0) {
        return lkey;
    }
    if (g_overflow_num.load(butil::memory_order_relaxed) == 0) {
        return 0;
    }
    BAIDU_SCOPED_LOCK(*g_overflow_mutex);
    // First region starting after buf; the candidate is the one before it.
    std::map<uintptr_t, UserRegion>::iterator it =
        g_overflow->upper_bound((uintptr_t)buf);
    if (it == g_overflow->begin()) {
        return 0;
    }
    --it;
    if ((uintptr_t)buf >= it->second.start + it->second.size) {
        return 0;
    }
    // Only RegisterDeviceMemory() overflows; pool regions have a slot each.
    if (is_pool_block) {
        *is_pool_block = false;
    }
    return it->second.lkey;
}

uint32_t RegisterDeviceMemory(void* dptr, size_t len) {
    if (!g_gdr_available) {
        errno = ENODEV;
        return 0;
    }
    if (dptr == nullptr || len == 0) {
        errno = EINVAL;
        return 0;
    }
    const uint32_t lkey = RegisterRaw(dptr, len);
    if (lkey == 0) {
        return 0;
    }
    if (GetPool().AddUserRegion(dptr, len, lkey) == 0) {
        return lkey;
    }
    // Out of slots. A process that registers a bounded set of buffers never
    // gets here; one that churns registrations does, because slots are never
    // reused (see DeregisterDeviceMemory). Degrade to a locked map rather
    // than fail the send.
    BAIDU_SCOPED_LOCK(*g_overflow_mutex);
    UserRegion region = { (uintptr_t)dptr, len, lkey };
    (*g_overflow)[(uintptr_t)dptr] = region;
    g_overflow_num.store((int)g_overflow->size(), butil::memory_order_relaxed);
    return lkey;
}

void DeregisterDeviceMemory(void* dptr) {
    if (!g_gdr_available || dptr == nullptr) {
        return;
    }
    // RemoveRegion() deregisters the region it drops, and refuses to touch a
    // region the pool carves into blocks.
    if (GetPool().RemoveRegion(dptr) == 0) {
        return;
    }
    if (g_overflow_num.load(butil::memory_order_relaxed) > 0) {
        BAIDU_SCOPED_LOCK(*g_overflow_mutex);
        std::map<uintptr_t, UserRegion>::iterator it =
            g_overflow->find((uintptr_t)dptr);
        if (it != g_overflow->end()) {
            g_overflow->erase(it);
            g_overflow_num.store((int)g_overflow->size(),
                                 butil::memory_order_relaxed);
            DeregisterRaw(dptr);
            return;
        }
    }
    LOG(WARNING) << "Try to deregister device memory at " << dptr
                 << " which was not registered by the user";
}

// All three below reach rdma_helper's user-MR map, and registration also
// needs the PD. Both are created by the same stretch of
// GlobalRdmaInitializeOrDie(), so the PD is the one thing to check: without
// it ibv_reg_mr() has no argument and the map's lock is still null, which a
// lock attempt turns into a segfault rather than an error.
//
// Note this is deliberately not IsRdmaAvailable(). That flag says "the RDMA
// stack is usable", which the unit tests force on to run socket-level code
// against stubs -- true as an intention, but the PD really is absent there.
uint32_t GetHostLKey(void* buf) {
    if (g_skip_device_alloc_for_test) {
        // Nothing was really registered, so there is nothing to look up. The
        // caller re-"registers" and gets FAKE_LKEY again, which is what the
        // device side does in stub mode too.
        return 0;
    }
#if BRPC_WITH_RDMA
    if (GetRdmaPd() == nullptr) {
        return 0;
    }
    return GetLKey(buf);
#else
    return 0;
#endif
}

uint32_t RegisterHostMemory(void* buf, size_t len) {
    if (g_skip_device_alloc_for_test) {
        return FAKE_LKEY;
    }
#if BRPC_WITH_RDMA
    if (GetRdmaPd() == nullptr) {
        // Same errno RegisterDeviceMemory() gives on a node without GDR:
        // with nothing to register against, an attachment cannot be built at
        // all, whatever kind of memory the caller holds.
        errno = ENODEV;
        return 0;
    }
    // Deliberately not RegisterDeviceMemory(): that one is gated on
    // g_gdr_available, which a host-only node does not have, and it would
    // spend one of the region slots and count host bytes in the device
    // memory stats.
    return RegisterMemoryForRdma(buf, len);
#else
    (void)len;
    errno = ENODEV;
    return 0;
#endif
}

void DeregisterHostMemory(void* buf) {
    if (g_skip_device_alloc_for_test) {
        return;
    }
#if BRPC_WITH_RDMA
    if (GetRdmaPd() == nullptr) {
        // Nothing was registered through us, so there is nothing to undo.
        return;
    }
    DeregisterMemoryForRdma(buf);
#else
    (void)buf;
#endif
}

bool IsDevicePointer(const void* ptr) {
    if (ptr == nullptr || g_skip_device_alloc_for_test) {
        // In stub mode the pool hands out host memory, but a pool pointer
        // never reaches here: it is found by GetDeviceLKey() first. What does
        // reach here is a buffer the test allocated itself, and calling that
        // host memory is both true and the safe answer.
        return false;
    }
    if (!g_can_probe_cuda && !CanProbeCudaLazily()) {
        // No GPU on this node, or a build without GDR. "Host" is then the
        // only possible answer, and it is also the safe one: it is what tells
        // the TCP fallback the bytes may be memcpy()ed.
        return false;
    }
#if BRPC_WITH_GDR
    cudaPointerAttributes attr;
    const cudaError_t err = cudaPointerGetAttributes(&attr, ptr);
    if (err != cudaSuccess) {
        // Unregistered host memory reports an error on old CUDA runtimes
        // instead of cudaMemoryTypeUnregistered. Clear the sticky error so a
        // later real CUDA call is not misdiagnosed.
        cudaGetLastError();
        return false;
    }
    return attr.type == cudaMemoryTypeDevice ||
           attr.type == cudaMemoryTypeManaged;
#else
    return false;
#endif
}

size_t GetDeviceRecvBlockSize() {
    return (size_t)FLAGS_rdma_gdr_recv_block_size;
}

// The direction argument is spelled out at each call site rather than using
// cudaMemcpyDefault, so that passing a host pointer where a device one was
// promised fails loudly instead of being silently papered over by unified
// addressing.
#if BRPC_WITH_GDR
static int CudaCopy(void* dst, const void* src, size_t len,
                    cudaMemcpyKind kind) {
    ScopedCudaDevice device_guard;
    const cudaError_t err = cudaMemcpy(dst, src, len, kind);
    if (err != cudaSuccess) {
        LOG(ERROR) << "Fail to cudaMemcpy " << len
                   << " bytes: " << cudaGetErrorString(err);
        errno = EIO;
        return -1;
    }
    return 0;
}
#endif

int CopyToDevice(void* dst, const void* src, size_t len) {
#if BRPC_WITH_GDR
    if (!g_skip_device_alloc_for_test) {
        return CudaCopy(dst, src, len, cudaMemcpyHostToDevice);
    }
#endif
    // Either there is no CUDA in this build, or the stub is on and every
    // "device" pointer is really host memory.
    memcpy(dst, src, len);
    return 0;
}

int CopyFromDevice(void* dst, const void* src, size_t len) {
#if BRPC_WITH_GDR
    if (!g_skip_device_alloc_for_test) {
        return CudaCopy(dst, src, len, cudaMemcpyDeviceToHost);
    }
#endif
    memcpy(dst, src, len);
    return 0;
}

void GetDeviceMemoryStat(int64_t* reserved_bytes, int64_t* in_use_bytes,
                         int* num_regions) {
    if (num_regions) {
        *num_regions = g_gdr_available ? GetPool().num_regions() : 0;
    }
    if (reserved_bytes) {
        *reserved_bytes = g_gdr_available ? GetPool().reserved_bytes() : 0;
    }
    if (in_use_bytes) {
        *in_use_bytes = g_gdr_available ? GetPool().in_use_bytes() : 0;
    }
}

void DumpDeviceMemoryInfo(std::ostream& os) {
    if (!g_gdr_available) {
        os << "GDR is not available\n";
        return;
    }
    GetPool().DumpInfo(os);
    if (!g_attachment_is_host) {
        os << "cuda_device=" << FLAGS_rdma_gdr_device_id << "\n";
    }
}

}  // namespace rdma
}  // namespace brpc
