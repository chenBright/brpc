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

#ifndef BRPC_RDMA_ENDPOINT_H
#define BRPC_RDMA_ENDPOINT_H

#if BRPC_WITH_RDMA
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <functional>
#include <infiniband/verbs.h>
#include "butil/atomicops.h"
#include "butil/iobuf.h"
#include "butil/macros.h"
#include "butil/synchronization/lock.h"
#include "butil/containers/mpsc_queue.h"
#include "butil/containers/optional.h"
#include "brpc/socket.h"
#include "brpc/device_attachment.h"
#include "brpc/rdma/rdma_handshake_server.h"


namespace brpc {
class Socket;
namespace rdma {

DECLARE_bool(rdma_use_polling);
DECLARE_int32(rdma_poller_num);
DECLARE_bool(rdma_disable_bthread);

class RdmaHandshakeClientV2;
class RdmaHandshakeServerV2;
class RdmaHandshakeClientV3;
class RdmaHandshakeServerV3;
struct ParsedHello;
enum class RemoteHelloResult;
class RdmaHello;
class RdmaEndpoint;
namespace v2_wire {
RemoteHelloResult ReadBodyAndNegotiate(RdmaEndpoint* ep, ParsedHello* remote);
int DrainBytes(RdmaEndpoint* ep, size_t n);
}  // namespace v2_wire

namespace v3_wire {
void FillLocalRdmaHello(const RdmaEndpoint* ep, RdmaHello* msg);
int  ReadAndParseV3Hello(RdmaEndpoint* ep, RdmaHello* out);
int  WriteV3Hello(RdmaEndpoint* ep, const RdmaHello& msg);
}  // namespace v3_wire

class RdmaConnect : public AppConnect {
public:
    void StartConnect(const Socket* socket, 
            void (*done)(int err, void* data), void* data) override;
    void StopConnect(Socket*) override;
    struct RunGuard {
        RunGuard(RdmaConnect* rc) { this_rc = rc; }
        ~RunGuard() { if (this_rc) this_rc->Run(); }
        RdmaConnect* this_rc;
    };

private:
    void Run();
    void (*_done)(int, void*){nullptr};
    void* _data{nullptr};
};

// The completion channel of one connection. Every CQ of the connection
// reports here, so a connection costs one fd and one epoll registration no
// matter how many QPs it runs, and PollCq() drains all of them on a single
// wakeup (docs/cn/gdr_design.md section 10).
//
// It is a type of its own because it is per-connection while everything in
// RdmaResource below is per-channel, and because ibv_create_cq() binds a CQ
// to its comp channel for good: the channel has to exist before any CQ of the
// connection does, rather than being a by-product of creating the first QP.
//
// Null on the whole connection in polling mode, where nobody waits on a comp
// channel.
struct RdmaCompChannel {
    ibv_comp_channel* channel{nullptr};

    // Create one, with its fd made close-on-exec and non-blocking -- it ends
    // up in an EventDispatcher, which needs both. Returns nullptr with errno
    // set, having logged.
    static RdmaCompChannel* Create();

    RdmaCompChannel() = default;
    ~RdmaCompChannel();

    // Only valid while the channel exists: destroying it closes this fd,
    // which is why DeallocateResources() removes the consumer first.
    int fd() const { return channel->fd; }

