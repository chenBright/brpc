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

#include "brpc/device_attachment.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include "brpc/rdma/device_memory.h"
#include "butil/iobuf.h"
#include "butil/logging.h"

namespace brpc {

DeviceAttachment::DeviceAttachment()
    : _size(0) {
}

DeviceAttachment::~DeviceAttachment() {
    clear();
}

DeviceAttachment::DeviceAttachment(DeviceAttachment&& other) noexcept
    : _segments(std::move(other._segments))
    , _size(other._size) {
    other._segments.clear();
    other._size = 0;
}

DeviceAttachment& DeviceAttachment::operator=(
    DeviceAttachment&& other) noexcept {
    if (this != &other) {
        clear();
        _segments = std::move(other._segments);
        _size = other._size;
        other._segments.clear();
        other._size = 0;
    }
    return *this;
}

DeviceAttachment::Block* DeviceAttachment::NewBlock() {
    Block* block = new (std::nothrow) Block;
    if (block == nullptr) {
        return nullptr;
    }
    block->base = nullptr;
    block->size = 0;
    block->lkey = 0;
    block->nshared.store(1, butil::memory_order_relaxed);
    block->deleter = nullptr;
    block->deleter_arg = nullptr;
    block->registered_here = false;
    block->is_host = false;
    return block;
}

void DeviceAttachment::ReleaseBlock(Block* block) {
    if (block == nullptr) {
        return;
    }
    if (block->nshared.fetch_sub(1, butil::memory_order_acq_rel) != 1) {
        return;
    }
    if (block->deleter) {
        if (block->registered_here) {
            if (block->is_host) {
                rdma::DeregisterHostMemory(block->base);
            } else {
                rdma::DeregisterDeviceMemory(block->base);
            }
        }
        block->deleter(block->base, block->deleter_arg);
    } else {
        rdma::DeallocDeviceBlock(block->base);
    }
    delete block;
}

namespace {
// Adapter so the one-argument deleter can share Block's two-argument slot.
void CallOneArgDeleter(void* dptr, void* fn) {
    ((void (*)(void*))fn)(dptr);
}
// A user block must never go back to the GDR pool, so it always needs a
// non-nullptr deleter even when the caller does not want to be called back.
void NoopDeleter(void*, void*) { }
// For append_new()'s no-pool fallback: the block is plain host memory that
// came from posix_memalign() and has to go back to free(), not to a pool
// that does not exist.
void FreeHostBlock(void* ptr, void*) { free(ptr); }
}  // namespace

void* DeviceAttachment::append_new(size_t size) {
    if (size == 0) {
        errno = EINVAL;
        return nullptr;
    }
    uint32_t lkey = 0;
    bool is_host = false;
    void (*deleter)(void*, void*) = nullptr;
    void* dptr = rdma::AllocDeviceBlock(size, &lkey);
    if (dptr != nullptr) {
        // What the pool is made of is a process-wide setting, so every pool
        // block answers the same and so does every attachment this process
        // receives. Note this asks the pool rather than the flag: in the test
        // stub the pool really hands out host memory, but it answers
        // "device", because letting stub runs disagree with real ones about
        // what callers may dereference is exactly what the stub exists to
        // avoid.
        is_host = !rdma::IsAttachmentMemoryDevice();
    } else if (!rdma::IsGdrAvailable()) {
        // No second channel anywhere in this process, so this attachment can
        // only ever travel inline on a TCP connection, where the memory needs
        // no registration and reading it from the host is the point. Page
        // aligned to match what the pool would have given back.
        if (posix_memalign(&dptr, 4096, size) != 0 || dptr == nullptr) {
            errno = ENOMEM;
            return nullptr;
        }
        is_host = true;
        deleter = FreeHostBlock;
    } else {
        // The pool is up and said no. Falling back to host memory here would
        // produce a segment that cannot be posted to the device QP this
        // process does have, which is a worse failure than this one.
        return nullptr;
    }
    Block* block = NewBlock();
    if (block == nullptr) {
        if (deleter) {
            free(dptr);
        } else {
            rdma::DeallocDeviceBlock(dptr);
        }
        errno = ENOMEM;
        return nullptr;
    }
    block->base = dptr;
    block->size = size;
    block->lkey = lkey;
    block->deleter = deleter;
    block->is_host = is_host;

    Ref ref;
    ref.block = block;
    ref.seg.ptr = dptr;
    // The pool rounds up to a power of two; only what was asked for is part
    // of the attachment, the rest of the block is padding.
    ref.seg.length = size;
    ref.seg.lkey = lkey;
    ref.seg.is_host = is_host;
    _segments.push_back(ref);
    _size += size;
    return dptr;
}

int DeviceAttachment::append_user_data(void* ptr, size_t size,
                                       void (*deleter)(void*)) {
    return append_user_data(ptr, size,
                            deleter ? CallOneArgDeleter : nullptr,
                            (void*)deleter);
}

int DeviceAttachment::append_user_data(void* ptr, size_t size,
                                       void (*deleter)(void*, void*),
                                       void* deleter_arg) {
    if (ptr == nullptr || size == 0) {
        errno = EINVAL;
        return -1;
    }
    // Deliberately no IsGdrAvailable() gate. A process with no second channel
    // can still send this attachment, inline on a TCP connection, and that is
    // what makes one body of application code run on both kinds of peer.
    //
    // Both registries, device first: it range-scans, so it also answers for
    // an interior pointer of a pool block or of a user device region, which
    // is the common case on the send path -- and it is the one that answers
    // for a block the peer's data landed in, which is how a received
    // attachment gets its memory kind.
    bool is_host = false;
    bool registered_here = false;
    bool from_pool = false;
    uint32_t lkey = rdma::GetDeviceLKey(ptr, &from_pool);
    if (lkey != 0) {
        // A hit is not by itself "device memory": the region table holds pool
        // regions and user RegisterDeviceMemory() regions in the same array.
        // A pool block is made of whatever the pool is made of, host memory
        // included; anything else in there is device memory by construction.
        // Same source of truth as append_new(), for the same reason.
        is_host = from_pool && !rdma::IsAttachmentMemoryDevice();
    } else {
        lkey = rdma::GetHostLKey(ptr);
        is_host = (lkey != 0);
    }
    if (lkey == 0) {
        // Neither knows it. Ask CUDA where it lives rather than guessing:
        // registering host memory through RegisterDeviceMemory() would fail
        // outright on a node with no GPU, and on a node with one it would
        // burn a region slot and count the bytes as device memory. The answer
        // also decides whether the TCP fallback may memcpy() from here.
        is_host = !rdma::IsDevicePointer(ptr);
        lkey = is_host ? rdma::RegisterHostMemory(ptr, size)
                       : rdma::RegisterDeviceMemory(ptr, size);
        if (lkey == 0) {
            if (rdma::IsGdrAvailable()) {
                // This process has a second channel, so this segment may well
                // have to be posted to a device QP -- and an ibv_sge with
                // lkey 0 goes nowhere. Failing now beats failing at the write.
                return -1;
            }
            // No second channel here, so the only route out is the TCP
            // fallback, which needs no lkey. Keeping the segment unregistered
            // is the difference between "works without RDMA" and "needs a
            // card to build an attachment at all".
        } else {
            registered_here = true;
        }
    }
    Block* block = NewBlock();
    if (block == nullptr) {
        if (registered_here) {
            if (is_host) {
                rdma::DeregisterHostMemory(ptr);
            } else {
                rdma::DeregisterDeviceMemory(ptr);
            }
        }
        errno = ENOMEM;
        return -1;
    }
    block->base = ptr;
    block->size = size;
    block->lkey = lkey;
    // A user block must never go back to the pool, so it always carries a
    // deleter -- a no-op one if the caller passed none.
    block->deleter = deleter ? deleter : NoopDeleter;
    block->deleter_arg = deleter ? deleter_arg : nullptr;
    block->registered_here = registered_here;
    block->is_host = is_host;

    Ref ref;
    ref.block = block;
    ref.seg.ptr = ptr;
    ref.seg.length = size;
    ref.seg.lkey = lkey;
    ref.seg.is_host = is_host;
    _segments.push_back(ref);
    _size += size;
    return 0;
}

int DeviceAttachment::append_user_data_with_lkey(void* ptr, size_t size,
                                                 uint32_t lkey, bool is_host,
                                                 void (*deleter)(void*)) {
    return append_user_data_with_lkey(ptr, size, lkey, is_host,
                                      deleter ? CallOneArgDeleter : nullptr,
                                      (void*)deleter);
}

int DeviceAttachment::append_user_data_with_lkey(
    void* ptr, size_t size, uint32_t lkey, bool is_host,
    void (*deleter)(void*, void*), void* deleter_arg) {
    if (ptr == nullptr || size == 0 || lkey == 0) {
        errno = EINVAL;
        return -1;
    }
    // No IsGdrAvailable() gate either: bringing your own lkey is how a caller
    // says "this is registered and this is what it is made of". On a
    // connection with no second channel the lkey simply goes unused and the
    // bytes travel inline (docs/cn/gdr_design.md section 7.3), for which
    // is_host is the field that matters.
    Block* block = NewBlock();
    if (block == nullptr) {
        errno = ENOMEM;
        return -1;
    }
    block->base = ptr;
    block->size = size;
    block->lkey = lkey;
    block->deleter = deleter ? deleter : NoopDeleter;
    block->deleter_arg = deleter ? deleter_arg : nullptr;
    // The caller owns the registration, so we must not undo it.
    block->registered_here = false;
    block->is_host = is_host;

    Ref ref;
    ref.block = block;
    ref.seg.ptr = ptr;
    ref.seg.length = size;
    ref.seg.lkey = lkey;
    ref.seg.is_host = is_host;
    _segments.push_back(ref);
    _size += size;
    return 0;
}

bool DeviceAttachment::is_host_readable() const {
    for (size_t i = 0; i < _segments.size(); ++i) {
        if (!_segments[i].seg.is_host) {
            return false;
        }
    }
    return true;
}

void DeviceAttachment::clear() {
    for (size_t i = 0; i < _segments.size(); ++i) {
        ReleaseBlock(_segments[i].block);
    }
    _segments.clear();
    _size = 0;
}

void DeviceAttachment::swap(DeviceAttachment& other) {
    _segments.swap(other._segments);
    const size_t tmp = _size;
    _size = other._size;
    other._size = tmp;
}

size_t DeviceAttachment::cutn(DeviceAttachment* to, size_t n) {
    if (to == this) {
        return 0;
    }
    size_t moved = 0;
    size_t consumed_segments = 0;
    for (size_t i = 0; i < _segments.size() && moved < n; ++i) {
        Ref& ref = _segments[i];
        const size_t want = n - moved;
        if (ref.seg.length <= want) {
            // Hand the whole segment over, reference and all.
            to->_segments.push_back(ref);
            to->_size += ref.seg.length;
            moved += ref.seg.length;
            ++consumed_segments;
        } else {
            // Split: both halves keep a reference to the same block.
            Ref head = ref;
            head.seg.length = want;
            ref.block->nshared.fetch_add(1, butil::memory_order_relaxed);
            to->_segments.push_back(head);
            to->_size += want;
            ref.seg.ptr = (char*)ref.seg.ptr + want;
            ref.seg.length -= want;
            moved += want;
        }
    }
    if (consumed_segments > 0) {
        _segments.erase(_segments.begin(),
                        _segments.begin() + consumed_segments);
    }
    _size -= moved;
    return moved;
}

size_t DeviceAttachment::pop_front(size_t n) {
    size_t dropped = 0;
    size_t consumed_segments = 0;
    for (size_t i = 0; i < _segments.size() && dropped < n; ++i) {
        Ref& ref = _segments[i];
        const size_t want = n - dropped;
        if (ref.seg.length <= want) {
            ReleaseBlock(ref.block);
            dropped += ref.seg.length;
            ++consumed_segments;
        } else {
            ref.seg.ptr = (char*)ref.seg.ptr + want;
            ref.seg.length -= want;
            dropped += want;
        }
    }
    if (consumed_segments > 0) {
        _segments.erase(_segments.begin(),
                        _segments.begin() + consumed_segments);
    }
    _size -= dropped;
    return dropped;
}

void DeviceAttachment::append_ref(const DeviceAttachment& other) {
    if (&other == this) {
        return;
    }
    _segments.reserve(_segments.size() + other._segments.size());
    for (size_t i = 0; i < other._segments.size(); ++i) {
        other._segments[i].block->nshared.fetch_add(
            1, butil::memory_order_relaxed);
        _segments.push_back(other._segments[i]);
    }
    _size += other._size;
}

void DeviceAttachment::append(DeviceAttachment&& other) {
    if (&other == this) {
        return;
    }
    _segments.insert(_segments.end(), other._segments.begin(),
                     other._segments.end());
    _size += other._size;
    other._segments.clear();
    other._size = 0;
}

int DeviceAttachment::copy_to(butil::IOBuf* out) const {
    // Everything goes through the zero-copy stream, host segments included.
    // It is one memcpy either way (IOBuf::append() would do the same one),
    // and appending to `out` directly in between Next()/BackUp() pairs would
    // leave the stream's current block no longer at the back of the IOBuf,
    // which BackUp() treats as fatal.
    butil::IOBufAsZeroCopyOutputStream zc(out);
    for (size_t i = 0; i < _segments.size(); ++i) {
        const Segment& seg = _segments[i].seg;
        const char* src = (const char*)seg.ptr;
        size_t left = seg.length;
        while (left > 0) {
            void* data = nullptr;
            int block_size = 0;
            if (!zc.Next(&data, &block_size)) {
                errno = ENOMEM;
                return -1;
            }
            const size_t n = std::min(left, (size_t)block_size);
            if (seg.is_host) {
                memcpy(data, src, n);
            } else if (rdma::CopyFromDevice(data, src, n) != 0) {
                zc.BackUp(block_size);
                return -1;
            }
            src += n;
            left -= n;
            if ((size_t)block_size > n) {
                zc.BackUp(block_size - n);
            }
        }
    }
    return 0;
}

int DeviceAttachment::append_from_iobuf(butil::IOBuf* from, size_t n) {
    if (n > from->size()) {
        errno = EINVAL;
        return -1;
    }
    if (n == 0) {
        return 0;
    }
    size_t chunk = rdma::GetDeviceRecvBlockSize();
    if (chunk == 0) {
        chunk = n;
    }
    // Built aside and only merged in on success, so a failure halfway leaves
    // both this attachment and *from exactly as they were.
    DeviceAttachment staged;
    // A cursor over the source's backing blocks rather than an
    // IOBufBytesIterator: the destination may be device memory, so each copy
    // has to be handed a contiguous run of the source explicitly.
    size_t block_index = 0;
    size_t block_offset = 0;
    size_t left = n;
    while (left > 0) {
        const size_t this_size = std::min(left, chunk);
        void* dst = staged.append_new(this_size);
        if (dst == nullptr) {
            return -1;
        }
        const bool dst_is_host =
            staged.segment(staged.segment_count() - 1).is_host;
        size_t filled = 0;
        while (filled < this_size) {
            if (block_index >= from->backing_block_num()) {
                // Unreachable given n <= from->size(); here so that a wrong
                // answer from either is a failed RPC rather than a spin.
                errno = EINVAL;
                return -1;
            }
            const butil::StringPiece block = from->backing_block(block_index);
            if (block_offset >= block.size()) {
                ++block_index;
                block_offset = 0;
                continue;
            }
            const size_t run = std::min(this_size - filled,
                                        block.size() - block_offset);
            const char* src = block.data() + block_offset;
            if (dst_is_host) {
                memcpy((char*)dst + filled, src, run);
            } else if (rdma::CopyToDevice((char*)dst + filled, src, run) != 0) {
                return -1;
            }
            block_offset += run;
            filled += run;
        }
        left -= this_size;
    }
    from->pop_front(n);
    append(std::move(staged));
    return 0;
}

DeviceStream::DeviceStream()
    : _cut_offset(0)
    , _send_offset(0)
    , _send_queued_bytes(0)
    , _pending_bytes(0)
    , _pending_msgs(0)
    , _pending_hook(nullptr)
    , _pending_hook_arg(nullptr) {
}

DeviceStream::~DeviceStream() {
}

size_t DeviceStream::cutn(DeviceAttachment* to, size_t n) {
    const size_t moved = _recv_stream.cutn(to, n);
    _cut_offset += moved;
    return moved;
}

void DeviceStream::AppendForSend(DeviceAttachment&& data) {
    // Queue only, exactly like the host side's Setup() appending to
    // req->data: the transport posts it, on this very thread. Ordering is the
    // contract here, not the counter -- this runs from the socket's single
    // writer, which is what makes "the k-th device segment belongs to the
    // k-th device-carrying host message" true; see docs/cn/gdr_design.md
    // section 8.1. _send_offset is bookkeeping for the status output.
    _send_offset += data.size();
    _send_stream.append(std::move(data));
    SyncQueuedBytes();
}

void DeviceStream::DiscardQueuedData() {
    _send_stream.clear();
    _send_queued_bytes.store(0, butil::memory_order_relaxed);
}

void DeviceStream::SetPending(size_t pending_msgs, size_t pending_bytes) {
    _pending_bytes.store(pending_bytes, butil::memory_order_relaxed);
    _pending_msgs.store(pending_msgs, butil::memory_order_relaxed);
    if (_pending_hook != nullptr) {
        _pending_hook(_pending_hook_arg);
    }
}

void DeviceStream::Reset() {
    _recv_stream.clear();
    _cut_offset = 0;
    DiscardQueuedData();
    _send_offset = 0;
    _pending_bytes.store(0, butil::memory_order_relaxed);
    _pending_msgs.store(0, butil::memory_order_relaxed);
}

}  // namespace brpc
