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

#ifndef BRPC_RDMA_DEVICE_MEMORY_H
#define BRPC_RDMA_DEVICE_MEMORY_H

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <gflags/gflags_declare.h>              // DECLARE_bool

namespace brpc {
namespace rdma {

DECLARE_bool(rdma_enable_gdr);

// The registered memory behind the second RDMA channel, normally GPU device
// memory (GPU Direct RDMA) but host memory under
// --rdma_attachment_memory=host.
//
// Callers must treat every pointer from here as a *device* pointer: never
// dereferenced by host code, and in particular never inside a butil::IOBuf --
// see docs/cn/gdr_design.md section 2 for why that would crash rather than
// merely be unsafe. In host mode the bytes really are reachable, but only
// DeviceAttachment::Segment::is_host says so, and only for an attachment
// this process received or built. Code down here cannot tell one deployment
// from the other and must not try.
//
// Blocks are allocated in power-of-two size classes and cached per class
// after being freed, because ibv_reg_mr() pins pages and is far too
// expensive to pay per RPC. A block keeps its registration for the lifetime
// of the process; freeing only returns it to the class free list.
//
// The pool is shaped after block_pool.cpp: one cudaMalloc + one ibv_reg_mr
// buys a whole region, blocks are carved out of it, and each thread keeps a
// small cache so the steady state takes no lock at all. Keeping the region
// count small and bounded is what lets GetDeviceLKey() resolve an address by
// scanning an array without a lock.

// Whether the second channel's memory pool is up, i.e. GDR was compiled in
// and initialized successfully. Everything below is a no-op returning an
// error when this is false. Says nothing about what the pool is made of --
// `host' mode is just as available.
bool IsGdrAvailable();

// Whether that pool hands out GPU memory rather than host memory. The only
// caller that should care is one whose reason is specific to GPU memory: as
// of now, the mlx5 scatter-to-CQE workaround in rdma_helper.cpp. Anything
// asking in order to decide whether the second channel exists wants
// IsGdrAvailable() instead.
bool IsAttachmentMemoryDevice();

// Initialize device memory support. Called by GlobalRdmaInitializeOrDie()
// when FLAGS_rdma_enable_gdr is set. Needs nothing from the RDMA context or
// PD -- blocks are registered lazily, on first allocation -- so it runs
// first, where a bad --rdma_gdr_device_id is easy to spot in the log.
// Returns 0 on success, -1 with errno set on failure.
int GlobalGdrInitialize();

void GlobalGdrRelease();

// Allocate a registered device block of at least `size` bytes.
// On success returns the device pointer and stores its lkey in *lkey.
// Returns nullptr with errno set on failure.
void* AllocDeviceBlock(size_t size, uint32_t* lkey);

// Return a block from AllocDeviceBlock() to its size-class free list.
// Returns 0 on success, -1 with errno set (ERANGE if not a pool block, or
// not the start of one).
int DeallocDeviceBlock(void* buf);

// lkey covering `buf`, which may point anywhere inside a pool region or
// inside a user region registered by RegisterDeviceMemory(). Returns 0 if
// the address belongs to no known region. Takes no lock unless the region
// array overflowed.
//
// *`is_pool_block' (may be nullptr, only written on a hit) says which of the two
// it was. That is the difference between "the pool made it", whose memory
// kind is IsAttachmentMemoryDevice(), and "the user registered it", which is
// device memory by construction -- the table holds both and the lkey alone
// cannot tell them apart. Callers that only want to build an ibv_sge can
// ignore it; callers that report a memory kind to the application cannot.
uint32_t GetDeviceLKey(const void* buf, bool* is_pool_block = nullptr);

// Register a caller-owned device pointer so it can be sent without a copy.
// Returns the lkey, or 0 on failure.
uint32_t RegisterDeviceMemory(void* dptr, size_t len);

void DeregisterDeviceMemory(void* dptr);

// The host-memory counterparts of the three above, so that a DeviceAttachment
// can carry registered host bytes down the device channel and land them in the
// peer's GPU without an H2D copy on this side. Thin wrappers over
// rdma_helper.h's GetLKey()/RegisterMemoryForRdma()/DeregisterMemoryForRdma(),
// here only so that callers do not need the BRPC_WITH_RDMA and test-stub
// conditionals.
//
// Note the asymmetry with GetDeviceLKey(): this one matches the registered
// base exactly rather than range-scanning, because that is all the underlying
// map supports. An interior pointer therefore misses and gets registered
// again -- callers holding one big buffer should register it once and pass
// the lkey to DeviceAttachment::append_user_data_with_lkey().
//
// These need RDMA itself, not GDR: they work on a node with no GPU. With no
// protection domain the lookup answers 0 and the registration fails with
// ENODEV rather than touching rdma_helper's not-yet-created MR map.
uint32_t GetHostLKey(void* buf);
uint32_t RegisterHostMemory(void* buf, size_t len);
void DeregisterHostMemory(void* buf);

// Whether `ptr` is GPU memory. False for host memory, for a pointer this
// process knows nothing about, and for every pointer in a build without GDR.
//
// Consulted when neither lkey table knows the address, i.e. on the
// registration path, so the cudaPointerGetAttributes() call is amortized over
// every later append of the same buffer. It is also what the TCP fallback
// relies on before it memcpy()s from a caller's buffer, which is why the
// query is still made when the device pool is down -- CUDA is then brought up
// lazily, once per process, rather than not asked at all.
bool IsDevicePointer(const void* ptr);

// Copy `len` bytes into / out of a pointer that really is GPU memory, i.e.
// one covered by a DeviceAttachment::Segment with is_host == false. Plain
// memcpy in a build without GDR and under the test stub, cudaMemcpy
// otherwise. Returns 0, or -1 with errno set.
//
// Only the TCP fallback calls these (docs/cn/gdr_design.md section 7.3): it
// is the one path that has to stage attachment bytes through an IOBuf. The
// GDR path never reads the bytes at all, which is its entire point. Callers
// must route host segments to memcpy themselves rather than passing them
// here -- cudaMemcpyHostToDevice on a host destination is an error, and the
// segment already says which kind it is.
int CopyToDevice(void* dst, const void* src, size_t len);
int CopyFromDevice(void* dst, const void* src, size_t len);

// Size of the blocks posted to the device QP's receive queue, i.e. the
// value advertised as RdmaDeviceChannel.block_size in the v3 handshake.
size_t GetDeviceRecvBlockSize();

// Any argument may be nullptr. `reserved_bytes' counts whole regions, so it
// includes the part of a region not handed out yet -- that is also what
// --rdma_gdr_max_device_bytes limits. `num_regions' is what GetDeviceLKey()
// scans, and should stay flat as blocks churn.
void GetDeviceMemoryStat(int64_t* reserved_bytes, int64_t* in_use_bytes,
                         int* num_regions);

void DumpDeviceMemoryInfo(std::ostream& os);

}  // namespace rdma
}  // namespace brpc

#endif  // BRPC_RDMA_DEVICE_MEMORY_H