    DISALLOW_COPY_AND_ASSIGN(RdmaCompChannel);
};

// One QP and the CQs it reports to. A GDR connection has two of these -- one
// per channel -- sharing the one RdmaCompChannel above; a plain RDMA
// connection has one. Everything listed here is owned; the comp channel is
// the connection's and is not.
//
// The two channels obtain theirs differently: host resources are
// pre-allocated in bulk (see PreparedResource), while a device one is built
// per connection, only once both sides have said they want a device channel
// -- giving every prepared QP a device QP would burn a QP plus two CQs on the
// vast majority of connections that never touch a GPU. A device resource also
// leaves `polling_cq' null: GDR does not support polling mode.
struct RdmaResource {
    ibv_qp* qp{nullptr};
    // For polling mode. Host only.
    ibv_cq* polling_cq{nullptr};
    // For event mode.
    ibv_cq* send_cq{nullptr};
    ibv_cq* recv_cq{nullptr};
    RdmaResource() = default;
    ~RdmaResource();
    DISALLOW_COPY_AND_ASSIGN(RdmaResource);
};

// One entry of the pre-allocated host resource pool (g_rdma_resource_list):
// a comp channel together with the QP and CQs built on it.
//
// The two are pooled as a unit because they cannot be pooled apart: a CQ
// belongs to the comp channel it was created on forever, so handing out a
// prepared QP means handing out its comp channel with it.
struct PreparedResource {
    PreparedResource* next{nullptr};
    // Null in polling mode, exactly as on the connection that takes this.
    RdmaCompChannel* comp_channel{nullptr};
    RdmaResource* resource{nullptr};
    PreparedResource() = default;
    ~PreparedResource();
    DISALLOW_COPY_AND_ASSIGN(PreparedResource);
};

// What both QPs of a connection have in common: the flow-control state.
//
// A connection has two channels: the host one that carries every message, and
// -- once negotiated -- the device one that carries the DeviceAttachments.
// Their credits are independent by design, so that a message with no device
// payload never waits behind one that has some (docs/cn/gdr_design.md
// sections 8 and 9). Independent credits means a full second copy of every
// counter, which is what this base is for: the same scheme twice over, so
// that the code implementing the scheme can be written once and handed
// whichever channel it is working on.
//
// Everything that is NOT common lives in HostChannel / DeviceChannel below,
// so that "which fields does this direction own" is answered by the type and
// not by a naming convention.
//
// Threading is the same on both, and is not something this struct enforces:
// the send side belongs to the socket's single writer, which posts and
// advances sq_current / sq_unsignaled; the poller hands credits back through
// the three atomics and wakes it.
struct QpChannel {
    // The QP and the CQs it reports to, or nullptr before they are allocated.
    RdmaResource* resource;
    // Capacity of the local Send Queue and Recv Queue.
    uint16_t sq_size;
    uint16_t rq_size;
    // The capacity of the local window: min(local SQ, remote RQ).
    uint16_t local_window_capacity;
    // The capacity of the remote window: min(local RQ, remote SQ).
    uint16_t remote_window_capacity;
    // The current index to send from.
    uint16_t sq_current;
    // The number of send WRs not signaled.
    uint16_t sq_unsignaled;
    // The just completed send WR's index.
    uint16_t sq_sent;
    // The just completed recv WR's index.
    uint16_t rq_received;
    // The number of IMM WRs we can post to the local Send Queue.
    uint16_t sq_imm_window_size;
    // The number of WRs we can send to the remote side.
    butil::atomic<uint16_t> remote_rq_window_size;
    // The number of WRs we can post to the local Send Queue.
    butil::atomic<uint16_t> sq_window_size;
    // The number of new WRs posted in the local Recv Queue.
    butil::atomic<uint16_t> new_rq_wrs;
    // The peer's receive block size, i.e. the most one WR of ours may carry.
    // The host and the device QP have different ones -- conflating them is
    // how PR #3144 overran by 64x, and giving each QP its own copy is how
    // that stops being possible to write.
    uint32_t remote_recv_block_size;
    // The number of CQ events taken from the comp channel and not yet acked.
    unsigned int send_cq_events;
    unsigned int recv_cq_events;

    QpChannel();
    virtual ~QpChannel() { }

    // Which of the two channels this is, for logging. The only other place
    // that needs to tell them apart is RdmaEndpoint::TakeAcks(), and what it
    // really asks is "are you my host channel", which it answers by comparing
    // addresses -- so this is the whole of the identity the base class needs.
    virtual const char* name() const = 0;

    // nullptr until the resources are allocated.
    ibv_qp* qp() const {
        return resource != nullptr ? resource->qp : nullptr;
    }

    // --- What the two channels actually do differently. -------------------
    //
    // These are the whole of it: everything else about posting, crediting,
    // soliciting, arming and reaping is identical and is written once, in
    // RdmaEndpoint, against this interface.

    // Size the send/recv slot arrays for the negotiated windows. Called once
    // per handshake, right after the QP exists. Returns false on OOM.
    virtual bool AllocateBuffers() = 0;

    // Take `len' bytes out of the receive slot at rq_received and hand them
    // on: the host channel into the messenger's read_buf, the device channel
    // into its DeviceStream. Returns the number of bytes that reached the
    // messenger (so the device channel returns 0, because none of its bytes
    // ever do), or -1 on an unrecoverable error.
    //
    // A slot whose block has been handed on is left as nullptr in rbuf_data,
    // which is how PrepareRecvSlot() below knows to fetch a fresh one.
    virtual ssize_t TakeRecvData(size_t len) = 0;

