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

#ifndef BRPC_DEVICE_ATTACHMENT_H
#define BRPC_DEVICE_ATTACHMENT_H

#include <cstddef>
#include <cstdint>
#include <vector>
#include "butil/atomicops.h"
#include "butil/macros.h"

namespace butil {
class IOBuf;
}

namespace brpc {

// A sequence of registered memory segments carried alongside an RPC, out of
// band from the protobuf request/response and from any IOBuf attachment.
// Everything in here travels on the connection's second QP.
//
// This is deliberately NOT an IOBuf. Device pointers must never enter an
// IOBuf: brpc's own protocol auto-detection and protocol fallback paths run
// host memcpy over whatever is in the read buffer, which would fault on a
// device pointer. See docs/cn/gdr_design.md section 2.
//
// Consequences of that rule, which are the whole point of this class:
//   * there is no data() and no way to read or write the bytes from the
//     host, not even for the segments that happen to be host memory. Use
//     cudaMemcpy yourself if you need to.
//   * appending never copies. append_new() hands you a device pointer to
//     write into with your own kernel or cudaMemcpyAsync; append_user_data()
//     takes a pointer you already own.
//
// Where the bytes live and which channel they travel on are two different
// questions, and only the second one is fixed here. A segment may be host
// memory: the NIC reads it with a host lkey and the peer's second-channel
// receive blocks decide where it lands, so pushing host bytes straight into
// the peer's GPU costs no H2D copy on this side. Received attachments are
// uniform -- every block came from the same pool -- but a sent one need not
// be, hence the per-segment `is_host'.
//
// Sending a DeviceAttachment is best served by a connection that negotiated
// a second channel during the RDMA handshake, which is what carries the
// bytes without ever touching them. A connection that has none still sends
// it, by staging the bytes through the host and putting them inline behind
// the message body (docs/cn/gdr_design.md section 7.3) -- correct, and two
// copies more expensive, which is the whole cost this class exists to avoid.
// Ask Controller::has_device_channel() to find out which of the two
// happened; demand the fast one with socket_mode =
// SOCKET_MODE_RDMA_AND_DEVICE, which makes a connection that cannot provide
// it fail outright instead. Neither end needs a GPU to agree to a second
// channel (see -rdma_attachment_memory).
//
// Move-only. Not thread-safe; the underlying blocks are refcounted so that
// cutn() between two attachments is cheap, but a single attachment must not
// be mutated from two threads at once.
class DeviceAttachment {
public:
    struct Segment {
        void* ptr;        // do not dereference on the host unless is_host
        size_t length;
        uint32_t lkey;    // for ibv_sge
        bool is_host;     // host memory, safe to memcpy from
    };

    DeviceAttachment();
    ~DeviceAttachment();

    DeviceAttachment(DeviceAttachment&& other) noexcept;
    DeviceAttachment& operator=(DeviceAttachment&& other) noexcept;

    // Allocate `size` bytes of registered device memory, append it, and
    // return the device pointer. Returns nullptr with errno set on failure.
    //
    // With no second-channel pool in this process the memory is instead an
    // ordinary page-aligned host allocation: lkey 0, is_host true, freed with
    // free(). That is what lets a process with no GDR at all still receive an
    // attachment over the TCP fallback, and still build one to send that way.
    // Such a segment can never reach a device QP -- a process with a second
    // channel always has the pool -- so nothing downstream has to cope with
    // an unregistered segment on the RDMA path.
    void* append_new(size_t size);

    // Append `size` bytes at `ptr` without copying. Device and host memory
    // are both accepted; which one it is is worked out here and recorded on
    // the segment. If the range was not registered before, it is registered
    // here and deregistered when the last reference goes away.
    // `deleter` (may be nullptr) is called with `ptr` once brpc is done with
    // the memory, which may be well after the RPC returns.
    // Returns 0 on success, -1 with errno set.
    //
    // Registration failing is only fatal when this process has a second
    // channel, i.e. when the segment might have to be posted to a device QP.
    // Without one the attachment can only go out on the TCP fallback, which
    // needs no lkey, so the segment is kept unregistered.
    //
    // The lkey lookup matches a device pointer anywhere inside a registered
    // region but a host pointer only at its registered base. Appending
    // interior slices of one big host buffer therefore re-registers each
    // time; use append_user_data_with_lkey() for that.
    int append_user_data(void* ptr, size_t size, void (*deleter)(void*));
    int append_user_data(void* ptr, size_t size,
                         void (*deleter)(void*, void*), void* deleter_arg);

