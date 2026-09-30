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

#include <gflags/gflags.h>
#include "butil/fd_utility.h"
#include "butil/logging.h"                   // CHECK, LOG
#include "butil/sys_byteorder.h"             // HostToNet,NetToHost
#include "bthread/bthread.h"
#include "brpc/errno.pb.h"
#include "brpc/event_dispatcher.h"
#include "brpc/input_messenger.h"
#include "brpc/socket.h"
#include "brpc/reloadable_flags.h"
#include "brpc/rdma/block_pool.h"
#include "brpc/rdma/device_memory.h"
#include "brpc/rdma/rdma_helper.h"
#include "brpc/rdma/rdma_endpoint.h"
#include "brpc/rdma_transport.h"
#include "brpc/rdma/rdma_handshake.h"
#include "brpc/rdma/rdma_handshake_constants.h"

DECLARE_int32(task_group_ntags);

namespace brpc {
namespace rdma {

extern ibv_cq* (*IbvCreateCq)(ibv_context*, int, void*, ibv_comp_channel*, int);
extern int (*IbvDestroyCq)(ibv_cq*);
extern ibv_comp_channel* (*IbvCreateCompChannel)(ibv_context*);
extern int (*IbvDestroyCompChannel)(ibv_comp_channel*);
extern int (*IbvGetCqEvent)(ibv_comp_channel*, ibv_cq**, void**);
extern void (*IbvAckCqEvents)(ibv_cq*, unsigned int);
extern ibv_qp* (*IbvCreateQp)(ibv_pd*, ibv_qp_init_attr*);
extern int (*IbvModifyQp)(ibv_qp*, ibv_qp_attr*, ibv_qp_attr_mask);
extern int (*IbvQueryQp)(ibv_qp*, ibv_qp_attr*, ibv_qp_attr_mask, ibv_qp_init_attr*);
extern int (*IbvDestroyQp)(ibv_qp*);
extern int (*IbvQueryEce)(ibv_qp*, ibv_ece*);
extern int (*IbvSetEce)(ibv_qp*, ibv_ece*);
extern bool g_skip_rdma_init;

// Only for UT: force AllocateResources() to fail, so that the "fallback to TCP" path
// of the handshake can be tested without a real RDMA device.
bool g_fail_resource_alloc_for_test = false;

DEFINE_int32(rdma_sq_size, 128, "SQ size for RDMA");
DEFINE_int32(rdma_rq_size, 128, "RQ size for RDMA");
DEFINE_bool(rdma_recv_zerocopy, true, "Enable zerocopy for receive side");
DEFINE_int32(rdma_zerocopy_min_size, 512, "The minimal size for receive zerocopy");
DEFINE_int32(rdma_cqe_poll_once, 32, "The maximum of cqe number polled once.");
DEFINE_int32(rdma_prepared_qp_size, 128, "SQ and RQ size for prepared QP.");
DEFINE_int32(rdma_prepared_qp_cnt, 1024, "Initial count of prepared QP.");
DEFINE_bool(rdma_trace_verbose, false, "Print log message verbosely");
BRPC_VALIDATE_GFLAG(rdma_trace_verbose, brpc::PassValidate);
DEFINE_bool(rdma_use_polling, false, "Use polling mode for RDMA.");
DEFINE_int32(rdma_poller_num, 1, "Poller number in RDMA polling mode.");
DEFINE_bool(rdma_poller_yield, false, "Yield thread in RDMA polling mode.");
DEFINE_bool(rdma_disable_bthread, false, "Disable bthread in RDMA");
DEFINE_int32(rdma_device_sq_size, 16, "SQ size of the GDR device QP. Kept far "
             "below --rdma_sq_size on purpose: each device WR can hold a whole "
             "--rdma_gdr_recv_block_size block of pinned GPU memory.");
DEFINE_int32(rdma_device_rq_size, 16, "RQ size of the GDR device QP. This many "
             "--rdma_gdr_recv_block_size device blocks are pinned per "
             "connection, so raising it costs GPU memory linearly.");
DEFINE_int64(rdma_gdr_pending_bytes_watermark, 256L * 1024 * 1024,
             "Stop returning host receive credits once the parser is holding "
             "this many bytes of half-arrived messages. A fuse against a slow "
             "consumer, not a normal-traffic mechanism.");
DEFINE_int64(rdma_gdr_pending_msgs_watermark, 1024,
             "Same fuse as --rdma_gdr_pending_bytes_watermark, counted in "
             "messages instead of bytes.");

static const size_t IOBUF_BLOCK_HEADER_LEN = 32; // implementation-dependent

// DO NOT change this value unless you know the safe value!!!
// This is the number of reserved WRs in SQ/RQ for pure ACK.
extern const size_t RESERVED_WR_NUM = 3;

// The local recv block size, set during GlobalInitialize.
uint32_t g_rdma_recv_block_size = 0;

// static const uint32_t MAX_INLINE_DATA = 64;
static const uint8_t MAX_HOP_LIMIT = 16;
static const uint8_t TIMEOUT = 14;
static const uint8_t RETRY_CNT = 7;
extern const uint16_t MIN_QP_SIZE = 16;
static const uint16_t MAX_QP_SIZE = 4096;
extern const uint16_t MIN_BLOCK_SIZE = 1024;

static butil::Mutex* g_rdma_resource_mutex = nullptr;
static PreparedResource* g_rdma_resource_list = nullptr;

RdmaCompChannel* RdmaCompChannel::Create() {
    std::unique_ptr<RdmaCompChannel> cc(new RdmaCompChannel);
    cc->channel = IbvCreateCompChannel(GetRdmaContext());
    if (nullptr == cc->channel) {
        PLOG(WARNING) << "Fail to create comp channel for CQ";
        return nullptr;
    }
    // Both attributes are about where this fd ends up: an EventDispatcher
    // reads it edge-triggered and must not block on it, and an exec'd child
    // has no business keeping a connection's completion channel open.
    if (butil::make_close_on_exec(cc->channel->fd) < 0) {
        PLOG(WARNING) << "Fail to set comp channel close-on-exec";
        return nullptr;
    }
    if (butil::make_non_blocking(cc->channel->fd) < 0) {
        PLOG(WARNING) << "Fail to set comp channel nonblocking";
        return nullptr;
    }
    return cc.release();
}

RdmaCompChannel::~RdmaCompChannel() {
    if (nullptr != channel) {
        int err = IbvDestroyCompChannel(channel);
        LOG_IF(WARNING, 0 != err) << "Fail to destroy CQ channel: " << berror(err);
        channel = nullptr;
    }
}

RdmaResource::~RdmaResource() {
    if (nullptr != qp) {
        IbvDestroyQp(qp);
    }
    if (nullptr != polling_cq) {
        IbvDestroyCq(polling_cq);
    }
    if (nullptr != send_cq) {
        IbvDestroyCq(send_cq);
    }
    if (nullptr != recv_cq) {
        IbvDestroyCq(recv_cq);
    }
}

PreparedResource::~PreparedResource() {
    // In this order: the CQs inside `resource' report to `comp_channel', and
    // ibv_destroy_comp_channel() refuses a channel that still has CQs on it.
    delete resource;
    resource = nullptr;
    delete comp_channel;
    comp_channel = nullptr;
}

QpChannel::QpChannel()
    : resource(nullptr)
    , sq_size(0)
    , rq_size(0)
    , local_window_capacity(0)
    , remote_window_capacity(0)
    , sq_current(0)
    , sq_unsignaled(0)
    , sq_sent(0)
    , rq_received(0)
    , sq_imm_window_size(0)
    , remote_rq_window_size(0)
    , sq_window_size(0)
    , new_rq_wrs(0)
    , remote_recv_block_size(0)
    , send_cq_events(0)
    , recv_cq_events(0) {
}

void QpChannel::Reset() {
    resource = nullptr;
    local_window_capacity = 0;
    remote_window_capacity = 0;
    sq_current = 0;
    sq_unsignaled = 0;
    sq_sent = 0;
    rq_received = 0;
    sq_imm_window_size = 0;
    remote_rq_window_size.store(0, butil::memory_order_relaxed);
    sq_window_size.store(0, butil::memory_order_relaxed);
    new_rq_wrs.store(0, butil::memory_order_relaxed);
    remote_recv_block_size = 0;
    send_cq_events = 0;
    recv_cq_events = 0;
}

namespace {
// Clamp a queue-depth flag into what a QP can actually be created with.
uint16_t ClampQpSize(int32_t flag_value) {
    return (uint16_t)std::min<int32_t>(
            std::max<int32_t>(flag_value, MIN_QP_SIZE), MAX_QP_SIZE);
}

// IOBuf user_data deleter for a received device block.
void ReturnBlockToDevicePool(void* dptr) {
    DeallocDeviceBlock(dptr);
}
}  // namespace

HostChannel::HostChannel() {
    sq_size = ClampQpSize(FLAGS_rdma_sq_size);
    rq_size = ClampQpSize(FLAGS_rdma_rq_size);
}

void HostChannel::Reset() {
    QpChannel::Reset();
    sbuf.clear();
    rbuf.clear();
    rbuf_data.clear();
    input_processor.Reset();
    unsolicited = 0;
    unsolicited_bytes = 0;
    accumulated_ack = 0;
}

bool HostChannel::AllocateBuffers() {
    sbuf.resize(sq_size - RESERVED_WR_NUM);
    if (sbuf.size() != sq_size - RESERVED_WR_NUM) {
        return false;
    }
    rbuf.resize(rq_size);
    if (rbuf.size() != rq_size) {
        return false;
    }
    rbuf_data.resize(rq_size, nullptr);
    if (rbuf_data.size() != rq_size) {
        return false;
    }
    return true;
}

ssize_t HostChannel::TakeRecvData(size_t len) {
    butil::IOPortal& read_buf = input_processor.read_buf();
    if (FLAGS_rdma_recv_zerocopy && len >= (size_t)FLAGS_rdma_zerocopy_min_size) {
        // Hand the whole block to the parser. The slot is now empty, which
        // is how PrepareRecvSlot() knows to fetch a fresh block for it.
        rbuf[rq_received].cutn(&read_buf, len);
        rbuf_data[rq_received] = nullptr;
    } else {
        // Copy data when the receive data is really small, so that the block
        // can be posted again in place.
        read_buf.append(rbuf_data[rq_received], len);
    }
    return len;
}

bool HostChannel::PrepareRecvSlot(uint16_t i, void** block, size_t* size,
                                  uint32_t* lkey) {
    if (nullptr == rbuf_data[i]) {
        rbuf[i].clear();
        butil::IOBufAsZeroCopyOutputStream os(
                &rbuf[i], g_rdma_recv_block_size + IOBUF_BLOCK_HEADER_LEN);
        int size_alloc = 0;
        if (!os.Next(&rbuf_data[i], &size_alloc)) {
            rbuf_data[i] = nullptr;
            PLOG(WARNING) << "Fail to allocate a host recv block";
            return false;
        }
        CHECK(static_cast<uint32_t>(size_alloc) == g_rdma_recv_block_size)
                << "size_alloc=" << size_alloc;
    }
    *block = rbuf_data[i];
    *size = g_rdma_recv_block_size;
    // Host blocks come from the block pool, which knows their lkey.
    *lkey = GetRegionId(*block);
    return true;
}

void HostChannel::ReleaseRecvSlot(uint16_t i) {
    rbuf[i].clear();
    rbuf_data[i] = nullptr;
}

bool HostChannel::SolicitWr(size_t len, uint32_t imm, bool must_solicit) {
    // Avoid too much recv completion event to reduce the cpu overhead
    bool solicited = must_solicit;
    if (!solicited) {
        if (unsolicited > local_window_capacity / 4) {
            // Make sure the recv side can be signaled to return ack
            solicited = true;
        } else if (accumulated_ack > remote_window_capacity / 4) {
            // Make sure the recv side can be signaled to handle ack
            solicited = true;
        } else if (unsolicited_bytes > 1048576) {
            // Make sure the recv side can be signaled when it receives enough data
            solicited = true;
        } else {
            ++unsolicited;
            unsolicited_bytes += len;
            accumulated_ack += imm;
        }
    }
    if (solicited) {
        unsolicited = 0;
        unsolicited_bytes = 0;
        accumulated_ack = 0;
    }
    return solicited;
}

DeviceChannel::DeviceChannel() {
    sq_size = ClampQpSize(FLAGS_rdma_device_sq_size);
    rq_size = ClampQpSize(FLAGS_rdma_device_rq_size);
    // Read here rather than at allocation time, because the server computes
    // the device windows from ApplyRemoteHello() and fills its hello from
    // these -- both before anything is allocated.
    recv_block_size = GetDeviceRecvBlockSize();
}

DeviceChannel::~DeviceChannel() {
    // No lock, for the same reason the host teardown needs none: the poller
    // does not post, so nothing can be halfway through
    // CutFromDeviceAttachment() here. Every path that destroys us has the
    // socket quiesced -- either the handshake has not brought the channel up
    // yet (nothing was ever posted), or we came from Reset(), which
    // Socket::WaitAndReset() and Socket::OnRecycle() only reach after the
    // references are gone.
    if (nullptr != resource) {
        if (nullptr != resource->send_cq) {
            IbvAckCqEvents(resource->send_cq, send_cq_events);
        }
        if (nullptr != resource->recv_cq) {
            IbvAckCqEvents(resource->recv_cq, recv_cq_events);
        }
        // Destroying the QP first is what makes the loop below safe: until
        // it is gone the NIC may still be writing into the posted blocks.
        // The connection's comp channel is not ours and stays behind.
        delete resource;
        resource = nullptr;
    }

    for (size_t i = 0; i < rbuf_data.size(); ++i) {
        if (nullptr != rbuf_data[i]) {
            DeallocDeviceBlock(rbuf_data[i]);
        }
    }
}

bool DeviceChannel::AllocateBuffers() {
    sbuf.resize(sq_size - RESERVED_WR_NUM);
    if (sbuf.size() != sq_size - RESERVED_WR_NUM) {
        return false;
    }
    rbuf_data.resize(rq_size, nullptr);
    if (rbuf_data.size() != rq_size) {
        return false;
    }
    rbuf_lkey.resize(rq_size, 0);
    if (rbuf_lkey.size() != rq_size) {
        return false;
    }
    return true;
}

ssize_t DeviceChannel::TakeRecvData(size_t len) {
    void* block = rbuf_data[rq_received];
    CHECK(block != nullptr) << "device recv slot " << rq_received
                            << " completed without a block";
    // Hand the block itself over: device memory is never copied here, the
    // DeviceStream owns it until the application is done with it. The slot
    // is now empty, which is how PrepareRecvSlot() knows to fetch a new one.
    if (stream.recv_stream()->append_user_data(
                block, len, ReturnBlockToDevicePool) < 0) {
        PLOG(WARNING) << "Fail to append a received device block of "
                      << len << " bytes";
        return -1;
    }
    rbuf_data[rq_received] = nullptr;
    rbuf_lkey[rq_received] = 0;
    // None of these bytes reach the messenger's read_buf.
    return 0;
}

bool DeviceChannel::PrepareRecvSlot(uint16_t i, void** block, size_t* size,
                                    uint32_t* lkey) {
    if (nullptr == rbuf_data[i]) {
        uint32_t new_lkey = 0;
        void* new_block = AllocDeviceBlock(recv_block_size, &new_lkey);
        if (nullptr == new_block) {
            PLOG(WARNING) << "Fail to allocate a device recv block of "
                          << recv_block_size << " bytes";
            return false;
        }
        rbuf_data[i] = new_block;
        rbuf_lkey[i] = new_lkey;
    }
    *block = rbuf_data[i];
    *size = recv_block_size;
    // Device memory is not in the host block pool; its registry hands the
    // lkey out at allocation time, which is why we kept it.
    *lkey = rbuf_lkey[i];
    return true;
}

RdmaEndpoint::RdmaEndpoint(Socket* s)
    : _socket(s)
    , _state(UNINIT)
    , _handshake_version(0)
    , _cq_sid(INVALID_SOCKET_ID)
{
    _read_butex = bthread::butex_create_checked<butil::atomic<int> >();
    _host.input_processor.Init(s, InputMessengerProcessor::STREAM_RDMA_QP);
    ResetDeviceChannel();
}

RdmaEndpoint::~RdmaEndpoint() {
    Reset();
    bthread::butex_destroy(_read_butex);
}

bool RdmaEndpoint::device_channel_required() const {
    return _socket->socket_mode() == SOCKET_MODE_RDMA_AND_DEVICE;
}

void RdmaEndpoint::ResetDeviceChannel() {
    CHECK(!_device.has_value());
    if (!device_channel_required()) {
        return;
    }
    // The channel exists before the handshake does, because the client's
    // hello has to carry a device qp_num and therefore the device QP must
    // already exist when the hello is written. That makes the client's hello
    // a proposal rather than a probe: if the server's hello comes back
    // without a device field, ApplyRemoteHello() hands the QP back.
    //
    // Proposing at all is the channel's configuration, not a capability
    // question: a channel that did not ask does not make its peer pay for
    // rq_size * -rdma_gdr_recv_block_size of registered receive blocks. What
    // could make the proposal impossible -- no GDR pool of our own, polling
    // mode, a v2 handshake -- is rejected at Channel::Init() by
    // RdmaTransport::ContextInitOrDie(), so there is nothing left to probe
    // for and anything that still goes wrong fails the connection.
    _device.emplace();
    _device->stream.set_pending_hook(OnDevicePendingChanged, this);
}

void RdmaEndpoint::Reset() {
    // Takes _device with it: the device channel hangs off the host one.
    DeallocateResources();

    _state.store(UNINIT, butil::memory_order_relaxed);
    _handshake_version = 0;
    _outgoing_ece.reset();
    _cq_sid = INVALID_SOCKET_ID;
    _host.Reset();
    // Back to what the constructor left, so that a reused endpoint cannot
    // come up host-only. The destructor runs through here too and rebuilds a
    // DeviceChannel that is destroyed a moment later with the member; three
    // flag reads are not worth splitting Reset() in two to avoid.
    ResetDeviceChannel();
}

void RdmaConnect::StartConnect(const Socket* socket,
                               void (*done)(int err, void* data),
                               void* data) {
    auto* rdma_transport = static_cast<RdmaTransport*>(socket->_transport.get());
    CHECK(rdma_transport->_rdma_ep != nullptr);
    SocketUniquePtr s;
    if (Socket::Address(socket->id(), &s) != 0) {
        return;
    }
    if (!IsRdmaAvailable()) {
        rdma_transport->_rdma_state = RdmaTransport::RDMA_OFF;
        rdma_transport->_rdma_ep->_state.store(
            RdmaEndpoint::FALLBACK_TCP, butil::memory_order_release);
        done(0, data);
        return;
    }
    _done = done;
    _data = data;
    bthread_t tid;
    bthread_attr_t attr = BTHREAD_ATTR_NORMAL;
    bthread_attr_set_name(&attr, "RdmaProcessHandshakeAtClient");
    if (bthread_start_background(&tid, &attr,
                                 RdmaEndpoint::ProcessHandshakeAtClient,
                                 rdma_transport->_rdma_ep) < 0) {
        LOG(FATAL) << "Fail to start handshake bthread";
        Run();
    } else {
        s.release();
    }
}

void RdmaConnect::StopConnect(Socket* socket) { }

void RdmaConnect::Run() {
    _done(errno, _data);
}

void RdmaEndpoint::OnNewDataFromTcp(Socket* s) {
    if (s->CreatedByConnect()) {
        OnNewDataFromTcpAtClient(s);
    } else {
        OnNewDataFromTcpAtServer(s);
    }
}

void RdmaEndpoint::OnNewDataFromTcpAtClient(Socket* s) {
    auto* rdma_transport = static_cast<RdmaTransport*>(s->_transport.get());
    RdmaEndpoint* ep = rdma_transport->GetRdmaEp();
    CHECK(ep != nullptr);

    int progress = Socket::PROGRESS_INIT;
    while (true) {
        // Pair with release stores of FALLBACK_TCP so RDMA_OFF is visible
        // before normal TCP message processing starts.
        const State state = ep->_state.load(butil::memory_order_acquire);
        if (state == UNINIT) {
            // The connection may be closed or reset before the client starts
            // handshake. This will be handled by client handshake. Ignore here.
        } else if (state < ESTABLISHED) {  // during handshake
            ep->_read_butex->fetch_add(1, butil::memory_order_release);
            bthread::butex_wake(ep->_read_butex);
        } else if (state == FALLBACK_TCP){  // handshake finishes
            InputMessenger::OnNewMessages(s);
            return;
        } else if (state == ESTABLISHED) {
            if (!ep->HandleTcpEventAfterEstablished()) {
                return;
            }
        }
        if (!s->MoreReadEvents(&progress)) {
            break;
        }
    }
}

void RdmaEndpoint::OnNewDataFromTcpAtServer(Socket* s) {
    auto* rdma_transport = static_cast<RdmaTransport*>(s->_transport.get());
    RdmaEndpoint* ep = rdma_transport->GetRdmaEp();
    CHECK(ep != nullptr);

    int progress = Socket::PROGRESS_INIT;
    while (true) {
        if (s->Failed()) {
            return;
        }

        // Pair with the release stores of ESTABLISHED / FALLBACK_TCP.
        if (ep->_state.load(butil::memory_order_acquire) != ESTABLISHED) {
            InputMessenger::OnNewMessages(s);
            // That call may have just finished the handshake and turned RDMA
            // on. Start consuming CQ events here rather than inside the parse
            // callback: by now OnNewMessages is done with the Socket's
            // `parsing_context` / `preferred_index`, so the QP stream can take
            // them over without ever overlapping with the fd stream. This is
            // the ordering StartCqEvents() asks for.
            if (!s->Failed() &&
                ep->_state.load(butil::memory_order_acquire) == ESTABLISHED &&
                ep->StartCqEvents() < 0) {
                const int saved_errno = errno;
                PLOG(WARNING) << "Fail to start cq events on " << *s;
                ep->_state.store(FAILED, butil::memory_order_relaxed);
                s->SetFailed(saved_errno, "Fail to start cq events on %s: %s",
                             s->description().c_str(), berror(saved_errno));
            }
            return;
        }
        // RDMA carries the RPCs now, so the fd is watched for EOF only and must
        // not be parsed: `preferred_index' / `parsing_context' live on the Socket
        // and the QP stream is driving them (https://github.com/apache/brpc/issues/3479).
        if (!ep->HandleTcpEventAfterEstablished()) {
            return;
        }
        if (!s->MoreReadEvents(&progress)) {
            break;
        }
    }
}

bool RdmaEndpoint::HandleTcpEventAfterEstablished() {
    uint8_t tmp;
    ssize_t nr = read(_socket->fd(), &tmp, 1);
    if (nr == 0) {
        _socket->SetEOF();
        return false;
    }
    if (nr > 0) {
        LOG(WARNING) << "Read unexpected data from " << *_socket;
        _socket->SetFailed(EPROTO, "Read unexpected data from %s",
                           _socket->description().c_str());
        return false;
    }

    if (errno != EAGAIN) {
        const int saved_errno = errno;
        PLOG(WARNING) << "Fail to read from " << *_socket;
        _socket->SetFailed(saved_errno, "Fail to read from %s: %s",
                           _socket->description().c_str(),
                           berror(saved_errno));
        // The socket is dead now, so do not come back for another read of it.
        return false;
    }
    return true;
}

static const int WAIT_TIMEOUT_MS = 50;

// Drive an EAGAIN-aware read loop to completion (exactly `len` bytes).
// `read_once(offset, remaining)` performs ONE underlying read attempt:
//   returns > 0  : number of bytes consumed (added to running total);
//   returns = 0  : end-of-stream (the loop fails with EEOF);
//   returns < 0  : errno set; EAGAIN is handled here via butex_wait,
//                   any other errno bubbles up.
// `offset` is bytes already received in THIS call (initially 0); the
// callable uses it to choose the next write target (e.g. `(char*)buf
// + offset`). Callables that don't need offset (e.g. IOPortal append)
// can ignore it.
//
// Centralizes the EAGAIN/butex/EOF loop so the two ReadFromFd
// overloads below stay one-liners; any future read source (memory-
// mapped, scatter-vector, etc.) can plug in by passing its own
// `read_once`.
template <class ReadOnce>
static int ReadFromFdLoop(butil::atomic<int>* read_butex,
                          size_t len, ReadOnce&& read_once) {
    size_t received = 0;
    while (received < len) {
        const int expected_val = read_butex->load(butil::memory_order_acquire);
        const timespec duetime = butil::milliseconds_from_now(WAIT_TIMEOUT_MS);
        ssize_t nr = read_once(received, len - received);
        if (nr < 0) {
            if (errno == EAGAIN) {
                if (bthread::butex_wait(read_butex, expected_val, &duetime) < 0) {
                    if (errno != EWOULDBLOCK && errno != ETIMEDOUT) {
                        return -1;
                    }
                }
            } else {
                return -1;
            }
        } else if (nr == 0) {  // Got EOF
            errno = EEOF;
            return -1;
        } else {
            received += nr;
        }
    }
    return 0;
}

int RdmaEndpoint::ReadFromFd(void* data, size_t len) {
    CHECK(data != nullptr);
    const int fd = _socket->fd();
    return ReadFromFdLoop(_read_butex, len,
        [data, fd](size_t offset, size_t remaining) {
            return read(fd, (uint8_t*)data + offset, remaining);
        });
}

int RdmaEndpoint::ReadFromFd(butil::IOPortal* data, size_t len) {
    CHECK(data != nullptr);
    const int fd = _socket->fd();
    return ReadFromFdLoop(_read_butex, len,
        [data, fd](size_t /*offset*/, size_t remaining) {
            return data->append_from_file_descriptor(fd, remaining);
        });
}

// Drive an EAGAIN-aware write loop to completion (exactly `len` bytes).
//
// `write_once(offset, remaining)` performs ONE underlying write attempt:
//   - returns >= 0 : number of bytes consumed (added to running total);
//   - returns < 0  : errno set; EAGAIN triggers `wait_writable(duetime)`,
//                   any other errno bubbles up.
// `offset` is bytes already written in THIS call (initially 0); the
// callable uses it to choose the next read source (e.g. `(char*)buf
// + offset`). Callables that drain a self-tracking sink (e.g.
// IOBuf::cut_into_file_descriptor) can ignore both args.
//
// `wait_writable(duetime)` is invoked on EAGAIN to park until the fd
// becomes writable again. It returns 0 on wake-up (or ETIMEDOUT),
// non-zero on hard failure.
template <class WriteOnce, class WaitWritable>
static int WriteToFdLoop(size_t len, WriteOnce&& write_once, WaitWritable&& wait_writable) {
    size_t written = 0;
    while (written < len) {
        const timespec duetime = butil::milliseconds_from_now(WAIT_TIMEOUT_MS);
        ssize_t nw = write_once(written, len - written);
        if (nw >= 0) {
            written += nw;
            continue;
        }

        if (errno != EAGAIN) {
            return -1;
        }
        if (!wait_writable(&duetime)) {
            return -1;
        }
    }
    return 0;
}

int RdmaEndpoint::WriteToFd(void* data, size_t len) {
    CHECK(data != nullptr);
    Socket* s = _socket;
    const int fd = s->fd();
    return WriteToFdLoop(len,
        [data, fd](size_t offset, size_t remaining) {
            return write(fd, (uint8_t*)data + offset, remaining);
        },
        [s, fd](const timespec* duetime) {
            return s->WaitEpollOut(fd, true, duetime) == 0 || errno == ETIMEDOUT;
        });
}

int RdmaEndpoint::WriteToFd(butil::IOBuf* data) {
    CHECK(data != nullptr);
    Socket* s = _socket;
    const int fd = s->fd();
    return WriteToFdLoop(data->size(),
        [data, fd](size_t /*offset*/, size_t /*remaining*/) {
            return data->cut_into_file_descriptor(fd);
        },
        [s, fd](const timespec* duetime) {
            return s->WaitEpollOut(fd, true, duetime) == 0 || errno == ETIMEDOUT;
        });
}

void RdmaEndpoint::ApplyRemoteHello(const ParsedHello& remote) {
    _host.remote_recv_block_size = remote.block_size;
    _host.local_window_capacity =
        std::min(_host.sq_size, remote.rq_size) - RESERVED_WR_NUM;
    _host.remote_window_capacity =
        std::min(_host.rq_size, remote.sq_size) - RESERVED_WR_NUM;
    _host.sq_imm_window_size = RESERVED_WR_NUM;
    _host.remote_rq_window_size.store(_host.local_window_capacity,
                                         butil::memory_order_relaxed);
    _host.sq_window_size.store(_host.local_window_capacity,
                                  butil::memory_order_relaxed);

    // The device channel exists only if both sides asked for one. Its
    // windows are computed from the device queue sizes alone, deliberately
    // sharing nothing with the host windows above: that independence is the
    // whole point of the second QP (docs/cn/gdr_design.md section 9).
    if (!_device.has_value()) {
        return;
    }
    if (!remote.device.has_value()) {
        // The peer does not want one, so hand ours back. This is the single
        // place where that happens, and it means something slightly different
        // at each end.
        //
        // On the client it takes back a QP allocated speculatively: its hello
        // had to carry a device qp_num, so it had to ask before it could know
        // the answer.
        //
        // On the server it is the whole of the decision. The server never
        // proposes a device channel, it only agrees to one the client already
        // asked for -- SOCKET_MODE_RDMA_AND_DEVICE means "willing" here
        // rather than "required", so a client that brings none is served
        // host-only instead of being refused: by the time we know, the
        // service has not run yet and failing would take down traffic that
        // never wanted the second channel. Agreeing is opt-in all the same,
        // because it costs rq_size * -rdma_gdr_recv_block_size of registered
        // memory per connection, which no client should be able to make us
        // spend just by asking. Nothing is spent before this point: the
        // buffers are AllocateBuffers()' doing, and the server runs this
        // before AllocateResources(). What that memory is made of is our own
        // business and the client is never told.
        _device.reset();
        return;
    }
    _device->remote_recv_block_size = remote.device->block_size;
    _device->local_window_capacity =
        std::min(_device->sq_size, remote.device->rq_size) - RESERVED_WR_NUM;
    _device->remote_window_capacity =
        std::min(_device->rq_size, remote.device->sq_size) - RESERVED_WR_NUM;
    _device->sq_imm_window_size = RESERVED_WR_NUM;
    _device->remote_rq_window_size.store(
        _device->local_window_capacity, butil::memory_order_relaxed);
    _device->sq_window_size.store(
        _device->local_window_capacity, butil::memory_order_relaxed);
}

// Client-side handshake entry: the state machine.
//
//   C_ALLOC_QPCQ
//     |
//     v
//   C_HELLO_SEND  (hs->SendLocalHello)
//     |
//     v
//   C_HELLO_WAIT  (hs->ReceiveAndParseRemoteHello)
//     |
//     v
//   [negotiation: ApplyRemoteHello + C_BRINGUP_QP]
//     |
//     v
//   C_ACK_SEND
//     |
//     v
//   ESTABLISHED / FALLBACK_TCP
void* RdmaEndpoint::ProcessHandshakeAtClient(void* arg) {
    auto ep = static_cast<RdmaEndpoint*>(arg);
    SocketUniquePtr s(ep->_socket);
    RdmaConnect::RunGuard rg((RdmaConnect*)s->_app_connect.get());
    auto rdma_transport = static_cast<RdmaTransport*>(s->_transport.get());

    LOG_IF(INFO, FLAGS_rdma_trace_verbose)
        << "Start handshake on " << s->description();

    std::unique_ptr<RdmaHandshake> handshake = CreateClientHandshake(ep);
    CHECK(handshake != nullptr);
    ep->_handshake_version = handshake->ProtocolVersion();

    // First initialize CQ and QP resources. _device is already engaged if
    // this channel is configured for one (see ResetDeviceChannel()), so the
    // device QP is allocated here together with the host one and is ready to
    // be named in the hello below.
    ep->_state.store(C_ALLOC_QPCQ, butil::memory_order_relaxed);
    if (ep->AllocateResources() < 0) {
        PLOG(WARNING) << "Fail to allocate rdma resources, fallback to tcp:"
                      << s->description();
        errno = 0;
        rdma_transport->_rdma_state = RdmaTransport::RDMA_OFF;
        ep->_state.store(FALLBACK_TCP, butil::memory_order_release);
        return nullptr;
    }

    // Send hello message to server
    ep->_state.store(C_HELLO_SEND, butil::memory_order_relaxed);
    if (handshake->SendLocalHello() < 0) {
        int saved_errno = errno;
        PLOG(WARNING) << "Fail to send hello message to server:"
                      << s->description();
        s->SetFailed(saved_errno, "Fail to complete rdma handshake from %s: %s",
                     s->description().c_str(), berror(saved_errno));
        ep->_state.store(FAILED, butil::memory_order_relaxed);
        return nullptr;
    }

    // Receive and parse remote hello.
    ep->_state.store(C_HELLO_WAIT, butil::memory_order_relaxed);
    ParsedHello remote{};
    const RemoteHelloResult r = handshake->ReceiveAndParseRemoteHello(&remote);
    if (r == RemoteHelloResult::ERROR) {
        int saved_errno = errno;
        PLOG(WARNING) << "Fail to receive hello from server:"
                      << s->description();
        s->SetFailed(saved_errno, "Fail to complete rdma handshake from %s: %s",
                     s->description().c_str(), berror(saved_errno));
        ep->_state.store(FAILED, butil::memory_order_relaxed);
        return nullptr;
    }

    if (r != RemoteHelloResult::NEGOTIATED) {
        LOG(WARNING) << "Fail to negotiate with server, fallback to tcp:"
                     << s->description();
        rdma_transport->_rdma_state = RdmaTransport::RDMA_OFF;
    } else {
        ep->ApplyRemoteHello(remote);
        ep->_state.store(C_BRINGUP_QP, butil::memory_order_relaxed);
        if (ep->BringUpQp(remote, /*is_server=*/false) < 0) {
            LOG(WARNING) << "Fail to bringup QP, fallback to tcp:"
                         << s->description();
            rdma_transport->_rdma_state = RdmaTransport::RDMA_OFF;
        } else {
            rdma_transport->_rdma_state = RdmaTransport::RDMA_ON;
        }
    }

    // SOCKET_MODE_RDMA_AND_DEVICE is a hard requirement, and this is the
    // single point where it is enforced. Every way of not getting a device channel
    // has converged by here: the peer declined, the device QP could not be
    // allocated or brought up, the handshake was v2, or RDMA itself fell back
    // to TCP.
    //
    // Checking per RPC instead would have to answer before the handshake
    // finished, where the honest answer is "not yet" -- that is the bug in
    // docs/cn/gdr_design.md section 6.2, where every connection's first
    // attachment-carrying RPC failed and its retry succeeded. Failing the
    // connection is only safe because the socket mode is part of the
    // ChannelSignature, so no channel that did not ask shares this Socket.
    if (rdma_transport->_rdma_state != RdmaTransport::RDMA_ON) {
        // The device channel rides on the host QP, so RDMA falling back or
        // failing takes it with it. Reset before the check below, which reads
        // _device as the answer to "did this connection get one".
        ep->_device.reset();
    }
    if (ep->device_channel_required() && !ep->_device.has_value()) {
        LOG(WARNING) << "No GDR device channel on " << s->description()
                     << " while the channel requires one, fail the connection";
        s->SetFailed(EDEVICECHANNEL, "Fail to establish the GDR device channel"
                     " required by this channel on %s", s->description().c_str());
        ep->_state.store(FAILED, butil::memory_order_relaxed);
        return nullptr;
    }

    // Send ACK message to server
    ep->_state.store(C_ACK_SEND, butil::memory_order_relaxed);
    bool rdma_on = rdma_transport->_rdma_state == RdmaTransport::RDMA_ON;
    uint32_t flags = rdma_on ? HELLO_ACK_RDMA_OK : 0;
    uint32_t flags_be = butil::HostToNet32(flags);
    if (ep->WriteToFd(&flags_be, HELLO_ACK_LEN) < 0) {
        int saved_errno = errno;
        PLOG(WARNING) << "Fail to send Ack Message to server:"
                      << s->description();
        s->SetFailed(saved_errno, "Fail to complete rdma handshake from %s: %s",
                     s->description().c_str(), berror(saved_errno));
        ep->_state.store(FAILED, butil::memory_order_relaxed);
        return nullptr;
    }

    if (rdma_transport->_rdma_state == RdmaTransport::RDMA_ON) {
        ep->_state.store(ESTABLISHED, butil::memory_order_release);
        // The handshake is over, so the QP stream may start parsing now.
        if (ep->StartCqEvents() < 0) {
            const int saved_errno = errno;
            PLOG(WARNING) << "Fail to start cq events on " << s->description();
            s->SetFailed(saved_errno, "Fail to complete rdma handshake from %s: %s",
                         s->description().c_str(), berror(saved_errno));
            ep->_state.store(FAILED, butil::memory_order_relaxed);
            return nullptr;
        }
        LOG_IF(INFO, FLAGS_rdma_trace_verbose)
            << "Client handshake ends (use rdma v" << ep->_handshake_version
            << ") on " << s->description();
    } else {
        ep->_state.store(FALLBACK_TCP, butil::memory_order_release);
        LOG_IF(INFO, FLAGS_rdma_trace_verbose)
            << "Client handshake ends (use tcp) on " << s->description();
    }

    errno = 0;

    return nullptr;
}

// Server-side handshake entry: the state machine.
//
//   S_HELLO_WAIT  (read magic + dispatch + hs->ReceiveAndParseRemoteHello)
//     |
//     v
//   [negotiation: ApplyRemoteHello + S_ALLOC_QPCQ + S_BRINGUP_QP]
//     |
//     v
//   S_HELLO_SEND  (hs->SendLocalHello)
//     |
//     v
//   S_ACK_WAIT
//     |
//     v
//   ESTABLISHED / FALLBACK_TCP
ParseResult RdmaEndpoint::ExecuteServerHandshake(butil::IOBuf* source, Socket* s) {
    RdmaTransport* rdma_transport = static_cast<RdmaTransport*>(s->_transport.get());
    RdmaEndpoint* ep = rdma_transport->_rdma_ep;
    CHECK(ep != nullptr);

    const State state = ep->_state.load(butil::memory_order_acquire);
    if (state >= ESTABLISHED) {
        // The handshake is over (ESTABLISHED / FALLBACK_TCP / FAILED). Data
        // arriving now belongs to a real protocol, yet CutInputMessage() still
        // reaches us.
        if (state == ESTABLISHED &&
            s->parsing_stream_type() == InputMessengerProcessor::STREAM_TCP_FD) {
            // RDMA is on, so the fd is not an RPC channel any more and whatever
            // shows up on it is a protocol error. Reached even though
            // OnNewDataFromTcpAtServer() stops handing the fd to OnNewMessages()
            // once RDMA is on, because the handshake completes inside OnNewMessages():
            // that round keeps reading the fd until it goes quiet.
            if (source->empty()) {
                // Nothing to reject yet. Asking for more data keeps the pin, so
                // the rest of this round comes back here rather than reaching a
                // real protocol, and lets OnNewMessages() report EOF as usual.
                return MakeParseError(PARSE_ERROR_NOT_ENOUGH_DATA);
            }
            LOG(WARNING) << "Unexpected " << source->size() << " bytes on the tcp "
                            "fd of an RDMA connection, drop connection: "
                         << s->description();
            ep->_state.store(FAILED, butil::memory_order_relaxed);
            return MakeParseError(PARSE_ERROR_ABSOLUTELY_WRONG);
        }
        // Anything else, for the real protocol to parse: the stream carried by
        // the QP, or an fd that stayed a normal RPC stream because the handshake
        // fell back or failed.
        return MakeParseError(PARSE_ERROR_TRY_OTHERS);
    }

    if (s->parsing_context() == nullptr) {
        // Phase 1: read the client hello, negotiate, reply server hello.
        if (source->size() < HELLO_MAGIC_LEN) {
            return MakeParseError(PARSE_ERROR_NOT_ENOUGH_DATA);
        }
        uint8_t magic[HELLO_MAGIC_LEN];
        CHECK_EQ(source->copy_to(magic, HELLO_MAGIC_LEN), HELLO_MAGIC_LEN);

        // Pick the version-specific server handshake from the peeked magic (the
        // magic is NOT consumed; ReceiveAndParseRemoteHello() reads it again
        // from `source`).
        std::unique_ptr<RdmaHandshake> hs = CreateServerHandshakeByMagic(ep, source, magic);
        if (hs == nullptr) {
            return MakeParseError(PARSE_ERROR_TRY_OTHERS);
        }
        ep->_handshake_version = hs->ProtocolVersion();
        ep->_state.store(S_HELLO_WAIT, butil::memory_order_relaxed);

        ParsedHello remote{};
        const RemoteHelloResult r = hs->ReceiveAndParseRemoteHello(&remote);
        if (r == RemoteHelloResult::NEED_MORE) {
            return MakeParseError(PARSE_ERROR_NOT_ENOUGH_DATA);
        }
        if (r == RemoteHelloResult::ERROR) {
            ep->_state.store(FAILED, butil::memory_order_relaxed);
            return MakeParseError(PARSE_ERROR_ABSOLUTELY_WRONG);
        }

        // Negotiate + allocate resources.
        bool negotiated = r == RemoteHelloResult::NEGOTIATED;
        if (negotiated) {
            // ApplyRemoteHello() runs before AllocateResources(), so a client
            // that did not ask for a device channel has already had ours
            // handed back by the time anything is allocated for it.
            ep->ApplyRemoteHello(remote);
            ep->_state.store(S_ALLOC_QPCQ, butil::memory_order_relaxed);
            if (ep->AllocateResources() < 0) {
                PLOG(WARNING) << "Fail to allocate rdma resources, fallback to tcp:"
                              << s->description();
                negotiated = false;
            } else {
                ep->_state.store(S_BRINGUP_QP, butil::memory_order_relaxed);
                if (ep->BringUpQp(remote, /*is_server=*/true) < 0) {
                    LOG(WARNING) << "Fail to bringup QP, fallback to tcp:"
                                 << s->description();
                    negotiated = false;
                }
            }
        }
        if (!negotiated) {
            rdma_transport->_rdma_state = RdmaTransport::RDMA_OFF;
            // Same rule as on the client: no host QP, no device channel. Also
            // makes the hello below a host-only one, which is what the client
            // must see for its own demand check to fire.
            ep->_device.reset();
        }

        // Reply the server hello.
        // Emits a real hello when _rdma_state != RDMA_OFF;
        // an un-negotiable one otherwise.
        ep->_state.store(S_HELLO_SEND, butil::memory_order_relaxed);
        if (hs->SendLocalHello() < 0) {
            PLOG(WARNING) << "Fail to send server hello to " << s->description();
            ep->_state.store(FAILED, butil::memory_order_relaxed);
            return MakeParseError(PARSE_ERROR_ABSOLUTELY_WRONG);
        }

        // Enter the wait-ACK phase. Whether negotiation succeeded is already
        // recorded in rdma_transport->_rdma_state (RDMA_OFF iff negotiation
        // failed), so the context itself needs no extra flag.
        s->reset_parsing_context(ServerHandshakeContext::Create());
        ep->_state.store(S_ACK_WAIT, butil::memory_order_relaxed);
        return MakeParseError(PARSE_ERROR_NOT_ENOUGH_DATA);
    }

    // Phase 2: drain the 4B ACK and finalize.
    if (source->size() < HELLO_ACK_LEN) {
        return MakeParseError(PARSE_ERROR_NOT_ENOUGH_DATA);
    }

    uint32_t flags_be = 0;
    CHECK_EQ(source->cutn(&flags_be, HELLO_ACK_LEN), HELLO_ACK_LEN);
    uint32_t flags = butil::NetToHost32(flags_be);
    bool client_ack_ok = (flags & HELLO_ACK_RDMA_OK) != 0;
    if (!client_ack_ok) {
        LOG_IF(INFO, FLAGS_rdma_trace_verbose)
            << "Server handshake ends (use tcp) on " << s->description();
        rdma_transport->_rdma_state = RdmaTransport::RDMA_OFF;
        ep->_state.store(FALLBACK_TCP, butil::memory_order_release);
        s->reset_parsing_context(nullptr);
        return MakeParseError(PARSE_ERROR_TRY_OTHERS);
    }

    if (rdma_transport->_rdma_state == RdmaTransport::RDMA_OFF) {
        LOG(WARNING) << "Client wants RDMA in ACK but server fell back: "
                     << s->description();
        ep->_state.store(FAILED, butil::memory_order_relaxed);
        s->reset_parsing_context(nullptr);
        return MakeParseError(PARSE_ERROR_ABSOLUTELY_WRONG);
    }

    if (!source->empty()) {
        // RDMA is on, so the TCP fd is no longer an RPC channel. Anything
        // trailing the ACK on it can only be a protocol error. This catches what
        // arrived in the same read as the ACK.
        LOG(WARNING) << "Unexpected " << source->size() << " bytes after the "
                        "handshake ACK of an RDMA connection, drop connection: "
                     << s->description();
        ep->_state.store(FAILED, butil::memory_order_relaxed);
        s->reset_parsing_context(nullptr);
        return MakeParseError(PARSE_ERROR_ABSOLUTELY_WRONG);
    }

    LOG_IF(INFO, FLAGS_rdma_trace_verbose)
        << "Server handshake ends (use rdma v" << ep->_handshake_version
        << ") on " << s->description();
    rdma_transport->_rdma_state = RdmaTransport::RDMA_ON;
    ep->_state.store(ESTABLISHED, butil::memory_order_release);
    s->reset_parsing_context(nullptr);

    // Two things are deliberately not done here.
    //
    // The CQ events are not started: this runs inside CutInputMessage, which
    // keeps touching `preferred_index` / `parsing_context` after we return,
    // and PollCq would race it for those. OnNewDataFromTcpAtServer() starts
    // them once that is over.
    //
    // TRY_OTHERS is not returned: it would hand `preferred_index` to the real
    // protocol, and the remaining reads of this OnNewMessages() round would
    // parse the fd as an RPC stream although RDMA has just taken over. Asking
    // for more data keeps this handler pinned, so those reads come back to the
    // guard at the top of this function and are rejected there.
    return MakeParseError(PARSE_ERROR_NOT_ENOUGH_DATA);
}

bool RdmaEndpoint::IsWritable() const {
    if (BAIDU_UNLIKELY(g_skip_rdma_init)) {
        // Just for UT
        return false;
    }

    // The device queue is tested FIRST, and when it is non-empty the device
    // windows alone decide. The writer only reaches here because
    // CutFromIOBufList() just failed to move anything, and that drains the
    // device queue before touching the host WRs -- so with bytes still queued,
    // whether this socket can make progress is a question about the device QP,
    // not the host one. Asking the host window first would park a writer whose
    // only remaining work is a device flush that the device credits do allow
    // (host shut + device queued + device open).
    //
    // Note this is NOT "writable if either QP is writable": with the host
    // window open and the device windows shut, a queue that is still there
    // means the host bytes are already gone (CutFromIOBufList() got that far),
    // so returning true would leave KeepWrite spinning -- Socket::
    // IsWriteComplete() keeps it looping while HasPendingWrite() is true.
    // It must park until the poller returns device credits.
    //
    // No spin in the other direction either: CutFromDeviceAttachment()
    // returns EAGAIN only when both device windows are 0, and posts at least
    // one WR otherwise. Device window open + queue non-empty => progress.
    if (BAIDU_UNLIKELY(HasQueuedDeviceData())) {
        return _device->IsWindowOpen();
    }
    return _host.IsWindowOpen();
}

// RdmaIOBuf inherits from IOBuf to provide a new function.
// The reason is that we need to use some protected member function of IOBuf.
class RdmaIOBuf : public butil::IOBuf {
friend class RdmaEndpoint;
private:
    // Cut the current IOBuf to ibv_sge list and `to' for at most first max_sge
    // blocks or first max_len bytes.
    // Return: the bytes included in the sglist, or -1 if failed
    ssize_t cut_into_sglist_and_iobuf(ibv_sge* sglist, size_t* sge_index,
                                      butil::IOBuf* to, size_t max_sge,
                                      size_t max_len) {
        size_t len = 0;
        while (*sge_index < max_sge) {
            if (len == max_len || _ref_num() == 0) {
                break;
            }
            butil::IOBuf::BlockRef const& r = _ref_at(0);
            CHECK(r.length > 0);
            const void* start = fetch1();
            uint32_t lkey = GetRegionId(start);
            if (lkey == 0) {  // get lkey for user registered memory
                uint64_t meta = get_first_data_meta();
                if (meta <= UINT_MAX) {
                    lkey = (uint32_t)meta;
                }
            }
            if (BAIDU_UNLIKELY(lkey == 0)) {  // only happens when meta is not specified
                lkey = GetLKey((char*)start - r.offset);
            }
            if (lkey == 0) {
                LOG(WARNING) << "Memory not registered for rdma. "
                             << "Is this iobuf allocated before calling "
                             << "GlobalRdmaInitializeOrDie? Or just forget to "
                             << "call RegisterMemoryForRdma for your own buffer?";
                errno = ERDMAMEM;
                return -1;
            }
            size_t i = *sge_index;
            if (len + r.length > max_len) {
                // Split the block to comply with size for receiving
                sglist[i].length = max_len - len;
                len = max_len;
            } else {
                sglist[i].length = r.length;
                len += r.length;
            }
            sglist[i].addr = (uint64_t)start;
            sglist[i].lkey = lkey;
            cutn(to, sglist[i].length);
            (*sge_index)++;
        }
        return len;
    }
};

// Note this function is coupled with the implementation of IOBuf
ssize_t RdmaEndpoint::CutFromIOBufList(butil::IOBuf** from, size_t ndata) {
    if (BAIDU_UNLIKELY(g_skip_rdma_init)) {
        // Just for UT
        errno = EAGAIN;
        return -1;
    }

    CHECK(from != nullptr);
    CHECK(ndata > 0);

    // Drain the device backlog first, on this same writer. The device SQ
    // ring is single-writer for the same reason the host one is: the poller
    // only returns credits and wakes us up, it never posts. What keeps the
    // backlog from being forgotten once the host bytes run out is
    // Socket::IsWriteComplete(), which asks HasPendingWrite() and so keeps
    // KeepWrite looping back here until the queue is empty.
    if (HasQueuedDeviceData() && FlushDeviceSendQueue() < 0) {
        return -1;
    }

    size_t total_len = 0;
    size_t current = 0;
    uint16_t remote_rq_window_size =
        _host.remote_rq_window_size.load(butil::memory_order_relaxed);
    uint16_t sq_window_size =
        _host.sq_window_size.load(butil::memory_order_relaxed);
    ibv_send_wr wr;
    int max_sge = GetRdmaMaxSge();
    ibv_sge sglist[max_sge];
    while (current < ndata) {
        if (remote_rq_window_size == 0 || sq_window_size == 0) {
            // There is no space left in SQ or remote RQ.
            if (total_len > 0) {
                break;
            } else {
                errno = EAGAIN;
                return -1;
            }
        }
        butil::IOBuf* to = &_host.sbuf[_host.sq_current];
        size_t this_len = 0;

        memset(&wr, 0, sizeof(wr));
        wr.sg_list = sglist;

        RdmaIOBuf* data = (RdmaIOBuf*)from[current];
        size_t sge_index = 0;
        while (sge_index < (uint32_t)max_sge &&
                this_len < _host.remote_recv_block_size) {
            if (data->empty()) {
                // The current IOBuf is empty, find next one
                ++current;
                if (current == ndata) {
                    break;
                }
                data = (RdmaIOBuf*)from[current];
                continue;
            }

            ssize_t len = data->cut_into_sglist_and_iobuf(
                sglist, &sge_index, to, max_sge,
                _host.remote_recv_block_size - this_len);
            if (len < 0) {
                return -1;
            }
            CHECK(len > 0);
            this_len += len;
            total_len += len;
        }
        if (this_len == 0) {
            continue;
        }

        wr.num_sge = sge_index;

        // Only the last message in the write queue or the last message in the
        // current window has to be flagged solicited; for the rest
        // HostChannel::SolicitWr() amortizes it. `force_signal' is false for
        // the same reason: forcing a send completion on the last WR would
        // produce one for nearly every WR, which is exactly the CPU cost the
        // unsignaled batching exists to avoid.
        const bool must_solicit = (remote_rq_window_size == 1 ||
                                   sq_window_size == 1 || current + 1 >= ndata);
        if (PostDataWr(_host, &wr, this_len, must_solicit, false,
                       &remote_rq_window_size, &sq_window_size) < 0) {
            return -1;
        }
    }

    return total_len;
}

bool RdmaEndpoint::IsDevicePendingOverWatermark() const {
    return _device->stream.pending_bytes() >=
               FLAGS_rdma_gdr_pending_bytes_watermark ||
           _device->stream.pending_msgs() >=
               FLAGS_rdma_gdr_pending_msgs_watermark;
}

uint32_t RdmaEndpoint::TakeAcks(QpChannel& qc) {
    // Neither direction throttles itself: HandleCompletion() re-posts a
    // receive block and returns its credit immediately, whether or not the
    // parser consumed anything. The host direction does throttle itself on
    // the device direction's behalf -- a peer that runs ahead of a slow
    // device stream would otherwise grow the pending list without bound, and
    // withholding host credits is the only backpressure that reaches it.
    //
    // Safe against overflow of the uint16_t counter: the peer cannot send
    // more than qc.remote_window_capacity WRs without credits, and that is
    // bounded by MAX_QP_SIZE.
    if (&qc == &_host && BAIDU_UNLIKELY(_device.has_value()) &&
        IsDevicePendingOverWatermark()) {
        return 0;
    }
    return qc.new_rq_wrs.exchange(0, butil::memory_order_relaxed);
}

int RdmaEndpoint::SendAck(QpChannel& qc, int num) {
    if (qc.new_rq_wrs.fetch_add(num, butil::memory_order_relaxed) >
            qc.remote_window_capacity / 2 &&
        qc.sq_imm_window_size > 0) {
        return SendImm(qc, TakeAcks(qc));
    }
    return 0;
}

int RdmaEndpoint::SendImm(QpChannel& qc, uint32_t imm) {
    if (imm == 0) {
        return 0;
    }
    ibv_qp* qp = qc.qp();
    if (BAIDU_UNLIKELY(qp == nullptr)) {
        // Only the UT gets here: it skips resource allocation, so there is no
        // QP to return the credits on and no peer waiting for them either.
        DCHECK(g_skip_rdma_init);
        return 0;
    }

    ibv_send_wr wr;
    memset(&wr, 0, sizeof(wr));
    wr.opcode = IBV_WR_SEND_WITH_IMM;
    wr.imm_data = butil::HostToNet32(imm);
    wr.send_flags |= IBV_SEND_SOLICITED | IBV_SEND_SIGNALED;
    wr.wr_id = 0;

    ibv_send_wr* bad = nullptr;
    int err = ibv_post_send(qp, &wr, &bad);
    if (err != 0) {
        std::ostringstream oss;
        DebugInfo(oss, ", ");
        // We use other way to guarantee the Send Queue is not full.
        // So we just consider this error as an unrecoverable error.
        LOG(WARNING) << "Fail to ibv_post_send an ack on the " << qc.name()
                     << " QP: " << berror(err) << " " << oss.str();
        return -1;
    }

    // `qc.sq_imm_window_size' will never be negative, because an IMM can only
    // be sent when it is greater than 0.
    qc.sq_imm_window_size -= 1;
    return 0;
}

int RdmaEndpoint::PostDataWr(QpChannel& qc, ibv_send_wr* wr, size_t len,
                             bool must_solicit, bool force_signal,
                             uint16_t* remote_rq_window_size,
                             uint16_t* sq_window_size) {
    wr->opcode = IBV_WR_SEND_WITH_IMM;
    const uint32_t imm = TakeAcks(qc);
    wr->imm_data = butil::HostToNet32(imm);
    if (qc.SolicitWr(len, imm, must_solicit)) {
        wr->send_flags |= IBV_SEND_SOLICITED;
    }

    // Avoid too much send completion event to reduce the CPU overhead
    ++qc.sq_unsignaled;
    if (qc.sq_unsignaled >= qc.local_window_capacity / 4 || force_signal) {
        // Refer to:
        // http::www.rdmamojo.com/2014/06/30/working-unsignaled-completions/
        wr->send_flags |= IBV_SEND_SIGNALED;
        wr->wr_id = qc.sq_unsignaled;
        qc.sq_unsignaled = 0;
    }

    ibv_send_wr* bad = nullptr;
    const int err = ibv_post_send(qc.qp(), wr, &bad);
    if (err != 0) {
        // We use other way to guarantee the Send Queue is not full.
        // So we just consider this error as an unrecoverable error.
        std::ostringstream oss;
        DebugInfo(oss, ", ");
        LOG(WARNING) << "Fail to ibv_post_send on the " << qc.name()
                     << " QP: " << berror(err) << " " << oss.str();
        errno = err;
        return -1;
    }

    ++qc.sq_current;
    if (qc.sq_current == qc.sq_size - RESERVED_WR_NUM) {
        qc.sq_current = 0;
    }

    // Update `remote_rq_window_size' and `sq_window_size' of the channel. Note
    // that they will never be negative. Because there is at most one thread
    // can enter this function for each Socket, and the other thread of
    // HandleCompletion can only add these counters.
    *remote_rq_window_size = qc.remote_rq_window_size.fetch_sub(
            1, butil::memory_order_relaxed) - 1;
    *sq_window_size = qc.sq_window_size.fetch_sub(
            1, butil::memory_order_relaxed) - 1;
    return 0;
}

namespace {
// The credit-return half of a send completion, the same on both QPs: release
// the `wnd' send buffers the completed WR held and hand their slots back to
// the SQ window. What a send buffer is differs between the channels, which is
// all QpChannel::ReleaseSendBuf() is for.
void ReturnSendCredits(QpChannel& qc, uint16_t wnd) {
    for (uint16_t i = 0; i < wnd; ++i) {
        qc.ReleaseSendBuf(qc.sq_sent++);
        if (qc.sq_sent == qc.sq_size - RESERVED_WR_NUM) {
            qc.sq_sent = 0;
        }
    }
    butil::subtle::MemoryBarrier();

    qc.sq_window_size.fetch_add(wnd, butil::memory_order_relaxed);
}
}  // namespace

ssize_t RdmaEndpoint::HandleCompletion(QpChannel& qc, ibv_wc& wc) {
    switch (wc.opcode) {
    case IBV_WC_SEND: {  // send completion
        if (0 == wc.wr_id) {
            qc.sq_imm_window_size += 1;
            // If there are any unacknowledged recvs, send an ack.
            SendAck(qc, 0);
            return 0;
        }
        // Update SQ window.
        ReturnSendCredits(qc, wc.wr_id);
        // Do not wake up the writing thread right after polling IBV_WC_SEND,
        // or it may switch to background too quickly -- unless it is parked
        // on a device backlog, in which case nothing else will wake it and
        // Socket::IsWriteComplete() is waiting for the queue to drain.
        if (HasQueuedDeviceData() ||
            qc.remote_rq_window_size.load(butil::memory_order_relaxed) >=
                qc.local_window_capacity / 8) {
            _socket->WakeAsEpollOut();
        }
        return 0;
    }
    case IBV_WC_RECV: {  // recv completion
        // Please note that only the first wc.byte_len bytes is valid
        ssize_t nr = 0;
        if (wc.byte_len > 0) {
            CHECK_NE(_state.load(butil::memory_order_relaxed), FALLBACK_TCP);
            nr = qc.TakeRecvData(wc.byte_len);
            if (nr < 0) {
                return -1;
            }
        }
        if (0 != (wc.wc_flags & IBV_WC_WITH_IMM) && wc.imm_data > 0) {
            // Update window
            uint32_t acks = butil::NetToHost32(wc.imm_data);
            uint32_t wnd_thresh = qc.local_window_capacity / 8;
            uint32_t remote_rq_window_size =
                qc.remote_rq_window_size.fetch_add(
                        acks, butil::memory_order_relaxed);
            if (qc.sq_window_size.load(butil::memory_order_relaxed) > 0 &&
                (HasQueuedDeviceData() ||
                 remote_rq_window_size >= wnd_thresh || acks >= wnd_thresh)) {
                // Same rule as above: hold off unless the window opened wide
                // or a device backlog is waiting on these very credits.
                _socket->WakeAsEpollOut();
            }
        }
        // We must re-post recv WR
        if (PostRecv(qc, 1) < 0) {
            return -1;
        }
        if (wc.byte_len > 0) {
            SendAck(qc, 1);
        }
        return nr;
    }
    default:
        // Some driver bugs may lead to unexpected completion opcode.
        // If this happens, please update your driver.
        CHECK(false) << "This should not happen. Got a completion with opcode="
                     << wc.opcode << " on the " << qc.name() << " QP";
        return -1;
    }
    return 0;
}

int RdmaEndpoint::DoPostRecv(QpChannel& qc, void* block, size_t block_size,
                             uint32_t lkey) {
    ibv_recv_wr wr;
    memset(&wr, 0, sizeof(wr));
    ibv_sge sge;
    sge.addr = (uint64_t)block;
    sge.length = block_size;
    sge.lkey = lkey;
    wr.num_sge = 1;
    wr.sg_list = &sge;

    ibv_recv_wr* bad = nullptr;
    int err = ibv_post_recv(qc.qp(), &wr, &bad);
    if (err != 0) {
        LOG(WARNING) << "Fail to ibv_post_recv on the " << qc.name()
                     << " QP: " << berror(err);
        errno = err;
        return -1;
    }
    return 0;
}

int RdmaEndpoint::PostRecv(QpChannel& qc, uint32_t num) {
    // We do the post repeatedly from the slot at qc.rq_received, filling any
    // slot whose block TakeRecvData() has handed on.
    while (num > 0) {
        void* block = nullptr;
        size_t block_size = 0;
        uint32_t lkey = 0;
        if (!qc.PrepareRecvSlot(qc.rq_received, &block, &block_size, &lkey)) {
            // Memory is not enough for preparing a block
            errno = ENOMEM;
            return -1;
        }
        if (DoPostRecv(qc, block, block_size, lkey) < 0) {
            qc.ReleaseRecvSlot(qc.rq_received);
            return -1;
        }
        --num;
        ++qc.rq_received;
        if (qc.rq_received == qc.rq_size) {
            qc.rq_received = 0;
        }
    }
    return 0;
}

// `for_device' marks the GDR device channel's QP, whose receive buffers live
// in GPU memory and which therefore cannot be created with plain
// ibv_create_qp(). See CreateDeviceQp().
static ibv_qp* AllocateQp(ibv_cq* send_cq, ibv_cq* recv_cq, uint32_t sq_size,
                          uint32_t rq_size, bool for_device = false) {
    ibv_qp_init_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.send_cq = send_cq;
    attr.recv_cq = recv_cq;
    attr.cap.max_send_wr = sq_size;
    attr.cap.max_recv_wr = rq_size;
    attr.cap.max_send_sge = GetRdmaMaxSge();
    attr.cap.max_recv_sge = 1;
    attr.qp_type = IBV_QPT_RC;
    if (for_device) {
        return CreateDeviceQp(&attr);
    }
    return IbvCreateQp(GetRdmaPd(), &attr);
}

// Create one CQ of `size' entries reporting on `channel' (nullptr in polling
// mode, where nobody waits on a comp channel). The two names are only for
// logging: which channel of the connection, and which direction.
static ibv_cq* AllocateCq(uint32_t size, ibv_comp_channel* channel,
                          const char* chan_name, const char* direction) {
    ibv_cq* cq = IbvCreateCq(GetRdmaContext(), size, nullptr, channel,
                             channel != nullptr ? GetRdmaCompVector() : 0);
    if (nullptr == cq) {
        PLOG(WARNING) << "Fail to create " << chan_name << " "
                      << direction << " CQ";
    }
    return cq;
}

// Build the QP of one channel and the CQs it reports to.
//
// `cc' is the connection's comp channel, shared by every CQ of every channel
// on it, or nullptr in polling mode -- where one CQ takes both directions and
// nobody waits for events. It has to be passed in rather than created here:
// ibv_create_cq() binds a CQ to its comp channel permanently, so the channel
// is older than any CQ of the connection and outlives them all.
//
// Each CQ is as deep as the queue that feeds it. A CQ shallower than its
// queue overflows as soon as the queue fills, which puts the CQ into an error
// state and takes the QP down with it.
//
// `for_device' marks the GDR device channel's QP, whose receive buffers live
// in GPU memory and which therefore cannot be created with plain
// ibv_create_qp(); see CreateDeviceQp(). `name' is only for logging.
static RdmaResource* AllocateQpCq(RdmaCompChannel* cc, uint16_t sq_size,
                                  uint16_t rq_size, bool for_device,
                                  const char* name) {
    std::unique_ptr<RdmaResource> resource(new RdmaResource);
    if (cc != nullptr) {
        resource->send_cq = AllocateCq(sq_size, cc->channel, name, "send");
        if (nullptr == resource->send_cq) {
            return nullptr;
        }
        resource->recv_cq = AllocateCq(rq_size, cc->channel, name, "recv");
        if (nullptr == resource->recv_cq) {
            return nullptr;
        }
        resource->qp = AllocateQp(resource->send_cq, resource->recv_cq,
                                  sq_size, rq_size, for_device);
    } else {
        resource->polling_cq = AllocateCq(sq_size + rq_size, nullptr,
                                          name, "polling");
        if (nullptr == resource->polling_cq) {
            return nullptr;
        }
        resource->qp = AllocateQp(resource->polling_cq, resource->polling_cq,
                                  sq_size, rq_size, for_device);
    }
    if (nullptr == resource->qp) {
        PLOG(WARNING) << "Fail to create " << name << " QP";
        return nullptr;
    }
    return resource.release();
}

// A comp channel with a host QP and CQs built on it -- one entry of the
// pre-allocated pool, and also exactly what a connection whose queues are too
// deep for the pool builds for itself. The pool is the only reason this pair
// has a name: the two cannot be pooled apart.
static PreparedResource* AllocatePreparedResource(uint16_t sq_size,
                                                  uint16_t rq_size) {
    std::unique_ptr<PreparedResource> prepared(new PreparedResource);
    if (!FLAGS_rdma_use_polling) {
        prepared->comp_channel = RdmaCompChannel::Create();
        if (nullptr == prepared->comp_channel) {
            return nullptr;
        }
    }
    prepared->resource = AllocateQpCq(prepared->comp_channel, sq_size,
                                      rq_size, false, "host");
    if (nullptr == prepared->resource) {
        return nullptr;
    }
    return prepared.release();
}

int RdmaEndpoint::AllocateResources() {
    if (DoAllocateResources() == 0) {
        return 0;
    }

    const int saved_errno = errno;
    DeallocateResources();
    _host.sbuf.clear();
    _host.rbuf.clear();
    _host.rbuf_data.clear();
    errno = saved_errno;
    return -1;
}

int RdmaEndpoint::DoAllocateResources() {
    if (BAIDU_UNLIKELY(g_skip_rdma_init)) {
        // For UT
        if (BAIDU_UNLIKELY(g_fail_resource_alloc_for_test)) {
            errno = EINVAL;
            return -1;
        }
        return 0;
    }

    CHECK(_host.resource == nullptr);
    CHECK(_comp_channel == nullptr);

    PreparedResource* prepared = nullptr;
    if (_host.sq_size <= FLAGS_rdma_prepared_qp_size &&
        _host.rq_size <= FLAGS_rdma_prepared_qp_size) {
        BAIDU_SCOPED_LOCK(*g_rdma_resource_mutex);
        if (g_rdma_resource_list) {
            prepared = g_rdma_resource_list;
            g_rdma_resource_list = g_rdma_resource_list->next;
        }
    }
    if (prepared == nullptr) {
        // Either the pool ran dry, or this connection's queues are deeper
        // than the pool prepares -- in which case the QP has to be built to
        // the configured depth, and so do the CQs behind it.
        prepared = AllocatePreparedResource(_host.sq_size, _host.rq_size);
        if (prepared == nullptr) {
            return -1;
        }
    }
    // The pooled pair splits here: the comp channel belongs to the
    // connection, the QP and its CQs to the host channel. The node that held
    // them together is the pool's own bookkeeping and goes away with it.
    _comp_channel = prepared->comp_channel;
    _host.resource = prepared->resource;
    prepared->comp_channel = nullptr;
    prepared->resource = nullptr;
    delete prepared;

    if (!FLAGS_rdma_use_polling && ArmChannelCqs(_host, false) < 0) {
        return -1;
    }

    if (!_host.AllocateBuffers()) {
        return -1;
    }

    // The device channel is not an add-on down here: a connection only
    // reaches this with _device engaged because both sides asked for one, so
    // a failure fails the whole connection exactly as a host resource
    // failure above does. AllocateResources() calls DeallocateResources() on
    // our way out, which resets _device and so releases whatever this got
    // halfway through allocating.
    //
    // Polling mode is not tested for here even though GDR does not support it
    // (docs/cn/gdr_design.md section 10, no comp channel to share): a process
    // configured for both never gets this far, RdmaTransport::
    // ContextInitOrDie() rejects the combination at startup.
    DCHECK(!(_device.has_value() && FLAGS_rdma_use_polling));
    if (_device.has_value() && AllocateDeviceResources() < 0) {
        PLOG(WARNING) << "Fail to allocate GDR device resources";
        return -1;
    }

    return 0;
}

int RdmaEndpoint::StartCqEvents() {
    if (InputMessengerProcessor::STREAM_NONE != _socket->parsing_stream_type()) {
        LOG(WARNING) << "StartCqEvents() called while " << *_socket << " is parsing";
        errno = ERDMA;
        return -1;
    }

    if (_cq_sid != INVALID_SOCKET_ID) {
        // Already started.
        return 0;
    }
    if (_host.resource == nullptr) {
        if (BAIDU_UNLIKELY(g_skip_rdma_init)) {
            // For UT: AllocateResources() succeeds without allocating anything.
            return 0;
        }

        LOG(WARNING) << "No RDMA resource to start CQ events on, " << *_socket;
        errno = ERDMA;
        return -1;
    }

    SocketOptions options;
    options.user = this;
    options.keytable_pool = _socket->_keytable_pool;
    if (!FLAGS_rdma_use_polling) {
        options.fd = _comp_channel->fd();
        options.on_edge_triggered_events = PollCq;
    }
    if (Socket::Create(options, &_cq_sid) < 0) {
        PLOG(WARNING) << "Fail to create socket for cq";
        return -1;
    }

    if (FLAGS_rdma_use_polling) {
        PollerAddCqSid();
    }

    return 0;
}

int RdmaEndpoint::ModifyQpToInit(QpChannel& qc) {
    ibv_qp_attr attr;

    attr.qp_state = IBV_QPS_INIT;
    attr.pkey_index = 0;  // TODO: support more pkey use in future
    attr.port_num = GetRdmaPortNum();
    attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE;
    int err = IbvModifyQp(qc.qp(), &attr, (ibv_qp_attr_mask)(
                IBV_QP_STATE |
                IBV_QP_PKEY_INDEX |
                IBV_QP_PORT |
                IBV_QP_ACCESS_FLAGS));
    if (err != 0) {
        LOG(WARNING) << "Fail to modify " << qc.name()
                     << " QP from RESET to INIT: " << berror(err);
        return -1;
    }
    return 0;
}

int RdmaEndpoint::ModifyQpToRts(QpChannel& qc, const ParsedHello& remote,
                                uint32_t dest_qp_num) {
    ibv_qp_attr attr;

    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_1024;  // TODO: support more mtu in future
    attr.ah_attr.grh.dgid = remote.gid;
    attr.ah_attr.grh.flow_label = 0;
    attr.ah_attr.grh.sgid_index = GetRdmaGidIndex();
    attr.ah_attr.grh.hop_limit = MAX_HOP_LIMIT;
    attr.ah_attr.grh.traffic_class = 0;
    attr.ah_attr.dlid = remote.lid;
    attr.ah_attr.sl = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.static_rate = 0;
    attr.ah_attr.is_global = 1;
    attr.ah_attr.port_num = GetRdmaPortNum();
    attr.dest_qp_num = dest_qp_num;
    attr.rq_psn = 0;
    attr.max_dest_rd_atomic = 0;
    attr.min_rnr_timer = 0;  // We do not allow rnr error
    int err = IbvModifyQp(qc.qp(), &attr, (ibv_qp_attr_mask)(
                IBV_QP_STATE |
                IBV_QP_PATH_MTU |
                IBV_QP_MIN_RNR_TIMER |
                IBV_QP_AV |
                IBV_QP_MAX_DEST_RD_ATOMIC |
                IBV_QP_DEST_QPN |
                IBV_QP_RQ_PSN));
    if (err != 0) {
        LOG(WARNING) << "Fail to modify " << qc.name()
                     << " QP from INIT to RTR: " << berror(err);
        return -1;
    }

    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = TIMEOUT;
    attr.retry_cnt = RETRY_CNT;
    attr.rnr_retry = 0;  // We do not allow rnr error
    attr.sq_psn = 0;
    attr.max_rd_atomic = 0;
    err = IbvModifyQp(qc.qp(), &attr, (ibv_qp_attr_mask)(
                IBV_QP_STATE |
                IBV_QP_RNR_RETRY |
                IBV_QP_RETRY_CNT |
                IBV_QP_TIMEOUT |
                IBV_QP_SQ_PSN |
                IBV_QP_MAX_QP_RD_ATOMIC));
    if (err != 0) {
        LOG(WARNING) << "Fail to modify " << qc.name()
                     << " QP from RTR to RTS: " << berror(err);
        return -1;
    }
    return 0;
}

int RdmaEndpoint::BringUpOneQp(QpChannel& qc, const ParsedHello& remote,
                               uint32_t dest_qp_num, const ibv_ece* in_ece,
                               butil::optional<ibv_ece>* negotiated_ece) {
    if (ModifyQpToInit(qc) < 0) {
        return -1;
    }

    // ECE negotiation, done while the QP is in INIT state (must be set
    // before the RTR transition).
    bool use_ece = true;
    if (IbvSetEce != nullptr && in_ece != nullptr) {
        ibv_ece ece = *in_ece;
        int err = IbvSetEce(qc.qp(), &ece);
        if (err != 0) {
            use_ece = false;
            LOG(WARNING) << "Fail to IbvSetEce, continue without ECE: "
                         << berror(err);
        }
    }

    // Fill the RQ before RTR: a QP that reaches RTR with an empty RQ can
    // RNR, and we do not allow RNR retries.
    if (PostRecv(qc, qc.rq_size) < 0) {
        PLOG(WARNING) << "Fail to post recv wr on the " << qc.name() << " QP";
        return -1;
    }

    if (ModifyQpToRts(qc, remote, dest_qp_num) < 0) {
        return -1;
    }

    // Now that the QP reached RTS, query the reduced/negotiated ECE (the
    // subset of enhancements supported by both peers) so that the caller can
    // return it to the peer.
    if (negotiated_ece != nullptr && use_ece &&
        IbvQueryEce != nullptr && in_ece != nullptr) {
        ibv_ece ece;
        int qerr = IbvQueryEce(qc.qp(), &ece);
        if (qerr == 0) {
            *negotiated_ece = ece;
        } else {
            LOG(WARNING) << "Fail to IbvQueryEce(negotiated), "
                            "continue without ECE: " << berror(qerr);
        }
    }

    return 0;
}

int RdmaEndpoint::BringUpQp(const ParsedHello& remote, bool is_server) {
    if (BAIDU_UNLIKELY(g_skip_rdma_init)) {
        // For UT
        return 0;
    }

    // End-to-end ECE model:
    //   Server: `remote.ece' is the client's queried ECE; set it on the QP,
    //           then hand back the reduced one for the server hello.
    //   Client: `remote.ece' is the server's reduced ECE; just set it.
    if (BringUpOneQp(_host, remote, remote.qp_num,
                     remote.ece.has_value() ? &*remote.ece : nullptr,
                     is_server ? &_outgoing_ece : nullptr) < 0) {
        return -1;
    }

    // The device QP is brought up right behind the host one, on the same
    // path and in the same handshake step, so that by the time either side
    // reports ESTABLISHED both directions are usable. _device is engaged
    // only when both sides asked for the second channel, so there is nothing
    // left to test here and nothing to degrade to: a device QP that will not
    // come up fails the connection, like a host one.
    //
    // No ECE on the device QP: ECE is a one-round-trip negotiation bound to
    // the hello exchange, and running a second, unsynchronized one here would
    // need another round trip to carry the reduced value back. The two QPs
    // share a port and a path, so the host QP's negotiation already reflects
    // what this link can do.
    if (_device.has_value()) {
        CHECK(remote.device.has_value());
        if (BringUpOneQp(*_device, remote, remote.device->qp_num,
                         nullptr, nullptr) < 0) {
            LOG(WARNING) << "Fail to bring up the GDR device QP";
            return -1;
        }
    }

    return 0;
}

// ---------------------------------------------------------------------------
// GPU Direct RDMA device channel.
//
// A second RC QP that carries nothing but registered device memory. Its two
// CQs hang off the host connection's comp_channel, so a connection still
// costs one fd and one epoll registration and PollCq drains all four CQs on
// a single wakeup.
//
// The device credits below mirror the host ones one for one, but are
// deliberately independent: a message with no device payload must never wait
// behind one that has some. What makes independent credits safe is the
// parser's pending list, which parks a half-arrived message instead of
// stalling the host stream (docs/cn/gdr_design.md sections 8 and 9).
// ---------------------------------------------------------------------------

int RdmaEndpoint::AllocateDeviceResources() {
    CHECK(_device->resource == nullptr);
    CHECK(_host.resource != nullptr);
    CHECK(_comp_channel != nullptr);

    // The same builder as the host channel, on the same comp channel: two QPs
    // and four CQs reporting to one fd. Assigned rather than published in
    // steps, so that a failure in the arming below is cleaned up by
    // ~DeviceChannel() instead of leaking the QP.
    _device->resource = AllocateQpCq(_comp_channel, _device->sq_size,
                                     _device->rq_size, true, _device->name());
    if (nullptr == _device->resource) {
        return -1;
    }

    if (ArmChannelCqs(*_device, false) < 0) {
        return -1;
    }

    if (!_device->AllocateBuffers()) {
        return -1;
    }
    return 0;
}

int RdmaEndpoint::FlushDeviceSendQueue() {
    // HasQueuedDeviceData() is false without a device channel, so there is no
    // "the channel went away under a queued message" case to handle: the
    // queue lives inside _device and goes away with it.
    if (!HasQueuedDeviceData()) {
        return 0;
    }
    const ssize_t nw =
        CutFromDeviceAttachment(_device->stream.send_stream());
    _device->stream.SyncQueuedBytes();
    if (nw < 0 && errno != EAGAIN) {
        // errno is already the post error. Let CutFromIOBufList() return it
        // to Socket::DoWrite(), which does the SetFailed() -- the host path
        // reports an ibv_post_send() failure the same way.
        return -1;
    }
    // A short post is not an error: what is left stays queued and goes out
    // from the next flush, which KeepWrite reaches after the poller returns
    // device credits and wakes it.
    return 0;
}

ssize_t RdmaEndpoint::CutFromDeviceAttachment(DeviceAttachment* data) {
    CHECK(data != nullptr);
    if (data->empty()) {
        return 0;
    }

    const int max_sge = GetRdmaMaxSge();
    ibv_sge sglist[max_sge];
    ibv_send_wr wr;
    size_t total_len = 0;
    uint16_t remote_rq_window_size =
        _device->remote_rq_window_size.load(butil::memory_order_relaxed);
    uint16_t sq_window_size =
        _device->sq_window_size.load(butil::memory_order_relaxed);

    while (!data->empty()) {
        if (remote_rq_window_size == 0 || sq_window_size == 0) {
            if (total_len > 0) {
                break;
            }
            errno = EAGAIN;
            return -1;
        }

        // Fill one WR with at most max_sge segments and at most one of the
        // peer's device receive blocks.
        int sge_index = 0;
        size_t this_len = 0;
        while (sge_index < max_sge &&
               this_len < _device->remote_recv_block_size &&
               (size_t)sge_index < data->segment_count()) {
            const DeviceAttachment::Segment& seg = data->segment(sge_index);
            const size_t room = _device->remote_recv_block_size - this_len;
            sglist[sge_index].addr = (uint64_t)seg.ptr;
            sglist[sge_index].length = std::min(seg.length, room);
            sglist[sge_index].lkey = seg.lkey;
            this_len += sglist[sge_index].length;
            ++sge_index;
        }
        if (this_len == 0) {
            break;
        }
        // Hold the sent bytes until their completion, otherwise the caller
        // may recycle the device memory while the NIC is still reading it.
        data->cutn(&_device->sbuf[_device->sq_current], this_len);

        memset(&wr, 0, sizeof(wr));
        wr.sg_list = sglist;
        wr.num_sge = sge_index;

        // must_solicit is moot -- DeviceChannel::SolicitWr() solicits every
        // WR -- but force_signal is not: signal the last WR of the batch so
        // that its credits and its send completion come back without waiting
        // for more traffic. The host channel cannot afford that (it would
        // signal nearly every WR); the device channel can, because its WRs
        // are large and rare.
        if (PostDataWr(*_device, &wr, this_len, true, data->empty(),
                       &remote_rq_window_size, &sq_window_size) < 0) {
            return -1;
        }
        total_len += this_len;
    }

    return total_len;
}

void RdmaEndpoint::OnDevicePendingChanged(void* arg) {
    static_cast<RdmaEndpoint*>(arg)->OnDevicePendingChanged();
}

void RdmaEndpoint::OnDevicePendingChanged() {
    const bool was_over = _device->pending_over_watermark;
    _device->pending_over_watermark = IsDevicePendingOverWatermark();
    if (was_over && !_device->pending_over_watermark) {
        // Back under the fuse: hand the peer the credits TakeAcks() has been
        // holding back, otherwise it stays stalled until some unrelated
        // traffic happens to flush them.
        if (_host.sq_imm_window_size > 0) {
            SendImm(_host, _host.new_rq_wrs.exchange(
                    0, butil::memory_order_relaxed));
        }
    }
}

static void DeallocateCq(ibv_cq* cq) {
    if (nullptr == cq) {
        return;
    }

    int err = IbvDestroyCq(cq);
    LOG_IF(WARNING, 0 != err) << "Fail to destroy CQ: " << berror(err);
}

static int DrainCq(ibv_cq* cq) {
    if (nullptr == cq) {
        return 0;
    }

    ibv_wc wc;
    int ret;
    do {
        ret = ibv_poll_cq(cq, 1, &wc);
    } while (ret > 0);

    LOG_IF(ERROR, ret < 0) << "drain CQ failed: " << ret;
    return ret;
}

void RdmaEndpoint::DeallocateResources() {
    // Always first: the device CQs report to _comp_channel, which is
    // destroyed below. Idempotent, so callers never have to remember.
    _device.reset();
    if (_host.resource == nullptr) {
        // DoAllocateResources() can fail between the comp channel and the QP
        // on it, leaving the connection holding only the former. Nothing is
        // registered with the poller that early -- StartCqEvents() needs the
        // host resource -- so this is just a delete.
        delete _comp_channel;
        _comp_channel = nullptr;
        return;
    }
    if (FLAGS_rdma_use_polling) {
        PollerRemoveCqSid();
    }
    bool move_to_rdma_resource_list = false;
    if (_host.sq_size <= FLAGS_rdma_prepared_qp_size &&
        _host.rq_size <= FLAGS_rdma_prepared_qp_size &&
        FLAGS_rdma_prepared_qp_cnt > 0) {
        ibv_qp_attr attr;
        attr.qp_state = IBV_QPS_RESET;
        if (IbvModifyQp(_host.resource->qp, &attr, IBV_QP_STATE) == 0) {
            move_to_rdma_resource_list = true;
        }
    }

    if (nullptr != _host.resource->send_cq) {
        IbvAckCqEvents(_host.resource->send_cq, _host.send_cq_events);
    }
    if (nullptr != _host.resource->recv_cq) {
        IbvAckCqEvents(_host.resource->recv_cq, _host.recv_cq_events);
    }

    bool remove_consumer = true;
_reclaim:
    if (!move_to_rdma_resource_list) {
        if (_host.resource->qp != nullptr) {
            int err = IbvDestroyQp(_host.resource->qp);
            LOG_IF(WARNING, 0 != err) << "Fail to destroy QP: " << berror(err);
            _host.resource->qp = nullptr;
        }

        DeallocateCq(_host.resource->polling_cq);
        DeallocateCq(_host.resource->send_cq);
        DeallocateCq(_host.resource->recv_cq);

        _host.resource->polling_cq = nullptr;
        _host.resource->send_cq = nullptr;
        _host.resource->recv_cq = nullptr;
        delete _host.resource;
        _host.resource = nullptr;

        // After the CQs above, never before: a comp channel that still has
        // CQs reporting to it cannot be destroyed.
        if (_comp_channel != nullptr) {
            if (_cq_sid != INVALID_SOCKET_ID) {
                // Destroying the comp channel closes this fd, so it has to
                // come out of the epoll set first.
                int fd = _comp_channel->fd();
                GetGlobalEventDispatcher(fd, _socket->_io_event.bthread_tag()).RemoveConsumer(fd);
                remove_consumer = false;
            }
            delete _comp_channel;
            _comp_channel = nullptr;
        }
    }

    if (INVALID_SOCKET_ID != _cq_sid) {
        SocketUniquePtr s;
        if (Socket::Address(_cq_sid, &s) == 0) {
            if (remove_consumer) {
                s->_io_event.RemoveConsumer(s->_fd);
            }
            s->_user = nullptr;  // Do not release user (this RdmaEndpoint).
            s->_fd = -1;  // Already remove fd from epoll fd.
            s->SetFailed();
        }
    }

    if (move_to_rdma_resource_list) {
        // When a QP is moved to the RESET state, all associated send and
        // receive queues are flushed, meaning any outstanding WRs are effectively
        // abandoned by the hardware.
        //
        // However, the CQ associated with that QP is *not* cleared automatically,
        // meaning that it will still contain entries for WRs that completed before
        // the reset.
        //
        // The application should finish polling the CQ to remove these obsolete
        // entries before reusing the QP.
        int ret = DrainCq(_host.resource->polling_cq);
        ret += DrainCq(_host.resource->send_cq);
        ret += DrainCq(_host.resource->recv_cq);
        if (ret < 0) {
            move_to_rdma_resource_list = false;
            goto _reclaim;
        }

        {
            // Back into the pool as the pair it came out as: the CQs are
            // still bound to this comp channel and cannot be reused on
            // another one.
            PreparedResource* prepared = new PreparedResource;
            prepared->comp_channel = _comp_channel;
            prepared->resource = _host.resource;
            BAIDU_SCOPED_LOCK(*g_rdma_resource_mutex);
            prepared->next = g_rdma_resource_list;
            g_rdma_resource_list = prepared;
        }
        _host.resource = nullptr;
        _comp_channel = nullptr;
    }

    // Detach everything from this endpoint so that the function is
    // idempotent: it is called both when the endpoint is reset/destroyed
    // and when AllocateResources() fails halfway.
    _cq_sid = INVALID_SOCKET_ID;
    _host.send_cq_events = 0;
    _host.recv_cq_events = 0;
}

static const int MAX_CQ_EVENTS = 128;

int RdmaEndpoint::CollectChannels(QpChannel** out) {
    int n = 0;
    out[n++] = &_host;
    if (_device.has_value()) {
        out[n++] = &*_device;
    }
    return n;
}

int RdmaEndpoint::GetAndAckEvents(SocketUniquePtr& s) {
    QpChannel* channels[MAX_CHANNELS];
    const int nchannels = CollectChannels(channels);

    void* context = nullptr;
    ibv_cq* cq = nullptr;
    while (true) {
        // One comp channel per connection, so this one loop reaps the events
        // of every CQ on it -- all four of them on a GDR connection.
        if (IbvGetCqEvent(_comp_channel->channel, &cq, &context) != 0) {
            if (errno != EAGAIN) {
                const int saved_errno = errno;
                PLOG(ERROR) << "Fail to get cq event from " << s->description();
                s->SetFailed(saved_errno, "Fail to get cq event from %s: %s",
                             s->description().c_str(), berror(saved_errno));
                return -1;
            }
            break;
        }
        int i = 0;
        for (; i < nchannels; ++i) {
            if (cq == channels[i]->resource->send_cq) {
                ++channels[i]->send_cq_events;
                break;
            }
            if (cq == channels[i]->resource->recv_cq) {
                ++channels[i]->recv_cq_events;
                break;
            }
        }
        if (i == nchannels) {
            // Unexpected CQ event that does not belong to
            // this endpoint's CQs.
            LOG(WARNING) << "Unexpected CQ event from cq=" << cq
                         << " of " << s->description();
            // Acknowledge this single event immediately
            // to avoid leaking unacknowledged events.
            IbvAckCqEvents(cq, 1);
        }
    }

    // Acking is expensive (it takes the CQ lock), so it is batched.
    for (int i = 0; i < nchannels; ++i) {
        QpChannel& qc = *channels[i];
        if (qc.send_cq_events >= MAX_CQ_EVENTS) {
            IbvAckCqEvents(qc.resource->send_cq, qc.send_cq_events);
            qc.send_cq_events = 0;
        }
        if (qc.recv_cq_events >= MAX_CQ_EVENTS) {
            IbvAckCqEvents(qc.resource->recv_cq, qc.recv_cq_events);
            qc.recv_cq_events = 0;
        }
    }
    return 0;
}

int RdmaEndpoint::ReqNotifyCq(ibv_cq* cq, bool solicited_only,
                              const QpChannel& qc, bool fatal_on_error) {
    const int err = ibv_req_notify_cq(cq, solicited_only ? 1 : 0);
    if (0 != err) {
        // Only the recv CQ is armed solicited-only, so this tells the two
        // CQs of the channel apart without a name of its own.
        const char* which = solicited_only ? "recv" : "send";
        errno = err;
        PLOG(WARNING) << "Fail to arm " << qc.name() << " " << which
                      << " CQ comp channel from " << _socket->description();
        if (fatal_on_error) {
            _socket->SetFailed(err, "Fail to arm %s %s CQ channel from %s: %s",
                               qc.name(), which,
                               _socket->description().c_str(), berror(err));
        }
        // The logging and SetFailed() above may clobber errno.
        errno = err;
        return -1;
    }

    return 0;
}

int RdmaEndpoint::ArmChannelCqs(QpChannel& qc, bool fatal_on_error) {
    if (0 != ReqNotifyCq(qc.resource->send_cq, false, qc, fatal_on_error)) {
        return -1;
    }
    if (0 != ReqNotifyCq(qc.resource->recv_cq, true, qc, fatal_on_error)) {
        return -1;
    }
    return 0;
}

int RdmaEndpoint::ReqNotifyAllCqs(bool fatal_on_error) {
    QpChannel* channels[MAX_CHANNELS];
    const int nchannels = CollectChannels(channels);
    for (int i = 0; i < nchannels; ++i) {
        if (0 != ArmChannelCqs(*channels[i], fatal_on_error)) {
            return -1;
        }
    }
    return 0;
}

int RdmaEndpoint::CollectPollTargets(PollTarget* out) {
    QpChannel* channels[MAX_CHANNELS];
    const int nchannels = CollectChannels(channels);
    int n = 0;
    // Recv CQs first: draining arriving data before completions means a
    // message whose device half lands in the same wakeup as its host half
    // is parsed in one go instead of parking on the pending list.
    for (int i = 0; i < nchannels; ++i) {
        out[n].cq = channels[i]->resource->recv_cq;
        out[n].is_send = false;
        out[n].channel = channels[i];
        ++n;
    }
    for (int i = 0; i < nchannels; ++i) {
        out[n].cq = channels[i]->resource->send_cq;
        out[n].is_send = true;
        out[n].channel = channels[i];
        ++n;
    }
    return n;
}

void RdmaEndpoint::PollCq(Socket* m) {
    RdmaEndpoint* ep = static_cast<RdmaEndpoint*>(m->user());
    if (!ep) {
        return;
    }

    SocketUniquePtr s;
    if (Socket::Address(ep->_socket->id(), &s) < 0) {
        return;
    }
    // A queued callback may outlive Reset() and see the main Socket after
    // it has been revived with another CQ.
    if (m->id() != ep->_cq_sid) {
        return;
    }
    auto* rdma_transport = static_cast<RdmaTransport*>(s->_transport.get());
    CHECK(ep == rdma_transport->_rdma_ep);
    CHECK_GE(ep->_state.load(butil::memory_order_acquire), ESTABLISHED);

    // The CQs to drain on this wakeup, in polling order. In polling mode
    // there is a single CQ and no comp channel; GDR is not supported there
    // (docs/cn/gdr_design.md section 10), so the rotation stays trivial.
    PollTarget targets[MAX_POLL_TARGETS];
    int ntargets = 1;
    if (!FLAGS_rdma_use_polling) {
        if (ep->GetAndAckEvents(s) < 0) {
            return;
        }
        ntargets = ep->CollectPollTargets(targets);
    } else {
        targets[0].cq = ep->_host.resource->polling_cq;
        // Polling is considered as non-send.
        targets[0].is_send = false;
        targets[0].channel = &ep->_host;
    }

    int current = 0;
    int progress = Socket::PROGRESS_INIT;
    bool notified = false;
    InputMessageClosure last_msg;
    ibv_wc wc[FLAGS_rdma_cqe_poll_once];
    while (true) {
        int cnt = ibv_poll_cq(targets[current].cq, FLAGS_rdma_cqe_poll_once, wc);
        if (cnt < 0) {
            const int saved_errno = errno;
            PLOG(WARNING) << "Fail to poll cq: " << s->description();
            s->SetFailed(saved_errno, "Fail to poll cq from %s: %s",
                         s->description().c_str(), berror(saved_errno));
            return;
        }
        if (cnt == 0) {
            if (FLAGS_rdma_use_polling) {
                return;
            }

            if (current + 1 < ntargets) {
                // Move on to the next CQ of the rotation.
                ++current;
                continue;
            }
            // Every CQ has been polled.
            if (!notified) {
                // Since RDMA only provides one shot event, we have to call the
                // notify function every time. Because there is a possibility
                // that the event arrives after the poll but before the notify,
                // we should re-poll the CQ once after the notify to check if
                // there is an available CQE.
                // The connection is already working in RDMA mode here, a
                // failed re-arm means no more CQ event will be reported,
                // which is fatal for this connection.
                if (0 != ep->ReqNotifyAllCqs(true)) {
                    return;
                }
                notified = true;
                // All CQs have just been re-armed, thus all of them must be
                // re-polled, so restart the rotation from the beginning.
                // Otherwise a CQE arriving in the window between the poll and
                // the notify of an earlier CQ would be left there without any
                // following event (one shot notification is not triggered by
                // the CQE which is already in the CQ before the arming),
                // which stalls the connection until the next CQE happens to
                // come.
                current = 0;
                continue;
            }
            if (!m->MoreReadEvents(&progress)) {
                break;
            }

            if (0 != ep->GetAndAckEvents(s)) {
                return;
            }

            current = 0;
            notified = false;
            continue;
        }
        notified = false;

        ssize_t bytes = 0;
        for (int i = 0; i < cnt; ++i) {
            if (s->Failed()) {
                return;
            }

            if (wc[i].status != IBV_WC_SUCCESS) {
                PLOG(WARNING) << "Fail to handle RDMA completion, error status("
                              << wc[i].status << "): " << s->description();
                s->SetFailed(ERDMA, "RDMA completion error(%d) from %s: %s",
                             wc[i].status, s->description().c_str(), berror(ERDMA));
                continue;
            }

            ssize_t nr = ep->HandleCompletion(*targets[current].channel, wc[i]);
            if (nr < 0) {
                const int saved_errno = errno;
                PLOG(WARNING) << "Fail to handle RDMA completion: " << s->description();
                s->SetFailed(saved_errno, "Fail to handle rdma completion from %s: %s",
                             s->description().c_str(), berror(saved_errno));
            } else {
                // Device bytes never land in _read_buf, so HandleCompletion()
                // reports 0 for them and they are not part of the byte count
                // the messenger accounts for. They still have to run the parse
                // loop below: a message parked on the pending list may have
                // become complete.
                bytes += nr;
            }
        }
        // Send CQE has no messages to process.
        if (targets[current].is_send) {
            continue;
        }

        // Just call PrcessNewMessage once for all of these CQEs.
        // Otherwise it may call too many bthread_flush to affect performance.
        const int64_t received_us = butil::cpuwide_time_us();
        const int64_t base_realtime = butil::gettimeofday_us() - received_us;
        if (ep->_host.input_processor.ProcessNewMessage(
                bytes, false, received_us, base_realtime, last_msg) < 0) {
            return;
        }
    }
}

std::string RdmaEndpoint::GetStateStr() const {
    switch (_state.load(butil::memory_order_relaxed)) {
    case UNINIT: return "UNINIT";
    case C_ALLOC_QPCQ: return "C_ALLOC_QPCQ";
    case C_HELLO_SEND: return "C_HELLO_SEND";
    case C_HELLO_WAIT: return "C_HELLO_WAIT";
    case C_BRINGUP_QP: return "C_BRINGUP_QP";
    case C_ACK_SEND: return "C_ACK_SEND";
    case S_HELLO_WAIT: return "S_HELLO_WAIT";
    case S_ALLOC_QPCQ: return "S_ALLOC_QPCQ";
    case S_BRINGUP_QP: return "S_BRINGUP_QP";
    case S_HELLO_SEND: return "S_HELLO_SEND";
    case S_ACK_WAIT: return "S_ACK_WAIT";
    case ESTABLISHED: return "ESTABLISHED";
    case FALLBACK_TCP: return "FALLBACK_TCP";
    case FAILED: return "FAILED";
    default: return "UNKNOWN";
    }
}

void RdmaEndpoint::DumpChannel(std::ostream& os, butil::StringPiece connector,
                               const char* prefix, const QpChannel& qc) const {
    os << connector << prefix << "sq_imm_window_size=" << qc.sq_imm_window_size
       << connector << prefix << "remote_rq_window_size="
       << qc.remote_rq_window_size.load(butil::memory_order_relaxed)
       << connector << prefix << "sq_window_size="
       << qc.sq_window_size.load(butil::memory_order_relaxed)
       << connector << prefix << "local_window_capacity="
       << qc.local_window_capacity
       << connector << prefix << "remote_window_capacity="
       << qc.remote_window_capacity
       << connector << prefix << "sbuf_head=" << qc.sq_current
       << connector << prefix << "sbuf_tail=" << qc.sq_sent
       << connector << prefix << "rbuf_head=" << qc.rq_received
       << connector << prefix << "unacked_rq_wr="
       << qc.new_rq_wrs.load(butil::memory_order_relaxed);
}

void RdmaEndpoint::DebugInfo(std::ostream& os, butil::StringPiece connector) const {
    os << "rdma_state=ON"
       << connector << "handshake_state=" << GetStateStr()
       << connector << "handshake_version=" << static_cast<int>(_handshake_version);
    DumpChannel(os, connector, "rdma_", _host);
    os << connector << "rdma_received_ack=" << _host.accumulated_ack
       << connector << "rdma_unsolicited_sent=" << _host.unsolicited
       << connector << "rdma_unsignaled_sq_wr=" << _host.sq_unsignaled
       << connector << "rdma_read_buf="
       << _host.input_processor.read_buf().size();
    if (!_device.has_value()) {
        os << connector << "gdr=off";
        return;
    }
    os << connector << "gdr=on";
    DumpChannel(os, connector, "gdr_", *_device);
    os << connector << "gdr_remote_recv_block_size="
       << _device->remote_recv_block_size
       << connector << "gdr_recv_stream_bytes=" << _device->stream.size()
       << connector << "gdr_cut_offset=" << _device->stream.cut_offset()
       << connector << "gdr_pending_bytes="
       << _device->stream.pending_bytes()
       << connector << "gdr_pending_msgs="
       << _device->stream.pending_msgs()
       << connector << "gdr_send_offset=" << _device->stream.send_offset()
       << connector << "gdr_send_queued="
       << _device->stream.queued_send_bytes();
}

int RdmaEndpoint::GlobalInitialize() {
    g_rdma_recv_block_size = GetRdmaBlockSize() - IOBUF_BLOCK_HEADER_LEN;
    if (g_rdma_recv_block_size <= 0) {
        LOG(ERROR) << "rdma_recv_block_type incorrect "
                   << "(valid value: default/large/huge)";
        errno = EINVAL;
        return -1;
    }

    g_rdma_resource_mutex = new butil::Mutex;
    for (int i = 0; i < FLAGS_rdma_prepared_qp_cnt; ++i) {
        PreparedResource* res = AllocatePreparedResource(
                FLAGS_rdma_prepared_qp_size, FLAGS_rdma_prepared_qp_size);
        if (!res) {
            return -1;
        }
        res->next = g_rdma_resource_list;
        g_rdma_resource_list = res;
    }

    if (FLAGS_rdma_use_polling) {
        _poller_groups = std::vector<PollerGroup>(FLAGS_task_group_ntags);
    }

    return 0;
}

void RdmaEndpoint::GlobalRelease() {
    if (g_rdma_resource_mutex) {
        BAIDU_SCOPED_LOCK(*g_rdma_resource_mutex);
        while (g_rdma_resource_list) {
            PreparedResource* res = g_rdma_resource_list;
            g_rdma_resource_list = g_rdma_resource_list->next;
            delete res;
        }
    }
    // release polling mode at exit or call RdmaEndpoint::PollingModeRelease
    // explicitly
    if (FLAGS_rdma_use_polling) {
        for (int i = 0; i < FLAGS_task_group_ntags; ++i) {
            PollingModeRelease(i);
        }
    }
}

std::vector<RdmaEndpoint::PollerGroup> RdmaEndpoint::_poller_groups;

int RdmaEndpoint::PollingModeInitialize(bthread_tag_t tag,
                                        std::function<void()> callback,
                                        std::function<void()> init_fn,
                                        std::function<void()> release_fn) {
    if (!FLAGS_rdma_use_polling) {
        return 0;
    }
    auto& group = _poller_groups[tag];
    auto& pollers = group.pollers;
    auto& running = group.running;
    bool expected = false;
    if (!running.compare_exchange_strong(expected, true)) {
        return 0;
    }
    struct FnArgs {
        Poller* poller;
        std::atomic<bool>* running;
    };
    auto fn = [](void* p) -> void* {
        std::unique_ptr<FnArgs> args(static_cast<FnArgs*>(p));
        auto poller = args->poller;
        auto running = args->running;
        std::unordered_set<SocketId> cq_sids;
        CqSidOp op;

        if (poller->init_fn) {
            poller->init_fn();
        }

        while (running->load(std::memory_order_relaxed)) {
            while (poller->op_queue.Dequeue(op)) {
                if (op.type == CqSidOp::ADD) {
                    cq_sids.emplace(op.sid);
                } else if (op.type == CqSidOp::REMOVE) {
                    cq_sids.erase(op.sid);
                }
            }
            for (auto sid : cq_sids) {
                SocketUniquePtr s;
                if (Socket::Address(sid, &s) < 0) {
                    continue;
                }
                PollCq(s.get());
            }
            if (poller->callback) {
                poller->callback();
            }
            if (FLAGS_rdma_poller_yield) {
                bthread_yield();
            }
        }

        if (poller->release_fn) {
            poller->release_fn();
        }

        return nullptr;
    };
    for (int i = 0; i < FLAGS_rdma_poller_num; ++i) {
        auto args = new FnArgs{&pollers[i], &running};
        auto attr = FLAGS_rdma_disable_bthread ? BTHREAD_ATTR_PTHREAD
                                               : BTHREAD_ATTR_NORMAL;
        attr.tag = tag;
        bthread_attr_set_name(&attr, "RdmaPolling");
        pollers[i].callback = callback;
        pollers[i].init_fn = init_fn;
        pollers[i].release_fn = release_fn;
        auto rc = bthread_start_background(&pollers[i].tid, &attr, fn, args);
        if (rc != 0) {
            LOG(ERROR) << "Fail to start rdma polling bthread";
            return -1;
        }
    }
    return 0;
}

void RdmaEndpoint::PollingModeRelease(bthread_tag_t tag) {
    if (!FLAGS_rdma_use_polling) {
        return;
    }
    auto& group = _poller_groups[tag];
    auto& pollers = group.pollers;
    auto& running = group.running;
    running.store(false, std::memory_order_relaxed);
    for (int i = 0; i < FLAGS_rdma_poller_num; ++i) {
        bthread_join(pollers[i].tid, nullptr);
    }
}

void RdmaEndpoint::PollerAddCqSid() {
    if (_cq_sid == INVALID_SOCKET_ID) {
        return;
    }

    auto index = butil::fmix32(_cq_sid) % FLAGS_rdma_poller_num;
    auto& group = _poller_groups[bthread_self_tag()];
    auto& pollers = group.pollers;
    auto& poller = pollers[index];
    poller.op_queue.Enqueue(CqSidOp{_cq_sid, CqSidOp::ADD});
}

void RdmaEndpoint::PollerRemoveCqSid() {
    if (INVALID_SOCKET_ID == _cq_sid) {
        return;
    }

    auto index = butil::fmix32(_cq_sid) % FLAGS_rdma_poller_num;
    auto& group = _poller_groups[bthread_self_tag()];
    auto& pollers = group.pollers;
    auto& poller = pollers[index];
    poller.op_queue.Enqueue(CqSidOp{_cq_sid, CqSidOp::REMOVE});
}

}  // namespace rdma
}  // namespace brpc

#endif  // if BRPC_WITH_RDMA