    // Release the in-flight send payload at index `i', whose send completion
    // has just been reaped.
    virtual void ReleaseSendBuf(uint16_t i) = 0;

    // Point `block' / `size' / `lkey' at the registered memory to post into
    // the recv slot at index `i', filling the slot first if it is empty.
    // Returns false if no memory could be had.
    virtual bool PrepareRecvSlot(uint16_t i, void** block, size_t* size,
                                 uint32_t* lkey) = 0;

    // Undo a PrepareRecvSlot() whose ibv_post_recv then failed.
    virtual void ReleaseRecvSlot(uint16_t i) = 0;

    // Whether the data WR about to be posted needs IBV_SEND_SOLICITED, i.e.
    // whether the peer's poller must be woken for it. `len' is the payload,
    // `imm' the credits riding along, `must_solicit' the caller's own reason
    // to insist. Called once per posted WR, and allowed to update whatever
    // counters the channel amortizes this decision over.
    virtual bool SolicitWr(size_t len, uint32_t imm, bool must_solicit) = 0;

    // Both windows open, i.e. this QP may post a data WR right now.
    bool IsWindowOpen() const {
        return remote_rq_window_size.load(butil::memory_order_relaxed) > 0 &&
               sq_window_size.load(butil::memory_order_relaxed) > 0;
    }

    // Back to the just-constructed state, so that the endpoint can be reused
    // for another connection. Every subclass clears its own fields and chains
    // to the base, so that the endpoint never has to know what those are:
    // RdmaEndpoint::Reset() just asks each channel to reset itself.
    //
    // Leaves sq_size and rq_size alone: they are this side's configuration,
    // set once per QP before any handshake, and a reconnect must not come up
    // with a different depth.
    virtual void Reset();

    DISALLOW_COPY_AND_ASSIGN(QpChannel);
};

// The host channel: the QP every message goes through. Every RDMA connection
// has exactly one, allocated (from a pool) at handshake time.
struct HostChannel : public QpChannel {
    // The input stream carried by the QP.
    InputMessengerProcessor input_processor;
    // Act as sendbuf and recvbuf, but requires no memcpy
    std::vector<butil::IOBuf> sbuf;
    std::vector<butil::IOBuf> rbuf;
    // Data address of rbuf. nullptr where the block has been cut out and
    // handed to the parser, i.e. where the slot needs a fresh one.
    std::vector<void*> rbuf_data;
    // The three counters the solicited/ack batching is amortized over.
    // Host-only: the device QP always signals, there is no small-payload
    // batching to amortize. Writer-thread only.
    uint16_t unsolicited{0};
    uint32_t unsolicited_bytes{0};
    uint16_t accumulated_ack{0};

    // Reads the local queue depths off -rdma_sq_size / -rdma_rq_size. There
    // is exactly one HostChannel per endpoint and it is never rebuilt, which
    // is what keeps a reconnect from coming up at a different depth.
    HostChannel();

    const char* name() const override { return "host"; }

    void Reset() override;

    bool AllocateBuffers() override;
    ssize_t TakeRecvData(size_t len) override;
    void ReleaseSendBuf(uint16_t i) override { sbuf[i].clear(); }
    bool PrepareRecvSlot(uint16_t i, void** block, size_t* size,
                         uint32_t* lkey) override;
    void ReleaseRecvSlot(uint16_t i) override;
    bool SolicitWr(size_t len, uint32_t imm, bool must_solicit) override;

    DISALLOW_COPY_AND_ASSIGN(HostChannel);
};

// The device channel: the second QP, carrying DeviceAttachments only.
//
// It exists -- as RdmaEndpoint::_device, a butil::optional -- exactly on the
// connections where both sides agreed to open one. "Is there a device
// channel" is therefore the same question as "is _device engaged", and there
// is no half-built state to ask about: anything that goes wrong while
// building it fails the whole connection, just as it does for the host
// channel (docs/cn/gdr_channel_unify_plan.md section 1).
//
// Its credits are independent of the host channel's by design: a message with
// no device payload must never wait behind one that has some, and the
// parser's pending list is what makes that safe (see
// docs/cn/gdr_design.md sections 8 and 9).
struct DeviceChannel : public QpChannel {
    // Size of each block we post to our own device RQ, and therefore the cap
    // we advertise to the peer. Not in QpChannel because the host side has no
    // equivalent: it posts -rdma_recv_block_size and reads the flag where it
    // needs it.
    uint32_t recv_block_size{0};
    // Last answer OnDevicePendingChanged() got from
    // IsDevicePendingOverWatermark(), so that it can spot the edge back
    // under the fuse. Input-thread only.
    bool pending_over_watermark{false};
    // In-flight send payloads, held until their send completion so the
    // caller's device memory is not recycled under the NIC.
    std::vector<DeviceAttachment> sbuf;
    // Blocks posted to the device RQ, and their lkeys. nullptr where the
    // block has been handed to the DeviceStream, i.e. where the slot needs a
    // fresh one.
    std::vector<void*> rbuf_data;
    std::vector<uint32_t> rbuf_lkey;
    // Both directions of the device channel, as buffers: what arrived and
    // has not been cut out yet, what AppendForSend() accepted and the
    // credits have not let us post yet, and how much the parser is holding
    // on its pending list. Receive half touched only by the input thread,
    // send half only by the writer -- see device_attachment.h.
    DeviceStream stream;