    // Same, for a caller who registered the memory itself and already knows
    // both answers. Nothing is registered or deregistered here; `lkey` must
    // cover [ptr, ptr + size) on the PD returned by rdma::GetRdmaPd(), and
    // `is_host` must be truthful -- it is what tells the receiving
    // application whether it may read the bytes.
    int append_user_data_with_lkey(void* ptr, size_t size, uint32_t lkey,
                                   bool is_host, void (*deleter)(void*));
    int append_user_data_with_lkey(void* ptr, size_t size, uint32_t lkey,
                                   bool is_host,
                                   void (*deleter)(void*, void*),
                                   void* deleter_arg);

    size_t size() const { return _size; }
    bool empty() const { return _size == 0; }
    void clear();
    void swap(DeviceAttachment& other);

    // Move the first `n` bytes of this attachment to the back of *to.
    // Returns the number of bytes moved, which is min(n, size()).
    size_t cutn(DeviceAttachment* to, size_t n);

    // Drop the first `n` bytes. Returns the number of bytes dropped.
    size_t pop_front(size_t n);

    // Move everything in `other` to the back of this one.
    void append(DeviceAttachment&& other);

    // Append a *reference* to everything in `other`, sharing the underlying
    // device memory instead of copying it (there is no way to copy it from
    // the host anyway). The blocks stay alive until both attachments are
    // gone. This is what lets a request attachment survive being handed to
    // the socket, so that a retry can send it again.
    void append_ref(const DeviceAttachment& other);

    // ---- TCP fallback ----
    //
    // The only two places that move attachment bytes through host memory.
    // Both exist for connections with no second channel, where the bytes
    // have to travel inline behind the message body; see
    // docs/cn/gdr_design.md section 7.3. Nothing on the RDMA path calls
    // them, which is why "a DeviceAttachment cannot be read from the host"
    // stays true of the API the application sees: these copy into and out of
    // buffers brpc owns, they hand out no pointer into the attachment.

    // Copy every byte to the back of *out, D2H-copying the segments that are
    // device memory and memcpy()ing the ones that are not. Leaves this
    // attachment unchanged. Returns 0, or -1 with errno set.
    int copy_to(butil::IOBuf* out) const;

    // Move `n` bytes off the front of *from into freshly allocated
    // attachment memory appended here, H2D-copying when that memory is
    // device memory. `n` must be <= from->size().
    //
    // Allocated in blocks of -rdma_gdr_recv_block_size so that an attachment
    // received over the fallback has the same shape as one received over the
    // device channel -- application code walking the segments does not need
    // to know which path it came in on. Nothing is appended and *from is
    // untouched if any allocation fails. Returns 0, or -1 with errno set.
    int append_from_iobuf(butil::IOBuf* from, size_t n);

    size_t segment_count() const { return _segments.size(); }
    const Segment& segment(size_t i) const { return _segments[i].seg; }

    // Whether every segment is host memory, so the whole attachment can be
    // read with plain memcpy. True for an empty attachment: there is nothing
    // unsafe to touch. Mixed attachments answer false -- check the segments
    // individually.
    bool is_host_readable() const;

private:
    DISALLOW_COPY_AND_ASSIGN(DeviceAttachment);

    // A registered region shared by any number of segments, so that cutting
    // an attachment in the middle of a block does not copy.
    struct Block {
        void* base;
        size_t size;
        uint32_t lkey;
        butil::atomic<int> nshared;
        // nullptr means the block came from the GDR pool and goes back to it.
        void (*deleter)(void*, void*);
        void* deleter_arg;
        // Whether this class registered `base` and must deregister it.
        bool registered_here;
        // Which registry `base` belongs to. Decides both how a
        // registered_here block is deregistered and what the segments
        // covering it report to the application.
        bool is_host;
    };

    struct Ref {
        Block* block;
        Segment seg;
    };

    static Block* NewBlock();
    static void ReleaseBlock(Block* block);

    std::vector<Ref> _segments;
    size_t _size;
};

// Whether a connection carries a device channel.
//
// Three-valued because brpc packs a request before the socket it will go out
// on is connected: on the first RPC over a fresh channel the handshake that
// decides this has not run yet. Callers that fail fast on "no device channel"
// must treat UNDECIDED as "keep going" -- the send path re-checks once the
// answer is final. See docs/cn/gdr_design.md section 8.1.
enum DeviceChannelState {
    DEVICE_CHANNEL_OFF,
    DEVICE_CHANNEL_ON,
    DEVICE_CHANNEL_UNDECIDED,
};

// The device channel of one connection, as a buffer.
//
// Concrete on purpose. Only RdmaEndpoint ever owns one, and it used to be an
// abstract base that RdmaEndpoint inherited -- a vtable bought solely to keep
// `brpc/rdma/*.h` and `#if BRPC_WITH_RDMA` out of core protocol code. It buys
// the same thing by composition: none of the state below touches verbs, so
// the endpoint holds one of these and does the RDMA part around it. Protocol
// code (baidu_std) gets a pointer to it from Socket::device_stream() and cuts
// device bytes out of the connection without including anything RDMA-specific.
//
// Two halves with two different owners, neither of them locked:
//   * the receive half is touched only by the socket's single active input
//     thread, the one running the parser;
//   * the send half only by the socket's single writer.
// The atomics are for the few values the other side reads (the writer asks
// how much the parser is holding; Socket::IsWriteComplete() asks whether the
// send queue is empty), not for concurrent mutation.
class DeviceStream {
public:
    DeviceStream();
    ~DeviceStream();