    // Reads the device queue depths and receive block size off their flags.
    // Unlike the host channel, this one is rebuilt for every handshake that
    // negotiates a device channel, so the flags are re-read each time.
    DeviceChannel();

    // Gives the QP, the CQ events and the posted receive blocks back. This
    // is the whole of the device teardown: RdmaEndpoint::_device.reset() is
    // all any caller has to do.
    ~DeviceChannel() override;

    const char* name() const override { return "device"; }

    bool AllocateBuffers() override;
    ssize_t TakeRecvData(size_t len) override;
    void ReleaseSendBuf(uint16_t i) override { sbuf[i].clear(); }
    bool PrepareRecvSlot(uint16_t i, void** block, size_t* size,
                         uint32_t* lkey) override;
    // Nothing to undo: a block that failed to post is still ours and still
    // registered, so the slot keeps it for the next attempt.
    void ReleaseRecvSlot(uint16_t) override { }
    // Every device WR is solicited. The bytes are large and rare compared to
    // the host channel's, so there is nothing worth batching, and a peer
    // sitting on a device payload has no other event to wake it.
    bool SolicitWr(size_t, uint32_t, bool) override { return true; }

    DISALLOW_COPY_AND_ASSIGN(DeviceChannel);
};

class BAIDU_CACHELINE_ALIGNMENT RdmaEndpoint : public SocketUser {
friend class RdmaConnect;
friend class Socket;
friend class RdmaHandshakeClientV2;
friend class RdmaHandshakeServerV2;
friend class RdmaHandshakeClientV3;
friend class RdmaHandshakeServerV3;
friend RemoteHelloResult v2_wire::ReadBodyAndNegotiate(RdmaEndpoint*, ParsedHello*);
friend int v2_wire::DrainBytes(RdmaEndpoint*, size_t);
friend void v3_wire::FillLocalRdmaHello(const RdmaEndpoint*, RdmaHello*);
friend int v3_wire::ReadAndParseV3Hello(RdmaEndpoint*, RdmaHello*);
friend int v3_wire::WriteV3Hello(RdmaEndpoint*, const RdmaHello&);
public:
    // Whether this connection runs a device channel is `s' socket mode and
    // nothing else, so the endpoint reads it from there rather than keeping a
    // copy: Socket::Create() sets _socket_mode before it builds the
    // transport, which is what makes it readable this early.
    explicit RdmaEndpoint(Socket* s);
    ~RdmaEndpoint() override;

    // Global initialization
    // Return 0 if success, -1 if failed and errno set
    static int GlobalInitialize();

    static void GlobalRelease();

    // Reset the endpoint (for next use)
    void Reset();

    // Cut data from the given IOBuf list and use RDMA to send
    // Return bytes cut if success, -1 if failed and errno set
    ssize_t CutFromIOBufList(butil::IOBuf** data, size_t ndata);

    // Whether the endpoint can send more data
    bool IsWritable() const;

    // ---- GPU Direct RDMA device channel ----

    // Whether this connection negotiated a device channel. Sending a
    // DeviceAttachment on a connection where this is false must fail loudly:
    // silently falling back to a D2H copy would hide exactly the cost the
    // caller reached for GDR to avoid.
    bool has_device_channel() const { return _device.has_value(); }

    // The buffering half of the device channel. Protocol code reaches it
    // through Socket::device_stream(); this endpoint does the verbs around
    // it. Only meaningful once has_device_channel() is true, which is what
    // RdmaTransport::GetDeviceStream() checks before handing it out.
    DeviceStream* device_stream() { return &_device->stream; }

    // Whether AppendForSend() is still holding bytes the device credits have
    // not let us post. Socket::IsWriteComplete() asks this (through
    // RdmaTransport::HasPendingWrite()) to keep KeepWrite alive until the
    // queue drains -- nothing else would come back to post it.
    bool HasQueuedDeviceData() const {
        return _device.has_value() && _device->stream.HasQueuedData();
    }

    // Throw the queue away. Only for a socket that has already given up on
    // the write; runs on the writer, like everything else that touches it.
    void DiscardQueuedDeviceData() {
        if (_device.has_value()) {
            _device->stream.DiscardQueuedData();
        }
    }

    // For debug
    void DebugInfo(std::ostream& os,
                   butil::StringPiece connector = "\n") const;

    // Callback when there is new epollin event on TCP fd.
    static void OnNewDataFromTcp(Socket* m);

    // Real handshake for RDMA-mode sockets.
    static ParseResult ExecuteServerHandshake(butil::IOBuf* source, Socket* socket);

    // Initialize polling mode
    static int PollingModeInitialize(bthread_tag_t tag,
                                     std::function<void(void)> callback,
                                     std::function<void(void)> init_fn,
                                     std::function<void(void)> release_fn);

    static void PollingModeRelease(bthread_tag_t tag);

private:
    enum State {
        UNINIT = 0x0,
        C_ALLOC_QPCQ = 0x1,
        C_HELLO_SEND = 0x2,
        C_HELLO_WAIT = 0x3,
        C_BRINGUP_QP = 0x4,
        C_ACK_SEND = 0x5,
        S_HELLO_WAIT = 0x11,
        S_ALLOC_QPCQ = 0x12,
        S_BRINGUP_QP = 0x13,
        S_HELLO_SEND = 0x14,
        S_ACK_WAIT = 0x15,
        ESTABLISHED = 0x100,
        FALLBACK_TCP = 0x200,
        FAILED = 0x300
    };

    // Process handshake at the client
    static void* ProcessHandshakeAtClient(void* arg);

    static void OnNewDataFromTcpAtClient(Socket* m);
    static void OnNewDataFromTcpAtServer(Socket* m);

    bool HandleTcpEventAfterEstablished();

    // Allocate resources. On failure the endpoint is left with no RDMA
    // resource attached, so that the handshake can safely fall back to TCP.
    // Return 0 if success, -1 if failed and errno set
    int AllocateResources();

    // The real implementation of AllocateResources(), which may return
    // in the middle with resources partially allocated.
    // Return 0 if success, -1 if failed and errno set
    int DoAllocateResources();

    // Release resources
    void DeallocateResources();

    // Create the Socket wrapping the CQ (and register it with the poller in
    // polling mode), which is what makes CQ events reachable and thus starts
    // PollCq.
    //
    // Must not be called before the handshake has reached ESTABLISHED, nor
    // from within the fd stream's parsing path: PollCq() parses the input
    // stream carried by the QP, and the Socket's `parsing_context` and
    // `preferred_index` belong to the fd stream until the handshake is over
    // and CutInputMessage has returned. It keeps writing both after the
    // handshake handler hands the stream back. Those two are per-Socket,
    // so letting PollCq in early makes two streams parse through one context.
    // The server therefore calls this from OnNewDataFromTcpAtServer(), after
    // OnNewMessages() returns, not from ExecuteServerHandshake().
    //
    // No CQE is lost by deferring: BringUpQp() fills the RQ before the QP
    // reaches RTS, both CQs are armed by DoAllocateResources(), and adding an
    // already readable fd to an edge-triggered epoll reports it immediately.
    //
    // Return 0 if success, -1 if failed and errno set
    int StartCqEvents();

    // Send Imm data to the remote side on one of the two QPs
    // Arguments:
    //     qc:  the channel to send on. Its qp() may be nullptr if the QP was
    //          never allocated, which happens in UT only.
    //     imm: imm data in the WR
    // Return:
    //     0:   success
    //     -1:  failed, errno set
    int SendImm(QpChannel& qc, uint32_t imm);