    // ---- receive side, for protocol code ----

    // Device bytes received and not yet cut out.
    size_t size() const { return _recv_stream.size(); }

    // Move the first `n` bytes to *to. Returns bytes moved; a short return
    // means the rest has not arrived yet.
    size_t cutn(DeviceAttachment* to, size_t n);

    // Total bytes cut out of this stream so far. Not used to validate
    // anything -- the frame header carries the size at a fixed offset, so
    // sender and receiver cannot disagree about it -- but it is the one
    // number that makes a block-boundary accounting bug in cutn() visible,
    // so it is exposed for tests and for the endpoint's status output.
    uint64_t cut_offset() const { return _cut_offset; }

    // ---- send side, for protocol code ----

    // Queue `data` for transmission. Cannot fail: it only appends, the
    // transport posts it later.
    //
    // MUST be called from SocketMessage::AppendAndDestroySelf(), i.e. from
    // the socket's single writer. That is the only place where the order of
    // outgoing host messages is already decided, and the whole scheme rests
    // on the k-th device segment belonging to the k-th device-carrying host
    // message. Calling it anywhere else silently misaligns the two streams.
    void AppendForSend(DeviceAttachment&& data);

    // ---- backpressure, for protocol code ----

    // How much the parser is currently holding on its pending list, in
    // absolute terms rather than as a delta so that a lost update (e.g. the
    // parsing context being dropped when the protocol is re-detected) cannot
    // wedge the connection. The transport uses this to decide whether to
    // withhold host-direction credits (docs/cn/gdr_design.md section 9), and
    // is told about every update through the hook below -- dropping back
    // under its watermark has to release the credits it was holding, or the
    // peer stays stalled until some unrelated traffic flushes them.
    void SetPending(size_t pending_msgs, size_t pending_bytes);

    int64_t pending_bytes() const {
        return _pending_bytes.load(butil::memory_order_relaxed);
    }
    int64_t pending_msgs() const {
        return _pending_msgs.load(butil::memory_order_relaxed);
    }

    // ---- for the transport that owns this ----

    // Called at the end of every SetPending(). A plain function pointer, not
    // a virtual method: this class has exactly one owner, and not having a
    // vtable is the point of it being concrete. Set once, before the stream
    // is reachable from protocol code.
    typedef void (*PendingHook)(void* arg);
    void set_pending_hook(PendingHook fn, void* arg) {
        _pending_hook = fn;
        _pending_hook_arg = arg;
    }

    // Where arriving device blocks are appended.
    DeviceAttachment* recv_stream() { return &_recv_stream; }

    // What the transport posts from. It cuts out as much as the credits
    // allow and then calls SyncQueuedBytes() to republish what is left.
    DeviceAttachment* send_stream() { return &_send_stream; }
    void SyncQueuedBytes() {
        _send_queued_bytes.store((int64_t)_send_stream.size(),
                                 butil::memory_order_relaxed);
    }

    // Whether AppendForSend() is still holding bytes the credits have not let
    // the transport post. Read from outside the writer, hence the atomic.
    bool HasQueuedData() const {
        return _send_queued_bytes.load(butil::memory_order_relaxed) > 0;
    }
    int64_t queued_send_bytes() const {
        return _send_queued_bytes.load(butil::memory_order_relaxed);
    }

    // Throw the send queue away. Only for a socket that has already given up
    // on the write, where the bytes can never go out anyway and holding them
    // would pin device memory until the socket is recycled.
    void DiscardQueuedData();

    // Running total of what AppendForSend() has accepted. Bookkeeping for
    // the transport's status output and for tests, like cut_offset().
    uint64_t send_offset() const { return _send_offset; }

    // Back to the just-constructed state, for a recycled connection.
    void Reset();

private:
    DeviceAttachment _recv_stream;
    uint64_t _cut_offset;

    DeviceAttachment _send_stream;
    uint64_t _send_offset;
    butil::atomic<int64_t> _send_queued_bytes;

    butil::atomic<int64_t> _pending_bytes;
    butil::atomic<int64_t> _pending_msgs;
    PendingHook _pending_hook;
    void* _pending_hook_arg;

    DISALLOW_COPY_AND_ASSIGN(DeviceStream);
};

}  // namespace brpc

#endif  // BRPC_DEVICE_ATTACHMENT_H