    // Post one data WR on `qc': fill in the parts of `wr' that every data WR
    // of either channel gets the same way, post it, and advance the channel.
    // The caller has already filled wr.sg_list / wr.num_sge and moved the
    // payload into the slot at qc.sq_current.
    // Arguments:
    //     len:          payload bytes in this WR, for SolicitWr()
    //     must_solicit: the caller's own reason to insist on solicited
    //     force_signal: insist on IBV_SEND_SIGNALED, i.e. on a send
    //                   completion for this WR, whatever the batching says
    //     remote_rq_window_size, sq_window_size: written with the two window
    //                   sizes left after this WR, so the caller's loop can
    //                   decide whether to post another
    // Return 0 on success, -1 with errno set. A failure here is not
    // recoverable: we keep the SQ from filling up by other means, so
    // ibv_post_send failing means something is wrong with the QP.
    int PostDataWr(QpChannel& qc, ibv_send_wr* wr, size_t len,
                   bool must_solicit, bool force_signal,
                   uint16_t* remote_rq_window_size, uint16_t* sq_window_size);

    // ---- GPU Direct RDMA device channel ----

    // Build _device iff this connection is configured for a device channel,
    // i.e. put it back to what the constructor left. The constructor and
    // Reset() are the only callers, because "configured" never changes: the
    // channel exists from construction and is rebuilt when the endpoint is
    // reused, and whether it survives the handshake is then decided by
    // ApplyRemoteHello() alone.
    //
    // A connection that ends up without one is a failed connection, not a
    // degraded one: everything between here and ESTABLISHED either succeeds
    // or fails the whole connection.
    void ResetDeviceChannel();

    // True when this connection requires a device channel, which is the whole
    // meaning of SOCKET_MODE_RDMA_AND_DEVICE (see socket_mode.h). It means
    // slightly different things at the two ends, because only one of them can
    // propose: on the client the handshake asks for a device channel and the
    // connection fails without one; on the server it is permission to agree to
    // a client that asked, and a client that did not is served host-only.
    bool device_channel_required() const;

    // Allocate the device QP and its two CQs on _comp_channel, the one the
    // host channel's CQs already report to. Return 0 on success, -1 with
    // errno set; the partial state a failure leaves behind is cleaned up by
    // ~DeviceChannel.
    int AllocateDeviceResources();

    // Cut as much as the device SQ and the peer's device RQ currently allow
    // out of `data` and post it. Returns the bytes posted, or -1 with errno
    // set; EAGAIN means the windows are shut and nothing moved.
    // Runs on the socket's single writer.
    ssize_t CutFromDeviceAttachment(DeviceAttachment* data);

    // Post whatever AppendForSend() queued and the windows now allow.
    // Idempotent, and a no-op on an empty queue. Returns 0, or -1 with errno
    // set if posting failed for good; a short post is not a failure, what is
    // left stays queued for the next call.
    int FlushDeviceSendQueue();

    // True when the parser is sitting on more half-arrived messages than
    // --rdma_gdr_pending_{bytes,msgs}_watermark allows.
    bool IsDevicePendingOverWatermark() const;

    // DeviceStream's pending hook: the parser updated its pending counters.
    // Runs on the input thread, like everything else in the receive half.
    static void OnDevicePendingChanged(void* arg);
    void OnDevicePendingChanged();

    // Reset qc.new_rq_wrs and return what it held, i.e. the receive credits
    // to hand back to the peer on that QP. The host QP returns 0 without
    // consuming them while IsDevicePendingOverWatermark(), which is the only
    // backpressure the host direction has (see the definition for why).
    uint32_t TakeAcks(QpChannel& qc);

    // Try to send pure ACK to the remote side on one of the two QPs
    // Arguments:
    //     qc:  the channel to send on
    //     num: the number of rq entry received
    // Return:
    //     0:   success
    //     -1:  failed, errno set
    int SendAck(QpChannel& qc, int num);

    // Handle one CQE of `qc'.
    // If wc is not RDMA RECV event:
    //     return 0 if success, -1 if failed and errno set
    // If wc is RDMA RECV event:
    //     return the bytes appended to the messenger's read_buf if success
    //     (always 0 on the device channel, whose bytes go to the
    //     DeviceStream instead), -1 if failed and errno set
    ssize_t HandleCompletion(QpChannel& qc, ibv_wc& wc);

    // Post a given number of WRs to the Recv Queue of `qc', filling any slot
    // whose block has been handed on.
    // Return 0 if success, -1 if failed and errno set
    int PostRecv(QpChannel& qc, uint32_t num);

    // Post a WR pointing to the block to the Recv Queue of `qc'
    // Arguments:
    //     block:      the addr to receive data (ibv_sge.addr)
    //     block_size: the maximum length can be received (ibv_sge.length)
    //     lkey:       the registered key of `block'. Host blocks come from
    //                 the block pool, so PrepareRecvSlot() reads it back with
    //                 GetRegionId(); device memory is not in that pool and
    //                 has its own registry, which hands the lkey out at
    //                 allocation time.
    // Return:
    //     0:   success
    //     -1:  failed, errno set
    int DoPostRecv(QpChannel& qc, void* block, size_t block_size,
                   uint32_t lkey);

    // Read at most len bytes from fd in _socket to data
    // wait for _read_butex if encounter EAGAIN
    // return -1 if encounter other errno (including EOF)
    int ReadFromFd(void* data, size_t len);
    int ReadFromFd(butil::IOPortal* data, size_t len);


    // Write at most len bytes from data to fd in _socket
    // wait for _epollout_butex if encounter EAGAIN
    // return -1 if encounter other errno
    int WriteToFd(void* data, size_t len);

    // Write data to fd in _socket.
    // wait for _epollout_butex if encounter EAGAIN.
    // return -1 if encounter other errno.
    int WriteToFd(butil::IOBuf* data);

    // Copy negotiated remote parameters into the endpoint and compute
    // the SQ/RQ window capacities. Called by both
    // ProcessHandshakeAtClient and ProcessHandshakeAtServer after the
    // peer's hello has been validated.
    void ApplyRemoteHello(const ParsedHello& remote);

    // Bringup both QPs of the connection from RESET state to RTS state.
    // Arguments:
    //   remote: parsed remote hello. Provides the remote LID/GID/QP
    //           number for the RTR transition, and (on v3) the peer's
    //           ECE to set during the INIT->RTR transition.
    //   is_server: true on the server side, false on the client side.
    // Returns 0 on success, -1 on failed and errno set.
    int BringUpQp(const ParsedHello& remote, bool is_server);

    // One QP of the connection, RESET -> INIT -> (fill RQ) -> RTR -> RTS.
    // Arguments:
    //   dest_qp_num:    the peer QP to pair with. The host QP takes
    //                   remote.qp_num, the device QP remote.device->qp_num;
    //                   everything else in the path is shared, because both
    //                   QPs live on the same port.
    //   in_ece:         the peer's ECE to set while in INIT, or nullptr for
    //                   no ECE negotiation on this QP.
    //   negotiated_ece: non-null to query the reduced ECE once the QP is in
    //                   RTS and store it here (the server, which sends it
    //                   back in its hello). Left untouched if the device or
    //                   the rdma-core in use has no ECE support.
    // Returns 0 on success, -1 on failed and errno set.
    int BringUpOneQp(QpChannel& qc, const ParsedHello& remote,
                     uint32_t dest_qp_num, const ibv_ece* in_ece,
                     butil::optional<ibv_ece>* negotiated_ece);

    // The two halves of the bringup above.
    //
    // RESET -> INIT. Both QPs of a connection live on the same port and take
    // the same access flags, so nothing else differs between them here.
    int ModifyQpToInit(QpChannel& qc);

    // INIT -> RTR -> RTS. Everything in the path comes from the peer's hello
    // and is shared by the two QPs except `dest_qp_num'.
    //
    // The caller must fill the RQ before calling this: a QP that reaches RTR
    // with an empty RQ can RNR, and we do not allow RNR retries.
    int ModifyQpToRts(QpChannel& qc, const ParsedHello& remote,
                      uint32_t dest_qp_num);

    // Get event from comp channel and ack the events
    int GetAndAckEvents(SocketUniquePtr& s);

    // Request completion notification on one CQ of `qc`. `solicited_only` is
    // ibv_req_notify_cq's second argument, and also tells the two CQs apart
    // for logging: only the recv CQ is armed solicited-only.
    int ReqNotifyCq(ibv_cq* cq, bool solicited_only, const QpChannel& qc,
                    bool fatal_on_error);

    // Arm both CQs of one channel.
    int ArmChannelCqs(QpChannel& qc, bool fatal_on_error);

    // Arm every CQ of this endpoint: the host pair, plus the device pair
    // when a device channel exists.
    int ReqNotifyAllCqs(bool fatal_on_error);

    // The channels of this connection: the host one, plus the device one
    // when it exists. Fills `out` (capacity MAX_CHANNELS) and returns the
    // number of entries written, so that everything which has to walk both
    // QPs -- arming, event acking, polling -- writes the walk once.
    static const int MAX_CHANNELS = 2;
    int CollectChannels(QpChannel** out);

    // One CQ in the polling rotation of a single comp_channel wakeup.
    struct PollTarget {
        ibv_cq* cq;
        bool is_send;
        QpChannel* channel;
    };
    static const int MAX_POLL_TARGETS = 2 * MAX_CHANNELS;

    // Fill `out` (capacity MAX_POLL_TARGETS) with the CQs to poll, recv
    // before send so that arriving data is handled before completions.
    // Returns the number of entries written.
    int CollectPollTargets(PollTarget* out);

    // Poll CQ and get the work completion
    static void PollCq(Socket* m);

    // Get the description of current handshake state
    std::string GetStateStr() const;

    // The flow-control state of one channel, for DebugInfo(). `prefix' is
    // prepended to every field name ("rdma_" for the host channel, "gdr_"
    // for the device one), so that /connections shows the two windows side
    // by side without either set of names having to be invented twice.
    void DumpChannel(std::ostream& os, butil::StringPiece connector,
                     const char* prefix, const QpChannel& qc) const;

    // Add cq socket id to poller
    void PollerAddCqSid();

    // Remove cq socket id to poller
    void PollerRemoveCqSid();

    // Not owner
    Socket* _socket;

    // State of Handshake. FALLBACK_TCP publishes RdmaTransport::_rdma_state
    // with release ordering and is consumed by OnNewDataFromTcpAtClient with acquire
    // ordering. Other state accesses do not publish data and use relaxed
    // ordering.
    butil::atomic<State> _state;

    // Wire-level handshake protocol version (set by dispatch in
    // ProcessHandshakeAtClient/Server). Aligned with the protocol code:
    //   0 = unnegotiated
    //   2 = v2 "RDMA"
    //   3 = v3 "RDM3"
    int _handshake_version;

    // ECE payload to advertise in the next local hello:
    //   Client: the locally queried ECE capabilities (filled
    //           before C_HELLO_SEND);
    //   Server: the reduced/negotiated ECE queried after the
    //           QP reached RTS (filled in BringUpQp).
    butil::optional<ibv_ece> _outgoing_ece;

    // The comp channel every CQ of this connection reports to, owned. Null
    // before the resources are allocated, and for the whole life of a polling
    // -mode connection. One per connection rather than per channel: that
    // relation is the "four CQs, one comp channel" of gdr_design.md section
    // 10, and it is why the device CQs can be created here at all.
    RdmaCompChannel* _comp_channel{nullptr};

    // The SocketId which wrap the comp channel of CQ. One per connection,
    // like _comp_channel itself.
    SocketId _cq_sid;

    // butex for inform read events on TCP fd during handshake
    butil::atomic<int> *_read_butex;

    // The two channels. Everything that belongs to one direction and not the
    // other lives in one of these; what is left above is genuinely shared.
    //
    // _device is engaged exactly on the connections that have a device
    // channel, which is a minority of them: a plain RDMA connection carries
    // no DeviceChannel at all rather than an inert one. It starts out
    // engaged on a configured connection (see ResetDeviceChannel()) and is
    // handed back if the handshake does not negotiate one. It is written only
    // by the constructor, by the handshake before RDMA_ON is published, and
    // by the teardown of an already quiesced connection -- which is why the
    // writer and the poller can read it without any synchronization of their
    // own.
    HostChannel _host;
    butil::optional<DeviceChannel> _device;

    DISALLOW_COPY_AND_ASSIGN(RdmaEndpoint);

    // Cq socket id operation type
    struct CqSidOp {
        enum OpType {
            ADD,
            REMOVE,
        };
        SocketId sid;
        OpType type;
    };
    // Poller instance
    struct BAIDU_CACHELINE_ALIGNMENT Poller {
        bthread_t tid{INVALID_BTHREAD};
        butil::MPSCQueue<CqSidOp, butil::ObjectPoolAllocator<CqSidOp>> op_queue;
        // Callback used for io_uring/spdk etc
        std::function<void()> callback;
        // Init and Destroy function
        std::function<void()> init_fn;
        std::function<void()> release_fn;
    };
    // Poller group
    struct BAIDU_CACHELINE_ALIGNMENT PollerGroup {
        PollerGroup() : pollers(FLAGS_rdma_poller_num), running(false) {}
        std::vector<Poller> pollers;
        std::atomic<bool> running;
    };
    static std::vector<PollerGroup> _poller_groups;
};

}  // namespace rdma
}  // namespace brpc

#else  // if BRPC_WITH_RDMA

class RdmaEndpoint { };

#endif  // ifdef USE_RD<A

#endif // BRPC_RDMA_ENDPOINT_H
