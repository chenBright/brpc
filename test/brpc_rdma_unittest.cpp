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


#include <netinet/in.h>
#include <sys/socket.h>
#include <gtest/gtest.h>
#include <gflags/gflags.h>
#if BRPC_WITH_RDMA
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <functional>
#include <set>
#include <vector>
#include <google/protobuf/descriptor.h>
#include "butil/endpoint.h"
#include "butil/fd_guard.h"
#include "butil/iobuf.h"
#include "butil/sys_byteorder.h"
#include "butil/time.h"
#include "butil/files/temp_file.h"
#include "brpc/acceptor.h"
#include "brpc/channel.h"
#include "brpc/controller.h"
#include "brpc/server.h"
#include "brpc/socket.h"
#include "brpc/errno.pb.h"
#include "brpc/parallel_channel.h"
#include "brpc/selective_channel.h"
#include "brpc/rdma_transport.h"
#include "brpc/device_attachment.h"
#include "brpc/rdma/block_pool.h"
#include "brpc/rdma/device_memory.h"
#include "brpc/rdma/rdma_endpoint.h"
#include "brpc/rdma/rdma_handshake.h"
#include "brpc/rdma/rdma_handshake_constants.h"
#include "brpc/rdma/rdma_handshake.pb.h"
#include "brpc/rdma/rdma_helper.h"
#include "brpc/policy/baidu_rpc_meta.pb.h"
#include "brpc/policy/baidu_rpc_protocol.h"
#include "brpc/policy/most_common_message.h"
#include "butil/raw_pack.h"
#include "echo.pb.h"
#if BRPC_WITH_GDR
// Only the end-to-end test at the bottom of this file touches CUDA directly:
// it has to fill and verify the device attachment it sends.
#include <cuda_runtime.h>
#endif

static const int PORT = 8713;

using namespace brpc;

namespace brpc {

DECLARE_int64(socket_max_unwritten_bytes);
DECLARE_bool(log_idle_connection_close);
DECLARE_uint64(max_body_size);
DEFINE_bool(rdma_test_enable, false, "Enable tests requring rdma runtime.");
// See the GPU Direct RDMA section near the bottom of this file. Defined here
// with the other test flag because gflags puts FLAGS_* in whatever namespace
// the macro is expanded in, and the tests below run under `using namespace
// brpc' where a second fLB namespace would be ambiguous.
DEFINE_bool(gdr_test_real_device, false,
            "Run the GDR tests against real device memory instead of the "
            "host-memory stand-in.");

namespace rdma {

// HELLO_V2_VERSION / IMPL_V2_VERSION come from
// brpc/rdma/rdma_handshake_constants.h (shared wire constants).

DECLARE_bool(rdma_trace_verbose);
DECLARE_int32(rdma_memory_pool_max_regions);
DECLARE_int32(rdma_client_handshake_version);
DECLARE_bool(rdma_ece);
DECLARE_bool(rdma_enable_gdr);
DECLARE_string(rdma_attachment_memory);
DECLARE_int32(rdma_device_sq_size);
DECLARE_int32(rdma_device_rq_size);
DECLARE_int32(rdma_gdr_recv_block_size);
DECLARE_int64(rdma_gdr_max_device_bytes);
DECLARE_int64(rdma_gdr_pending_bytes_watermark);
DECLARE_int64(rdma_gdr_pending_msgs_watermark);
DECLARE_int32(rdma_gdr_device_id);

extern ibv_cq* (*IbvCreateCq)(ibv_context*, int, void*, ibv_comp_channel*, int);
extern int (*IbvDestroyCq)(ibv_cq*);
extern ibv_qp* (*IbvCreateQp)(ibv_pd*, ibv_qp_init_attr*);
extern int (*IbvModifyQp)(ibv_qp*, ibv_qp_attr*, ibv_qp_attr_mask);
extern int (*IbvQueryQp)(ibv_qp*, ibv_qp_attr*, ibv_qp_attr_mask, ibv_qp_init_attr*);
extern int (*IbvDestroyQp)(ibv_qp*);
extern ibv_comp_channel* (*IbvCreateCompChannel)(ibv_context*);
extern int (*IbvDestroyCompChannel)(ibv_comp_channel*);
extern butil::atomic<bool> g_rdma_available;
extern bool g_skip_rdma_init;
extern bool g_fail_resource_alloc_for_test;
extern bool g_skip_device_alloc_for_test;
} // namespace rdma
} // namespace brpc

static std::string g_ip = "127.0.0.1";
static butil::EndPoint g_ep;

// Number of Echo requests the server has actually served. The churn tests below
// cut the connection while requests are in flight, and from the client side
// "the server never saw this request" is indistinguishable from "the server
// answered it and the reply died with the connection" -- both just look like a
// failed RPC. Only this counter tells the two apart, and it is the server having
// real work in flight that makes the race those tests hunt reachable at all.
static butil::atomic<int> g_echo_served(0);

// The server side runs in its own threads, so the only thing a test can rely on
// is that an expected transition happens *eventually*. Polling with a generous
// upper bound keeps the common case as fast as the machine allows and does not
// turn into a flake when CI is loaded, which a fixed sleep does.
static const int64_t WAIT_TIMEOUT_US = 5000000;
static const int64_t WAIT_INTERVAL_US = 1000;

// Returns true if `pred` turned true before the timeout expired.
static bool WaitUntil(const std::function<bool()>& pred,
                      int64_t timeout_us = WAIT_TIMEOUT_US) {
    const int64_t deadline = butil::gettimeofday_us() + timeout_us;
    while (!pred()) {
        if (butil::gettimeofday_us() >= deadline) {
            return false;
        }
        usleep((useconds_t)WAIT_INTERVAL_US);
    }
    return true;
}

// write(2) may transfer less than asked for, so a test that cares about the
// whole buffer reaching the peer has to loop.
static bool WriteAll(int fd, const void* buf, size_t len) {
    const uint8_t* p = (const uint8_t*)buf;
    for (size_t done = 0; done < len; ) {
        ssize_t n = write(fd, p + done, len - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        done += n;
    }
    return true;
}

// Same for read(2). Returns false on error, on timeout (see ConnectToServer)
// and on EOF before the whole buffer arrived.
static bool ReadAll(int fd, void* buf, size_t len) {
    uint8_t* p = (uint8_t*)buf;
    for (size_t done = 0; done < len; ) {
        ssize_t n = read(fd, p + done, len - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        done += n;
    }
    return true;
}

// Connect a raw socket to the test server. Note that sin_addr is taken from
// `g_ep`: a zeroed one means 0.0.0.0, which only happens to reach the local
// server on some systems. The receive timeout keeps a missing server reply from
// hanging the test forever.
static void ConnectToServer(butil::fd_guard* sockfd) {
    sockaddr_in addr;
    bzero((char*)&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(g_ep.port);
    addr.sin_addr = g_ep.ip;
    sockfd->reset(socket(AF_INET, SOCK_STREAM, 0));
    ASSERT_TRUE(*sockfd >= 0);
    timeval tv;
    tv.tv_sec = WAIT_TIMEOUT_US / 1000000;
    tv.tv_usec = WAIT_TIMEOUT_US % 1000000;
    ASSERT_EQ(0, setsockopt(*sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)));
    ASSERT_EQ(0, connect(*sockfd, (sockaddr*)&addr, sizeof(addr)));
}

class MyEchoService : public ::test::EchoService {
    void Echo(google::protobuf::RpcController* cntl_base,
              const ::test::EchoRequest* req,
              ::test::EchoResponse* res,
              google::protobuf::Closure* done) {
        Controller* cntl = static_cast<Controller*>(cntl_base);
        ClosureGuard done_guard(done);
        g_echo_served.fetch_add(1, butil::memory_order_relaxed);
        if (req->server_fail()) {
            cntl->SetFailed(req->server_fail(), "Server fail1");
            cntl->SetFailed(req->server_fail(), "Server fail2");
            return;
        }
        if (req->close_fd()) {
            usleep(1);
            LOG(INFO) << "close fd...";
            cntl->CloseConnection("Close connection according to request");
            return;
        }
        if (req->sleep_us() > 0) {
            LOG(INFO) << "sleep " << req->sleep_us() << "us...";
            bthread_usleep(req->sleep_us());
        }
        res->set_message("MyEchoService");
        if (req->code() != 0) {
            res->add_code_list(req->code());
        }
        cntl->response_attachment().append(cntl->request_attachment());
    }
};

class RdmaTest : public ::testing::Test {
protected:
    RdmaTest() {
        butil::ip_t ip;
        EXPECT_EQ(0, butil::str2ip(g_ip.c_str(), &ip));
        butil::EndPoint ep(ip, PORT);
        g_ep = ep;
        EXPECT_EQ(0, _server_list.save(butil::endpoint2str(g_ep).c_str()));
        _naming_url = std::string("File://") + _server_list.fname();
        _server.AddService(&_svc, SERVER_DOESNT_OWN_SERVICE);
    }
    ~RdmaTest() { }

    virtual void SetUp() { }

    virtual void TearDown() {
        rdma::DumpMemoryPoolInfo(std::cout);
    }

protected:
    void StartServer(bool use_rdma = true) {
        ServerOptions options;
        options.enabled_protocols = "baidu_std";
        options.socket_mode = use_rdma ? SOCKET_MODE_RDMA : SOCKET_MODE_TCP;
        options.idle_timeout_sec = 5;
        options.max_concurrency = 0;
        options.internal_port = -1;
        EXPECT_EQ(0, _server.Start(PORT, &options));
    }

    void StopServer() {
        _server.Stop(0);
        _server.Join();
    }

    Socket* GetSocketFromServer(size_t index) {
        std::vector<SocketId> sids;
        _server._am->ListConnections(&sids);
        if (index >= sids.size()) {
            return nullptr;
        }
        SocketUniquePtr s;
        if (Socket::Address(sids[index], &s) == 0) {
            return s.get();
        }
        return nullptr;
    }

    // Accepting the connection happens in the server threads, so poll for it
    // rather than sleeping. Returns nullptr if it never showed up.
    Socket* WaitForServerSocket() {
        Socket* s = nullptr;
        WaitUntil([this, &s] { return (s = GetSocketFromServer(0)) != nullptr; });
        return s;
    }

    // Ditto for the connection going away.
    bool WaitForServerSocketGone() {
        return WaitUntil([this] { return GetSocketFromServer(0) == nullptr; });
    }

    butil::TempFile _server_list;
    std::string _naming_url;

    Server _server;
    MyEchoService _svc;
};

// Shorthand for the RDMA transport behind a Socket, which every endpoint state
// check below has to go through.
static RdmaTransport* RdmaTransportOf(Socket* s) {
    return static_cast<RdmaTransport*>(s->_transport.get());
}
static RdmaTransport* RdmaTransportOf(const SocketUniquePtr& s) {
    return RdmaTransportOf(s.get());
}

// Polls until the endpoint reaches `expected` and returns the last state seen,
// so that ASSERT_RDMA_STATE() reports what the endpoint actually settled on.
static rdma::RdmaEndpoint::State WaitForRdmaState(
        RdmaTransport* transport, rdma::RdmaEndpoint::State expected) {
    rdma::RdmaEndpoint::State state = transport->_rdma_ep->_state;
    WaitUntil([transport, expected, &state] {
        state = transport->_rdma_ep->_state;
        return state == expected;
    });
    return state;
}

// Waits for `transport` to reach `expected`, failing the test if it does not.
#define ASSERT_RDMA_STATE(expected, transport) \
    ASSERT_EQ(expected, WaitForRdmaState(transport, expected))

// Polls until the fd stream of `s` holds exactly `size` bytes. Tests asserting
// that a state did NOT change need this: waiting for the state itself would
// return before the peer had read anything at all.
static bool WaitForFdReadBuf(Socket* s, size_t size) {
    return WaitUntil([s, size] {
        return s->fd_input_processor().read_buf().size() == size;
    });
}

// Build a well-formed v2 client hello: "RDMA" followed by the 36B body.
static void MakeV2ClientHello(uint8_t (&data)[rdma::HELLO_V2_MSG_LEN_MIN]) {
    rdma::v2_wire::HelloMessage msg{};
    msg.msg_len = rdma::HELLO_V2_MSG_LEN_MIN;
    msg.hello_ver = rdma::HELLO_V2_VERSION;
    msg.impl_ver = rdma::IMPL_V2_VERSION;
    msg.sq_size = 16;
    msg.rq_size = 16;
    msg.block_size = 8192;
    msg.qp_num = 0;
    msg.gid = rdma::GetRdmaGid();
    memcpy(data, "RDMA", 4);
    msg.Serialize(data + 4);
}

// Parameterized fixture used by upper-layer RPC tests that have no
// dependency on the handshake wire format. The parameter is the
// client-side handshake protocol version (FLAGS_rdma_client_handshake_version),
// so every TEST_P below is automatically executed once per supported
// version. Add a new version to INSTANTIATE_TEST_SUITE_P at the bottom
// of this file and these RPC tests will gain coverage for free.
class RdmaRpcTest : public RdmaTest,
                    public ::testing::WithParamInterface<int> {
protected:
    void SetUp() override {
        RdmaTest::SetUp();
        _saved_handshake_version = rdma::FLAGS_rdma_client_handshake_version;
        rdma::FLAGS_rdma_client_handshake_version = GetParam();
    }
    void TearDown() override {
        rdma::FLAGS_rdma_client_handshake_version = _saved_handshake_version;
        RdmaTest::TearDown();
    }

private:
    int _saved_handshake_version = 2;
};

TEST_F(RdmaTest, stale_cq_callback_does_not_poll_new_generation) {
    SocketOptions main_options;
    main_options.socket_mode = SOCKET_MODE_RDMA;
    SocketId main_sid;
    ASSERT_EQ(0, Socket::Create(main_options, &main_sid));

    SocketUniquePtr main_socket;
    ASSERT_EQ(0, Socket::Address(main_sid, &main_socket));
    RdmaTransport* transport =
        static_cast<RdmaTransport*>(main_socket->_transport.get());
    rdma::RdmaEndpoint* ep = transport->_rdma_ep;

    SocketOptions cq_options;
    cq_options.user = ep;
    SocketId stale_cq_sid;
    SocketId current_cq_sid;
    ASSERT_EQ(0, Socket::Create(cq_options, &stale_cq_sid));
    ASSERT_EQ(0, Socket::Create(cq_options, &current_cq_sid));

    SocketUniquePtr stale_cq_socket;
    SocketUniquePtr current_cq_socket;
    ASSERT_EQ(0, Socket::Address(stale_cq_sid, &stale_cq_socket));
    ASSERT_EQ(0, Socket::Address(current_cq_sid, &current_cq_socket));
    ep->_cq_sid = current_cq_sid;

    rdma::RdmaEndpoint::PollCq(stale_cq_socket.get());

    stale_cq_socket->_user = NULL;
    current_cq_socket->_user = NULL;
    stale_cq_socket->SetFailed();
    current_cq_socket->SetFailed();
    main_socket->SetFailed();
}

TEST_F(RdmaTest, client_close_before_hello_send) {
    StartServer();

    butil::fd_guard sockfd;
    ASSERT_NO_FATAL_FAILURE(ConnectToServer(&sockfd));
    Socket* s = WaitForServerSocket();
    ASSERT_TRUE(s != nullptr);
    ASSERT_EQ(rdma::RdmaEndpoint::UNINIT, RdmaTransportOf(s)->_rdma_ep->_state);
    sockfd.reset(-1);
    ASSERT_TRUE(WaitForServerSocketGone());

    StopServer();
}

TEST_F(RdmaTest, client_hello_msg_invalid_magic_str) {
    StartServer();

    butil::fd_guard sockfd;
    ASSERT_NO_FATAL_FAILURE(ConnectToServer(&sockfd));
    Socket* s = WaitForServerSocket();
    ASSERT_TRUE(s != nullptr);
    ASSERT_EQ(rdma::RdmaEndpoint::UNINIT, RdmaTransportOf(s)->_rdma_ep->_state);

    uint8_t data[rdma::HELLO_V2_MSG_LEN_MIN];
    memcpy(data, "PRPC", 4);  // send as normal baidu_std protocol
    ASSERT_TRUE(WriteAll(sockfd, data, 4));
    // Wait for the bytes to show up in the fd stream (baidu_std wants 12B of
    // header, so they stay buffered). Waiting on the state instead would prove
    // nothing: it is already UNINIT before the server has read anything.
    ASSERT_TRUE(WaitForFdReadBuf(s, 4));
    // A non-RDMA magic makes ParseRdmaHandshake return TRY_OTHERS and hand the
    // bytes to other protocols; it does not touch the endpoint state, so it
    // stays UNINIT (the old blocking handshake used to set FALLBACK_TCP here).
    ASSERT_EQ(rdma::RdmaEndpoint::UNINIT, RdmaTransportOf(s)->_rdma_ep->_state);

    StopServer();
}

TEST_F(RdmaTest, client_close_during_hello_send) {
    StartServer();

    Socket* s = nullptr;
    uint8_t data[8];

    butil::fd_guard sockfd1;
    ASSERT_NO_FATAL_FAILURE(ConnectToServer(&sockfd1));
    s = WaitForServerSocket();
    ASSERT_TRUE(s != nullptr);
    ASSERT_EQ(rdma::RdmaEndpoint::UNINIT, RdmaTransportOf(s)->_rdma_ep->_state);
    memcpy(data, "RD", 2);
    ASSERT_TRUE(WriteAll(sockfd1, data, 2));  // break in magic str
    // Fewer than 4 magic bytes: ParseRdmaHandshake can't tell yet, returns
    // NOT_ENOUGH_DATA and leaves the endpoint UNINIT (the old blocking
    // handshake used to set S_HELLO_WAIT before reading the magic). Wait for
    // the bytes to be buffered, the state alone would prove nothing.
    ASSERT_TRUE(WaitForFdReadBuf(s, 2));
    ASSERT_EQ(rdma::RdmaEndpoint::UNINIT, RdmaTransportOf(s)->_rdma_ep->_state);
    sockfd1.reset(-1);
    ASSERT_TRUE(WaitForServerSocketGone());

    butil::fd_guard sockfd2;
    ASSERT_NO_FATAL_FAILURE(ConnectToServer(&sockfd2));
    s = WaitForServerSocket();
    ASSERT_TRUE(s != nullptr);
    ASSERT_EQ(rdma::RdmaEndpoint::UNINIT, RdmaTransportOf(s)->_rdma_ep->_state);
    memcpy(data, "RDMA", 4);
    ASSERT_TRUE(WriteAll(sockfd2, data, 4));  // break after magic str
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::S_HELLO_WAIT, RdmaTransportOf(s));
    sockfd2.reset(-1);
    ASSERT_TRUE(WaitForServerSocketGone());

    butil::fd_guard sockfd3;
    ASSERT_NO_FATAL_FAILURE(ConnectToServer(&sockfd3));
    s = WaitForServerSocket();
    ASSERT_TRUE(s != nullptr);
    ASSERT_EQ(rdma::RdmaEndpoint::UNINIT, RdmaTransportOf(s)->_rdma_ep->_state);
    // Send the 4B magic plus a valid msg_len (=40) but no body, so the server
    // recognizes an RDMA v2 hello and waits for the remaining bytes. (A zero
    // msg_len would now be rejected up-front as a protocol error.)
    memcpy(data, "RDMA", 4);
    uint16_t v2_len = butil::HostToNet16(rdma::HELLO_V2_MSG_LEN_MIN);
    memcpy(data + 4, &v2_len, sizeof(v2_len));
    ASSERT_TRUE(WriteAll(sockfd3, data, 6));  // magic + msg_len, body missing
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::S_HELLO_WAIT, RdmaTransportOf(s));
    sockfd3.reset(-1);
    ASSERT_TRUE(WaitForServerSocketGone());

    StopServer();
}

TEST_F(RdmaTest, client_hello_msg_invalid_len) {
    StartServer();

    Socket* s = nullptr;
    uint8_t data[rdma::HELLO_V2_MSG_LEN_MIN];

    butil::fd_guard sockfd1;
    ASSERT_NO_FATAL_FAILURE(ConnectToServer(&sockfd1));
    s = WaitForServerSocket();
    ASSERT_TRUE(s != nullptr);
    ASSERT_EQ(rdma::RdmaEndpoint::UNINIT, RdmaTransportOf(s)->_rdma_ep->_state);
    memcpy(data, "RDMA", 4);
    ASSERT_TRUE(WriteAll(sockfd1, data, 4)); // Write magic string.
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::S_HELLO_WAIT, RdmaTransportOf(s));
    memset(data + 4, 0, 36);
    ASSERT_TRUE(WriteAll(sockfd1, data + 4, 36));  // Write invalid length.
    ASSERT_TRUE(WaitForServerSocketGone());

    butil::fd_guard sockfd2;
    ASSERT_NO_FATAL_FAILURE(ConnectToServer(&sockfd2));
    s = WaitForServerSocket();
    ASSERT_TRUE(s != nullptr);
    ASSERT_EQ(rdma::RdmaEndpoint::UNINIT, RdmaTransportOf(s)->_rdma_ep->_state);
    memcpy(data, "RDMA", 4);
    ASSERT_TRUE(WriteAll(sockfd2, data, 4)); // Write magic string.
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::S_HELLO_WAIT, RdmaTransportOf(s));
    uint16_t len = butil::HostToNet16(35);
    memcpy(data + 4, &len, sizeof(len));
    memset(data + 6, 0, 34);
    ASSERT_TRUE(WriteAll(sockfd2, data + 4, 36));  // write invalid length
    ASSERT_TRUE(WaitForServerSocketGone());

    StopServer();
}

TEST_F(RdmaTest, client_hello_msg_invalid_version) {
    StartServer();

    Socket* s = nullptr;
    uint8_t data[rdma::HELLO_V2_MSG_LEN_MIN];
    uint16_t len = butil::HostToNet16(rdma::HELLO_V2_MSG_LEN_MIN);
    uint16_t ver = butil::HostToNet16(1);

    butil::fd_guard sockfd1;
    ASSERT_NO_FATAL_FAILURE(ConnectToServer(&sockfd1));
    s = WaitForServerSocket();
    ASSERT_TRUE(s != nullptr);
    ASSERT_EQ(rdma::RdmaEndpoint::UNINIT, RdmaTransportOf(s)->_rdma_ep->_state);
    memcpy(data, "RDMA", 4);
    ASSERT_TRUE(WriteAll(sockfd1, data, 4)); // Write magic string.
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::S_HELLO_WAIT, RdmaTransportOf(s));
    memcpy(data + 4, &len, 2);
    memset(data + 6, 0, 34);
    memcpy(data + 6, &ver, 2);  // hello_ver == 1, impl_ver == 0
    // Write the 36B base starting at data + 4 (NOT data). Pre-Step-1 this
    // UT mistakenly wrote `data, 36` which included the leftover "RDMA"
    // magic at data[0..4); the server parsed it as msg_len = 0x5244 and
    // happened to fall through to NegotiationValid (which then failed on
    // hello_ver). Now that Step 1 enforces a HELLO_V2_MSG_LEN_MAX upper bound,
    // such an oversized msg_len would be rejected before reaching the
    // version check, breaking the intent of this UT.
    ASSERT_TRUE(WriteAll(sockfd1, data + 4, 36));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::S_ACK_WAIT, RdmaTransportOf(s));
    ASSERT_EQ(RdmaTransport::RDMA_OFF, RdmaTransportOf(s)->_rdma_state);
    uint32_t flags = 0;
    ASSERT_TRUE(WriteAll(sockfd1, &flags, sizeof(flags)));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::FALLBACK_TCP, RdmaTransportOf(s));
    sockfd1.reset(-1);
    ASSERT_TRUE(WaitForServerSocketGone());

    butil::fd_guard sockfd2;
    ASSERT_NO_FATAL_FAILURE(ConnectToServer(&sockfd2));
    s = WaitForServerSocket();
    ASSERT_TRUE(s != nullptr);
    ASSERT_EQ(rdma::RdmaEndpoint::UNINIT, RdmaTransportOf(s)->_rdma_ep->_state);
    memcpy(data, "RDMA", 4);
    ASSERT_TRUE(WriteAll(sockfd2, data, 4)); // Write magic string.
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::S_HELLO_WAIT, RdmaTransportOf(s));
    memcpy(data + 4, &len, 2);
    memset(data + 6, 0, 32);
    memcpy(data + 8, &ver, 2);  // hello_ver == 0, impl_ver == 1
    // See comment above on `WriteAll(sockfd1, data + 4, 36)` for why we
    // write from data + 4 instead of data.
    ASSERT_TRUE(WriteAll(sockfd2, data + 4, 36));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::S_ACK_WAIT, RdmaTransportOf(s));
    ASSERT_EQ(RdmaTransport::RDMA_OFF, RdmaTransportOf(s)->_rdma_state);
    ASSERT_TRUE(WriteAll(sockfd2, &flags, sizeof(flags)));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::FALLBACK_TCP, RdmaTransportOf(s));
    sockfd2.reset(-1);
    ASSERT_TRUE(WaitForServerSocketGone());

    StopServer();
}

TEST_F(RdmaTest, client_hello_msg_invalid_sq_rq_block_size) {
    StartServer();

    Socket* s = nullptr;
    uint32_t flags = butil::HostToNet32(0);
    rdma::v2_wire::HelloMessage msg{};
    uint8_t data[rdma::HELLO_V2_MSG_LEN_MIN];
    msg.msg_len = rdma::HELLO_V2_MSG_LEN_MIN;
    msg.hello_ver = rdma::HELLO_V2_VERSION;
    msg.impl_ver = rdma::IMPL_V2_VERSION;

    msg.sq_size = 10;
    msg.rq_size = 16;
    msg.block_size = 8192;
    memcpy(data, "RDMA", 4);
    msg.Serialize(data + 4);
    butil::fd_guard sockfd1;
    ASSERT_NO_FATAL_FAILURE(ConnectToServer(&sockfd1));
    s = WaitForServerSocket();
    ASSERT_TRUE(s != nullptr);
    ASSERT_EQ(rdma::RdmaEndpoint::UNINIT, RdmaTransportOf(s)->_rdma_ep->_state);
    ASSERT_TRUE(WriteAll(sockfd1, data, 4)); // Write magic string.
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::S_HELLO_WAIT, RdmaTransportOf(s));
    ASSERT_TRUE(WriteAll(sockfd1, data + 4, 36));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::S_ACK_WAIT, RdmaTransportOf(s));
    ASSERT_EQ(RdmaTransport::RDMA_OFF, RdmaTransportOf(s)->_rdma_state);
    ASSERT_TRUE(WriteAll(sockfd1, &flags, sizeof(flags)));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::FALLBACK_TCP, RdmaTransportOf(s));
    sockfd1.reset(-1);
    ASSERT_TRUE(WaitForServerSocketGone());

    msg.sq_size = 16;
    msg.rq_size = 10;
    msg.block_size = 8192;
    memcpy(data, "RDMA", 4);
    msg.Serialize(data + 4);
    butil::fd_guard sockfd2;
    ASSERT_NO_FATAL_FAILURE(ConnectToServer(&sockfd2));
    s = WaitForServerSocket();
    ASSERT_TRUE(s != nullptr);
    ASSERT_EQ(rdma::RdmaEndpoint::UNINIT, RdmaTransportOf(s)->_rdma_ep->_state);
    ASSERT_TRUE(WriteAll(sockfd2, data, 4)); // Write magic string.
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::S_HELLO_WAIT, RdmaTransportOf(s));
    ASSERT_TRUE(WriteAll(sockfd2, data + 4, 36));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::S_ACK_WAIT, RdmaTransportOf(s));
    ASSERT_EQ(RdmaTransport::RDMA_OFF, RdmaTransportOf(s)->_rdma_state);
    ASSERT_TRUE(WriteAll(sockfd2, &flags, sizeof(flags)));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::FALLBACK_TCP, RdmaTransportOf(s));
    sockfd2.reset(-1);
    ASSERT_TRUE(WaitForServerSocketGone());

    msg.sq_size = 16;
    msg.rq_size = 16;
    msg.block_size = 1000;
    memcpy(data, "RDMA", 4);
    msg.Serialize(data + 4);
    butil::fd_guard sockfd3;
    ASSERT_NO_FATAL_FAILURE(ConnectToServer(&sockfd3));
    s = WaitForServerSocket();
    ASSERT_TRUE(s != nullptr);
    ASSERT_EQ(rdma::RdmaEndpoint::UNINIT, RdmaTransportOf(s)->_rdma_ep->_state);
    ASSERT_TRUE(WriteAll(sockfd3, data, 4)); // Write magic string.
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::S_HELLO_WAIT, RdmaTransportOf(s));
    ASSERT_TRUE(WriteAll(sockfd3, data + 4, 36));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::S_ACK_WAIT, RdmaTransportOf(s));
    ASSERT_EQ(RdmaTransport::RDMA_OFF, RdmaTransportOf(s)->_rdma_state);
    ASSERT_TRUE(WriteAll(sockfd3, &flags, sizeof(flags)));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::FALLBACK_TCP, RdmaTransportOf(s));
    sockfd3.reset(-1);
    ASSERT_TRUE(WaitForServerSocketGone());

    StopServer();
}

TEST_F(RdmaTest, client_close_after_qp_build) {
    StartServer();

    Socket* s = nullptr;
    uint8_t data[rdma::HELLO_V2_MSG_LEN_MIN];
    MakeV2ClientHello(data);

    butil::fd_guard sockfd1;
    ASSERT_NO_FATAL_FAILURE(ConnectToServer(&sockfd1));
    s = WaitForServerSocket();
    ASSERT_TRUE(s != nullptr);
    ASSERT_EQ(rdma::RdmaEndpoint::UNINIT, RdmaTransportOf(s)->_rdma_ep->_state);
    ASSERT_TRUE(WriteAll(sockfd1, data, sizeof(data)));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::S_ACK_WAIT, RdmaTransportOf(s));
    sockfd1.reset(-1);
    ASSERT_TRUE(WaitForServerSocketGone());

    StopServer();
}

TEST_F(RdmaTest, client_close_during_ack_send) {
    StartServer();

    Socket* s = nullptr;
    uint8_t data[rdma::HELLO_V2_MSG_LEN_MIN];
    MakeV2ClientHello(data);

    butil::fd_guard sockfd1;
    ASSERT_NO_FATAL_FAILURE(ConnectToServer(&sockfd1));
    s = WaitForServerSocket();
    ASSERT_TRUE(s != nullptr);
    ASSERT_EQ(rdma::RdmaEndpoint::UNINIT, RdmaTransportOf(s)->_rdma_ep->_state);
    ASSERT_TRUE(WriteAll(sockfd1, data, 4)); // Write magic string.
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::S_HELLO_WAIT, RdmaTransportOf(s));
    ASSERT_TRUE(WriteAll(sockfd1, data + 4, 36));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::S_ACK_WAIT, RdmaTransportOf(s));
    uint32_t flags = butil::HostToNet32(1);
    ASSERT_TRUE(WriteAll(sockfd1, &flags, sizeof(flags)));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::ESTABLISHED, RdmaTransportOf(s));
    sockfd1.reset(-1);
    ASSERT_TRUE(WaitForServerSocketGone());

    StopServer();
}

TEST_F(RdmaTest, client_close_after_ack_send) {
    StartServer();

    Socket* s = nullptr;
    uint8_t data[rdma::HELLO_V2_MSG_LEN_MIN];
    MakeV2ClientHello(data);

    butil::fd_guard sockfd1;
    ASSERT_NO_FATAL_FAILURE(ConnectToServer(&sockfd1));
    s = WaitForServerSocket();
    ASSERT_TRUE(s != nullptr);
    ASSERT_EQ(rdma::RdmaEndpoint::UNINIT, RdmaTransportOf(s)->_rdma_ep->_state);
    ASSERT_TRUE(WriteAll(sockfd1, data, 4)); // Write magic string.
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::S_HELLO_WAIT, RdmaTransportOf(s));
    ASSERT_TRUE(WriteAll(sockfd1, data + 4, 36));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::S_ACK_WAIT, RdmaTransportOf(s));
    uint32_t flags = butil::HostToNet32(0);
    ASSERT_TRUE(WriteAll(sockfd1, &flags, sizeof(flags)));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::FALLBACK_TCP, RdmaTransportOf(s));
    ASSERT_EQ(RdmaTransport::RDMA_OFF, RdmaTransportOf(s)->_rdma_state);
    sockfd1.reset(-1);
    ASSERT_TRUE(WaitForServerSocketGone());

    butil::fd_guard sockfd2;
    ASSERT_NO_FATAL_FAILURE(ConnectToServer(&sockfd2));
    s = WaitForServerSocket();
    ASSERT_TRUE(s != nullptr);
    ASSERT_EQ(rdma::RdmaEndpoint::UNINIT, RdmaTransportOf(s)->_rdma_ep->_state);
    ASSERT_TRUE(WriteAll(sockfd2, data, 4)); // Write magic string.
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::S_HELLO_WAIT, RdmaTransportOf(s));
    ASSERT_TRUE(WriteAll(sockfd2, data + 4, 36));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::S_ACK_WAIT, RdmaTransportOf(s));
    flags = butil::HostToNet32(1);
    ASSERT_TRUE(WriteAll(sockfd2, &flags, sizeof(flags)));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::ESTABLISHED, RdmaTransportOf(s));
    sockfd2.reset(-1);
    ASSERT_TRUE(WaitForServerSocketGone());

    StopServer();
}

TEST_F(RdmaTest, client_send_data_on_tcp_after_ack_send) {
    StartServer();

    Socket* s = nullptr;
    uint8_t data[rdma::HELLO_V2_MSG_LEN_MIN];
    MakeV2ClientHello(data);

    butil::fd_guard sockfd1;
    ASSERT_NO_FATAL_FAILURE(ConnectToServer(&sockfd1));
    s = WaitForServerSocket();
    ASSERT_TRUE(s != nullptr);
    ASSERT_EQ(rdma::RdmaEndpoint::UNINIT, RdmaTransportOf(s)->_rdma_ep->_state);
    ASSERT_TRUE(WriteAll(sockfd1, data, 4)); // Write magic string.
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::S_HELLO_WAIT, RdmaTransportOf(s));
    ASSERT_TRUE(WriteAll(sockfd1, data + 4, 36));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::S_ACK_WAIT, RdmaTransportOf(s));
    uint32_t flags = butil::HostToNet32(0);
    ASSERT_TRUE(WriteAll(sockfd1, &flags, sizeof(flags)));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::FALLBACK_TCP, RdmaTransportOf(s));
    // 4 more bytes on a fd that fell back to TCP are not a protocol baidu_std
    // knows, so the connection is dropped.
    ASSERT_TRUE(WriteAll(sockfd1, &flags, sizeof(flags)));
    ASSERT_TRUE(WaitForServerSocketGone());

    butil::fd_guard sockfd2;
    ASSERT_NO_FATAL_FAILURE(ConnectToServer(&sockfd2));
    s = WaitForServerSocket();
    ASSERT_TRUE(s != nullptr);
    ASSERT_EQ(rdma::RdmaEndpoint::UNINIT, RdmaTransportOf(s)->_rdma_ep->_state);
    ASSERT_TRUE(WriteAll(sockfd2, data, 4)); // Write magic string.
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::S_HELLO_WAIT, RdmaTransportOf(s));
    ASSERT_TRUE(WriteAll(sockfd2, data + 4, 36));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::S_ACK_WAIT, RdmaTransportOf(s));
    flags = butil::HostToNet32(1);
    ASSERT_TRUE(WriteAll(sockfd2, &flags, sizeof(flags)));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::ESTABLISHED, RdmaTransportOf(s));
    // Once RDMA is on the fd carries no RPC data at all, so this is an error.
    ASSERT_TRUE(WriteAll(sockfd2, &flags, sizeof(flags)));
    ASSERT_TRUE(WaitForServerSocketGone());

    StopServer();
}

// Connect, push a well-formed v2 hello and read back the server's reply, which
// leaves the server in S_ACK_WAIT waiting for the 4B ACK.
static void HandshakeUntilAckWait(butil::fd_guard* sockfd) {
    ASSERT_NO_FATAL_FAILURE(ConnectToServer(sockfd));

    uint8_t hello[rdma::HELLO_V2_MSG_LEN_MIN];
    MakeV2ClientHello(hello);
    ASSERT_TRUE(WriteAll(*sockfd, hello, sizeof(hello)));
    // The server answers only once it has consumed our hello, so reading the
    // whole reply is a synchronization point: no sleeping needed here.
    uint8_t reply[rdma::HELLO_V2_MSG_LEN_MIN];
    ASSERT_TRUE(ReadAll(*sockfd, reply, sizeof(reply)));
}

// A client is free to pipeline its first request right behind the handshake
// ACK. Only the 4B ACK belongs to the handshake. Whatever follows it must be
// handed over to the real protocol instead of dropping the connection.
TEST_F(RdmaTest, server_accepts_data_pipelined_behind_fallback_ack) {
    StartServer();

    butil::fd_guard sockfd;
    ASSERT_NO_FATAL_FAILURE(HandshakeUntilAckWait(&sockfd));
    Socket* s = WaitForServerSocket();
    ASSERT_TRUE(s != nullptr);
    auto* transport = RdmaTransportOf(s);
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::S_ACK_WAIT, transport);

    // An ACK asking for TCP, plus the first 4 bytes of a baidu_std request. One
    // write, so that both end up in the same read on the server.
    uint8_t ack_and_data[rdma::HELLO_ACK_LEN + 4];
    const uint32_t flags = butil::HostToNet32(0);
    memcpy(ack_and_data, &flags, rdma::HELLO_ACK_LEN);
    memcpy(ack_and_data + rdma::HELLO_ACK_LEN, "PRPC", 4);
    ASSERT_TRUE(WriteAll(sockfd, ack_and_data, sizeof(ack_and_data)));

    // The handshake took the ACK only and left "PRPC" to baidu_std, which is
    // now waiting for the rest of its 12B header. So the connection lives on
    // with those 4 bytes still buffered. Note that baidu_std gets them a moment
    // after the handshake gave up the stream, hence the wait.
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::FALLBACK_TCP, transport);
    ASSERT_EQ(RdmaTransport::RDMA_OFF, transport->_rdma_state);
    ASSERT_TRUE(GetSocketFromServer(0) != nullptr);
    ASSERT_TRUE(WaitForFdReadBuf(s, 4));

    sockfd.reset(-1);
    ASSERT_TRUE(WaitForServerSocketGone());

    StopServer();
}

// Once RDMA is on, the TCP fd is no longer an RPC channel, so bytes trailing
// the ACK can only be a protocol error.
TEST_F(RdmaTest, server_rejects_data_pipelined_behind_rdma_ack) {
    StartServer();

    butil::fd_guard sockfd;
    ASSERT_NO_FATAL_FAILURE(HandshakeUntilAckWait(&sockfd));
    Socket* s = WaitForServerSocket();
    ASSERT_TRUE(s != nullptr);
    auto* transport = RdmaTransportOf(s);
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::S_ACK_WAIT, transport);

    uint8_t ack_and_data[rdma::HELLO_ACK_LEN + 4];
    const uint32_t flags = butil::HostToNet32(rdma::HELLO_ACK_RDMA_OK);
    memcpy(ack_and_data, &flags, rdma::HELLO_ACK_LEN);
    memcpy(ack_and_data + rdma::HELLO_ACK_LEN, "PRPC", 4);
    ASSERT_TRUE(WriteAll(sockfd, ack_and_data, sizeof(ack_and_data)));

    // Note that `transport->_rdma_ep` is gone by now: dropping the connection
    // recycles the Socket, and RdmaTransport::Release() deletes the endpoint.
    ASSERT_TRUE(WaitForServerSocketGone());

    StopServer();
}

// Once RDMA is on, the server must stop parsing its TCP fd altogether.
TEST_F(RdmaTest, server_stops_parsing_tcp_fd_once_rdma_is_on) {
    StartServer();

    butil::fd_guard sockfd;
    ASSERT_NO_FATAL_FAILURE(HandshakeUntilAckWait(&sockfd));
    Socket* s = WaitForServerSocket();
    ASSERT_TRUE(s != nullptr);
    auto* transport = RdmaTransportOf(s);
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::S_ACK_WAIT, transport);

    // A bare ACK asking for RDMA. Nothing trails it, so the handshake ends in
    // ESTABLISHED instead of being rejected (see the test above).
    const uint32_t flags = butil::HostToNet32(rdma::HELLO_ACK_RDMA_OK);
    ASSERT_TRUE(WriteAll(sockfd, &flags, sizeof(flags)));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::ESTABLISHED, transport);
    ASSERT_EQ(RdmaTransport::RDMA_ON, transport->_rdma_state);
    ASSERT_TRUE(GetSocketFromServer(0) != nullptr);

    ASSERT_TRUE(WriteAll(sockfd, "PRPC", 4));
    ASSERT_TRUE(WaitForServerSocketGone());

    StopServer();
}

// The same bytes on the stream carried by the QP are a real RPC, and the handler
// must decline so that CutInputMessage() moves on to the protocol handlers.
TEST_F(RdmaTest, server_parses_qp_stream_after_rdma_is_on) {
    StartServer();

    butil::fd_guard sockfd;
    ASSERT_NO_FATAL_FAILURE(HandshakeUntilAckWait(&sockfd));
    Socket* s = WaitForServerSocket();
    ASSERT_TRUE(s != nullptr);
    auto* transport = RdmaTransportOf(s);
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::S_ACK_WAIT, transport);

    const uint32_t flags = butil::HostToNet32(rdma::HELLO_ACK_RDMA_OK);
    ASSERT_TRUE(WriteAll(sockfd, &flags, sizeof(flags)));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::ESTABLISHED, transport);

    InputMessengerProcessor& qp_stream =
            transport->_rdma_ep->_host.input_processor;
    ASSERT_TRUE(qp_stream.read_buf().empty());
    qp_stream.read_buf().append("PRPC");
    InputMessageClosure last_msg;
    // The real caller stamps messages with cpuwide_time_us() and derives
    // base_realtime from it, so feed ProcessNewMessage() the same time domain:
    // received_us also ends up in Socket::_last_readtime_us.
    const uint64_t received_us = butil::cpuwide_time_us();
    const uint64_t base_realtime = butil::gettimeofday_us() - received_us;
    ASSERT_EQ(0, qp_stream.ProcessNewMessage(
                     4, false, received_us, base_realtime, last_msg));
    // baidu_std claimed the stream and is waiting for the rest of its header.
    ASSERT_EQ((int)PROTOCOL_BAIDU_STD, s->preferred_index());
    ASSERT_EQ(4u, qp_stream.read_buf().size());
    ASSERT_TRUE(s->fd_input_processor().read_buf().empty());
    ASSERT_EQ(rdma::RdmaEndpoint::ESTABLISHED, transport->_rdma_ep->_state);
    ASSERT_FALSE(s->Failed());

    StopServer();
}

// After the handshake is over, CutInputMessage() still offers the data to every
// registered handler, this one included. It must decline instead of reading the
// data as a fresh client hello.
TEST_F(RdmaTest, server_declines_handshake_bytes_after_fallback) {
    StartServer();

    butil::fd_guard sockfd;
    ASSERT_NO_FATAL_FAILURE(HandshakeUntilAckWait(&sockfd));
    Socket* s = WaitForServerSocket();
    ASSERT_TRUE(s != nullptr);
    auto* transport = RdmaTransportOf(s);

    const uint32_t flags = butil::HostToNet32(0);
    ASSERT_TRUE(WriteAll(sockfd, &flags, sizeof(flags)));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::FALLBACK_TCP, transport);

    // Replay a valid hello. baidu_std rejects it and no other protocol claims
    // it, so the connection is dropped. What must NOT happen is a second
    // handshake: that would answer with another server hello.
    uint8_t hello[rdma::HELLO_V2_MSG_LEN_MIN];
    MakeV2ClientHello(hello);
    ASSERT_TRUE(WriteAll(sockfd, hello, sizeof(hello)));

    // Note that `transport->_rdma_ep` is gone by now: dropping the connection
    // recycles the Socket, and RdmaTransport::Release() deletes the endpoint.
    ASSERT_TRUE(WaitForServerSocketGone());
    uint8_t reply[rdma::HELLO_V2_MSG_LEN_MIN];
    ASSERT_LE(recv(sockfd, reply, sizeof(reply), MSG_DONTWAIT), 0);

    StopServer();
}

TEST_F(RdmaTest, fd_and_qp_input_streams_are_separate) {
    StartServer();

    butil::fd_guard sockfd;
    ASSERT_NO_FATAL_FAILURE(ConnectToServer(&sockfd));
    Socket* s = WaitForServerSocket();
    ASSERT_TRUE(s != nullptr);
    auto* transport = RdmaTransportOf(s);

    // The fd stream belongs to the Socket, the QP stream to the endpoint.
    InputMessengerProcessor& fd_stream = s->fd_input_processor();
    InputMessengerProcessor& qp_stream =
            transport->_rdma_ep->_host.input_processor;
    ASSERT_NE(&fd_stream, &qp_stream);
    ASSERT_NE(&fd_stream.read_buf(), &qp_stream.read_buf());

    // Two magic bytes are too few to dispatch on, so they stay buffered. In the
    // fd stream, and only there.
    ASSERT_TRUE(WriteAll(sockfd, "RD", 2));
    ASSERT_TRUE(WaitForFdReadBuf(s, 2));
    ASSERT_EQ(rdma::RdmaEndpoint::UNINIT, transport->_rdma_ep->_state);
    ASSERT_EQ(2u, fd_stream.read_buf().size());
    ASSERT_TRUE(qp_stream.read_buf().empty());

    sockfd.reset(-1);
    ASSERT_TRUE(WaitForServerSocketGone());

    StopServer();
}

TEST_F(RdmaTest, server_miss_before_hello_send) {
    butil::fd_guard sockfd(butil::tcp_listen(g_ep));
    EXPECT_TRUE(sockfd >= 0);

    Channel channel;
    ChannelOptions chan_options;
    chan_options.socket_mode = SOCKET_MODE_RDMA;
    chan_options.connect_timeout_ms = 500;
    chan_options.timeout_ms = 500;
    chan_options.max_retry = 0;
    ASSERT_EQ(0, channel.Init(g_ep, &chan_options));

    Controller cntl;
    test::EchoRequest req;
    test::EchoResponse res;
    req.set_message(__FUNCTION__);
    google::protobuf::Closure* done = DoNothing();
    ::test::EchoService::Stub(&channel).Echo(&cntl, &req, &res, done);

    SocketUniquePtr s;
    ASSERT_EQ(0, Socket::Address(cntl._single_server_id, &s));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::C_HELLO_WAIT, RdmaTransportOf(s));

    butil::fd_guard acc_fd(accept(sockfd, nullptr, nullptr));
    ASSERT_TRUE(acc_fd >= 0);
    bthread_id_join(cntl.call_id());

    ASSERT_EQ(ERPCTIMEDOUT, cntl.ErrorCode());
}

TEST_F(RdmaTest, server_close_before_hello_send) {
    butil::fd_guard sockfd(butil::tcp_listen(g_ep));
    EXPECT_TRUE(sockfd >= 0);

    Channel channel;
    ChannelOptions chan_options;
    chan_options.socket_mode = SOCKET_MODE_RDMA;
    chan_options.connect_timeout_ms = 500;
    chan_options.timeout_ms = 500;
    chan_options.max_retry = 0;
    ASSERT_EQ(0, channel.Init(g_ep, &chan_options));

    Controller cntl;
    test::EchoRequest req;
    test::EchoResponse res;
    req.set_message(__FUNCTION__);
    google::protobuf::Closure* done = DoNothing();
    ::test::EchoService::Stub(&channel).Echo(&cntl, &req, &res, done);

    SocketUniquePtr s;
    ASSERT_EQ(0, Socket::Address(cntl._single_server_id, &s));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::C_HELLO_WAIT, RdmaTransportOf(s));

    butil::fd_guard acc_fd(accept(sockfd, nullptr, nullptr));
    ASSERT_TRUE(acc_fd >= 0);
    uint8_t data[rdma::HELLO_V2_MSG_LEN_MIN];
    ASSERT_EQ(rdma::HELLO_V2_MSG_LEN_MIN, read(acc_fd, data, rdma::HELLO_V2_MSG_LEN_MIN));
    close(acc_fd);
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::FAILED, RdmaTransportOf(s));
    bthread_id_join(cntl.call_id());

    ASSERT_EQ(EEOF, cntl.ErrorCode());
}

TEST_F(RdmaTest, server_miss_during_magic_str) {
    butil::fd_guard sockfd(butil::tcp_listen(g_ep));
    EXPECT_TRUE(sockfd >= 0);

    Channel channel;
    ChannelOptions chan_options;
    chan_options.socket_mode = SOCKET_MODE_RDMA;
    chan_options.connect_timeout_ms = 500;
    chan_options.timeout_ms = 500;
    chan_options.max_retry = 0;
    ASSERT_EQ(0, channel.Init(g_ep, &chan_options));

    Controller cntl;
    test::EchoRequest req;
    test::EchoResponse res;
    req.set_message(__FUNCTION__);
    google::protobuf::Closure* done = DoNothing();
    ::test::EchoService::Stub(&channel).Echo(&cntl, &req, &res, done);

    SocketUniquePtr s;
    ASSERT_EQ(0, Socket::Address(cntl._single_server_id, &s));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::C_HELLO_WAIT, RdmaTransportOf(s));

    butil::fd_guard acc_fd(accept(sockfd, nullptr, nullptr));
    ASSERT_TRUE(acc_fd >= 0);
    uint8_t data[rdma::HELLO_V2_MSG_LEN_MIN];
    ASSERT_EQ(rdma::HELLO_V2_MSG_LEN_MIN, read(acc_fd, data, rdma::HELLO_V2_MSG_LEN_MIN));
    ASSERT_TRUE(WriteAll(acc_fd, "RD", 2));
    // Half a magic is not enough to decide anything, so the client stays stuck
    // in the handshake read and the RPC runs into its timeout. Joining below
    // waits for exactly that, no sleeping needed.
    bthread_id_join(cntl.call_id());

    ASSERT_EQ(ERPCTIMEDOUT, cntl.ErrorCode());
}

TEST_F(RdmaTest, server_close_during_magic_str) {
    butil::fd_guard sockfd(butil::tcp_listen(g_ep));
    EXPECT_TRUE(sockfd >= 0);

    Channel channel;
    ChannelOptions chan_options;
    chan_options.socket_mode = SOCKET_MODE_RDMA;
    chan_options.connect_timeout_ms = 500;
    chan_options.timeout_ms = 500;
    chan_options.max_retry = 0;
    ASSERT_EQ(0, channel.Init(g_ep, &chan_options));

    Controller cntl;
    test::EchoRequest req;
    test::EchoResponse res;
    req.set_message(__FUNCTION__);
    google::protobuf::Closure* done = DoNothing();
    ::test::EchoService::Stub(&channel).Echo(&cntl, &req, &res, done);

    SocketUniquePtr s;
    ASSERT_EQ(0, Socket::Address(cntl._single_server_id, &s));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::C_HELLO_WAIT, RdmaTransportOf(s));

    butil::fd_guard acc_fd(accept(sockfd, nullptr, nullptr));
    ASSERT_TRUE(acc_fd >= 0);
    uint8_t data[rdma::HELLO_V2_MSG_LEN_MIN];
    ASSERT_EQ(rdma::HELLO_V2_MSG_LEN_MIN, read(acc_fd, data, rdma::HELLO_V2_MSG_LEN_MIN));
    // Half a magic and then EOF. TCP keeps the order, so the client always sees
    // the two bytes first and then the close, which is what this test is about.
    ASSERT_TRUE(WriteAll(acc_fd, "RD", 2));
    acc_fd.reset(-1);
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::FAILED, RdmaTransportOf(s));
    bthread_id_join(cntl.call_id());

    ASSERT_EQ(EEOF, cntl.ErrorCode());
}

TEST_F(RdmaTest, server_hello_invalid_magic_str) {
    butil::fd_guard sockfd(butil::tcp_listen(g_ep));
    EXPECT_TRUE(sockfd >= 0);

    Channel channel;
    ChannelOptions chan_options;
    chan_options.socket_mode = SOCKET_MODE_RDMA;
    chan_options.connect_timeout_ms = 500;
    chan_options.timeout_ms = 500;
    chan_options.max_retry = 0;
    ASSERT_EQ(0, channel.Init(g_ep, &chan_options));

    Controller cntl;
    test::EchoRequest req;
    test::EchoResponse res;
    req.set_message(__FUNCTION__);
    google::protobuf::Closure* done = DoNothing();
    ::test::EchoService::Stub(&channel).Echo(&cntl, &req, &res, done);

    SocketUniquePtr s;
    ASSERT_EQ(0, Socket::Address(cntl._single_server_id, &s));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::C_HELLO_WAIT, RdmaTransportOf(s));

    butil::fd_guard acc_fd(accept(sockfd, nullptr, nullptr));
    ASSERT_TRUE(acc_fd >= 0);
    uint8_t data[rdma::HELLO_V2_MSG_LEN_MIN];
    ASSERT_EQ(rdma::HELLO_V2_MSG_LEN_MIN, read(acc_fd, data, rdma::HELLO_V2_MSG_LEN_MIN));
    ASSERT_EQ(4, write(acc_fd, "ABCD", 4));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::FAILED, RdmaTransportOf(s));
    bthread_id_join(cntl.call_id());

    ASSERT_EQ(EPROTO, cntl.ErrorCode());
}

TEST_F(RdmaTest, server_miss_during_hello_msg) {
    butil::fd_guard sockfd(butil::tcp_listen(g_ep));
    EXPECT_TRUE(sockfd >= 0);

    Channel channel;
    ChannelOptions chan_options;
    chan_options.socket_mode = SOCKET_MODE_RDMA;
    chan_options.connect_timeout_ms = 500;
    chan_options.timeout_ms = 500;
    chan_options.max_retry = 0;
    ASSERT_EQ(0, channel.Init(g_ep, &chan_options));

    Controller cntl;
    test::EchoRequest req;
    test::EchoResponse res;
    req.set_message(__FUNCTION__);
    google::protobuf::Closure* done = DoNothing();
    ::test::EchoService::Stub(&channel).Echo(&cntl, &req, &res, done);

    SocketUniquePtr s;
    ASSERT_EQ(0, Socket::Address(cntl._single_server_id, &s));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::C_HELLO_WAIT, RdmaTransportOf(s));

    butil::fd_guard acc_fd(accept(sockfd, nullptr, nullptr));
    ASSERT_TRUE(acc_fd >= 0);
    uint8_t data[rdma::HELLO_V2_MSG_LEN_MIN];
    ASSERT_EQ(rdma::HELLO_V2_MSG_LEN_MIN, read(acc_fd, data, rdma::HELLO_V2_MSG_LEN_MIN));
    ASSERT_EQ(4, write(acc_fd, "RDMA", 4));
    ASSERT_EQ(2, write(acc_fd, "00", 2));
    bthread_id_join(cntl.call_id());

    ASSERT_EQ(ERPCTIMEDOUT, cntl.ErrorCode());
}

TEST_F(RdmaTest, server_close_during_hello_msg) {
    butil::fd_guard sockfd(butil::tcp_listen(g_ep));
    EXPECT_TRUE(sockfd >= 0);

    Channel channel;
    ChannelOptions chan_options;
    chan_options.socket_mode = SOCKET_MODE_RDMA;
    chan_options.connect_timeout_ms = 500;
    chan_options.timeout_ms = 500;
    chan_options.max_retry = 0;
    ASSERT_EQ(0, channel.Init(g_ep, &chan_options));

    Controller cntl;
    test::EchoRequest req;
    test::EchoResponse res;
    req.set_message(__FUNCTION__);
    google::protobuf::Closure* done = DoNothing();
    ::test::EchoService::Stub(&channel).Echo(&cntl, &req, &res, done);

    SocketUniquePtr s;
    ASSERT_EQ(0, Socket::Address(cntl._single_server_id, &s));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::C_HELLO_WAIT, RdmaTransportOf(s));

    butil::fd_guard acc_fd(accept(sockfd, nullptr, nullptr));
    ASSERT_TRUE(acc_fd >= 0);
    uint8_t data[rdma::HELLO_V2_MSG_LEN_MIN];
    ASSERT_EQ(rdma::HELLO_V2_MSG_LEN_MIN, read(acc_fd, data, rdma::HELLO_V2_MSG_LEN_MIN));
    ASSERT_EQ(4, write(acc_fd, "RDMA", 4));
    ASSERT_EQ(2, write(acc_fd, "00", 2));
    close(acc_fd);
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::FAILED, RdmaTransportOf(s));
    bthread_id_join(cntl.call_id());

    ASSERT_EQ(EEOF, cntl.ErrorCode());
}

TEST_F(RdmaTest, server_hello_invalid_msg_len) {
    butil::fd_guard sockfd(butil::tcp_listen(g_ep));
    EXPECT_TRUE(sockfd >= 0);

    Channel channel;
    ChannelOptions chan_options;
    chan_options.socket_mode = SOCKET_MODE_RDMA;
    chan_options.connect_timeout_ms = 500;
    chan_options.timeout_ms = 500;
    chan_options.max_retry = 0;
    ASSERT_EQ(0, channel.Init(g_ep, &chan_options));

    Controller cntl;
    test::EchoRequest req;
    test::EchoResponse res;
    req.set_message(__FUNCTION__);
    google::protobuf::Closure* done = DoNothing();
    ::test::EchoService::Stub(&channel).Echo(&cntl, &req, &res, done);

    SocketUniquePtr s;
    ASSERT_EQ(0, Socket::Address(cntl._single_server_id, &s));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::C_HELLO_WAIT, RdmaTransportOf(s));

    butil::fd_guard acc_fd(accept(sockfd, nullptr, nullptr));
    ASSERT_TRUE(acc_fd >= 0);
    uint8_t data[rdma::HELLO_V2_MSG_LEN_MIN];
    ASSERT_EQ(rdma::HELLO_V2_MSG_LEN_MIN, read(acc_fd, data, rdma::HELLO_V2_MSG_LEN_MIN));
    memcpy(data, "RDMA", 4);
    uint16_t len = butil::HostToNet16(35);
    memcpy(data + 4, &len, 2);
    memset(data + 6, 0, 32);
    ASSERT_EQ(rdma::HELLO_V2_MSG_LEN_MIN, write(acc_fd, data, rdma::HELLO_V2_MSG_LEN_MIN));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::FAILED, RdmaTransportOf(s));
    bthread_id_join(cntl.call_id());

    ASSERT_EQ(EPROTO, cntl.ErrorCode());
}

TEST_F(RdmaTest, server_hello_invalid_version) {
    butil::fd_guard sockfd(butil::tcp_listen(g_ep));
    EXPECT_TRUE(sockfd >= 0);

    Channel channel;
    ChannelOptions chan_options;
    chan_options.socket_mode = SOCKET_MODE_RDMA;
    chan_options.connect_timeout_ms = 500;
    chan_options.timeout_ms = 500;
    chan_options.max_retry = 0;
    ASSERT_EQ(0, channel.Init(g_ep, &chan_options));

    Controller cntl;
    test::EchoRequest req;
    test::EchoResponse res;
    req.set_message(__FUNCTION__);
    google::protobuf::Closure* done = DoNothing();
    ::test::EchoService::Stub(&channel).Echo(&cntl, &req, &res, done);

    SocketUniquePtr s;
    ASSERT_EQ(0, Socket::Address(cntl._single_server_id, &s));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::C_HELLO_WAIT, RdmaTransportOf(s));

    butil::fd_guard acc_fd(accept(sockfd, nullptr, nullptr));
    ASSERT_TRUE(acc_fd >= 0);
    uint8_t data[rdma::HELLO_V2_MSG_LEN_MIN];
    ASSERT_EQ(rdma::HELLO_V2_MSG_LEN_MIN, read(acc_fd, data, rdma::HELLO_V2_MSG_LEN_MIN));
    memcpy(data, "RDMA", 4);
    uint16_t len = butil::HostToNet16(rdma::HELLO_V2_MSG_LEN_MIN);
    memcpy(data + 4, &len, 2);
    memset(data + 6, 0, 32);
    ASSERT_EQ(rdma::HELLO_V2_MSG_LEN_MIN, write(acc_fd, data, rdma::HELLO_V2_MSG_LEN_MIN));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::FALLBACK_TCP, RdmaTransportOf(s));
    ASSERT_EQ(4, read(acc_fd, data, 4));
    uint32_t* tmp = (uint32_t*)data;
    ASSERT_EQ(0, butil::NetToHost32(*tmp));
    bthread_id_join(cntl.call_id());

    ASSERT_EQ(ERPCTIMEDOUT, cntl.ErrorCode());
}

TEST_F(RdmaTest, server_hello_invalid_sq_rq_size) {
    butil::fd_guard sockfd(butil::tcp_listen(g_ep));
    EXPECT_TRUE(sockfd >= 0);

    Channel channel;
    ChannelOptions chan_options;
    chan_options.socket_mode = SOCKET_MODE_RDMA;
    chan_options.connect_timeout_ms = 500;
    chan_options.timeout_ms = 500;
    chan_options.max_retry = 0;
    ASSERT_EQ(0, channel.Init(g_ep, &chan_options));

    Controller cntl;
    test::EchoRequest req;
    test::EchoResponse res;
    req.set_message(__FUNCTION__);
    google::protobuf::Closure* done = DoNothing();
    ::test::EchoService::Stub(&channel).Echo(&cntl, &req, &res, done);

    SocketUniquePtr s;
    ASSERT_EQ(0, Socket::Address(cntl._single_server_id, &s));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::C_HELLO_WAIT, RdmaTransportOf(s));

    butil::fd_guard acc_fd(accept(sockfd, nullptr, nullptr));
    ASSERT_TRUE(acc_fd >= 0);
    uint8_t data[rdma::HELLO_V2_MSG_LEN_MIN];
    ASSERT_EQ(rdma::HELLO_V2_MSG_LEN_MIN, read(acc_fd, data, rdma::HELLO_V2_MSG_LEN_MIN));

    rdma::v2_wire::HelloMessage msg{};
    msg.msg_len = rdma::HELLO_V2_MSG_LEN_MIN;
    msg.hello_ver = 1;
    msg.impl_ver = 1;
    msg.sq_size = 0;
    msg.rq_size = 0;
    msg.block_size = 8192;
    msg.qp_num = 0;
    msg.gid = rdma::GetRdmaGid();
    memcpy(data, "RDMA", 4);
    msg.Serialize(data + 4);
    ASSERT_EQ(rdma::HELLO_V2_MSG_LEN_MIN, write(acc_fd, data, rdma::HELLO_V2_MSG_LEN_MIN));

    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::FALLBACK_TCP, RdmaTransportOf(s));
    ASSERT_EQ(4, read(acc_fd, data, 4));
    uint32_t* tmp = (uint32_t*)data;
    ASSERT_EQ(0, butil::NetToHost32(*tmp));
    bthread_id_join(cntl.call_id());

    ASSERT_EQ(ERPCTIMEDOUT, cntl.ErrorCode());
}

TEST_F(RdmaTest, server_miss_after_ack) {
    butil::fd_guard sockfd(butil::tcp_listen(g_ep));
    EXPECT_TRUE(sockfd >= 0);

    Channel channel;
    ChannelOptions chan_options;
    chan_options.socket_mode = SOCKET_MODE_RDMA;
    chan_options.connect_timeout_ms = 500;
    chan_options.timeout_ms = 500;
    chan_options.max_retry = 0;
    ASSERT_EQ(0, channel.Init(g_ep, &chan_options));

    Controller cntl;
    test::EchoRequest req;
    test::EchoResponse res;
    req.set_message(__FUNCTION__);
    google::protobuf::Closure* done = DoNothing();
    ::test::EchoService::Stub(&channel).Echo(&cntl, &req, &res, done);

    SocketUniquePtr s;
    ASSERT_EQ(0, Socket::Address(cntl._single_server_id, &s));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::C_HELLO_WAIT, RdmaTransportOf(s));

    butil::fd_guard acc_fd(accept(sockfd, nullptr, nullptr));
    ASSERT_TRUE(acc_fd >= 0);
    uint8_t data[rdma::HELLO_V2_MSG_LEN_MIN];
    ASSERT_EQ(rdma::HELLO_V2_MSG_LEN_MIN, read(acc_fd, data, rdma::HELLO_V2_MSG_LEN_MIN));

    rdma::v2_wire::HelloMessage msg{};
    msg.msg_len = rdma::HELLO_V2_MSG_LEN_MIN;
    msg.hello_ver = rdma::HELLO_V2_VERSION;
    msg.impl_ver = rdma::IMPL_V2_VERSION;
    msg.sq_size = 16;
    msg.rq_size = 16;
    msg.block_size = 8192;
    msg.qp_num = 0;
    msg.gid = rdma::GetRdmaGid();
    memcpy(data, "RDMA", 4);
    msg.Serialize(data + 4);
    ASSERT_EQ(rdma::HELLO_V2_MSG_LEN_MIN, write(acc_fd, data, rdma::HELLO_V2_MSG_LEN_MIN));

    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::ESTABLISHED, RdmaTransportOf(s));
    ASSERT_EQ(4, read(acc_fd, data, 4));
    uint32_t* tmp = (uint32_t*)data;
    ASSERT_EQ(1, butil::NetToHost32(*tmp));
    bthread_id_join(cntl.call_id());

    ASSERT_EQ(ERPCTIMEDOUT, cntl.ErrorCode());
}

TEST_F(RdmaTest, server_close_after_ack) {
    butil::fd_guard sockfd(butil::tcp_listen(g_ep));
    EXPECT_TRUE(sockfd >= 0);

    Channel channel;
    ChannelOptions chan_options;
    chan_options.socket_mode = SOCKET_MODE_RDMA;
    chan_options.connect_timeout_ms = 500;
    chan_options.timeout_ms = 500;
    chan_options.max_retry = 0;
    ASSERT_EQ(0, channel.Init(g_ep, &chan_options));

    Controller cntl;
    test::EchoRequest req;
    test::EchoResponse res;
    req.set_message(__FUNCTION__);
    google::protobuf::Closure* done = DoNothing();
    ::test::EchoService::Stub(&channel).Echo(&cntl, &req, &res, done);

    SocketUniquePtr s;
    ASSERT_EQ(0, Socket::Address(cntl._single_server_id, &s));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::C_HELLO_WAIT, RdmaTransportOf(s));

    butil::fd_guard acc_fd(accept(sockfd, nullptr, nullptr));
    ASSERT_TRUE(acc_fd >= 0);
    uint8_t data[rdma::HELLO_V2_MSG_LEN_MIN];
    ASSERT_EQ(rdma::HELLO_V2_MSG_LEN_MIN, read(acc_fd, data, rdma::HELLO_V2_MSG_LEN_MIN));

    rdma::v2_wire::HelloMessage msg{};
    msg.msg_len = rdma::HELLO_V2_MSG_LEN_MIN;
    msg.hello_ver = rdma::HELLO_V2_VERSION;
    msg.impl_ver = rdma::IMPL_V2_VERSION;
    msg.sq_size = 16;
    msg.rq_size = 16;
    msg.block_size = 8192;
    msg.qp_num = 0;
    msg.gid = rdma::GetRdmaGid();
    memcpy(data, "RDMA", 4);
    msg.Serialize(data + 4);
    ASSERT_EQ(rdma::HELLO_V2_MSG_LEN_MIN, write(acc_fd, data, rdma::HELLO_V2_MSG_LEN_MIN));

    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::ESTABLISHED, RdmaTransportOf(s));
    ASSERT_EQ(4, read(acc_fd, data, 4));
    uint32_t* tmp = (uint32_t*)data;
    ASSERT_EQ(1, butil::NetToHost32(*tmp));
    close(acc_fd);
    bthread_id_join(cntl.call_id());

    ASSERT_EQ(EEOF, cntl.ErrorCode());
}

TEST_F(RdmaTest, server_send_data_on_tcp_after_ack) {
    butil::fd_guard sockfd(butil::tcp_listen(g_ep));
    EXPECT_TRUE(sockfd >= 0);

    Channel channel;
    ChannelOptions chan_options;
    chan_options.socket_mode = SOCKET_MODE_RDMA;
    chan_options.connect_timeout_ms = 500;
    chan_options.timeout_ms = 500;
    chan_options.max_retry = 0;
    ASSERT_EQ(0, channel.Init(g_ep, &chan_options));

    Controller cntl;
    test::EchoRequest req;
    test::EchoResponse res;
    req.set_message(__FUNCTION__);
    google::protobuf::Closure* done = DoNothing();
    ::test::EchoService::Stub(&channel).Echo(&cntl, &req, &res, done);

    SocketUniquePtr s;
    ASSERT_EQ(0, Socket::Address(cntl._single_server_id, &s));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::C_HELLO_WAIT, RdmaTransportOf(s));

    butil::fd_guard acc_fd(accept(sockfd, nullptr, nullptr));
    ASSERT_TRUE(acc_fd >= 0);
    uint8_t data[rdma::HELLO_V2_MSG_LEN_MIN];
    ASSERT_EQ(rdma::HELLO_V2_MSG_LEN_MIN, read(acc_fd, data, rdma::HELLO_V2_MSG_LEN_MIN));

    rdma::v2_wire::HelloMessage msg{};
    msg.msg_len = rdma::HELLO_V2_MSG_LEN_MIN;
    msg.hello_ver = rdma::HELLO_V2_VERSION;
    msg.impl_ver = rdma::IMPL_V2_VERSION;
    msg.sq_size = 16;
    msg.rq_size = 16;
    msg.block_size = 8192;
    msg.qp_num = 0;
    msg.gid = rdma::GetRdmaGid();
    memcpy(data, "RDMA", 4);
    msg.Serialize(data + 4);
    ASSERT_EQ(rdma::HELLO_V2_MSG_LEN_MIN, write(acc_fd, data, rdma::HELLO_V2_MSG_LEN_MIN));

    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::ESTABLISHED, RdmaTransportOf(s));
    ASSERT_EQ(rdma::HELLO_V2_MSG_LEN_MIN, write(acc_fd, data, rdma::HELLO_V2_MSG_LEN_MIN));
    bthread_id_join(cntl.call_id());

    ASSERT_EQ(EPROTO, cntl.ErrorCode());
}


TEST_F(RdmaTest, v2_client_hello_bytes_baseline) {
    butil::fd_guard sockfd(butil::tcp_listen(g_ep));
    EXPECT_TRUE(sockfd >= 0);

    Channel channel;
    ChannelOptions chan_options;
    chan_options.socket_mode = SOCKET_MODE_RDMA;
    chan_options.connect_timeout_ms = 500;
    chan_options.timeout_ms = 500;
    chan_options.max_retry = 0;
    ASSERT_EQ(0, channel.Init(g_ep, &chan_options));

    Controller cntl;
    test::EchoRequest req;
    test::EchoResponse res;
    req.set_message(__FUNCTION__);
    google::protobuf::Closure* done = DoNothing();
    ::test::EchoService::Stub(&channel).Echo(&cntl, &req, &res, done);

    SocketUniquePtr s;
    ASSERT_EQ(0, Socket::Address(cntl._single_server_id, &s));

    butil::fd_guard acc_fd(accept(sockfd, nullptr, nullptr));
    ASSERT_TRUE(acc_fd >= 0);

    uint8_t data[rdma::HELLO_V2_MSG_LEN_MIN];
    ASSERT_EQ(rdma::HELLO_V2_MSG_LEN_MIN, read(acc_fd, data, rdma::HELLO_V2_MSG_LEN_MIN));

    // [0..4) magic
    ASSERT_EQ(0, memcmp(data, "RDMA", 4));
    // [4..6) msg_len, big-endian uint16 == 40
    ASSERT_EQ(rdma::HELLO_V2_MSG_LEN_MIN,
              (size_t)(((uint16_t)data[4] << 8) | (uint16_t)data[5]));
    // [6..8) hello_ver, big-endian uint16 == rdma::HELLO_V2_VERSION
    ASSERT_EQ(rdma::HELLO_V2_VERSION,
              (uint16_t)(((uint16_t)data[6] << 8) | (uint16_t)data[7]));
    // [8..10) impl_ver, big-endian uint16 == rdma::IMPL_V2_VERSION
    ASSERT_EQ(rdma::IMPL_V2_VERSION,
              (uint16_t)(((uint16_t)data[8] << 8) | (uint16_t)data[9]));

    rdma::v2_wire::HelloMessage msg{};
    msg.Deserialize(data + 4);
    ASSERT_EQ(rdma::HELLO_V2_MSG_LEN_MIN, msg.msg_len);
    ASSERT_EQ(rdma::HELLO_V2_VERSION, msg.hello_ver);
    ASSERT_EQ(rdma::IMPL_V2_VERSION,  msg.impl_ver);

    bthread_id_join(cntl.call_id());
}

TEST_F(RdmaTest, v2_server_hello_bytes_baseline) {
    StartServer();

    sockaddr_in addr;
    bzero((char*)&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);

    butil::fd_guard sockfd(socket(AF_INET, SOCK_STREAM, 0));
    ASSERT_TRUE(sockfd >= 0);
    ASSERT_EQ(0, connect(sockfd, (sockaddr*)&addr, sizeof(sockaddr)));
    Socket* s = WaitForServerSocket();
    ASSERT_EQ(rdma::RdmaEndpoint::UNINIT, RdmaTransportOf(s)->_rdma_ep->_state);

    // Send a well-formed v2 hello so the server enters S_ACK_WAIT.
    rdma::v2_wire::HelloMessage msg{};
    msg.msg_len = rdma::HELLO_V2_MSG_LEN_MIN;
    msg.hello_ver = rdma::HELLO_V2_VERSION;
    msg.impl_ver = rdma::IMPL_V2_VERSION;
    msg.sq_size = 16;
    msg.rq_size = 16;
    msg.block_size = 8192;
    msg.qp_num = 0;
    msg.gid = rdma::GetRdmaGid();

    uint8_t data[rdma::HELLO_V2_MSG_LEN_MIN];
    memcpy(data, "RDMA", 4);
    msg.Serialize(data + 4);
    ASSERT_EQ(rdma::HELLO_V2_MSG_LEN_MIN, write(sockfd, data, rdma::HELLO_V2_MSG_LEN_MIN));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::S_ACK_WAIT, RdmaTransportOf(s));

    // Read server's reply hello and assert its byte-level layout.
    uint8_t reply[rdma::HELLO_V2_MSG_LEN_MIN];
    ASSERT_EQ(rdma::HELLO_V2_MSG_LEN_MIN, read(sockfd, reply, rdma::HELLO_V2_MSG_LEN_MIN));

    ASSERT_EQ(0, memcmp(reply, "RDMA", 4));
    ASSERT_EQ(rdma::HELLO_V2_MSG_LEN_MIN,
              (size_t)(((uint16_t)reply[4] << 8) | (uint16_t)reply[5]));
    ASSERT_EQ(rdma::HELLO_V2_VERSION,
              (uint16_t)(((uint16_t)reply[6] << 8) | (uint16_t)reply[7]));
    ASSERT_EQ(rdma::IMPL_V2_VERSION,
              (uint16_t)(((uint16_t)reply[8] << 8) | (uint16_t)reply[9]));

    rdma::v2_wire::HelloMessage reply_msg{};
    reply_msg.Deserialize(reply + 4);
    ASSERT_EQ(rdma::HELLO_V2_MSG_LEN_MIN, reply_msg.msg_len);
    ASSERT_EQ(rdma::HELLO_V2_VERSION, reply_msg.hello_ver);
    ASSERT_EQ(rdma::IMPL_V2_VERSION,  reply_msg.impl_ver);

    // Drive the server into FALLBACK_TCP via ACK flags=0 so the test ends
    // cleanly without requiring real RDMA hardware.
    uint32_t flags = butil::HostToNet32(0);
    ASSERT_EQ(sizeof(flags), write(sockfd, &flags, sizeof(flags)));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::FALLBACK_TCP, RdmaTransportOf(s));

    sockfd.reset(-1);
    ASSERT_TRUE(WaitForServerSocketGone());

    StopServer();
}

TEST_F(RdmaTest, v2_server_drains_tail_then_reads_ack) {
    StartServer();

    sockaddr_in addr;
    bzero((char*)&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    butil::fd_guard sockfd(socket(AF_INET, SOCK_STREAM, 0));
    ASSERT_TRUE(sockfd >= 0);
    ASSERT_EQ(0, connect(sockfd, (sockaddr*)&addr, sizeof(sockaddr)));
    Socket* s = WaitForServerSocket();
    ASSERT_TRUE(s != nullptr);
    ASSERT_EQ(rdma::RdmaEndpoint::UNINIT, RdmaTransportOf(s)->_rdma_ep->_state);

    // Build a v2 hello with msg_len = 48 (40 base + 8B zero tail).
    rdma::v2_wire::HelloMessage msg{};
    msg.msg_len = 48;
    msg.hello_ver = rdma::HELLO_V2_VERSION;
    msg.impl_ver = rdma::IMPL_V2_VERSION;
    msg.sq_size = 16;
    msg.rq_size = 16;
    msg.block_size = 8192;
    msg.qp_num = 0;
    msg.gid = rdma::GetRdmaGid();

    uint8_t buf[48];
    memcpy(buf, "RDMA", 4);
    msg.Serialize(buf + 4);
    memset(buf + 40, 0x00, 8);  // 8B zero tail
    ASSERT_TRUE(WriteAll(sockfd, buf, 48));
    // The tail is drained as part of the hello, so the server ends up waiting
    // for the ACK. Wait for that before sending it, otherwise the ACK could
    // ride along in the same read and this would no longer test the drain.
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::S_ACK_WAIT, RdmaTransportOf(s));

    // Send the real ACK (flags=1 = ACK_MSG_RDMA_OK).
    uint32_t flags = butil::HostToNet32(1);
    ASSERT_EQ(sizeof(flags), write(sockfd, &flags, sizeof(flags)));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::ESTABLISHED, RdmaTransportOf(s));

    sockfd.reset(-1);
    ASSERT_TRUE(WaitForServerSocketGone());

    StopServer();
}

TEST_F(RdmaTest, v2_server_rejects_oversized_msg_len) {
    StartServer();

    sockaddr_in addr;
    bzero((char*)&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    butil::fd_guard sockfd(socket(AF_INET, SOCK_STREAM, 0));
    ASSERT_TRUE(sockfd >= 0);
    ASSERT_EQ(0, connect(sockfd, (sockaddr*)&addr, sizeof(sockaddr)));
    Socket* s = WaitForServerSocket();
    ASSERT_TRUE(s != nullptr);
    ASSERT_EQ(rdma::RdmaEndpoint::UNINIT, RdmaTransportOf(s)->_rdma_ep->_state);

    // Build a v2 hello with msg_len = 4097 (HELLO_V2_MSG_LEN_MAX + 1).
    // We only send the 40B base; the server must reject before reading
    // (and definitely before attempting to drain) any "tail".
    rdma::v2_wire::HelloMessage msg{};
    msg.msg_len = 4097;
    msg.hello_ver = rdma::HELLO_V2_VERSION;
    msg.impl_ver = rdma::IMPL_V2_VERSION;
    msg.sq_size = 16;
    msg.rq_size = 16;
    msg.block_size = 8192;
    msg.qp_num = 0;
    msg.gid = rdma::GetRdmaGid();

    uint8_t buf[rdma::HELLO_V2_MSG_LEN_MIN];
    memcpy(buf, "RDMA", 4);
    msg.Serialize(buf + 4);
    ASSERT_EQ(rdma::HELLO_V2_MSG_LEN_MIN, write(sockfd, buf, rdma::HELLO_V2_MSG_LEN_MIN));
    ASSERT_TRUE(WaitForServerSocketGone());

    sockfd.reset(-1);

    StopServer();
}

// RAII for FLAGS_rdma_client_handshake_version: lets us flip the
// client-side handshake version for a single test and restore it on
// scope exit so subsequent tests stay on the v2 default.
class HandshakeVersionFlag {
public:
    explicit HandshakeVersionFlag(int v)
        : _saved(rdma::FLAGS_rdma_client_handshake_version) {
        rdma::FLAGS_rdma_client_handshake_version = v;
    }
    ~HandshakeVersionFlag() {
        rdma::FLAGS_rdma_client_handshake_version = _saved;
    }
private:
    int _saved;
};

// Build a v3 wire packet from an RdmaHello: "RDM3" + pb_size_be + body.
std::string MakeV3Packet(const rdma::RdmaHello& msg) {
    std::string body;
    EXPECT_TRUE(msg.SerializeToString(&body));
    std::string packet;
    packet.reserve(4 + 4 + body.size());
    packet.append("RDM3", 4);
    uint32_t pb_size_be =
        butil::HostToNet32(static_cast<uint32_t>(body.size()));
    packet.append(reinterpret_cast<const char*>(&pb_size_be), 4);
    packet.append(body);
    return packet;
}

// Build a fully-valid RdmaHello: all 6 required fields are set, with
// values that pass RdmaHelloV3Wire::RdmaHelloValid().
//   - block_size = 8192 (>= MIN_BLOCK_SIZE)
//   - sq_size / rq_size = 16 (>= MIN_QP_SIZE)
//   - gid = exactly 16B (sizeof(ibv_gid))
//   - qp_num = 0  (allowed because g_skip_rdma_init in UT)
rdma::RdmaHello MakeValidV3Hello() {
    rdma::RdmaHello msg;
    msg.set_block_size(8192);
    msg.set_sq_size(16);
    msg.set_rq_size(16);
    msg.set_lid(0);
    ibv_gid gid = rdma::GetRdmaGid();
    msg.set_gid(std::string(reinterpret_cast<const char*>(gid.raw),
                            sizeof(gid.raw)));
    msg.set_qp_num(0);
    return msg;
}

// Inverse of MakeV3Packet(): read one whole v3 hello off `fd'. Uses ASSERT_*,
// so it is void and callers need ASSERT_NO_FATAL_FAILURE.
static void ReadV3Hello(int fd, rdma::RdmaHello* out) {
    uint8_t magic[rdma::HELLO_MAGIC_LEN];
    ASSERT_TRUE(ReadAll(fd, magic, sizeof(magic)));
    ASSERT_EQ(0, memcmp(magic, "RDM3", sizeof(magic)));
    uint8_t size_buf[rdma::HELLO_V3_PB_SIZE_LEN];
    ASSERT_TRUE(ReadAll(fd, size_buf, sizeof(size_buf)));
    const uint32_t pb_size =
        butil::NetToHost32(*reinterpret_cast<uint32_t*>(size_buf));
    ASSERT_GT(pb_size, 0u);
    ASSERT_LE(pb_size, 4096u);
    std::string body(pb_size, '\0');
    ASSERT_TRUE(ReadAll(fd, &body[0], pb_size));
    ASSERT_TRUE(out->ParseFromString(body));
}

TEST_F(RdmaTest, v3_client_hello_bytes_baseline) {
    HandshakeVersionFlag _hsv(3);

    butil::fd_guard sockfd(butil::tcp_listen(g_ep));
    EXPECT_TRUE(sockfd >= 0);

    Channel channel;
    ChannelOptions chan_options;
    chan_options.socket_mode = SOCKET_MODE_RDMA;
    chan_options.connect_timeout_ms = 500;
    chan_options.timeout_ms = 500;
    chan_options.max_retry = 0;
    ASSERT_EQ(0, channel.Init(g_ep, &chan_options));

    Controller cntl;
    test::EchoRequest req;
    test::EchoResponse res;
    req.set_message(__FUNCTION__);
    google::protobuf::Closure* done = DoNothing();
    ::test::EchoService::Stub(&channel).Echo(&cntl, &req, &res, done);

    butil::fd_guard acc_fd(accept(sockfd, nullptr, nullptr));
    ASSERT_TRUE(acc_fd >= 0);

    // [0..4) magic "RDM3"
    uint8_t magic[4];
    ASSERT_EQ(4, read(acc_fd, magic, 4));
    ASSERT_EQ(0, memcmp(magic, "RDM3", 4));

    // [4..8) pb_size, big-endian uint32, must be in (0, 4096]
    uint8_t size_buf[4];
    ASSERT_EQ(4, read(acc_fd, size_buf, 4));
    uint32_t pb_size =
        butil::NetToHost32(*reinterpret_cast<uint32_t*>(size_buf));
    ASSERT_GT(pb_size, 0u);
    ASSERT_LE(pb_size, 4096u);

    // [8..8+pb_size) RdmaHello protobuf body.
    std::string body(pb_size, '\0');
    ASSERT_EQ((ssize_t)pb_size, read(acc_fd, &body[0], pb_size));
    rdma::RdmaHello msg;
    ASSERT_TRUE(msg.ParseFromString(body));

    // All 6 required fields must be present (ParseFromString would
    // have already returned false otherwise).
    ASSERT_TRUE(msg.has_block_size());
    ASSERT_TRUE(msg.has_sq_size());
    ASSERT_TRUE(msg.has_rq_size());
    ASSERT_TRUE(msg.has_lid());
    ASSERT_TRUE(msg.has_gid());
    ASSERT_TRUE(msg.has_qp_num());
    // gid wire encoding must be exactly 16 bytes (sizeof(ibv_gid)).
    ASSERT_EQ(sizeof(ibv_gid), msg.gid().size());

    // Let the RPC time out and release resources.
    bthread_id_join(cntl.call_id());
}

TEST_F(RdmaTest, v3_server_hello_bytes_baseline) {
    StartServer();

    sockaddr_in addr;
    bzero((char*)&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);

    butil::fd_guard sockfd(socket(AF_INET, SOCK_STREAM, 0));
    ASSERT_TRUE(sockfd >= 0);
    ASSERT_EQ(0, connect(sockfd, (sockaddr*)&addr, sizeof(sockaddr)));
    Socket* s = WaitForServerSocket();
    ASSERT_EQ(rdma::RdmaEndpoint::UNINIT, RdmaTransportOf(s)->_rdma_ep->_state);

    // Send a valid v3 hello.
    std::string packet = MakeV3Packet(MakeValidV3Hello());
    ASSERT_EQ((ssize_t)packet.size(),
              write(sockfd, packet.data(), packet.size()));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::S_ACK_WAIT, RdmaTransportOf(s));

    // Read server's reply hello: 4B magic + 4B pb_size + body.
    uint8_t reply_magic[4];
    ASSERT_EQ(4, read(sockfd, reply_magic, 4));
    ASSERT_EQ(0, memcmp(reply_magic, "RDM3", 4));

    uint8_t size_buf[4];
    ASSERT_EQ(4, read(sockfd, size_buf, 4));
    uint32_t pb_size =
        butil::NetToHost32(*reinterpret_cast<uint32_t*>(size_buf));
    ASSERT_GT(pb_size, 0u);
    ASSERT_LE(pb_size, 4096u);

    std::string body(pb_size, '\0');
    ASSERT_EQ((ssize_t)pb_size, read(sockfd, &body[0], pb_size));
    rdma::RdmaHello reply;
    ASSERT_TRUE(reply.ParseFromString(body));
    ASSERT_TRUE(reply.has_block_size());
    ASSERT_TRUE(reply.has_sq_size());
    ASSERT_TRUE(reply.has_rq_size());
    ASSERT_TRUE(reply.has_gid());
    ASSERT_EQ(sizeof(ibv_gid), reply.gid().size());

    // Drive the server into FALLBACK_TCP via ACK flags=0 so the test ends
    // cleanly without requiring real RDMA hardware.
    uint32_t flags = butil::HostToNet32(0);
    ASSERT_EQ((ssize_t)sizeof(flags),
              write(sockfd, &flags, sizeof(flags)));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::FALLBACK_TCP, RdmaTransportOf(s));

    sockfd.reset(-1);
    ASSERT_TRUE(WaitForServerSocketGone());

    StopServer();
}

TEST_F(RdmaTest, v3_server_rejects_zero_pb_size) {
    StartServer();

    sockaddr_in addr;
    bzero((char*)&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    butil::fd_guard sockfd(socket(AF_INET, SOCK_STREAM, 0));
    ASSERT_TRUE(sockfd >= 0);
    ASSERT_EQ(0, connect(sockfd, (sockaddr*)&addr, sizeof(sockaddr)));
    Socket* s = WaitForServerSocket();
    ASSERT_TRUE(s != nullptr);

    // "RDM3" + pb_size = 0 (4B big-endian zero).
    uint8_t buf[8] = {'R', 'D', 'M', '3', 0, 0, 0, 0};
    ASSERT_EQ(8, write(sockfd, buf, 8));
    ASSERT_TRUE(WaitForServerSocketGone());

    sockfd.reset(-1);
    StopServer();
}

TEST_F(RdmaTest, v3_server_rejects_oversized_pb_size) {
    StartServer();

    sockaddr_in addr;
    bzero((char*)&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    butil::fd_guard sockfd(socket(AF_INET, SOCK_STREAM, 0));
    ASSERT_TRUE(sockfd >= 0);
    ASSERT_EQ(0, connect(sockfd, (sockaddr*)&addr, sizeof(sockaddr)));
    Socket* s = WaitForServerSocket();
    ASSERT_TRUE(s != nullptr);

    uint8_t buf[8];
    memcpy(buf, "RDM3", 4);
    // pb_size just above the allowed maximum -> rejected.
    uint32_t pb_size_be =
        butil::HostToNet32(static_cast<uint32_t>(rdma::HELLO_V3_MAX_PB_SIZE + 1));
    memcpy(buf + 4, &pb_size_be, 4);
    ASSERT_EQ(8, write(sockfd, buf, 8));
    ASSERT_TRUE(WaitForServerSocketGone());

    sockfd.reset(-1);
    StopServer();
}

TEST_F(RdmaTest, v3_server_rejects_invalid_pb_bytes) {
    StartServer();

    sockaddr_in addr;
    bzero((char*)&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    butil::fd_guard sockfd(socket(AF_INET, SOCK_STREAM, 0));
    ASSERT_TRUE(sockfd >= 0);
    ASSERT_EQ(0, connect(sockfd, (sockaddr*)&addr, sizeof(sockaddr)));
    Socket* s = WaitForServerSocket();
    ASSERT_TRUE(s != nullptr);

    // "RDM3" + pb_size = 8 + 8 bytes of 0xff (invalid protobuf body).
    uint8_t buf[16];
    memcpy(buf, "RDM3", 4);
    uint32_t pb_size_be = butil::HostToNet32(8);
    memcpy(buf + 4, &pb_size_be, 4);
    memset(buf + 8, 0xff, 8);
    ASSERT_EQ(16, write(sockfd, buf, 16));
    ASSERT_TRUE(WaitForServerSocketGone());

    sockfd.reset(-1);
    StopServer();
}

TEST_F(RdmaTest, v3_server_invalid_sq_size_falls_back) {
    StartServer();

    sockaddr_in addr;
    bzero((char*)&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    butil::fd_guard sockfd(socket(AF_INET, SOCK_STREAM, 0));
    ASSERT_TRUE(sockfd >= 0);
    ASSERT_EQ(0, connect(sockfd, (sockaddr*)&addr, sizeof(sockaddr)));
    Socket* s = WaitForServerSocket();
    ASSERT_TRUE(s != nullptr);

    rdma::RdmaHello msg = MakeValidV3Hello();
    msg.set_sq_size(0);  // invalid: < MIN_QP_SIZE (16)
    std::string packet = MakeV3Packet(msg);
    ASSERT_TRUE(WriteAll(sockfd, packet.data(), packet.size()));

    // Server validated the hello as invalid -> _rdma_state = RDMA_OFF,
    // but still proceeds to S_ACK_WAIT (sends its own reply hello).
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::S_ACK_WAIT, RdmaTransportOf(s));
    ASSERT_EQ(RdmaTransport::RDMA_OFF, RdmaTransportOf(s)->_rdma_state);

    // Drain server's reply hello (content not asserted here; covered
    // by v3_server_hello_bytes_baseline).
    uint8_t reply_hdr[8];
    ASSERT_EQ(8, read(sockfd, reply_hdr, 8));
    ASSERT_EQ(0, memcmp(reply_hdr, "RDM3", 4));
    uint32_t reply_pb_size = butil::NetToHost32(
            *reinterpret_cast<uint32_t*>(reply_hdr + 4));
    std::string reply_body(reply_pb_size, '\0');
    ASSERT_EQ((ssize_t)reply_pb_size,
              read(sockfd, &reply_body[0], reply_pb_size));

    // Client ACK flags=0 -> server settles into FALLBACK_TCP.
    uint32_t flags = butil::HostToNet32(0);
    ASSERT_EQ((ssize_t)sizeof(flags),
              write(sockfd, &flags, sizeof(flags)));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::FALLBACK_TCP, RdmaTransportOf(s));

    sockfd.reset(-1);
    ASSERT_TRUE(WaitForServerSocketGone());

    StopServer();
}

// RAII guard to toggle FLAGS_rdma_ece for a single test and restore it.
class EceFlagGuard {
public:
    explicit EceFlagGuard(bool v) : _saved(rdma::FLAGS_rdma_ece) {
        rdma::FLAGS_rdma_ece = v;
    }
    ~EceFlagGuard() {
        rdma::FLAGS_rdma_ece = _saved;
    }
private:
    bool _saved;
};

// Build a valid v3 hello that also carries an ECE block.
rdma::RdmaHello MakeValidV3HelloWithEce(uint32_t vendor_id,
                                        uint32_t options,
                                        uint32_t comp_mask) {
    rdma::RdmaHello msg = MakeValidV3Hello();
    rdma::RdmaEce* ece = msg.mutable_ece();
    ece->set_vendor_id(vendor_id);
    ece->set_options(options);
    ece->set_comp_mask(comp_mask);
    return msg;
}

// Read the server's v3 reply hello (4B magic + 4B pb_size + body) and parse
// it into `reply`. Asserts the framing along the way.
static void ReadServerV3Reply(int fd, rdma::RdmaHello* reply) {
    uint8_t reply_hdr[8];
    ASSERT_EQ(8, read(fd, reply_hdr, 8));
    ASSERT_EQ(0, memcmp(reply_hdr, "RDM3", 4));
    uint32_t reply_pb_size = butil::NetToHost32(*reinterpret_cast<uint32_t*>(reply_hdr + 4));
    ASSERT_GT(reply_pb_size, 0u);
    ASSERT_LE(reply_pb_size, 4096u);
    std::string reply_body(reply_pb_size, '\0');
    ASSERT_EQ((ssize_t)reply_pb_size,
              read(fd, &reply_body[0], reply_pb_size));
    ASSERT_TRUE(reply->ParseFromString(reply_body));
}

// A client hello carrying ECE must not break the server handshake: with ECE
// enabled the server still parses the hello and advances to S_ACK_WAIT.
TEST_F(RdmaTest, v3_server_accepts_client_hello_with_ece) {
    EceFlagGuard ece_flag_guard(true);
    StartServer();

    sockaddr_in addr;
    bzero((char*)&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    butil::fd_guard sockfd(socket(AF_INET, SOCK_STREAM, 0));
    ASSERT_TRUE(sockfd >= 0);
    ASSERT_EQ(0, connect(sockfd, (sockaddr*)&addr, sizeof(sockaddr)));
    Socket* s = WaitForServerSocket();
    ASSERT_TRUE(s != nullptr);

    rdma::RdmaHello msg = MakeValidV3HelloWithEce(0x02c9, 0x1, 0x0);
    std::string packet = MakeV3Packet(msg);
    ASSERT_EQ((ssize_t)packet.size(),
              write(sockfd, packet.data(), packet.size()));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::S_ACK_WAIT, RdmaTransportOf(s));

    rdma::RdmaHello reply;
    ReadServerV3Reply(sockfd, &reply);

    // ACK flags=0 -> clean FALLBACK_TCP so the test ends without hardware.
    uint32_t flags = butil::HostToNet32(0);
    ASSERT_EQ((ssize_t)sizeof(flags), write(sockfd, &flags, sizeof(flags)));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::FALLBACK_TCP, RdmaTransportOf(s));

    sockfd.reset(-1);
    ASSERT_TRUE(WaitForServerSocketGone());
    StopServer();
}

// When ECE negotiation is disabled, the server reply must NOT advertise ECE,
// even if the client advertised it (FillLocalRdmaHello degrade branch #1).
TEST_F(RdmaTest, v3_server_reply_has_no_ece_when_disabled) {
    EceFlagGuard ece_flag_guard(false);
    StartServer();

    sockaddr_in addr;
    bzero((char*)&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    butil::fd_guard sockfd(socket(AF_INET, SOCK_STREAM, 0));
    ASSERT_TRUE(sockfd >= 0);
    ASSERT_EQ(0, connect(sockfd, (sockaddr*)&addr, sizeof(sockaddr)));
    Socket* s = WaitForServerSocket();
    ASSERT_TRUE(s != nullptr);

    rdma::RdmaHello msg = MakeValidV3HelloWithEce(0x02c9, 0x1, 0x0);
    std::string packet = MakeV3Packet(msg);
    ASSERT_TRUE(WriteAll(sockfd, packet.data(), packet.size()));

    // Reading the reply in full doubles as the synchronization point.
    rdma::RdmaHello reply;
    ReadServerV3Reply(sockfd, &reply);
    EXPECT_FALSE(reply.has_ece());

    uint32_t flags = butil::HostToNet32(0);
    ASSERT_TRUE(WriteAll(sockfd, &flags, sizeof(flags)));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::FALLBACK_TCP, RdmaTransportOf(s));

    sockfd.reset(-1);
    ASSERT_TRUE(WaitForServerSocketGone());
    StopServer();
}

// When ECE is enabled but there is no negotiated result (UT skips the real QP
// bring-up, so the server never fills _outgoing_ece), the server reply must
// still NOT advertise ECE (FillLocalRdmaHello degrade branch #2 -> degrade-safe).
TEST_F(RdmaTest, v3_server_reply_has_no_ece_without_hw_negotiation) {
    EceFlagGuard ece_flag_guard(true);
    StartServer();

    sockaddr_in addr;
    bzero((char*)&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    butil::fd_guard sockfd(socket(AF_INET, SOCK_STREAM, 0));
    ASSERT_TRUE(sockfd >= 0);
    ASSERT_EQ(0, connect(sockfd, (sockaddr*)&addr, sizeof(sockaddr)));
    Socket* s = WaitForServerSocket();
    ASSERT_TRUE(s != nullptr);

    rdma::RdmaHello msg = MakeValidV3HelloWithEce(0x02c9, 0x1, 0x0);
    std::string packet = MakeV3Packet(msg);
    ASSERT_TRUE(WriteAll(sockfd, packet.data(), packet.size()));

    // Reading the reply in full doubles as the synchronization point.
    rdma::RdmaHello reply;
    ReadServerV3Reply(sockfd, &reply);
    EXPECT_FALSE(reply.has_ece());

    uint32_t flags = butil::HostToNet32(0);
    ASSERT_TRUE(WriteAll(sockfd, &flags, sizeof(flags)));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::FALLBACK_TCP, RdmaTransportOf(s));

    sockfd.reset(-1);
    ASSERT_TRUE(WaitForServerSocketGone());
    StopServer();
}

class ResourceAllocFailGuard {
public:
    explicit ResourceAllocFailGuard(bool v)
        : _saved(rdma::g_fail_resource_alloc_for_test) {
        rdma::g_fail_resource_alloc_for_test = v;
    }
    ~ResourceAllocFailGuard() {
        rdma::g_fail_resource_alloc_for_test = _saved;
    }
private:
    bool _saved;
};

TEST_F(RdmaTest, client_alloc_resource_fail_fallback_tcp) {
    StartServer();
    ResourceAllocFailGuard alloc_fail_guard(true);

    Channel channel;
    ChannelOptions chan_options;
    chan_options.socket_mode = SOCKET_MODE_RDMA;
    chan_options.connect_timeout_ms = 500;
    chan_options.timeout_ms = 500;
    chan_options.max_retry = 0;
    ASSERT_EQ(0, channel.Init(g_ep, &chan_options));

    Controller cntl;
    test::EchoRequest req;
    test::EchoResponse res;
    req.set_message(__FUNCTION__);
    req.set_sleep_us(200000);
    google::protobuf::Closure* done = DoNothing();
    ::test::EchoService::Stub(&channel).Echo(&cntl, &req, &res, done);

    SocketUniquePtr s;
    ASSERT_EQ(0, Socket::Address(cntl._single_server_id, &s));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::FALLBACK_TCP, RdmaTransportOf(s));
    ASSERT_EQ(RdmaTransport::RDMA_OFF, RdmaTransportOf(s)->_rdma_state);
    // The socket must not be failed, otherwise it can no longer carry TCP.
    ASSERT_FALSE(s->Failed());

    // The RPC still completes over TCP.
    bthread_id_join(cntl.call_id());
    ASSERT_EQ(0, cntl.ErrorCode()) << cntl.ErrorText();

    StopServer();
}

TEST_F(RdmaTest, server_alloc_resource_fail_fallback_tcp) {
    StartServer();
    ResourceAllocFailGuard alloc_fail_guard(true);

    sockaddr_in addr;
    bzero((char*)&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    butil::fd_guard sockfd(socket(AF_INET, SOCK_STREAM, 0));
    ASSERT_TRUE(sockfd >= 0);
    ASSERT_EQ(0, connect(sockfd, (sockaddr*)&addr, sizeof(sockaddr)));
    Socket* s = WaitForServerSocket();
    ASSERT_TRUE(s != nullptr);
    ASSERT_EQ(rdma::RdmaEndpoint::UNINIT, RdmaTransportOf(s)->_rdma_ep->_state);

    // Send a well-formed v2 hello: the negotiation succeeds
    // but the resource allocation does not.
    rdma::v2_wire::HelloMessage msg{};
    msg.msg_len = rdma::HELLO_V2_MSG_LEN_MIN;
    msg.hello_ver = rdma::HELLO_V2_VERSION;
    msg.impl_ver = rdma::IMPL_V2_VERSION;
    msg.sq_size = 16;
    msg.rq_size = 16;
    msg.block_size = 8192;
    msg.qp_num = 0;
    msg.gid = rdma::GetRdmaGid();

    uint8_t data[rdma::HELLO_V2_MSG_LEN_MIN];
    memcpy(data, "RDMA", 4);
    msg.Serialize(data + 4);
    ASSERT_EQ(rdma::HELLO_V2_MSG_LEN_MIN,
              write(sockfd, data, rdma::HELLO_V2_MSG_LEN_MIN));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::S_ACK_WAIT, RdmaTransportOf(s));
    ASSERT_EQ(RdmaTransport::RDMA_OFF, RdmaTransportOf(s)->_rdma_state);
    ASSERT_FALSE(s->Failed());

    // Ack without RDMA so that the server finishes the handshake in TCP mode.
    uint32_t flags = butil::HostToNet32(0);
    ASSERT_EQ(sizeof(flags), write(sockfd, &flags, sizeof(flags)));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::FALLBACK_TCP, RdmaTransportOf(s));
    ASSERT_FALSE(s->Failed());

    sockfd.reset(-1);
    ASSERT_TRUE(WaitForServerSocketGone());

    StopServer();
}

TEST_F(RdmaTest, try_global_disable_rdma) {
    StartServer();
    rdma::g_rdma_available.store(false, butil::memory_order_relaxed);

    Channel channel;
    ChannelOptions chan_options;
    chan_options.socket_mode = SOCKET_MODE_RDMA;
    chan_options.connect_timeout_ms = 500;
    chan_options.timeout_ms = 500;
    chan_options.max_retry = 0;
    ASSERT_EQ(0, channel.Init(g_ep, &chan_options));
    Controller cntl;
    test::EchoRequest req;
    test::EchoResponse res;

    req.set_message(__FUNCTION__);
    req.set_sleep_us(200000);
    google::protobuf::Closure* done = DoNothing();
    ::test::EchoService::Stub(&channel).Echo(&cntl, &req, &res, done);
    SocketUniquePtr s;
    ASSERT_EQ(0, Socket::Address(cntl._single_server_id, &s));
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::FALLBACK_TCP, RdmaTransportOf(s));
    bthread_id_join(cntl.call_id());
    ASSERT_EQ(0, cntl.ErrorCode());

    StopServer();
    rdma::g_rdma_available.store(true, butil::memory_order_relaxed);
}

TEST_F(RdmaTest, server_option_invalid) {
    Server server;
    ServerOptions options;
    options.socket_mode = SOCKET_MODE_RDMA;

    // rtmp and rdma are incompatible
    options.rtmp_service = (RtmpService*)1;
    ASSERT_EQ(-1, server.Start(PORT, &options));

    // nshead and rdma are incompatible
    options.rtmp_service = nullptr;
    options.nshead_service = (NsheadService*)1;
    ASSERT_EQ(-1, server.Start(PORT, &options));

    // mongo and rdma are incompatible
    options.nshead_service = nullptr;
    options.mongo_service_adaptor = (MongoServiceAdaptor*)1;
    ASSERT_EQ(-1, server.Start(PORT, &options));

    // ssl and rdma are incompatible
    options.mongo_service_adaptor = nullptr;
    options.mutable_ssl_options()->default_cert.certificate = "test";
    ASSERT_EQ(-1, server.Start(PORT, &options));
}

TEST_F(RdmaTest, channel_option_invalid) {
    Channel channel;
    ChannelOptions chan_options;
    chan_options.socket_mode = SOCKET_MODE_RDMA;

    // rtmp and rdma are incompatible
    chan_options.protocol = "rtmp";
    ASSERT_EQ(-1, channel.Init(g_ep, &chan_options));

    chan_options.protocol = "streaming_rpc";
    ASSERT_EQ(-1, channel.Init(g_ep, &chan_options));

    // nshead and rdma are incompatible
    chan_options.protocol = "nshead";
    ASSERT_EQ(-1, channel.Init(g_ep, &chan_options));
    chan_options.protocol = "nshead_mcpack";
    ASSERT_EQ(-1, channel.Init(g_ep, &chan_options));

    // nova_pbrpc and rdma are incompatible
    chan_options.protocol = "nova_pbrpc";
    ASSERT_EQ(-1, channel.Init(g_ep, &chan_options));

    // public_pbrpc and rdma are incompatible
    chan_options.protocol = "public_pbrpc";
    ASSERT_EQ(-1, channel.Init(g_ep, &chan_options));

    // redis and rdma are incompatible
    chan_options.protocol = "redis";
    ASSERT_EQ(-1, channel.Init(g_ep, &chan_options));

    // memcache and rdma are incompatible
    chan_options.protocol = "memcache";
    ASSERT_EQ(-1, channel.Init(g_ep, &chan_options));

    // ubrpc and rdma are incompatible
    chan_options.protocol = "ubrpc_compack";
    ASSERT_EQ(-1, channel.Init(g_ep, &chan_options));

    // itp and rdma are incompatible
    chan_options.protocol = "itp";
    ASSERT_EQ(-1, channel.Init(g_ep, &chan_options));

    // esp and rdma are incompatible
    chan_options.protocol = "esp";
    ASSERT_EQ(-1, channel.Init(g_ep, &chan_options));

    // hulu_pbrpc and rdma are incompatible
    chan_options.protocol = "hulu_pbrpc";
    ASSERT_EQ(-1, channel.Init(g_ep, &chan_options));

    // sofa_pbrpc and rdma are incompatible
    chan_options.protocol = "sofa_pbrpc";
    ASSERT_EQ(-1, channel.Init(g_ep, &chan_options));

    // http and rdma are incompatible
    chan_options.protocol = "http";
    ASSERT_EQ(-1, channel.Init(g_ep, &chan_options));

    // ssl and rdma are incompatible
    chan_options.protocol = "baidu_std";
    chan_options.mutable_ssl_options()->sni_name = "test";
    ASSERT_EQ(-1, channel.Init(g_ep, &chan_options));
}

// Rounds, per-round RPC count and attachment sizes shared by the end-to-end
// tests below. One RPC per test leaves everything that only shows up on the
// second message untouched -- buffer reuse, an EOF read racing another writer
// of the same input stream, resource recycling.
static const int E2E_ROUND_NUM = 3;
static const int E2E_RPC_NUM = 32;
static const size_t E2E_ATTACH_SIZE[] = { 0, 4096, 128 * 1024 };

static void ShutdownClientConnection(Controller& cntl) {
    SocketUniquePtr s;
    if (Socket::Address(cntl._single_server_id, &s) == 0) {
        ::shutdown(s->fd(), SHUT_WR);
    }
}

// Returns the number of RPCs that succeeded. A test that severs the connection
// cannot predict which ones make it, but the ones that do must still be right,
// so failures are tolerated here and the caller decides how many it demands.
static int SendEchoRpcs(Channel& channel, int rpc_num, size_t attach_size,
                        const std::function<void(Controller&)>& disturb = nullptr,
                        int disturb_at = 0) {
    std::vector<Controller> cntl(rpc_num);
    std::vector<test::EchoRequest> req(rpc_num);
    std::vector<test::EchoResponse> res(rpc_num);
    std::vector<butil::IOBuf> attach(rpc_num);
    for (int i = 0; i < rpc_num; ++i) {
        req[i].set_message("hello");
        req[i].set_code(i + 1);
        if (attach_size > 0) {
            EXPECT_EQ(0, attach[i].resize(
                attach_size, static_cast<char>('a' + i % 26)));
            cntl[i].request_attachment().append(attach[i]);
        }
        ::test::EchoService::Stub(&channel).Echo(&cntl[i], &req[i], &res[i], DoNothing());
        if (disturb && i == disturb_at) {
            disturb(cntl[i]);
        }
    }
    int succeeded = 0;
    for (int i = 0; i < rpc_num; ++i) {
        bthread_id_join(cntl[i].call_id());
        if (cntl[i].Failed()) {
            continue;
        }
        ++succeeded;
        EXPECT_EQ("MyEchoService", res[i].message()) << "rpc[" << i << "]";
        EXPECT_EQ(1, res[i].code_list_size()) << "rpc[" << i << "]";
        if (res[i].code_list_size() == 1) {
            EXPECT_EQ(i + 1, res[i].code_list(0)) << "rpc[" << i << "]";
        }
        EXPECT_EQ(attach_size, cntl[i].response_attachment().size()) << "rpc[" << i << "]";
        EXPECT_TRUE(attach[i].equals(cntl[i].response_attachment())) << "rpc[" << i << "]";
    }
    return succeeded;
}

static void SendEchoRpcsInRounds(Channel& channel) {
    for (int round = 0; round < E2E_ROUND_NUM; ++round) {
        for (size_t i = 0; i < arraysize(E2E_ATTACH_SIZE); ++i) {
            ASSERT_EQ(E2E_RPC_NUM,
                      SendEchoRpcs(channel, E2E_RPC_NUM, E2E_ATTACH_SIZE[i]))
                    << "round=" << round
                    << " attach_size=" << E2E_ATTACH_SIZE[i];
        }
    }
}

TEST_P(RdmaRpcTest, rdma_client_to_rdma_server) {
    if (!FLAGS_rdma_test_enable) {
        return;
    }

    StartServer();

    Channel channel;
    ChannelOptions chan_options;
    chan_options.socket_mode = SOCKET_MODE_RDMA;
    chan_options.connect_timeout_ms = 500;
    chan_options.timeout_ms = 5000;
    chan_options.max_retry = 0;
    ASSERT_EQ(0, channel.Init(g_ep, &chan_options));
    ASSERT_NO_FATAL_FAILURE(SendEchoRpcsInRounds(channel));

    StopServer();
}

TEST_P(RdmaRpcTest, tcp_client_to_tcp_server) {
    StartServer(false);

    Channel channel;
    ChannelOptions chan_options;
    chan_options.connect_timeout_ms = 500;
    chan_options.timeout_ms = 5000;
    chan_options.max_retry = 0;
    ASSERT_EQ(0, channel.Init(g_ep, &chan_options));
    ASSERT_NO_FATAL_FAILURE(SendEchoRpcsInRounds(channel));

    StopServer();
}

TEST_P(RdmaRpcTest, tcp_client_to_rdma_server) {
    StartServer();

    Channel channel;
    ChannelOptions chan_options;
    chan_options.connect_timeout_ms = 500;
    chan_options.timeout_ms = 5000;
    chan_options.max_retry = 0;
    ASSERT_EQ(0, channel.Init(g_ep, &chan_options));
    ASSERT_NO_FATAL_FAILURE(SendEchoRpcsInRounds(channel));

    StopServer();
}

TEST_P(RdmaRpcTest, rdma_client_to_tcp_server) {
    StartServer(false);

    Channel channel;
    ChannelOptions chan_options;
    chan_options.socket_mode = SOCKET_MODE_RDMA;
    chan_options.connect_timeout_ms = 500;
    chan_options.timeout_ms = 5000;
    chan_options.max_retry = 0;
    ASSERT_EQ(0, channel.Init(g_ep, &chan_options));
    ASSERT_NO_FATAL_FAILURE(SendEchoRpcsInRounds(channel));

    StopServer();
}

TEST_P(RdmaRpcTest, tcp_client_to_rdma_server_short_connection) {
    StartServer();

    Channel channel;
    ChannelOptions chan_options;
    chan_options.connect_timeout_ms = 1000;
    chan_options.timeout_ms = 10000;
    chan_options.max_retry = 0;
    chan_options.connection_type = "short";
    ASSERT_EQ(0, channel.Init(g_ep, &chan_options));
    for (int round = 0; round < 8; ++round) {
        ASSERT_EQ(E2E_RPC_NUM, SendEchoRpcs(channel, E2E_RPC_NUM, 4096))
                << "round=" << round;
    }

    StopServer();
}

// Rounds of connection churn: a race needs attempts, not one well-timed shot.
static const int CHURN_ROUND_NUM = 16;
static const int CHURN_RPC_NUM = 64;
static const size_t CHURN_ATTACH_SIZE = 32 * 1024;

TEST_P(RdmaRpcTest, rdma_server_survives_connection_churn) {
    StartServer();

    ChannelOptions chan_options;
    chan_options.connect_timeout_ms = 1000;
    chan_options.timeout_ms = 3000;
    chan_options.max_retry = 0;
    const int served_before = g_echo_served.load(butil::memory_order_relaxed);
    int succeeded = 0;
    for (int round = 0; round < CHURN_ROUND_NUM; ++round) {
        // A fresh Channel per round so the Socket is dropped from the socket
        // map when the Channel dies, instead of the next round inheriting it.
        Channel channel;
        ASSERT_EQ(0, channel.Init(g_ep, &chan_options));
        // Warm up before cutting. Without this the cut of round 0 lands on a
        // connection that has not finished connecting yet, where fd() is still
        // -1 and shutdown() is a silent no-op.
        ASSERT_EQ(1, SendEchoRpcs(channel, 1, 0)) << "round=" << round;
        succeeded += SendEchoRpcs(channel, CHURN_RPC_NUM, CHURN_ATTACH_SIZE,
                                  ShutdownClientConnection,
                                  round * CHURN_RPC_NUM / CHURN_ROUND_NUM);
        ASSERT_FALSE(HasFailure()) << "round=" << round;
    }

    const int served = g_echo_served.load(butil::memory_order_relaxed) -
                       served_before - CHURN_ROUND_NUM;
    LOG(INFO) << "server served " << served << " of "
              << CHURN_ROUND_NUM * CHURN_RPC_NUM << " requests during the churn, "
              << succeeded << " replies made it back";
    ASSERT_GT(served, 0);

    // The churn must leave the server able to serve a fresh connection, and
    // serve it correctly.
    Channel channel;
    ASSERT_EQ(0, channel.Init(g_ep, &chan_options));
    ASSERT_EQ(CHURN_RPC_NUM, SendEchoRpcs(channel, CHURN_RPC_NUM, CHURN_ATTACH_SIZE));

    StopServer();
}

static const int RPC_NUM = 1024;

void DumpRdmaEndpointInfo(Socket* client, Socket* server) {
    std::cout << std::endl << "client:";
    static_cast<RdmaTransport*>(client->_transport.get())->_rdma_ep->DebugInfo(std::cout);
    std::cout << std::endl << "server:";
    static_cast<RdmaTransport*>(server->_transport.get())->_rdma_ep->DebugInfo(std::cout);
}

TEST_P(RdmaRpcTest, send_rpcs_in_one_qp) {
    if (!FLAGS_rdma_test_enable) {
        return;
    }

    StartServer();

    Channel channel;
    ChannelOptions chan_options;
    chan_options.socket_mode = SOCKET_MODE_RDMA;
    chan_options.connect_timeout_ms = 500;
    chan_options.timeout_ms = 50000;
    chan_options.max_retry = 0;
    ASSERT_EQ(0, channel.Init(g_ep, &chan_options));
    Controller cntl[RPC_NUM];
    test::EchoRequest req[RPC_NUM];
    test::EchoResponse res[RPC_NUM];

    LOG(INFO) << "send 0 attachment";
    for (int i = 0; i < RPC_NUM; ++i) {
        req[i].set_message(__FUNCTION__);
        google::protobuf::Closure* done = DoNothing();
        ::test::EchoService::Stub(&channel).Echo(&cntl[i], &req[i], &res[i], done);
    }
    for (int i = 0; i < RPC_NUM; ++i) {
        bthread_id_join(cntl[i].call_id());
        if (cntl[i].ErrorCode() == ERPCTIMEDOUT) {
            SocketUniquePtr s;
            ASSERT_EQ(0, Socket::Address(cntl[i]._single_server_id, &s));
            Socket* m = GetSocketFromServer(0);
            DumpRdmaEndpointInfo(s.get(), m);
        }
        ASSERT_EQ(0, cntl[i].ErrorCode()) << "req[" << i << "]";
    }

    LOG(INFO) << "send 4KB attachment";
    butil::IOBuf attach;
    attach.resize(4096);
    for (int i = 0; i < RPC_NUM; ++i) {
        cntl[i].Reset();
        cntl[i].request_attachment().append(attach);
        google::protobuf::Closure* done = DoNothing();
        ::test::EchoService::Stub(&channel).Echo(&cntl[i], &req[i], &res[i], done);
    }
    for (int i = 0; i < RPC_NUM; ++i) {
        bthread_id_join(cntl[i].call_id());
        if (cntl[i].ErrorCode() == ERPCTIMEDOUT) {
            SocketUniquePtr s;
            ASSERT_EQ(0, Socket::Address(cntl[i]._single_server_id, &s));
            Socket* m = GetSocketFromServer(0);
            DumpRdmaEndpointInfo(s.get(), m);
        }
        ASSERT_EQ(0, cntl[i].ErrorCode()) << "req[" << i << "]";
    }

    LOG(INFO) << "send 1MB attachment";
    attach.resize(1048576);
    for (int i = 0; i < RPC_NUM; ++i) {
        cntl[i].Reset();
        cntl[i].request_attachment().append(attach);
        google::protobuf::Closure* done = DoNothing();
        ::test::EchoService::Stub(&channel).Echo(&cntl[i], &req[i], &res[i], done);
    }
    for (int i = 0; i < RPC_NUM; ++i) {
        bthread_id_join(cntl[i].call_id());
        if (cntl[i].ErrorCode() == ERPCTIMEDOUT) {
            SocketUniquePtr s;
            ASSERT_EQ(0, Socket::Address(cntl[i]._single_server_id, &s));
            Socket* m = GetSocketFromServer(0);
            DumpRdmaEndpointInfo(s.get(), m);
        }
        ASSERT_TRUE(0 == cntl[i].ErrorCode() ||
                    EOVERCROWDED == cntl[i].ErrorCode()) << "req[" << i << "] " << berror(cntl[i].ErrorCode());
    }

    SocketUniquePtr s;
    ASSERT_EQ(0, Socket::Address(cntl[0]._single_server_id, &s));
    Socket* m = GetSocketFromServer(0);
    DumpRdmaEndpointInfo(s.get(), m);

    StopServer();
}

TEST_P(RdmaRpcTest, send_rpc_in_many_qp) {
    if (!FLAGS_rdma_test_enable) {
        return;
    }

    butil::ip_t ip;
    ASSERT_EQ(0, butil::str2ip(g_ip.c_str(), &ip));

    Server server[100];
    MyEchoService svc[100];
    int num = 100;
    butil::EndPoint server_eps[100];
    for (int i = 0; i < num; ++i) {
        ServerOptions options;
        options.socket_mode = SOCKET_MODE_RDMA;
        options.idle_timeout_sec = 1;
        options.max_concurrency = 0;
        options.internal_port = -1;
        server[i].AddService(&svc[i], SERVER_DOESNT_OWN_SERVICE);
        ASSERT_EQ(0, server[i].Start(0, &options));
        server_eps[i] = butil::EndPoint(ip, server[i].listen_address().port);
    }

    int port = 0;
    butil::IOBuf attach;
    attach.resize(4096);
    ChannelOptions chan_options;
    chan_options.socket_mode = SOCKET_MODE_RDMA;
    chan_options.connect_timeout_ms = 500;
    chan_options.timeout_ms = 100000;
    chan_options.max_retry = 0;
    Channel channel[RPC_NUM];
    Server* svr[RPC_NUM];
    Controller cntl[RPC_NUM];
    test::EchoRequest req[RPC_NUM];
    test::EchoResponse res[RPC_NUM];
    for (int i = 0; i < RPC_NUM; ++i) {
        svr[i] = &server[i % num];
        ASSERT_EQ(0, channel[i].Init(server_eps[(port++) % num], &chan_options));
        req[i].set_message(__FUNCTION__);
        cntl[i].request_attachment().append(attach);
        google::protobuf::Closure* done = DoNothing();
        ::test::EchoService::Stub(&channel[i]).Echo(&cntl[i], &req[i], &res[i], done);
    }
    for (int i = 0; i < RPC_NUM; ++i) {
        bthread_id_join(cntl[i].call_id());
        if (cntl[i].ErrorCode() == ERPCTIMEDOUT) {
            SocketUniquePtr s;
            EXPECT_EQ(0, Socket::Address(cntl[i]._single_server_id, &s));
            if (s && svr[i] && svr[i]->_am) {
                std::vector<SocketId> sids;
                svr[i]->_am->ListConnections(&sids);
                for (size_t j = 0; j < sids.size(); ++j) {
                    SocketUniquePtr m;
                    if (Socket::AddressFailedAsWell(sids[j], &m) == 0) {
                        DumpRdmaEndpointInfo(s.get(), m.get());
                    }
                }
            }
        }
        EXPECT_EQ(0, cntl[i].ErrorCode()) << "req[" << i << "]";
    }

    for (int i = 0; i < num; ++i) {
        server[i].Stop(0);
        server[i].Join();
    }
}

TEST_P(RdmaRpcTest, send_rpcs_as_pooled_connection) {
    if (!FLAGS_rdma_test_enable) {
        return;
    }

    StartServer();

    Channel channel;
    ChannelOptions chan_options;
    chan_options.socket_mode = SOCKET_MODE_RDMA;
    chan_options.connect_timeout_ms = 30000;  // it may very slow
    chan_options.timeout_ms = 30000;
    chan_options.max_retry = 0;
    chan_options.connection_type = "pooled";
    ASSERT_EQ(0, channel.Init(g_ep, &chan_options));
    Controller cntl[RPC_NUM];
    test::EchoRequest req[RPC_NUM];
    test::EchoResponse res[RPC_NUM];

    butil::IOBuf attach;
    attach.resize(4096);
    for (int i = 0; i < RPC_NUM; ++i) {
        req[i].set_message(__FUNCTION__);
        cntl[i].request_attachment().append(attach);
        google::protobuf::Closure* done = DoNothing();
        ::test::EchoService::Stub(&channel).Echo(&cntl[i], &req[i], &res[i], done);
    }
    for (int i = 0; i < RPC_NUM; ++i) {
        bthread_id_join(cntl[i].call_id());
        if (cntl[i].ErrorCode() == ERPCTIMEDOUT) {
            SocketUniquePtr s;
            ASSERT_EQ(0, Socket::Address(cntl[i]._single_server_id, &s));
            Socket* m = GetSocketFromServer(0);
            DumpRdmaEndpointInfo(s.get(), m);
        }
        ASSERT_EQ(0, cntl[i].ErrorCode()) << "req[" << i << "]";
    }

    StopServer();
}

TEST_P(RdmaRpcTest, send_rpcs_as_short_connection) {
    if (!FLAGS_rdma_test_enable) {
        return;
    }

    StartServer();

    Channel channel;
    ChannelOptions chan_options;
    chan_options.socket_mode = SOCKET_MODE_RDMA;
    chan_options.connect_timeout_ms = 30000;  // it may very slow
    chan_options.timeout_ms = 30000;
    chan_options.max_retry = 0;
    chan_options.connection_type = "short";
    ASSERT_EQ(0, channel.Init(g_ep, &chan_options));
    Controller cntl[RPC_NUM];
    test::EchoRequest req[RPC_NUM];
    test::EchoResponse res[RPC_NUM];

    butil::IOBuf attach;
    attach.resize(4096);
    for (int i = 0; i < RPC_NUM; ++i) {
        req[i].set_message(__FUNCTION__);
        cntl[i].request_attachment().append(attach);
        google::protobuf::Closure* done = DoNothing();
        ::test::EchoService::Stub(&channel).Echo(&cntl[i], &req[i], &res[i], done);
    }
    for (int i = 0; i < RPC_NUM; ++i) {
        bthread_id_join(cntl[i].call_id());
        if (cntl[i].ErrorCode() == ERPCTIMEDOUT) {
            SocketUniquePtr s;
            ASSERT_EQ(0, Socket::Address(cntl[i]._single_server_id, &s));
            Socket* m = GetSocketFromServer(0);
            DumpRdmaEndpointInfo(s.get(), m);
        }
        ASSERT_EQ(0, cntl[i].ErrorCode()) << "req[" << i << "]";
    }

    StopServer();
}

TEST_P(RdmaRpcTest, server_stop_during_rpc) {
    if (!FLAGS_rdma_test_enable) {
        return;
    }

    StartServer();

    Channel channel;
    ChannelOptions chan_options;
    chan_options.socket_mode = SOCKET_MODE_RDMA;
    chan_options.connect_timeout_ms = 500;
    chan_options.timeout_ms = 3000;
    chan_options.max_retry = 0;
    ASSERT_EQ(0, channel.Init(g_ep, &chan_options));
    Controller cntl[RPC_NUM];
    test::EchoRequest req[RPC_NUM];
    test::EchoResponse res[RPC_NUM];

    butil::IOBuf attach;
    attach.resize(4096);
    for (int i = 0; i < RPC_NUM; ++i) {
        req[i].set_message(__FUNCTION__);
        cntl[i].request_attachment().append(attach);
        google::protobuf::Closure* done = DoNothing();
        ::test::EchoService::Stub(&channel).Echo(&cntl[i], &req[i], &res[i], done);
    }

    for (int i = 0; i < RPC_NUM; ++i) {
        bthread_id_join(cntl[i].call_id());
        if (i == 0) StopServer();
        int error_code = cntl[i].ErrorCode();
        ASSERT_TRUE(error_code == 0 ||
                    error_code == EEOF ||
                    error_code == ELOGOFF ||
                    error_code == EHOSTDOWN) << "req[" << i << "]: " << error_code;
    }
}

TEST_P(RdmaRpcTest, server_close_during_rpc) {
    if (!FLAGS_rdma_test_enable) {
        return;
    }

    StartServer();

    Channel channel;
    ChannelOptions chan_options;
    chan_options.socket_mode = SOCKET_MODE_RDMA;
    chan_options.connect_timeout_ms = 500;
    chan_options.timeout_ms = 3000;
    chan_options.max_retry = 0;
    ASSERT_EQ(0, channel.Init(g_ep, &chan_options));
    Controller cntl[RPC_NUM];
    test::EchoRequest req[RPC_NUM];
    test::EchoResponse res[RPC_NUM];

    butil::IOBuf attach;
    attach.resize(4096);
    for (int i = 0; i < RPC_NUM; ++i) {
        req[i].set_message(__FUNCTION__);
        cntl[i].request_attachment().append(attach);
        if (i == RPC_NUM / 2) {
            req[i].set_close_fd(true);
        }
        google::protobuf::Closure* done = DoNothing();
        ::test::EchoService::Stub(&channel).Echo(&cntl[i], &req[i], &res[i], done);
    }

    for (int i = 0; i < RPC_NUM; ++i) {
        bthread_id_join(cntl[i].call_id());
        int error_code = cntl[i].ErrorCode();
        ASSERT_TRUE(error_code == 0 ||
                    error_code == EEOF ||
                    error_code == EFAILEDSOCKET ||
                    error_code == EHOSTDOWN) << "req[" << i << "]: " << error_code;
    }

    StopServer();
}

TEST_P(RdmaRpcTest, client_close_during_rpc) {
    if (!FLAGS_rdma_test_enable) {
        return;
    }

    StartServer();

    Channel channel;
    ChannelOptions chan_options;
    chan_options.socket_mode = SOCKET_MODE_RDMA;
    chan_options.connect_timeout_ms = 500;
    chan_options.timeout_ms = 3000;
    chan_options.max_retry = 0;
    ASSERT_EQ(0, channel.Init(g_ep, &chan_options));
    Controller cntl[RPC_NUM];
    test::EchoRequest req[RPC_NUM];
    test::EchoResponse res[RPC_NUM];

    butil::IOBuf attach;
    attach.resize(4096);
    for (int i = 0; i < RPC_NUM; ++i) {
        req[i].set_message(__FUNCTION__);
        cntl[i].request_attachment().append(attach);
        google::protobuf::Closure* done = DoNothing();
        ::test::EchoService::Stub(&channel).Echo(&cntl[i], &req[i], &res[i], done);
    }

    cntl[0].CloseConnection("Close connection");

    for (int i = 0; i < RPC_NUM; ++i) {
        bthread_id_join(cntl[i].call_id());
        int error_code = cntl[i].ErrorCode();
        ASSERT_TRUE(error_code == 0 ||
                    error_code == ECLOSE ||
                    error_code == EHOSTDOWN) << "req[" << i << "]: " << error_code;
    }

    StopServer();
}

TEST_P(RdmaRpcTest, rdma_client_close_during_rpc_repeatedly) {
    if (!FLAGS_rdma_test_enable) {
        return;
    }

    StartServer();

    ChannelOptions chan_options;
    chan_options.socket_mode = SOCKET_MODE_RDMA;
    chan_options.connect_timeout_ms = 1000;
    chan_options.timeout_ms = 3000;
    chan_options.max_retry = 0;
    const int served_before = g_echo_served.load(butil::memory_order_relaxed);
    int succeeded = 0;
    for (int round = 0; round < CHURN_ROUND_NUM; ++round) {
        Channel channel;
        ASSERT_EQ(0, channel.Init(g_ep, &chan_options));
        // Warm up so the cut lands on an established RDMA connection.
        ASSERT_EQ(1, SendEchoRpcs(channel, 1, 0)) << "round=" << round;
        succeeded += SendEchoRpcs(channel, CHURN_RPC_NUM, CHURN_ATTACH_SIZE,
                                  ShutdownClientConnection,
                                  round * CHURN_RPC_NUM / CHURN_ROUND_NUM);
        ASSERT_FALSE(HasFailure()) << "round=" << round;
    }


    const int served = g_echo_served.load(butil::memory_order_relaxed) -
                       served_before - CHURN_ROUND_NUM;
    LOG(INFO) << "server served " << served << " of "
              << CHURN_ROUND_NUM * CHURN_RPC_NUM << " requests during the churn, "
              << succeeded << " replies made it back";
    ASSERT_GT(served, 0);

    Channel channel;
    ASSERT_EQ(0, channel.Init(g_ep, &chan_options));
    ASSERT_EQ(CHURN_RPC_NUM, SendEchoRpcs(channel, CHURN_RPC_NUM, CHURN_ATTACH_SIZE));

    StopServer();
}

TEST_P(RdmaRpcTest, verbs_error_handling) {
    if (!FLAGS_rdma_test_enable) {
        return;
    }

    StartServer();

    Channel channel;
    ChannelOptions chan_options;
    chan_options.socket_mode = SOCKET_MODE_RDMA;
    chan_options.connect_timeout_ms = 500;
    chan_options.timeout_ms = 500;
    chan_options.max_retry = 0;
    ASSERT_EQ(0, channel.Init(g_ep, &chan_options));

    Controller cntl;
    test::EchoRequest req;
    test::EchoResponse res;
    req.set_message(__FUNCTION__);
    req.set_sleep_us(200000);
    google::protobuf::Closure* done = DoNothing();
    ::test::EchoService::Stub(&channel).Echo(&cntl, &req, &res, done);

    SocketUniquePtr s;
    ASSERT_EQ(0, Socket::Address(cntl._single_server_id, &s));
    // The QP below only exists once the handshake is over.
    ASSERT_RDMA_STATE(rdma::RdmaEndpoint::ESTABLISHED, RdmaTransportOf(s));
    ibv_send_wr wr;
    memset(&wr, 0, sizeof(wr));
    ibv_sge sge;
    void* buf = malloc(8192);
    sge.addr = (uint64_t)buf;
    sge.length = 8192;
    sge.lkey = 1;  // incorrect lkey
    wr.sg_list = &sge;
    wr.num_sge = 1;
    ibv_send_wr* bad = nullptr;
    auto rdma_transport = RdmaTransportOf(s);
    ibv_post_send(rdma_transport->_rdma_ep->_host.resource->qp, &wr, &bad);
    bthread_id_join(cntl.call_id());
    ASSERT_EQ(ERDMA, cntl.ErrorCode());
    free(buf);

    StopServer();
}

TEST_P(RdmaRpcTest, rdma_use_parallel_channel) {
    if (!FLAGS_rdma_test_enable) {
        return;
    }

    StartServer();

    const size_t NCHANS = 8;
    Channel subchans[NCHANS];
    ParallelChannel channel;
    ChannelOptions opts;
    opts.socket_mode = SOCKET_MODE_RDMA;
    for (size_t i = 0; i < NCHANS; ++i) {
        ASSERT_EQ(0, subchans[i].Init(_naming_url.c_str(), "rR", &opts));
        ASSERT_EQ(0, channel.AddChannel(
                    &subchans[i], DOESNT_OWN_CHANNEL,
                    nullptr, nullptr));
    }
    ASSERT_EQ(0, channel.Init(nullptr));

    Controller cntl;
    test::EchoRequest req;
    test::EchoResponse res;
    req.set_message(__FUNCTION__);
    ::test::EchoService::Stub(&channel).Echo(&cntl, &req, &res, nullptr);

    ASSERT_EQ(0, cntl.ErrorCode());
    ASSERT_EQ(NCHANS, (size_t)cntl.sub_count());

    StopServer();
}

TEST_P(RdmaRpcTest, rdma_use_selective_channel) {
    if (!FLAGS_rdma_test_enable) {
        return;
    }

    StartServer();

    const size_t NCHANS = 8;
    SelectiveChannel channel;
    ChannelOptions opts;
    opts.socket_mode = SOCKET_MODE_RDMA;
    ASSERT_EQ(0, channel.Init("rr", &opts));
    for (size_t i = 0; i < NCHANS; ++i) {
        Channel* subchan = new Channel;
        ASSERT_EQ(0, subchan->Init(_naming_url.c_str(), "rR", &opts));
        ASSERT_EQ(0, channel.AddChannel(subchan, nullptr));
    }

    Controller cntl;
    test::EchoRequest req;
    test::EchoResponse res;
    req.set_message(__FUNCTION__);
    ::test::EchoService::Stub(&channel).Echo(&cntl, &req, &res, nullptr);

    ASSERT_EQ(0, cntl.ErrorCode()) << cntl.ErrorText();
    ASSERT_EQ(1, cntl.sub_count());

    StopServer();
}

static void MockFree(void* buf) { }

TEST_P(RdmaRpcTest, send_rpcs_with_user_defined_iobuf) {
    if (!FLAGS_rdma_test_enable) {
        return;
    }

    StartServer();

    Channel channel;
    ChannelOptions chan_options;
    chan_options.socket_mode = SOCKET_MODE_RDMA;
    chan_options.connect_timeout_ms = 500;
    chan_options.timeout_ms = 500;
    chan_options.max_retry = 0;
    ASSERT_EQ(0, channel.Init(g_ep, &chan_options));
    Controller cntl[RPC_NUM];
    test::EchoRequest req[RPC_NUM];
    test::EchoResponse res[RPC_NUM];

    butil::IOBuf attach;
    void* data = malloc(4096);;
    attach.append_user_data(data, 4096, nullptr);
    req[0].set_message(__FUNCTION__);
    cntl[0].request_attachment().append(attach);
    google::protobuf::Closure* done = DoNothing();
    ::test::EchoService::Stub(&channel).Echo(&cntl[0], &req[0], &res[0], done);
    bthread_id_join(cntl[0].call_id());
    ASSERT_EQ(ERDMAMEM, cntl[0].ErrorCode());
    attach.clear();
    sleep(2);  // wait for client recover from EHOSTDOWN
    cntl[0].Reset();

    char* mr[2 * RPC_NUM];
    uint32_t lkey[2 * RPC_NUM];
    for (size_t i = 0; i < RPC_NUM; ++i) {
        mr[2 * i] = (char*)malloc(4096);
        memset(mr[2 * i], i % 100, 4096);
        lkey[2 * i] = rdma::RegisterMemoryForRdma(mr[2 * i], 4096);
        ASSERT_TRUE(lkey[2 * i] != 0);
        cntl[i].request_attachment().append_user_data_with_meta(mr[2 * i] + i, 4096 - i, MockFree, lkey[2 * i]);
        mr[2 * i + 1] = (char*)malloc(4096);
        memset(mr[2 * i + 1], i % 100, 4096);
        lkey[2 * i + 1] = rdma::RegisterMemoryForRdma(mr[2 * i + 1], 4096);
        ASSERT_TRUE(lkey[2 * i + 1] != 0);
        cntl[i].request_attachment().append_user_data_with_meta(mr[2 * i + 1] + i, 4096 - i, MockFree, lkey[2 * i + 1]);
        req[i].set_message(__FUNCTION__);
        google::protobuf::Closure* done = DoNothing();
        ::test::EchoService::Stub(&channel).Echo(&cntl[i], &req[i], &res[i], done);
    }
    for (size_t i = 0; i < RPC_NUM; ++i) {
        bthread_id_join(cntl[i].call_id());
        ASSERT_EQ(0, cntl[i].ErrorCode()) << "req[" << i << "]";
        ASSERT_EQ(2 * (4096 - i), cntl[i].response_attachment().size());
        char tmp[8192];
        cntl[i].response_attachment().copy_to(tmp, 2 * (4096 - i));
        ASSERT_EQ(0, memcmp(mr[2 * i] + i, tmp, 4096 - i));
        ASSERT_EQ(0, memcmp(mr[2 * i + 1] + i, tmp + 4096 - i, 4096 - i));
        // Both halves, not mr[i]: registrations are keyed on the exact
        // address, so half of them used to be left behind pointing at memory
        // that is about to be freed. A later malloc() handing back one of
        // those addresses makes plain heap memory look registered, and the
        // ERDMAMEM check at the top of this test then sees a successful RPC.
        rdma::DeregisterMemoryForRdma(mr[2 * i]);
        rdma::DeregisterMemoryForRdma(mr[2 * i + 1]);
        free(mr[2 * i]);
        free(mr[2 * i + 1]);
    }

    StopServer();
}

TEST_P(RdmaRpcTest, try_memory_pool_empty) {
    if (!FLAGS_rdma_test_enable) {
        return;
    }

    StartServer();

    Channel channel;
    ChannelOptions chan_options;
    chan_options.socket_mode = SOCKET_MODE_RDMA;
    chan_options.connect_timeout_ms = 500;
    chan_options.timeout_ms = 60000;
    chan_options.max_retry = 0;
    ASSERT_EQ(0, channel.Init(g_ep, &chan_options));
    Controller cntl[RPC_NUM];
    test::EchoRequest req[RPC_NUM];
    test::EchoResponse res[RPC_NUM];

    butil::IOBuf iobuf[RPC_NUM];
    for (int i = 0; i < 1024; ++i) {
        if (iobuf[i].resize(1048576 * 8)) {
            // 8MB for each iobuf
            break;
        }
    }

    for (int i = 0; i < RPC_NUM; ++i) {
        req[i].set_message(__FUNCTION__);
        cntl[i].request_attachment().append(iobuf[i]);
        google::protobuf::Closure* done = DoNothing();
        ::test::EchoService::Stub(&channel).Echo(&cntl[i], &req[i], &res[i], done);
    }
    for (int i = 0; i < RPC_NUM; ++i) {
        bthread_id_join(cntl[i].call_id());
    }

    StopServer();
}

// Run every TEST_P(RdmaRpcTest, ...) above twice: once with the
// client-side handshake forced to v2 ("RDMA" magic + fixed-layout
// HelloMessage), once with v3 ("RDM3" magic + protobuf RdmaHello).
// The server always accepts both via magic-byte dispatch, so this
// proves the upper-layer RPC paths behave identically under either
// wire format.
INSTANTIATE_TEST_SUITE_P(
    HandshakeVersion, RdmaRpcTest,
    ::testing::Values(2, 3),
    [](const ::testing::TestParamInfo<int>& info) {
        return std::string("v") + std::to_string(info.param);
    });

// ============================ GPU Direct RDMA ============================
//
// These run on plain CI with neither a GPU nor a RoCE card: the fixtures set
// rdma::g_skip_device_alloc_for_test, which makes the device pool hand out
// posix_memalign'd host memory with a fake lkey instead of calling cudaMalloc
// and ibv_reg_mr. That is exactly the pointer bookkeeping the pool and
// DeviceAttachment do, and none of the code under test is allowed to
// dereference the pointers anyway.
//
// On a machine that has the real thing, --gdr_test_real_device runs the same
// tests against cudaMalloc and ibv_reg_mr instead. That needs a GDR build
// (bazel --config=gdr), a CUDA device, a card, and a peer-memory module
// (nv_peer_mem or dmabuf); without them GlobalGdrInitialize() fails and every
// GDR test fails loudly in SetUp rather than quietly testing the stand-in.

// Shared SetUp half of the two GDR fixtures. Uses ASSERT_*, so callers have to
// be void and gtest aborts the test on failure.
static void EnableGdrForTest() {
    if (FLAGS_gdr_test_real_device) {
        // main() leaves rdma::g_skip_rdma_init set unless --rdma_test_enable,
        // which leaves g_pd nullptr and segfaults inside ibv_reg_mr rather than
        // failing. Cheaper to say so than to debug it again.
        ASSERT_TRUE(FLAGS_rdma_test_enable)
                << "--gdr_test_real_device needs --rdma_test_enable too";
        // cudaMalloc'd memory still has to be registered on the card's PD,
        // and nothing in these tests opens a connection that would have
        // brought the device up.
        rdma::GlobalRdmaInitializeOrDie();
        rdma::g_skip_device_alloc_for_test = false;
    } else {
        rdma::g_skip_device_alloc_for_test = true;
    }
    rdma::FLAGS_rdma_enable_gdr = true;
    ASSERT_EQ(0, rdma::GlobalGdrInitialize());
    ASSERT_TRUE(rdma::IsGdrAvailable());
}

class GdrTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_NO_FATAL_FAILURE(EnableGdrForTest());
    }

    void TearDown() override {
        rdma::GlobalGdrRelease();
        rdma::FLAGS_rdma_enable_gdr = false;
        rdma::g_skip_device_alloc_for_test = false;
    }
};

TEST_F(GdrTest, alloc_rounds_up_to_size_class) {
    uint32_t lkey = 0;
    // 5000 falls in the 8KB class.
    void* p = rdma::AllocDeviceBlock(5000, &lkey);
    ASSERT_TRUE(p != nullptr);
    ASSERT_NE(0u, lkey);
    // The whole 8KB is registered, not just the 5000 asked for. So is the
    // rest of the region it was carved out of: the MR covers a region, not a
    // block, which is exactly why the region count has to stay small.
    ASSERT_EQ(lkey, rdma::GetDeviceLKey((char*)p + 8191));
    ASSERT_EQ(lkey, rdma::GetDeviceLKey((char*)p + 8192));
    // A pointer into the middle of a block was never handed out, and letting
    // it back in would make the pool serve overlapping blocks from then on.
    ASSERT_EQ(-1, rdma::DeallocDeviceBlock((char*)p + 4096));
    ASSERT_EQ(ERANGE, errno);
    ASSERT_EQ(0, rdma::DeallocDeviceBlock(p));
}

TEST_F(GdrTest, alloc_rejects_zero_and_oversize) {
    uint32_t lkey = 0;
    ASSERT_TRUE(nullptr == rdma::AllocDeviceBlock(0, &lkey));
    ASSERT_EQ(EINVAL, errno);
    // Past the 1GB top size class.
    ASSERT_TRUE(nullptr == rdma::AllocDeviceBlock(2UL << 30, &lkey));
    ASSERT_EQ(E2BIG, errno);
}

TEST_F(GdrTest, freed_block_is_reused_not_reregistered) {
    uint32_t lkey1 = 0;
    void* p1 = rdma::AllocDeviceBlock(4096, &lkey1);
    ASSERT_TRUE(p1 != nullptr);
    ASSERT_EQ(0, rdma::DeallocDeviceBlock(p1));

    // ibv_reg_mr pins pages and is far too slow to pay per RPC, so a freed
    // block keeps its registration and comes straight back out of the free
    // list -- same address, same lkey.
    uint32_t lkey2 = 0;
    void* p2 = rdma::AllocDeviceBlock(4096, &lkey2);
    ASSERT_EQ(p1, p2);
    ASSERT_EQ(lkey1, lkey2);
    ASSERT_EQ(0, rdma::DeallocDeviceBlock(p2));
}

TEST_F(GdrTest, dealloc_rejects_foreign_pointer) {
    int stack_var = 0;
    ASSERT_EQ(-1, rdma::DeallocDeviceBlock(&stack_var));
    ASSERT_EQ(ERANGE, errno);
    ASSERT_EQ(-1, rdma::DeallocDeviceBlock(nullptr));
    ASSERT_EQ(EINVAL, errno);
}

TEST_F(GdrTest, lkey_of_unknown_address_is_zero) {
    int stack_var = 0;
    ASSERT_EQ(0u, rdma::GetDeviceLKey(&stack_var));
    ASSERT_EQ(0u, rdma::GetDeviceLKey(nullptr));
}

TEST_F(GdrTest, alloc_fails_past_max_device_bytes) {
    // Restored even when an assertion below returns early -- otherwise an
    // 8KB cap leaks into every test that runs after this one, and they fail
    // in ways that have nothing to do with what they are testing.
    struct MaxBytesGuard {
        int64_t saved;
        MaxBytesGuard() : saved(rdma::FLAGS_rdma_gdr_max_device_bytes) { }
        ~MaxBytesGuard() { rdma::FLAGS_rdma_gdr_max_device_bytes = saved; }
    } guard;
    rdma::FLAGS_rdma_gdr_max_device_bytes = 8192;
    // The cap is one of the options MemoryPool copies when it comes up, not
    // a flag it re-reads on every extension, so the pool SetUp() built has
    // to be replaced rather than merely reconfigured.
    rdma::GlobalGdrRelease();
    ASSERT_NO_FATAL_FAILURE(EnableGdrForTest());

    uint32_t lkey = 0;
    void* p1 = rdma::AllocDeviceBlock(4096, &lkey);
    ASSERT_TRUE(p1 != nullptr);
    void* p2 = rdma::AllocDeviceBlock(4096, &lkey);
    ASSERT_TRUE(p2 != nullptr);
    ASSERT_TRUE(nullptr == rdma::AllocDeviceBlock(4096, &lkey));
    ASSERT_EQ(ENOMEM, errno);

    // Freeing does not return memory to the OS, but it does make the block
    // available again, so the next allocation of that class succeeds.
    ASSERT_EQ(0, rdma::DeallocDeviceBlock(p1));
    void* p3 = rdma::AllocDeviceBlock(4096, &lkey);
    ASSERT_EQ(p1, p3);

    ASSERT_EQ(0, rdma::DeallocDeviceBlock(p2));
    ASSERT_EQ(0, rdma::DeallocDeviceBlock(p3));
}

// Slab allocation is the whole point of the pool rewrite: 4096 blocks must
// not become 4096 registrations, because GetDeviceLKey() resolves an address
// by scanning the region array with no lock held.
TEST_F(GdrTest, region_count_stays_bounded_as_blocks_churn) {
    int num_regions = -1;
    rdma::GetDeviceMemoryStat(nullptr, nullptr, &num_regions);
    ASSERT_EQ(0, num_regions);

    const int kBlockSize = 4096;
    const int kBlocks = 512;
    std::vector<void*> blocks;
    for (int i = 0; i < kBlocks; ++i) {
        uint32_t lkey = 0;
        void* p = rdma::AllocDeviceBlock(kBlockSize, &lkey);
        ASSERT_TRUE(p != nullptr) << "at " << i;
        ASSERT_NE(0u, lkey);
        ASSERT_EQ(lkey, rdma::GetDeviceLKey(p));
        blocks.push_back(p);
    }

    int64_t reserved = 0;
    int64_t in_use = 0;
    rdma::GetDeviceMemoryStat(&reserved, &in_use, &num_regions);
    // 64 blocks to a region, so 512 blocks fit in 8 of them -- not 512.
    ASSERT_EQ(8, num_regions);
    ASSERT_EQ((int64_t)kBlocks * kBlockSize, in_use);
    ASSERT_EQ((int64_t)kBlocks * kBlockSize, reserved);

    for (size_t i = 0; i < blocks.size(); ++i) {
        ASSERT_EQ(0, rdma::DeallocDeviceBlock(blocks[i]));
    }
    rdma::GetDeviceMemoryStat(&reserved, &in_use, &num_regions);
    ASSERT_EQ(0, in_use);
    // Freeing returns blocks to the pool, never regions to the device.
    ASSERT_EQ(8, num_regions);
    ASSERT_EQ((int64_t)kBlocks * kBlockSize, reserved);
}

namespace {

struct DevicePoolChurnArg {
    int block_size;
    std::vector<void*> blocks;
    std::vector<uint32_t> lkeys;
    bool ok;
};

void* DevicePoolChurn(void* void_arg) {
    DevicePoolChurnArg* arg = (DevicePoolChurnArg*)void_arg;
    // Churn first so that by the time we start holding blocks, this thread
    // has filled and flushed its cache and the global lists have been hit
    // from every direction.
    for (int i = 0; i < 256; ++i) {
        uint32_t lkey = 0;
        void* p = rdma::AllocDeviceBlock(arg->block_size, &lkey);
        if (p == nullptr || lkey == 0 || lkey != rdma::GetDeviceLKey(p) ||
            rdma::DeallocDeviceBlock(p) != 0) {
            arg->ok = false;
            return nullptr;
        }
    }
    for (size_t i = 0; i < arg->blocks.size(); ++i) {
        uint32_t lkey = 0;
        void* p = rdma::AllocDeviceBlock(arg->block_size, &lkey);
        if (p == nullptr || lkey == 0 || lkey != rdma::GetDeviceLKey(p)) {
            arg->ok = false;
            return nullptr;
        }
        arg->blocks[i] = p;
        arg->lkeys[i] = lkey;
    }
    return nullptr;
}

}  // namespace

// The pool hands blocks out from a thread cache, from per-class free lists
// and by carving fresh regions, all at once. Handing the same block to two
// threads is the failure this rules out -- and it is invisible to a
// single-threaded test no matter how long it runs.
TEST_F(GdrTest, pool_is_thread_safe_under_churn) {
    const int kThreads = 4;
    const int kPerThread = 64;
    DevicePoolChurnArg args[kThreads];
    pthread_t tids[kThreads];
    for (int t = 0; t < kThreads; ++t) {
        args[t].block_size = 4096;
        args[t].blocks.resize(kPerThread, nullptr);
        args[t].lkeys.resize(kPerThread, 0);
        args[t].ok = true;
        ASSERT_EQ(0, pthread_create(&tids[t], nullptr, DevicePoolChurn, &args[t]));
    }

    std::set<void*> all;
    for (int t = 0; t < kThreads; ++t) {
        ASSERT_EQ(0, pthread_join(tids[t], nullptr));
        EXPECT_TRUE(args[t].ok) << "thread " << t;
        for (int i = 0; i < kPerThread; ++i) {
            ASSERT_TRUE(args[t].blocks[i] != nullptr);
            // Same block twice means two threads carved the same offset.
            ASSERT_TRUE(all.insert(args[t].blocks[i]).second)
                    << "block " << args[t].blocks[i] << " handed out twice";
            ASSERT_EQ(args[t].lkeys[i], rdma::GetDeviceLKey(args[t].blocks[i]));
        }
    }
    ASSERT_EQ((size_t)(kThreads * kPerThread), all.size());

    int num_regions = 0;
    rdma::GetDeviceMemoryStat(nullptr, nullptr, &num_regions);
    ASSERT_GT(num_regions, 0);
    ASSERT_LE(num_regions, 32);  // --rdma_gdr_max_regions

    // Freeing from a thread other than the one that allocated is the normal
    // case here: receive blocks are allocated by the poller and released by
    // whichever bthread consumed the attachment.
    for (std::set<void*>::iterator it = all.begin(); it != all.end(); ++it) {
        ASSERT_EQ(0, rdma::DeallocDeviceBlock(*it));
    }
    int64_t in_use = -1;
    rdma::GetDeviceMemoryStat(nullptr, &in_use, nullptr);
    ASSERT_EQ(0, in_use);
}

TEST_F(GdrTest, register_user_device_memory) {
    void* buf = nullptr;
    ASSERT_EQ(0, posix_memalign(&buf, 4096, 16384));
    ASSERT_EQ(0u, rdma::GetDeviceLKey(buf));

    const uint32_t lkey = rdma::RegisterDeviceMemory(buf, 16384);
    ASSERT_NE(0u, lkey);
    ASSERT_EQ(lkey, rdma::GetDeviceLKey(buf));
    ASSERT_EQ(lkey, rdma::GetDeviceLKey((char*)buf + 16383));
    ASSERT_EQ(0u, rdma::GetDeviceLKey((char*)buf + 16384));

    // A user region is not a pool block and must not be recyclable as one.
    ASSERT_EQ(-1, rdma::DeallocDeviceBlock(buf));
    ASSERT_EQ(ERANGE, errno);

    rdma::DeregisterDeviceMemory(buf);
    ASSERT_EQ(0u, rdma::GetDeviceLKey(buf));
    free(buf);
}

TEST_F(GdrTest, attachment_append_new) {
    DeviceAttachment da;
    ASSERT_TRUE(da.empty());
    ASSERT_EQ(0u, da.size());

    void* p = da.append_new(5000);
    ASSERT_TRUE(p != nullptr);
    ASSERT_EQ(5000u, da.size());
    ASSERT_EQ(1u, da.segment_count());
    // The pool block is 8KB, but only what was asked for is attachment data.
    ASSERT_EQ(p, da.segment(0).ptr);
    ASSERT_EQ(5000u, da.segment(0).length);
    ASSERT_EQ(rdma::GetDeviceLKey(p), da.segment(0).lkey);

    da.clear();
    ASSERT_TRUE(da.empty());
    ASSERT_EQ(0u, da.segment_count());
}

TEST_F(GdrTest, attachment_returns_block_to_pool_on_destruction) {
    void* first = nullptr;
    {
        DeviceAttachment da;
        first = da.append_new(4096);
        ASSERT_TRUE(first != nullptr);
    }
    DeviceAttachment da2;
    ASSERT_EQ(first, da2.append_new(4096));
}

TEST_F(GdrTest, attachment_cutn_splits_a_segment_without_copying) {
    DeviceAttachment src;
    char* base = (char*)src.append_new(8000);
    ASSERT_TRUE(base != nullptr);

    DeviceAttachment dst;
    ASSERT_EQ(3000u, src.cutn(&dst, 3000));

    ASSERT_EQ(3000u, dst.size());
    ASSERT_EQ(1u, dst.segment_count());
    ASSERT_EQ(base, dst.segment(0).ptr);
    ASSERT_EQ(3000u, dst.segment(0).length);

    // The tail stays where it was; nothing moved in device memory.
    ASSERT_EQ(5000u, src.size());
    ASSERT_EQ(1u, src.segment_count());
    ASSERT_EQ(base + 3000, src.segment(0).ptr);
    ASSERT_EQ(5000u, src.segment(0).length);
}

TEST_F(GdrTest, attachment_cutn_spans_segments) {
    DeviceAttachment src;
    char* a = (char*)src.append_new(4096);
    char* b = (char*)src.append_new(4096);
    ASSERT_TRUE(a != nullptr && b != nullptr);
    ASSERT_EQ(8192u, src.size());

    DeviceAttachment dst;
    ASSERT_EQ(5000u, src.cutn(&dst, 5000));
    ASSERT_EQ(2u, dst.segment_count());
    ASSERT_EQ(a, dst.segment(0).ptr);
    ASSERT_EQ(4096u, dst.segment(0).length);
    ASSERT_EQ(b, dst.segment(1).ptr);
    ASSERT_EQ(904u, dst.segment(1).length);

    ASSERT_EQ(3192u, src.size());
    ASSERT_EQ(1u, src.segment_count());
    ASSERT_EQ(b + 904, src.segment(0).ptr);
}

TEST_F(GdrTest, attachment_cutn_short_when_asking_for_too_much) {
    DeviceAttachment src;
    ASSERT_TRUE(src.append_new(4096) != nullptr);
    DeviceAttachment dst;
    // A short return is how the parser learns the rest has not arrived yet.
    ASSERT_EQ(4096u, src.cutn(&dst, 100000));
    ASSERT_TRUE(src.empty());
    ASSERT_EQ(4096u, dst.size());
}

TEST_F(GdrTest, attachment_split_block_survives_until_both_halves_die) {
    void* base = nullptr;
    {
        DeviceAttachment src;
        base = src.append_new(8192);
        ASSERT_TRUE(base != nullptr);
        DeviceAttachment dst;
        ASSERT_EQ(4000u, src.cutn(&dst, 4000));

        // Dropping one half must not recycle the block: the other half still
        // points into it.
        src.clear();
        DeviceAttachment probe;
        ASSERT_NE(base, probe.append_new(8192));
    }
    // Both halves gone -- now it comes back.
    DeviceAttachment probe;
    ASSERT_EQ(base, probe.append_new(8192));
}

TEST_F(GdrTest, attachment_pop_front) {
    DeviceAttachment da;
    char* a = (char*)da.append_new(4096);
    da.append_new(4096);
    ASSERT_EQ(1000u, da.pop_front(1000));
    ASSERT_EQ(7192u, da.size());
    ASSERT_EQ(a + 1000, da.segment(0).ptr);
    ASSERT_EQ(3096u, da.segment(0).length);

    ASSERT_EQ(3096u, da.pop_front(3096));
    ASSERT_EQ(1u, da.segment_count());
    ASSERT_EQ(4096u, da.size());

    ASSERT_EQ(4096u, da.pop_front(999999));
    ASSERT_TRUE(da.empty());
}

TEST_F(GdrTest, attachment_append_and_move) {
    DeviceAttachment a;
    void* pa = a.append_new(4096);
    DeviceAttachment b;
    void* pb = b.append_new(4096);

    a.append(std::move(b));
    ASSERT_TRUE(b.empty());
    ASSERT_EQ(8192u, a.size());
    ASSERT_EQ(2u, a.segment_count());
    ASSERT_EQ(pa, a.segment(0).ptr);
    ASSERT_EQ(pb, a.segment(1).ptr);

    DeviceAttachment c(std::move(a));
    ASSERT_TRUE(a.empty());
    ASSERT_EQ(8192u, c.size());
    ASSERT_EQ(pa, c.segment(0).ptr);

    DeviceAttachment d;
    d.append_new(4096);
    d = std::move(c);
    ASSERT_EQ(8192u, d.size());
    ASSERT_EQ(pa, d.segment(0).ptr);
}

TEST_F(GdrTest, attachment_append_ref_shares_blocks) {
    DeviceAttachment a;
    void* p0 = a.append_new(4096);
    void* p1 = a.append_new(8192);

    DeviceAttachment b;
    b.append_ref(a);
    // Both see the same device memory; nothing was copied (there is no way to
    // copy it from the host anyway).
    ASSERT_EQ(12288u, a.size());
    ASSERT_EQ(12288u, b.size());
    ASSERT_EQ(2u, b.segment_count());
    ASSERT_EQ(p0, b.segment(0).ptr);
    ASSERT_EQ(p1, b.segment(1).ptr);

    // Appending a reference onto a non-empty attachment concatenates.
    b.append_ref(a);
    ASSERT_EQ(24576u, b.size());
    ASSERT_EQ(4u, b.segment_count());
    ASSERT_EQ(p0, b.segment(2).ptr);
}

TEST_F(GdrTest, attachment_append_ref_keeps_blocks_alive) {
    // This is the property PackRpcRequest() relies on for retries: the
    // Controller keeps its attachment while the socket owns a reference, and
    // whichever dies last returns the block.
    void* p = nullptr;
    {
        DeviceAttachment b;
        {
            DeviceAttachment a;
            p = a.append_new(4096);
            b.append_ref(a);
        }
        // `a' is gone but the block must not be back in the pool yet.
        DeviceAttachment probe;
        void* other = probe.append_new(4096);
        ASSERT_NE(p, other);
    }
    // Now that both are gone, the block is reusable.
    DeviceAttachment probe;
    ASSERT_EQ(p, probe.append_new(4096));
}

TEST_F(GdrTest, attachment_append_ref_of_self_is_a_noop) {
    DeviceAttachment a;
    a.append_new(4096);
    a.append_ref(a);
    ASSERT_EQ(4096u, a.size());
    ASSERT_EQ(1u, a.segment_count());
}

TEST_F(GdrTest, attachment_swap) {
    DeviceAttachment a;
    void* pa = a.append_new(4096);
    DeviceAttachment b;
    b.append_new(8192);
    a.swap(b);
    ASSERT_EQ(8192u, a.size());
    ASSERT_EQ(4096u, b.size());
    ASSERT_EQ(pa, b.segment(0).ptr);
}

static int g_user_deleter_calls = 0;
static void* g_user_deleter_arg = nullptr;
static void CountingDeleter(void* dptr) {
    ++g_user_deleter_calls;
    g_user_deleter_arg = dptr;
}

TEST_F(GdrTest, attachment_user_data_deleter_runs_once) {
    void* buf = nullptr;
    ASSERT_EQ(0, posix_memalign(&buf, 4096, 8192));
    g_user_deleter_calls = 0;
    g_user_deleter_arg = nullptr;
    {
        DeviceAttachment da;
        ASSERT_EQ(0, da.append_user_data(buf, 8192, CountingDeleter));
        ASSERT_EQ(8192u, da.size());
        ASSERT_NE(0u, da.segment(0).lkey);

        // Splitting shares the block; the deleter must still run exactly once.
        DeviceAttachment other;
        ASSERT_EQ(4096u, da.cutn(&other, 4096));
        da.clear();
        ASSERT_EQ(0, g_user_deleter_calls);
    }
    ASSERT_EQ(1, g_user_deleter_calls);
    ASSERT_EQ(buf, g_user_deleter_arg);
    // A user pointer is never recycled into the pool, so it is still ours.
    free(buf);
}

TEST_F(GdrTest, attachment_user_data_without_deleter) {
    void* buf = nullptr;
    ASSERT_EQ(0, posix_memalign(&buf, 4096, 4096));
    {
        DeviceAttachment da;
        ASSERT_EQ(0, da.append_user_data(buf, 4096, nullptr));
        ASSERT_EQ(4096u, da.size());
    }
    free(buf);
}

TEST_F(GdrTest, attachment_user_data_rejects_bad_args) {
    DeviceAttachment da;
    ASSERT_EQ(-1, da.append_user_data(nullptr, 4096, nullptr));
    ASSERT_EQ(EINVAL, errno);
    int stack_var = 0;
    ASSERT_EQ(-1, da.append_user_data(&stack_var, 0, nullptr));
    ASSERT_EQ(EINVAL, errno);
    ASSERT_TRUE(da.empty());
}

// A pool block is device memory even in the stub, where it is really host
// memory underneath: saying otherwise would let stub runs pass code that
// dereferences a segment a real run would fault on.
TEST_F(GdrTest, pool_blocks_are_not_host_readable) {
    DeviceAttachment da;
    ASSERT_TRUE(da.is_host_readable());  // vacuously, nothing to touch
    ASSERT_TRUE(da.append_new(4096) != nullptr);
    ASSERT_FALSE(da.segment(0).is_host);
    ASSERT_FALSE(da.is_host_readable());
}

// The whole point of the split: host bytes may ride the device channel, and
// they are registered through the host registry rather than burning one of
// the 64 device region slots.
TEST_F(GdrTest, host_user_data_is_marked_host_and_skips_the_device_regions) {
    int regions_before = 0;
    rdma::GetDeviceMemoryStat(nullptr, nullptr, &regions_before);

    void* buf = nullptr;
    ASSERT_EQ(0, posix_memalign(&buf, 4096, 8192));
    {
        DeviceAttachment da;
        ASSERT_EQ(0, da.append_user_data(buf, 8192, nullptr));
        ASSERT_NE(0u, da.segment(0).lkey);
        ASSERT_TRUE(da.segment(0).is_host);
        ASSERT_TRUE(da.is_host_readable());
        // Registered as host memory, so the device lkey table still does not
        // know the address.
        ASSERT_EQ(0u, rdma::GetDeviceLKey(buf));
    }
    int regions_after = 0;
    rdma::GetDeviceMemoryStat(nullptr, nullptr, &regions_after);
    ASSERT_EQ(regions_before, regions_after);
    free(buf);
}

// A pointer inside a registered device region is device memory, and it is
// found without re-registering -- that lookup range-scans, unlike the host one.
TEST_F(GdrTest, device_user_data_is_recognized_by_an_interior_pointer) {
    void* buf = nullptr;
    ASSERT_EQ(0, posix_memalign(&buf, 4096, 16384));
    const uint32_t lkey = rdma::RegisterDeviceMemory(buf, 16384);
    ASSERT_NE(0u, lkey);
    {
        DeviceAttachment da;
        ASSERT_EQ(0, da.append_user_data((char*)buf + 4096, 4096, nullptr));
        ASSERT_EQ(lkey, da.segment(0).lkey);
        ASSERT_FALSE(da.segment(0).is_host);
        ASSERT_FALSE(da.is_host_readable());
    }
    // Still registered: the attachment did not register it, so it must not
    // have deregistered it either.
    ASSERT_EQ(lkey, rdma::GetDeviceLKey(buf));
    rdma::DeregisterDeviceMemory(buf);
    free(buf);
}

// Send-side attachments may mix, which is why the bit lives on the segment.
TEST_F(GdrTest, attachment_can_mix_host_and_device_segments) {
    void* host_buf = nullptr;
    ASSERT_EQ(0, posix_memalign(&host_buf, 4096, 4096));
    DeviceAttachment da;
    ASSERT_TRUE(da.append_new(4096) != nullptr);
    ASSERT_EQ(0, da.append_user_data(host_buf, 4096, nullptr));
    ASSERT_EQ(2u, da.segment_count());
    ASSERT_FALSE(da.segment(0).is_host);
    ASSERT_TRUE(da.segment(1).is_host);
    ASSERT_FALSE(da.is_host_readable());
    ASSERT_EQ(8192u, da.size());
    da.clear();
    free(host_buf);
}

// The explicit-lkey form is for a caller holding one big registration and
// slicing it up; it must not touch the registration on either side.
TEST_F(GdrTest, attachment_user_data_with_lkey_borrows_the_registration) {
    void* buf = nullptr;
    ASSERT_EQ(0, posix_memalign(&buf, 4096, 16384));
    const uint32_t lkey = rdma::RegisterHostMemory(buf, 16384);
    ASSERT_NE(0u, lkey);
    g_user_deleter_calls = 0;
    {
        DeviceAttachment da;
        for (int i = 0; i < 4; ++i) {
            ASSERT_EQ(0, da.append_user_data_with_lkey(
                             (char*)buf + i * 4096, 4096, lkey,
                             /*is_host=*/true, i == 3 ? CountingDeleter : nullptr));
            ASSERT_EQ(lkey, da.segment(i).lkey);
            ASSERT_TRUE(da.segment(i).is_host);
        }
        ASSERT_EQ(16384u, da.size());
        ASSERT_TRUE(da.is_host_readable());
        // Host memory, so it really is safe to read -- that is what the bit
        // promises the receiving application.
        memset(buf, 0x5a, 16384);
        ASSERT_EQ(0x5a, *((unsigned char*)da.segment(2).ptr));
    }
    ASSERT_EQ(1, g_user_deleter_calls);
    rdma::DeregisterHostMemory(buf);
    free(buf);
}

TEST_F(GdrTest, attachment_user_data_with_lkey_rejects_bad_args) {
    void* buf = nullptr;
    ASSERT_EQ(0, posix_memalign(&buf, 4096, 4096));
    DeviceAttachment da;
    ASSERT_EQ(-1, da.append_user_data_with_lkey(nullptr, 4096, 1, true, nullptr));
    ASSERT_EQ(EINVAL, errno);
    ASSERT_EQ(-1, da.append_user_data_with_lkey(buf, 0, 1, true, nullptr));
    ASSERT_EQ(EINVAL, errno);
    // A zero lkey would be posted to the NIC verbatim and fail the WR.
    ASSERT_EQ(-1, da.append_user_data_with_lkey(buf, 4096, 0, true, nullptr));
    ASSERT_EQ(EINVAL, errno);
    ASSERT_TRUE(da.empty());
    free(buf);
}

// cutn() carries the bit along; a device half must not become host-readable
// just because it was split off.
TEST_F(GdrTest, cut_segments_keep_their_memory_kind) {
    void* host_buf = nullptr;
    ASSERT_EQ(0, posix_memalign(&host_buf, 4096, 4096));
    DeviceAttachment da;
    ASSERT_TRUE(da.append_new(4096) != nullptr);
    ASSERT_EQ(0, da.append_user_data(host_buf, 4096, nullptr));

    DeviceAttachment head;
    // Cuts through the middle of the device segment.
    ASSERT_EQ(2048u, da.cutn(&head, 2048));
    ASSERT_FALSE(head.segment(0).is_host);
    ASSERT_FALSE(head.is_host_readable());

    DeviceAttachment tail;
    ASSERT_EQ(6144u, da.cutn(&tail, 6144));
    ASSERT_EQ(2u, tail.segment_count());
    ASSERT_FALSE(tail.segment(0).is_host);
    ASSERT_TRUE(tail.segment(1).is_host);

    head.clear();
    tail.clear();
    free(host_buf);
}

// ------------------------ --rdma_attachment_memory ------------------------
//
// What this end's attachments are made of, which is a purely local choice and
// never appears on the wire. Its own fixture because the flag has to be set
// before GlobalGdrInitialize() reads it, which GdrTest has already done by
// the time a test body runs.

// What the receive path passes to append_user_data(), so that a test holding
// a pool block by pointer disposes of it the same way the endpoint does.
static void ReturnBlockToPool(void* block) {
    ASSERT_EQ(0, rdma::DeallocDeviceBlock(block));
}

// RAII for --rdma_gdr_device_id, so a test that pins a GPU cannot leak that
// choice into the next one.
class GdrDeviceIdFlag {
public:
    explicit GdrDeviceIdFlag(int v)
        : _saved(rdma::FLAGS_rdma_gdr_device_id) {
        rdma::FLAGS_rdma_gdr_device_id = v;
    }
    ~GdrDeviceIdFlag() { rdma::FLAGS_rdma_gdr_device_id = _saved; }
private:
    int _saved;
};

class GdrAttachmentMemoryTest : public ::testing::Test {
protected:
    void SetUp() override {
        _saved = rdma::FLAGS_rdma_attachment_memory;
        if (FLAGS_gdr_test_real_device) {
            ASSERT_TRUE(FLAGS_rdma_test_enable)
                    << "--gdr_test_real_device needs --rdma_test_enable too";
            rdma::GlobalRdmaInitializeOrDie();
            rdma::g_skip_device_alloc_for_test = false;
        } else {
            rdma::g_skip_device_alloc_for_test = true;
        }
        rdma::FLAGS_rdma_enable_gdr = true;
    }

    void TearDown() override {
        rdma::GlobalGdrRelease();
        rdma::FLAGS_rdma_enable_gdr = false;
        rdma::FLAGS_rdma_attachment_memory = _saved;
        rdma::g_skip_device_alloc_for_test = false;
    }

private:
    std::string _saved;
};

TEST_F(GdrAttachmentMemoryTest, device_is_the_default) {
    ASSERT_EQ("device", rdma::FLAGS_rdma_attachment_memory);
    ASSERT_EQ(0, rdma::GlobalGdrInitialize());
    ASSERT_TRUE(rdma::IsGdrAvailable());
    ASSERT_TRUE(rdma::IsAttachmentMemoryDevice());
}

TEST_F(GdrAttachmentMemoryTest, host_mode_still_brings_the_channel_up) {
    rdma::FLAGS_rdma_attachment_memory = "host";
    ASSERT_EQ(0, rdma::GlobalGdrInitialize());
    // The second channel exists either way -- that is the whole point of
    // splitting the two bits apart. Only the memory behind it differs.
    ASSERT_TRUE(rdma::IsGdrAvailable());
    ASSERT_FALSE(rdma::IsAttachmentMemoryDevice());
}

TEST_F(GdrAttachmentMemoryTest, host_mode_blocks_are_readable_and_say_so) {
    rdma::FLAGS_rdma_attachment_memory = "host";
    ASSERT_EQ(0, rdma::GlobalGdrInitialize());

    DeviceAttachment da;
    void* p = da.append_new(4096);
    ASSERT_TRUE(p != nullptr);
    ASSERT_EQ(1u, da.segment_count());
    ASSERT_TRUE(da.segment(0).is_host);
    ASSERT_TRUE(da.is_host_readable());

    // Unlike device mode, dereferencing really is allowed here, and is the
    // only assertion that would catch the pool still calling cudaMalloc.
    memset(p, 0x5a, 4096);
    ASSERT_EQ(0x5a, ((unsigned char*)p)[4095]);
}

TEST_F(GdrAttachmentMemoryTest, device_mode_blocks_are_not_host_readable) {
    ASSERT_EQ(0, rdma::GlobalGdrInitialize());
    DeviceAttachment da;
    ASSERT_TRUE(da.append_new(4096) != nullptr);
    // True even in the stub, where the pool really did hand out host memory:
    // the bit follows the configured mode, not what the allocator happened to
    // do, so a stub run cannot tell callers something a real run would not.
    ASSERT_FALSE(da.segment(0).is_host);
    ASSERT_FALSE(da.is_host_readable());
}

// The region table holds pool regions and user RegisterDeviceMemory() regions
// in one array, so a lookup hit says "registered", not "device memory". That
// distinction is invisible in device mode, where both answers coincide, and it
// is the whole story in host mode.
TEST_F(GdrAttachmentMemoryTest, lkey_lookup_tells_pool_blocks_from_user_regions) {
    rdma::FLAGS_rdma_attachment_memory = "host";
    ASSERT_EQ(0, rdma::GlobalGdrInitialize());

    DeviceAttachment pool_holder;
    void* const pool_block = pool_holder.append_new(4096);
    ASSERT_TRUE(pool_block != nullptr);

    void* user = nullptr;
    ASSERT_EQ(0, posix_memalign(&user, 4096, 16384));
    ASSERT_NE(0u, rdma::RegisterDeviceMemory(user, 16384));

    // Seeded with the wrong answer each time: the out-param is only written
    // on a hit, and a test that pre-set it to the expected value would pass
    // even if nothing ever wrote it.
    bool is_pool = false;
    ASSERT_NE(0u, rdma::GetDeviceLKey(pool_block, &is_pool));
    ASSERT_TRUE(is_pool);
    // Interior pointers too -- that is the case the receive path hits after
    // an attachment has been cut in the middle of a block.
    is_pool = false;
    ASSERT_NE(0u, rdma::GetDeviceLKey((char*)pool_block + 2048, &is_pool));
    ASSERT_TRUE(is_pool);

    is_pool = true;
    ASSERT_NE(0u, rdma::GetDeviceLKey(user, &is_pool));
    ASSERT_FALSE(is_pool);
    is_pool = true;
    ASSERT_NE(0u, rdma::GetDeviceLKey((char*)user + 16383, &is_pool));
    ASSERT_FALSE(is_pool);

    rdma::DeregisterDeviceMemory(user);
    free(user);
}

// The regression behind the four-combination E2E: a received attachment is
// built by handing the block the data landed in to append_user_data(), which
// works out the memory kind from the registries. In host mode that block is
// host memory sitting in the pool's region table, so a lookup that reports
// "found in the device table, therefore device memory" mislabels every
// attachment this process receives -- and the application, told it may not
// memcpy, has no way to get at bytes that were readable all along.
TEST_F(GdrAttachmentMemoryTest, host_mode_pool_block_appended_by_pointer_says_host) {
    rdma::FLAGS_rdma_attachment_memory = "host";
    ASSERT_EQ(0, rdma::GlobalGdrInitialize());

    uint32_t lkey = 0;
    void* const block = rdma::AllocDeviceBlock(4096, &lkey);
    ASSERT_TRUE(block != nullptr);
    {
        DeviceAttachment da;
        ASSERT_EQ(0, da.append_user_data(block, 1000, ReturnBlockToPool));
        ASSERT_EQ(1u, da.segment_count());
        ASSERT_TRUE(da.segment(0).is_host);
        ASSERT_TRUE(da.is_host_readable());
        ASSERT_EQ(lkey, da.segment(0).lkey);
        // Same answer append_new() gives for the same block, which is the
        // point: how the pointer reached the attachment must not change what
        // the attachment says about it.
        DeviceAttachment other;
        ASSERT_TRUE(other.append_new(4096) != nullptr);
        ASSERT_EQ(other.segment(0).is_host, da.segment(0).is_host);
    }

    // A user region in the same table is still device memory, in host mode as
    // in any other: the kind follows what the memory is, not what the pool
    // happens to be made of.
    void* user = nullptr;
    ASSERT_EQ(0, posix_memalign(&user, 4096, 16384));
    ASSERT_NE(0u, rdma::RegisterDeviceMemory(user, 16384));
    {
        DeviceAttachment da;
        ASSERT_EQ(0, da.append_user_data(user, 16384, nullptr));
        ASSERT_FALSE(da.segment(0).is_host);
    }
    rdma::DeregisterDeviceMemory(user);
    free(user);
}

TEST_F(GdrAttachmentMemoryTest, unknown_memory_kind_fails_initialization) {
    rdma::FLAGS_rdma_attachment_memory = "gpu";
    ASSERT_EQ(-1, rdma::GlobalGdrInitialize());
    ASSERT_EQ(EINVAL, errno);
    // Nothing half-built: GlobalRdmaInitializeOrDie() turns this into
    // ExitWithError(), and a process that somehow continued must not find a
    // usable channel.
    ASSERT_FALSE(rdma::IsGdrAvailable());
    ASSERT_FALSE(rdma::IsAttachmentMemoryDevice());
}

// Device mode needs a GPU and says so; host mode never asks.
//
// Needs a real device because the stub skips the CUDA probe altogether, and
// an unreachable GPU is faked with an out-of-range ordinal rather than by
// hiding the real ones: CUDA_VISIBLE_DEVICES has to be set before the runtime
// initializes, which by this point in the binary it has.
//
// The other half of this contract -- GlobalRdmaInitializeOrDie() turning the
// -1 into ExitWithError() rather than serving without a channel the operator
// asked for -- is rdma_helper.cpp's, alongside a dozen sibling call sites
// that fail the same way.
TEST_F(GdrAttachmentMemoryTest, device_mode_needs_a_gpu_and_host_mode_does_not) {
    if (!FLAGS_gdr_test_real_device) {
        return;
    }
    GdrDeviceIdFlag device_id_guard(99999);

    ASSERT_EQ(-1, rdma::GlobalGdrInitialize());
    ASSERT_EQ(ENODEV, errno);
    ASSERT_FALSE(rdma::IsGdrAvailable());

    // Same host, same missing GPU, and the second channel comes up anyway:
    // nothing in host mode allocates on a device, so there is nothing to
    // refuse. This is what lets a GPU-less node talk to a GPU one.
    rdma::FLAGS_rdma_attachment_memory = "host";
    ASSERT_EQ(0, rdma::GlobalGdrInitialize());
    ASSERT_TRUE(rdma::IsGdrAvailable());
    ASSERT_FALSE(rdma::IsAttachmentMemoryDevice());
}

TEST_F(GdrAttachmentMemoryTest, release_forgets_the_memory_kind) {
    rdma::FLAGS_rdma_attachment_memory = "host";
    ASSERT_EQ(0, rdma::GlobalGdrInitialize());
    ASSERT_FALSE(rdma::IsAttachmentMemoryDevice());

    rdma::GlobalGdrRelease();
    ASSERT_FALSE(rdma::IsGdrAvailable());

    // A second init with the flag put back must not inherit "host" from the
    // first one. Only tests ever do this, but a stale mode would make them
    // pass or fail depending on their order.
    rdma::FLAGS_rdma_attachment_memory = "device";
    ASSERT_EQ(0, rdma::GlobalGdrInitialize());
    ASSERT_TRUE(rdma::IsAttachmentMemoryDevice());
}

// ---------------------- baidu_std over a device channel ----------------------
//
// Exercises the two halves of the protocol change against a Socket whose
// device channel is forced on: the parser's pending list, and the
// SocketMessage that assigns device offsets at write time.
//
// No card and no GPU is needed. The endpoint's receive stream is an ordinary
// DeviceAttachment, so a test fills it exactly the way a device-channel recv
// completion does, and g_skip_device_alloc_for_test keeps every "device"
// pointer a host allocation that nothing under test is allowed to dereference
// anyway. As with GdrTest, --gdr_test_real_device swaps in real device memory.

// The attachment's own D2H path, which is also what the TCP fallback uses on
// the send side. Handy for asserting on received bytes without caring whether
// this run has real device memory.
static std::string DeviceToString(const DeviceAttachment& da) {
    butil::IOBuf out;
    EXPECT_EQ(0, da.copy_to(&out));
    return out.to_string();
}

// The inverse, through the same H2D path the TCP fallback uses on the receive
// side, so a test can put known bytes into an attachment without caring
// whether this run's attachment memory is real device memory.
static void MakeDeviceAttachment(DeviceAttachment* da, const std::string& s) {
    butil::IOBuf buf;
    buf.append(s);
    ASSERT_EQ(0, da->append_from_iobuf(&buf, s.size()));
    ASSERT_EQ(0u, buf.size());
}

class GdrProtocolTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_NO_FATAL_FAILURE(EnableGdrForTest());
        // A device channel demands the v3 handshake, and ChannelOptions
        // validation now enforces that at Init() time rather than letting
        // every connection fail. The tests below build device-mode Channels,
        // so the process has to be configured for one.
        _saved_handshake_version = rdma::FLAGS_rdma_client_handshake_version;
        rdma::FLAGS_rdma_client_handshake_version = 3;

        _ep = nullptr;
        ASSERT_EQ(0, MakeSocket(true, &_id, &_sock, &_ep));
        ASSERT_EQ(_ep->device_stream(), _sock->device_stream());
    }

    void TearDown() override {
        _sock.reset();
        Socket::SetFailed(_id);
        rdma::GlobalGdrRelease();
        rdma::FLAGS_rdma_enable_gdr = false;
        rdma::g_skip_device_alloc_for_test = false;
        rdma::FLAGS_rdma_client_handshake_version = _saved_handshake_version;
    }

    // An unconnected Socket, optionally with its RDMA device channel forced
    // on. Nothing here does I/O; the tests drive the parser and the packer
    // directly.
    static int MakeSocket(bool with_device_channel, SocketId* id,
                          SocketUniquePtr* out, rdma::RdmaEndpoint** ep) {
        SocketOptions options;
        // The socket mode is the whole of the configuration: the endpoint's
        // constructor reads it back and engages the device channel itself,
        // which is what a real device-mode socket looks like before its
        // handshake runs.
        options.socket_mode = with_device_channel
                ? SOCKET_MODE_RDMA_AND_DEVICE : SOCKET_MODE_TCP;
        if (Socket::Create(options, id) != 0) {
            return -1;
        }
        if (Socket::Address(*id, out) != 0) {
            return -1;
        }
        if (with_device_channel) {
            RdmaTransport* t =
                static_cast<RdmaTransport*>((*out)->_transport.get());
            t->_rdma_state = RdmaTransport::RDMA_ON;
            *ep = t->_rdma_ep;
        }
        return 0;
    }

    // What the completion handler does when a device block lands.
    void ArriveDeviceBytes(size_t n) {
        ASSERT_TRUE(_ep->device_stream()->recv_stream()->append_new(n)
                    != nullptr);
    }

    static void MakeRpcFrame(butil::IOBuf* out, int64_t correlation_id,
                             const std::string& payload, uint32_t device_size) {
        policy::RpcMeta meta;
        meta.set_correlation_id(correlation_id);
        meta.mutable_request()->set_service_name("test.EchoService");
        meta.mutable_request()->set_method_name("Echo");
        AppendFrame(out, meta, payload, device_size);
    }

    // PRPC framing, or GDRB (4 bytes longer, device size at a fixed offset)
    // when this message carries a device half.
    static void AppendFrame(butil::IOBuf* out, const policy::RpcMeta& meta,
                            const std::string& payload,
                            uint32_t device_size = 0) {
        const std::string meta_str = meta.SerializeAsString();
        char header[16];
        uint32_t* magic = (uint32_t*)header;
        butil::RawPacker packer(header + 4);
        packer.pack32(meta_str.size() + payload.size())
              .pack32(meta_str.size());
        size_t header_size = 12;
        if (device_size > 0) {
            *magic = *(const uint32_t*)"GDRB";
            packer.pack32(device_size);
            header_size = 16;
        } else {
            *magic = *(const uint32_t*)"PRPC";
        }
        out->append(header, header_size);
        out->append(meta_str);
        out->append(payload);
    }

    // Inverse of AppendFrame(). *device_size is set to 0 for a PRPC frame.
    static bool SplitFrame(butil::IOBuf* in, policy::RpcMeta* meta,
                           std::string* payload, uint32_t* device_size = nullptr) {
        char header[16];
        const size_t n = in->copy_to(header, sizeof(header));
        if (n < 12) {
            return false;
        }
        size_t header_size;
        if (memcmp(header, "PRPC", 4) == 0) {
            header_size = 12;
        } else if (memcmp(header, "GDRB", 4) == 0) {
            header_size = 16;
        } else {
            return false;
        }
        if (n < header_size) {
            return false;
        }
        uint32_t body_size = 0;
        uint32_t meta_size = 0;
        butil::RawUnpacker(header + 4).unpack32(body_size).unpack32(meta_size);
        uint32_t dev = 0;
        if (header_size == 16) {
            butil::RawUnpacker(header + 12).unpack32(dev);
        }
        if (device_size != nullptr) {
            *device_size = dev;
        }
        if (in->size() < header_size + body_size) {
            return false;
        }
        in->pop_front(header_size);
        butil::IOBuf meta_buf;
        in->cutn(&meta_buf, meta_size);
        if (!meta->ParseFromString(meta_buf.to_string())) {
            return false;
        }
        butil::IOBuf payload_buf;
        in->cutn(&payload_buf, body_size - meta_size);
        *payload = payload_buf.to_string();
        return true;
    }

    ParseResult Parse(butil::IOBuf* buf) {
        return policy::ParseRpcMessage(buf, _sock.get(), false, nullptr);
    }

    static policy::MostCommonMessage* AsMsg(const ParseResult& pr) {
        return static_cast<policy::MostCommonMessage*>(pr.message());
    }

    SocketId _id;
    SocketUniquePtr _sock;
    rdma::RdmaEndpoint* _ep;
    int _saved_handshake_version;
};

TEST_F(GdrProtocolTest, message_without_device_attachment_is_dispatched_at_once) {
    butil::IOBuf buf;
    MakeRpcFrame(&buf, 1, "hello", 0);

    ParseResult pr = Parse(&buf);
    ASSERT_TRUE(pr.is_ok());
    policy::MostCommonMessage* msg = AsMsg(pr);
    ASSERT_TRUE(msg != nullptr);
    ASSERT_EQ("hello", msg->payload.to_string());
    ASSERT_TRUE(msg->device_payload.empty());
    // Nothing was parked, so no context was allocated.
    ASSERT_TRUE(nullptr == _sock->parsing_context());
    msg->Destroy();
}

TEST_F(GdrProtocolTest, device_bytes_already_here_are_cut_on_the_spot) {
    ArriveDeviceBytes(4096);
    butil::IOBuf buf;
    MakeRpcFrame(&buf, 1, "hello", 4096);

    ParseResult pr = Parse(&buf);
    ASSERT_TRUE(pr.is_ok());
    policy::MostCommonMessage* msg = AsMsg(pr);
    ASSERT_TRUE(msg != nullptr);
    ASSERT_EQ(4096u, msg->device_payload.size());
    ASSERT_EQ(4096u, _ep->device_stream()->cut_offset());
    ASSERT_EQ(0u, _ep->device_stream()->size());
    // The fast path never touches the pending list.
    ASSERT_TRUE(nullptr == _sock->parsing_context());
    msg->Destroy();
}

TEST_F(GdrProtocolTest, message_is_parked_until_its_device_bytes_arrive) {
    butil::IOBuf buf;
    MakeRpcFrame(&buf, 1, "hello", 4096);

    // Parsed OK, but there is no message yet: the host half is consumed and
    // the message waits on the pending list.
    ParseResult pr = Parse(&buf);
    ASSERT_TRUE(pr.is_ok());
    ASSERT_TRUE(nullptr == pr.message());
    ASSERT_EQ(0u, buf.size());
    ASSERT_TRUE(_sock->parsing_context() != nullptr);

    // Nothing more to do until the device bytes show up.
    pr = Parse(&buf);
    ASSERT_FALSE(pr.is_ok());
    ASSERT_EQ(PARSE_ERROR_NOT_ENOUGH_DATA, pr.error());

    // A device completion re-runs the parser with an empty read buffer; that
    // is what drains the list.
    ArriveDeviceBytes(4096);
    pr = Parse(&buf);
    ASSERT_TRUE(pr.is_ok());
    policy::MostCommonMessage* msg = AsMsg(pr);
    ASSERT_TRUE(msg != nullptr);
    ASSERT_EQ("hello", msg->payload.to_string());
    ASSERT_EQ(4096u, msg->device_payload.size());
    ASSERT_EQ(4096u, _ep->device_stream()->cut_offset());
    msg->Destroy();
}

TEST_F(GdrProtocolTest, partial_device_bytes_keep_the_message_parked) {
    butil::IOBuf buf;
    MakeRpcFrame(&buf, 1, "hello", 8192);
    ASSERT_TRUE(Parse(&buf).is_ok());

    ArriveDeviceBytes(4096);
    ParseResult pr = Parse(&buf);
    ASSERT_FALSE(pr.is_ok());
    ASSERT_EQ(PARSE_ERROR_NOT_ENOUGH_DATA, pr.error());
    // Cutting half a message would desynchronize the stream for everyone
    // behind it, so nothing is cut at all.
    ASSERT_EQ(4096u, _ep->device_stream()->size());
    ASSERT_EQ(0u, _ep->device_stream()->cut_offset());

    ArriveDeviceBytes(4096);
    pr = Parse(&buf);
    ASSERT_TRUE(pr.is_ok());
    ASSERT_TRUE(pr.message() != nullptr);
    ASSERT_EQ(8192u, AsMsg(pr)->device_payload.size());
    pr.message()->Destroy();
}

TEST_F(GdrProtocolTest, device_free_messages_jump_the_pending_queue) {
    // The whole point of the pending list: one large tensor transfer must not
    // add latency to the small RPCs sharing the connection. A message with no
    // device bytes consumes none, so letting it overtake cannot disturb the
    // alignment of the device stream.
    butil::IOBuf buf;
    MakeRpcFrame(&buf, 1, "big", 4096);
    ASSERT_TRUE(Parse(&buf).is_ok());
    ASSERT_TRUE(nullptr == Parse(&buf).message());  // still empty, parked above

    MakeRpcFrame(&buf, 2, "small", 0);
    ParseResult pr = Parse(&buf);
    ASSERT_TRUE(pr.is_ok());
    ASSERT_TRUE(pr.message() != nullptr);
    ASSERT_EQ("small", AsMsg(pr)->payload.to_string());
    pr.message()->Destroy();

    ArriveDeviceBytes(4096);
    pr = Parse(&buf);
    ASSERT_TRUE(pr.is_ok());
    ASSERT_TRUE(pr.message() != nullptr);
    ASSERT_EQ("big", AsMsg(pr)->payload.to_string());
    ASSERT_EQ(4096u, AsMsg(pr)->device_payload.size());
    pr.message()->Destroy();
}

TEST_F(GdrProtocolTest, pending_list_drains_in_arrival_order) {
    butil::IOBuf buf;
    MakeRpcFrame(&buf, 1, "first", 4096);
    MakeRpcFrame(&buf, 2, "second", 8192);
    ASSERT_TRUE(nullptr == Parse(&buf).message());
    ASSERT_TRUE(nullptr == Parse(&buf).message());
    ASSERT_EQ(0u, buf.size());

    // Enough for the first only.
    ArriveDeviceBytes(4096);
    ParseResult pr = Parse(&buf);
    ASSERT_TRUE(pr.is_ok());
    ASSERT_EQ("first", AsMsg(pr)->payload.to_string());
    ASSERT_EQ(4096u, AsMsg(pr)->device_payload.size());
    pr.message()->Destroy();

    pr = Parse(&buf);
    ASSERT_EQ(PARSE_ERROR_NOT_ENOUGH_DATA, pr.error());

    ArriveDeviceBytes(8192);
    pr = Parse(&buf);
    ASSERT_TRUE(pr.is_ok());
    ASSERT_EQ("second", AsMsg(pr)->payload.to_string());
    ASSERT_EQ(8192u, AsMsg(pr)->device_payload.size());
    ASSERT_EQ(12288u, _ep->device_stream()->cut_offset());
    pr.message()->Destroy();
}

TEST_F(GdrProtocolTest, device_bytes_spanning_blocks_are_stitched_together) {
    // The device stream is a byte stream: a message's bytes may span several
    // receive blocks, and one block may hold the tail of one message and the
    // head of the next.
    ArriveDeviceBytes(4096);
    ArriveDeviceBytes(4096);
    butil::IOBuf buf;
    MakeRpcFrame(&buf, 1, "a", 6000);
    MakeRpcFrame(&buf, 2, "b", 2192);

    ParseResult pr = Parse(&buf);
    ASSERT_TRUE(pr.is_ok());
    ASSERT_EQ(6000u, AsMsg(pr)->device_payload.size());
    ASSERT_EQ(2u, AsMsg(pr)->device_payload.segment_count());
    pr.message()->Destroy();

    pr = Parse(&buf);
    ASSERT_TRUE(pr.is_ok());
    ASSERT_EQ(2192u, AsMsg(pr)->device_payload.size());
    ASSERT_EQ(8192u, _ep->device_stream()->cut_offset());
    pr.message()->Destroy();
}

TEST_F(GdrProtocolTest, gdrb_frame_declaring_no_device_bytes_is_rejected) {
    // The sender only reaches for GDRB when it has device bytes, so this is a
    // malformed peer. Rejecting it keeps "GDRB implies a device half" an
    // invariant the parser can rely on.
    butil::IOBuf buf;
    policy::RpcMeta meta;
    meta.set_correlation_id(1);
    meta.mutable_request()->set_service_name("test.EchoService");
    meta.mutable_request()->set_method_name("Echo");
    const std::string meta_str = meta.SerializeAsString();
    char header[16];
    *(uint32_t*)header = *(const uint32_t*)"GDRB";
    butil::RawPacker(header + 4)
        .pack32(meta_str.size() + 5)
        .pack32(meta_str.size())
        .pack32(0);
    buf.append(header, sizeof(header));
    buf.append(meta_str);
    buf.append("hello");

    ParseResult pr = Parse(&buf);
    ASSERT_FALSE(pr.is_ok());
    ASSERT_EQ(PARSE_ERROR_ABSOLUTELY_WRONG, pr.error());
}

TEST_F(GdrProtocolTest, pending_size_is_reported_to_the_endpoint) {
    // This is what feeds the host-credit fuse: without it a peer that runs
    // ahead of a slow device stream grows the pending list without bound.
    butil::IOBuf buf;
    MakeRpcFrame(&buf, 1, "a", 4096);
    MakeRpcFrame(&buf, 2, "b", 8192);
    ASSERT_TRUE(nullptr == Parse(&buf).message());
    ASSERT_TRUE(nullptr == Parse(&buf).message());
    ASSERT_EQ(2, _ep->device_stream()->pending_msgs());
    ASSERT_EQ(12288, _ep->device_stream()->pending_bytes());

    ArriveDeviceBytes(12288);
    Parse(&buf).message()->Destroy();
    ASSERT_EQ(1, _ep->device_stream()->pending_msgs());
    ASSERT_EQ(8192, _ep->device_stream()->pending_bytes());
    Parse(&buf).message()->Destroy();
    ASSERT_EQ(0, _ep->device_stream()->pending_msgs());
    ASSERT_EQ(0, _ep->device_stream()->pending_bytes());
}

TEST_F(GdrProtocolTest, dropped_parsing_context_does_not_wedge_host_credits) {
    // InputMessenger drops the parsing context whenever it re-detects the
    // protocol. If the reported pending size survived that, the endpoint
    // would withhold host credits forever for a list that no longer exists.
    butil::IOBuf buf;
    MakeRpcFrame(&buf, 1, "a", 4096);
    ASSERT_TRUE(nullptr == Parse(&buf).message());
    ASSERT_EQ(1, _ep->device_stream()->pending_msgs());

    _sock->reset_parsing_context(nullptr);
    ASSERT_TRUE(Parse(&buf).error() == PARSE_ERROR_NOT_ENOUGH_DATA);
    ASSERT_EQ(0, _ep->device_stream()->pending_msgs());
    ASSERT_EQ(0, _ep->device_stream()->pending_bytes());
}

TEST_F(GdrProtocolTest, host_credits_are_withheld_over_the_watermark) {
    const int64_t saved = rdma::FLAGS_rdma_gdr_pending_msgs_watermark;
    rdma::FLAGS_rdma_gdr_pending_msgs_watermark = 2;
    _ep->_host.new_rq_wrs.store(7, butil::memory_order_relaxed);

    butil::IOBuf buf;
    MakeRpcFrame(&buf, 1, "a", 4096);
    ASSERT_TRUE(nullptr == Parse(&buf).message());
    // One parked message is still under the fuse.
    ASSERT_EQ(7u, _ep->TakeAcks(_ep->_host));

    _ep->_host.new_rq_wrs.store(5, butil::memory_order_relaxed);
    MakeRpcFrame(&buf, 2, "b", 4096);
    ASSERT_TRUE(nullptr == Parse(&buf).message());
    // Two is at the fuse: the peer gets nothing until the parser catches up.
    ASSERT_EQ(0u, _ep->TakeAcks(_ep->_host));
    // Withheld, not dropped.
    ASSERT_EQ(5, _ep->_host.new_rq_wrs.load(butil::memory_order_relaxed));

    ArriveDeviceBytes(8192);
    Parse(&buf).message()->Destroy();
    ASSERT_EQ(5u, _ep->TakeAcks(_ep->_host));
    Parse(&buf).message()->Destroy();

    rdma::FLAGS_rdma_gdr_pending_msgs_watermark = saved;
}

// IsWritable() decides whether KeepWrite parks or retries. Two QPs make that
// a two-variable question, and neither "the host window decides" (which parks
// a writer whose only remaining work the device credits do allow) nor "either
// QP decides" (which spins on a backlog that cannot move) is right.
TEST_F(GdrProtocolTest, is_writable_follows_the_queue_that_still_has_work) {
    // The real decision is short-circuited while the UT is skipping RDMA
    // initialization. Nothing below touches a QP, so turn it off here.
    struct SkipOff {
        SkipOff() : saved(rdma::g_skip_rdma_init) {
            rdma::g_skip_rdma_init = false;
        }
        ~SkipOff() { rdma::g_skip_rdma_init = saved; }
        bool saved;
    } skip_off;

    auto set_window = [](rdma::QpChannel& fc, uint16_t open) {
        fc.remote_rq_window_size.store(open, butil::memory_order_relaxed);
        fc.sq_window_size.store(open, butil::memory_order_relaxed);
    };

    // Nothing queued: the host window is the whole answer, as before GDR.
    ASSERT_FALSE(_ep->HasQueuedDeviceData());
    set_window(_ep->_host, 1);
    set_window(*_ep->_device, 0);
    ASSERT_TRUE(_ep->IsWritable());
    set_window(_ep->_host, 0);
    ASSERT_FALSE(_ep->IsWritable());

    DeviceAttachment queued;
    ASSERT_NO_FATAL_FAILURE(
            MakeDeviceAttachment(&queued, std::string(64, 'w')));
    _ep->device_stream()->AppendForSend(std::move(queued));
    ASSERT_TRUE(_ep->HasQueuedDeviceData());

    // The row that used to be wrong: the host window is shut, but the writer
    // is only here because CutFromIOBufList() could not drain the device
    // queue, and the device credits say it can.
    set_window(*_ep->_device, 1);
    ASSERT_TRUE(_ep->IsWritable());

    // And the row that stops this from being "either QP is writable": with a
    // backlog that cannot move, an open host window means the host bytes are
    // already gone. Returning true would leave KeepWrite spinning, because
    // Socket::IsWriteComplete() keeps looping while the queue is non-empty.
    set_window(_ep->_host, 1);
    set_window(*_ep->_device, 0);
    ASSERT_FALSE(_ep->IsWritable());

    // Both open is writable for either reason.
    set_window(*_ep->_device, 1);
    ASSERT_TRUE(_ep->IsWritable());

    _ep->DiscardQueuedDeviceData();
}

// "Does this connection have a device channel" and "is _device engaged" are
// the same question now, and that is the whole of the state: there is no
// wanted/established pair to disagree with it, and nothing to leave half-built
// (docs/cn/gdr_channel_unify_plan.md section 1). What still has to hold is
// that everything reading the channel goes through the optional, including the
// paths that run after it is gone.
TEST_F(GdrProtocolTest, device_channel_lives_and_dies_with_the_optional) {
    ASSERT_TRUE(_ep->has_device_channel());
    ASSERT_TRUE(_ep->device_stream() != nullptr);
    ASSERT_EQ(_ep->device_stream(), _sock->device_stream());
    ASSERT_FALSE(_ep->HasQueuedDeviceData());

    DeviceAttachment queued;
    ASSERT_NO_FATAL_FAILURE(
            MakeDeviceAttachment(&queued, std::string(64, 'q')));
    _ep->device_stream()->AppendForSend(std::move(queued));
    ASSERT_TRUE(_ep->HasQueuedDeviceData());

    // A reconnect starts here: the channel is torn down whole, queue
    // included, and comes back empty. It comes back at all because the socket
    // is still configured for one -- a connection that flapped must not start
    // serving host-only from then on.
    _ep->Reset();
    ASSERT_TRUE(_ep->has_device_channel());
    ASSERT_FALSE(_ep->HasQueuedDeviceData());
    ASSERT_EQ(_ep->device_stream(), _sock->device_stream());

    // And this is what a handshake that did not negotiate one leaves behind:
    // from here the channel really is gone.
    _ep->_device.reset();
    ASSERT_FALSE(_ep->has_device_channel());
    // HasQueuedDeviceData() is the one of these a dead connection still gets
    // asked -- Socket::IsWriteComplete() reaches it through
    // RdmaTransport::HasPendingWrite(), deliberately ungated on the RDMA state
    // so that queued bytes can never be stranded -- so it has to answer false
    // rather than walk a disengaged optional.
    ASSERT_FALSE(_ep->HasQueuedDeviceData());
    ASSERT_FALSE(RdmaTransportOf(_sock)->HasPendingWrite());
    // And nothing hands the stream out any more, so no protocol code can
    // append to a channel that is not there.
    ASSERT_TRUE(nullptr == _sock->device_stream());
}

// Whether a connection runs a device channel is its socket mode and nothing
// else, so the endpoint builds one as soon as it is constructed -- long
// before any handshake. The client needs that ordering (its hello has to
// carry a device qp_num) and the server needs the mode to be re-read rather
// than copied, or a Reset() endpoint could come back host-only.
TEST_F(GdrProtocolTest, device_channel_is_built_by_the_constructor) {
    SocketId plain_id;
    SocketUniquePtr plain;
    SocketOptions options;
    options.socket_mode = SOCKET_MODE_RDMA;
    ASSERT_EQ(0, Socket::Create(options, &plain_id));
    ASSERT_EQ(0, Socket::Address(plain_id, &plain));
    rdma::RdmaEndpoint* plain_ep = RdmaTransportOf(plain)->_rdma_ep;
    ASSERT_TRUE(plain_ep != nullptr);

    // _ep comes from the fixture's SOCKET_MODE_RDMA_AND_DEVICE socket, and
    // not one step of its handshake has run.
    ASSERT_EQ(rdma::RdmaEndpoint::UNINIT,
              _ep->_state.load(butil::memory_order_relaxed));
    ASSERT_TRUE(_ep->device_channel_required());
    ASSERT_TRUE(_ep->has_device_channel());

    ASSERT_EQ(rdma::RdmaEndpoint::UNINIT,
              plain_ep->_state.load(butil::memory_order_relaxed));
    ASSERT_FALSE(plain_ep->device_channel_required());
    ASSERT_FALSE(plain_ep->has_device_channel());

    // Reusing either endpoint puts it back the way its own socket mode says,
    // which is the whole reason the mode is not copied into the endpoint.
    _ep->Reset();
    plain_ep->Reset();
    ASSERT_TRUE(_ep->has_device_channel());
    ASSERT_FALSE(plain_ep->has_device_channel());

    plain->SetFailed();
}

// RdmaEndpoint::Reset() used to clear the host channel's fields one by one,
// from outside the class, which meant a list of HostChannel's private members
// maintained in RdmaEndpoint. Each channel clears its own now, and this is
// what stops a field added to HostChannel from being left dirty into the next
// connection: carried-over credits would let the writer post past the peer's
// receive queue, and a carried-over read_buf would prepend the dead
// connection's trailing bytes to the new one's first message.
TEST_F(GdrProtocolTest, host_channel_resets_every_field_it_owns) {
    rdma::HostChannel& hc = _ep->_host;
    const uint16_t sq_size = hc.sq_size;
    const uint16_t rq_size = hc.rq_size;
    ASSERT_GT(sq_size, 0);
    ASSERT_GT(rq_size, 0);

    // Every field of both halves, set to something no reset value could be
    // mistaken for. The resource is a bare stub: Reset() only drops the
    // pointer -- freeing is DeallocateResources()' job -- so nothing here is
    // ever dereferenced.
    rdma::RdmaResource resource;
    hc.resource = &resource;
    hc.local_window_capacity = 1;
    hc.remote_window_capacity = 2;
    hc.sq_current = 3;
    hc.sq_unsignaled = 4;
    hc.sq_sent = 5;
    hc.rq_received = 6;
    hc.sq_imm_window_size = 7;
    hc.remote_rq_window_size.store(8, butil::memory_order_relaxed);
    hc.sq_window_size.store(9, butil::memory_order_relaxed);
    hc.new_rq_wrs.store(10, butil::memory_order_relaxed);
    hc.remote_recv_block_size = 11;
    hc.send_cq_events = 12;
    hc.recv_cq_events = 13;
    hc.sbuf.resize(2);
    hc.rbuf.resize(2);
    hc.rbuf_data.resize(2, nullptr);
    hc.input_processor.read_buf().append("half a message");
    hc.unsolicited = 14;
    hc.unsolicited_bytes = 15;
    hc.accumulated_ack = 16;

    hc.Reset();

    ASSERT_TRUE(nullptr == hc.resource);
    ASSERT_TRUE(nullptr == hc.qp());
    ASSERT_EQ(0, hc.local_window_capacity);
    ASSERT_EQ(0, hc.remote_window_capacity);
    ASSERT_EQ(0, hc.sq_current);
    ASSERT_EQ(0, hc.sq_unsignaled);
    ASSERT_EQ(0, hc.sq_sent);
    ASSERT_EQ(0, hc.rq_received);
    ASSERT_EQ(0, hc.sq_imm_window_size);
    ASSERT_EQ(0, hc.remote_rq_window_size.load(butil::memory_order_relaxed));
    ASSERT_EQ(0, hc.sq_window_size.load(butil::memory_order_relaxed));
    ASSERT_EQ(0, hc.new_rq_wrs.load(butil::memory_order_relaxed));
    ASSERT_EQ(0u, hc.remote_recv_block_size);
    ASSERT_EQ(0u, hc.send_cq_events);
    ASSERT_EQ(0u, hc.recv_cq_events);
    ASSERT_TRUE(hc.sbuf.empty());
    ASSERT_TRUE(hc.rbuf.empty());
    ASSERT_TRUE(hc.rbuf_data.empty());
    ASSERT_EQ(0u, hc.input_processor.read_buf().size());
    ASSERT_EQ(0, hc.unsolicited);
    ASSERT_EQ(0u, hc.unsolicited_bytes);
    ASSERT_EQ(0, hc.accumulated_ack);

    // The queue depths are this side's configuration, read once from the
    // flags when the channel was built. Clearing them would bring the next
    // connection up at a depth nobody asked for -- zero, in fact.
    ASSERT_EQ(sq_size, hc.sq_size);
    ASSERT_EQ(rq_size, hc.rq_size);
}

// --- the connection's comp channel -----------------------------------------
//
// IbvCreateCompChannel / IbvDestroyCompChannel are global function pointers
// filled in by LoadSymbol(), so these two can be taken over without a card.
// The CQ and QP calls around them are not exercised here: ibv_req_notify_cq()
// and ibv_poll_cq() are static inlines in verbs.h and cannot be stubbed, so
// the shape of a whole connection is asserted under --gdr_test_real_device
// instead (four_cqs_report_to_one_comp_channel).

static int g_comp_channel_destroys = 0;
static ibv_comp_channel g_fake_comp_channel;

static ibv_comp_channel* CreateFakeCompChannel(ibv_context*) {
    return &g_fake_comp_channel;
}

static int DestroyFakeCompChannel(ibv_comp_channel*) {
    ++g_comp_channel_destroys;
    return 0;
}

// Swaps in whichever of the two stubs it is given, and resets the counter.
class ScopedCompChannelStubs {
public:
    ScopedCompChannelStubs(ibv_comp_channel* (*create)(ibv_context*),
                           int (*destroy)(ibv_comp_channel*))
        : _saved_create(rdma::IbvCreateCompChannel)
        , _saved_destroy(rdma::IbvDestroyCompChannel) {
        if (create != NULL) {
            rdma::IbvCreateCompChannel = create;
        }
        if (destroy != NULL) {
            rdma::IbvDestroyCompChannel = destroy;
        }
        g_comp_channel_destroys = 0;
    }
    ~ScopedCompChannelStubs() {
        rdma::IbvCreateCompChannel = _saved_create;
        rdma::IbvDestroyCompChannel = _saved_destroy;
    }
private:
    DISALLOW_COPY_AND_ASSIGN(ScopedCompChannelStubs);
    ibv_comp_channel* (*_saved_create)(ibv_context*);
    int (*_saved_destroy)(ibv_comp_channel*);
};

// The comp channel belongs to the connection, not to the host QP, so it can
// exist while the QP does not: DoAllocateResources() creates it first, and
// creating the QP on it is what fails next. DeallocateResources() returns
// early on a null host resource, so that one edge is the whole reason this
// test exists -- a comp channel dropped there is a leaked fd per failed
// handshake, and the process runs out.
TEST_F(GdrProtocolTest, deallocate_releases_a_comp_channel_without_a_qp) {
    ScopedCompChannelStubs stubs(NULL, DestroyFakeCompChannel);

    ASSERT_TRUE(NULL == _ep->_host.resource);
    _ep->_comp_channel = new rdma::RdmaCompChannel;
    _ep->_comp_channel->channel = &g_fake_comp_channel;

    _ep->DeallocateResources();
    ASSERT_EQ(1, g_comp_channel_destroys);
    ASSERT_TRUE(NULL == _ep->_comp_channel);

    // Still idempotent, which both callers rely on: Reset() and the
    // AllocateResources() failure path each run it, sometimes both.
    _ep->DeallocateResources();
    ASSERT_EQ(1, g_comp_channel_destroys);
}

// The comp channel's fd goes into an EventDispatcher, which reads it
// edge-triggered and must never block on it, and an exec'd child has no
// business keeping it open. Both attributes used to be set halfway down
// AllocateQpCq() with nothing asserting them.
TEST_F(GdrProtocolTest, comp_channel_fd_is_cloexec_and_nonblocking) {
    // A fake channel over a real fd: the flags under test are the kernel's,
    // so the fd has to be one, but nothing here needs it to be a verbs fd.
    int pipefd[2];
    ASSERT_EQ(0, pipe(pipefd));
    butil::fd_guard read_guard(pipefd[0]);
    butil::fd_guard write_guard(pipefd[1]);
    g_fake_comp_channel.fd = pipefd[0];
    ScopedCompChannelStubs stubs(CreateFakeCompChannel, DestroyFakeCompChannel);

    // Neither flag is on a fresh pipe end, so both assertions below are about
    // Create() and not about what pipe() happened to hand back.
    ASSERT_EQ(0, fcntl(pipefd[0], F_GETFD) & FD_CLOEXEC);
    ASSERT_EQ(0, fcntl(pipefd[0], F_GETFL) & O_NONBLOCK);

    rdma::RdmaCompChannel* cc = rdma::RdmaCompChannel::Create();
    ASSERT_TRUE(cc != NULL);
    ASSERT_EQ(pipefd[0], cc->fd());
    ASSERT_TRUE((fcntl(pipefd[0], F_GETFD) & FD_CLOEXEC) != 0);
    ASSERT_TRUE((fcntl(pipefd[0], F_GETFL) & O_NONBLOCK) != 0);

    delete cc;
    ASSERT_EQ(1, g_comp_channel_destroys);
}

// The two channels used to be reaped by HandleCompletion() and
// HandleDeviceCompletion(), two copies of the same handling. They are one
// function now and the differences are virtuals on the channel, so what is
// worth pinning is that a completion credits the channel it was handed and no
// other: reaped on the wrong one, it would release a send buffer the NIC is
// still reading from and hand out credits the peer never returned.
TEST_F(GdrProtocolTest, send_completion_credits_only_the_channel_it_came_on) {
    rdma::QpChannel* channels[2] = { &_ep->_host, &*_ep->_device };
    for (rdma::QpChannel* qc : channels) {
        // What a handshake would have left behind. The windows are shut so
        // that the wake-the-writer heuristic stays quiet: this socket has no
        // fd, and waking it is not what is under test here.
        qc->sq_size = 16;
        qc->local_window_capacity = 16;
        qc->remote_window_capacity = 16;
        qc->sq_sent = 0;
        qc->sq_imm_window_size = 0;
        qc->new_rq_wrs.store(0, butil::memory_order_relaxed);
        qc->remote_rq_window_size.store(0, butil::memory_order_relaxed);
        qc->sq_window_size.store(0, butil::memory_order_relaxed);
    }
    _ep->_host.sbuf.resize(16);
    _ep->_device->sbuf.resize(16);
    _ep->_host.sbuf[0].append("in flight");
    ASSERT_NO_FATAL_FAILURE(MakeDeviceAttachment(&_ep->_device->sbuf[0],
                                                 std::string(64, 'd')));

    ibv_wc wc;
    memset(&wc, 0, sizeof(wc));
    wc.opcode = IBV_WC_SEND;
    wc.wr_id = 1;
    ASSERT_EQ(0, _ep->HandleCompletion(_ep->_host, wc));
    ASSERT_EQ(1, _ep->_host.sq_sent);
    ASSERT_EQ(1, _ep->_host.sq_window_size.load(butil::memory_order_relaxed));
    ASSERT_TRUE(_ep->_host.sbuf[0].empty());
    // The device channel did not move, and is still holding the caller's
    // device memory.
    ASSERT_EQ(0, _ep->_device->sq_sent);
    ASSERT_EQ(0, _ep->_device->sq_window_size.load(butil::memory_order_relaxed));
    ASSERT_EQ(64u, _ep->_device->sbuf[0].size());

    ASSERT_EQ(0, _ep->HandleCompletion(*_ep->_device, wc));
    ASSERT_EQ(1, _ep->_device->sq_sent);
    ASSERT_EQ(1, _ep->_device->sq_window_size.load(butil::memory_order_relaxed));
    ASSERT_TRUE(_ep->_device->sbuf[0].empty());
    ASSERT_EQ(1, _ep->_host.sq_sent);
    ASSERT_EQ(1, _ep->_host.sq_window_size.load(butil::memory_order_relaxed));

    // wr_id == 0 is a bare ack WR: it holds no send buffer and returns an IMM
    // slot instead of send credits -- again on its own channel only.
    wc.wr_id = 0;
    ASSERT_EQ(0, _ep->HandleCompletion(*_ep->_device, wc));
    ASSERT_EQ(1, _ep->_device->sq_imm_window_size);
    ASSERT_EQ(1, _ep->_device->sq_sent);
    ASSERT_EQ(1, _ep->_device->sq_window_size.load(butil::memory_order_relaxed));
    ASSERT_EQ(0, _ep->_host.sq_imm_window_size);
}

TEST_F(GdrProtocolTest, connection_without_device_channel_parses_as_before) {
    SocketId id;
    SocketUniquePtr sock;
    rdma::RdmaEndpoint* ep = nullptr;
    ASSERT_EQ(0, MakeSocket(false, &id, &sock, &ep));
    ASSERT_TRUE(nullptr == sock->device_stream());

    butil::IOBuf buf;
    MakeRpcFrame(&buf, 1, "hello", 0);
    ParseResult pr = policy::ParseRpcMessage(&buf, sock.get(), false, nullptr);
    ASSERT_TRUE(pr.is_ok());
    ASSERT_TRUE(pr.message() != nullptr);
    ASSERT_EQ("hello", AsMsg(pr)->payload.to_string());
    ASSERT_TRUE(nullptr == sock->parsing_context());
    pr.message()->Destroy();

    sock.reset();
    Socket::SetFailed(id);
}

TEST_F(GdrProtocolTest, gdrb_frame_without_a_device_channel_reads_inline) {
    // The peer says this message has a device half, and this connection has
    // no second channel to carry it -- so the bytes are right behind the
    // body, on this very socket. GDRB says how long the device half is, not
    // which line it came down; both ends read that off the same handshake.
    SocketId id;
    SocketUniquePtr sock;
    rdma::RdmaEndpoint* ep = nullptr;
    ASSERT_EQ(0, MakeSocket(false, &id, &sock, &ep));
    ASSERT_TRUE(nullptr == sock->device_stream());

    const std::string device_bytes(4096, 'z');
    butil::IOBuf buf;
    MakeRpcFrame(&buf, 1, "hello", device_bytes.size());
    buf.append(device_bytes);

    ParseResult pr = policy::ParseRpcMessage(&buf, sock.get(), false, nullptr);
    ASSERT_TRUE(pr.is_ok());
    ASSERT_TRUE(pr.message() != nullptr);
    ASSERT_EQ("hello", AsMsg(pr)->payload.to_string());
    ASSERT_EQ(device_bytes.size(), AsMsg(pr)->device_payload.size());
    ASSERT_EQ(device_bytes, DeviceToString(AsMsg(pr)->device_payload));
    // Everything was consumed, and no pending list was created: there is
    // nothing to wait for when both halves come down the same socket.
    ASSERT_EQ(0u, buf.size());
    ASSERT_TRUE(nullptr == sock->parsing_context());
    pr.message()->Destroy();

    sock.reset();
    Socket::SetFailed(id);
}

TEST_F(GdrProtocolTest, inline_device_bytes_are_waited_for_not_parked) {
    // With a second channel a message whose device half is late goes on the
    // pending list, because its host bytes are already consumed and cannot be
    // rewound. Inline there is nothing to park: the missing bytes are on
    // their way down this socket, so the parser simply asks for more.
    SocketId id;
    SocketUniquePtr sock;
    rdma::RdmaEndpoint* ep = nullptr;
    ASSERT_EQ(0, MakeSocket(false, &id, &sock, &ep));

    butil::IOBuf buf;
    MakeRpcFrame(&buf, 1, "hello", 4096);
    buf.append(std::string(4095, 'z'));
    const size_t before = buf.size();
    ParseResult pr = policy::ParseRpcMessage(&buf, sock.get(), false, nullptr);
    ASSERT_FALSE(pr.is_ok());
    ASSERT_EQ(PARSE_ERROR_NOT_ENOUGH_DATA, pr.error());
    // Nothing consumed, so the retry below sees the whole frame again.
    ASSERT_EQ(before, buf.size());

    buf.append("z");
    pr = policy::ParseRpcMessage(&buf, sock.get(), false, nullptr);
    ASSERT_TRUE(pr.is_ok());
    ASSERT_TRUE(pr.message() != nullptr);
    ASSERT_EQ(4096u, AsMsg(pr)->device_payload.size());
    pr.message()->Destroy();

    sock.reset();
    Socket::SetFailed(id);
}

TEST_F(GdrProtocolTest, inline_device_bytes_count_towards_max_body_size) {
    // They really are host bytes piling up in the read buffer, unlike the
    // ones on a device channel whose volume that channel's own flow control
    // bounds. Without this a peer could declare a 4GB device_size and make
    // the receiver buffer it.
    SocketId id;
    SocketUniquePtr sock;
    rdma::RdmaEndpoint* ep = nullptr;
    ASSERT_EQ(0, MakeSocket(false, &id, &sock, &ep));

    const uint64_t saved = FLAGS_max_body_size;
    FLAGS_max_body_size = 1024;
    butil::IOBuf buf;
    MakeRpcFrame(&buf, 1, "hello", 4096);
    ParseResult pr = policy::ParseRpcMessage(&buf, sock.get(), false, nullptr);
    FLAGS_max_body_size = saved;
    ASSERT_FALSE(pr.is_ok());
    ASSERT_EQ(PARSE_ERROR_TOO_BIG_DATA, pr.error());

    sock.reset();
    Socket::SetFailed(id);
}

// ------------------------------- send side -------------------------------

TEST_F(GdrProtocolTest, pack_request_queues_device_data_in_write_order) {
    // PackRpcRequest() cannot queue the device half itself -- Socket::Write()
    // is a lock-free MPSC enqueue, so the outgoing order is not decided yet --
    // so it defers to a SocketMessage whose AppendAndDestroySelf() the socket
    // runs oldest-first. That order is the only thing pairing the two streams.
    const google::protobuf::MethodDescriptor* method =
        test::EchoService::descriptor()->method(0);

    SocketMessage* packets[2] = { nullptr, nullptr };
    Controller cntl[2];
    const size_t sizes[2] = { 4096, 8192 };
    for (int i = 0; i < 2; ++i) {
        ASSERT_EQ(0, Socket::Address(_id, &cntl[i]._current_call.sending_sock));
        ASSERT_TRUE(cntl[i].has_device_channel());
        ASSERT_TRUE(cntl[i].request_device_attachment().append_new(sizes[i]) != nullptr);
        butil::IOBuf body;
        body.append("body");
        butil::IOBuf req_buf;
        policy::PackRpcRequest(&req_buf, &packets[i], i + 1, method, &cntl[i],
                               body, nullptr);
        ASSERT_FALSE(cntl[i].Failed()) << cntl[i].ErrorText();
        // The whole message went into the SocketMessage, not the buffer.
        ASSERT_EQ(0u, req_buf.size());
        ASSERT_TRUE(packets[i] != nullptr);
        // The Controller keeps its copy so that a retry can send it again.
        ASSERT_EQ(sizes[i], cntl[i].request_device_attachment().size());
    }

    for (int i = 0; i < 2; ++i) {
        butil::IOBuf out;
        ASSERT_TRUE(packets[i]->AppendAndDestroySelf(&out, _sock.get()).ok());
        // The device size rides in the frame header, so the receiver never has
        // to deserialize the meta to find it.
        char magic[4];
        ASSERT_EQ(4u, out.copy_to(magic, 4));
        ASSERT_EQ(0, memcmp(magic, "GDRB", 4));
        policy::RpcMeta meta;
        std::string payload;
        uint32_t device_size = 0;
        ASSERT_TRUE(SplitFrame(&out, &meta, &payload, &device_size));
        ASSERT_EQ("body", payload);
        ASSERT_EQ((uint32_t)sizes[i], device_size);
        // Consumed exactly one frame: device bytes are not part of body_size.
        ASSERT_EQ(0u, out.size());
    }
    // Neither could be posted (there is no QP here), so both are still queued
    // in the order AppendAndDestroySelf() ran.
    ASSERT_EQ(12288u, _ep->device_stream()->send_stream()->size());
    ASSERT_EQ(12288u, _ep->device_stream()->send_offset());

    for (int i = 0; i < 2; ++i) {
        cntl[i]._current_call.sending_sock.reset();
    }
}

TEST_F(GdrProtocolTest, request_without_device_data_keeps_prpc_framing) {
    // Why the magic and not a flag in the meta: on a GDR connection the
    // messages that carry no GPU memory must stay on the 12-byte header and
    // cost nothing extra, on the wire or in the parser.
    const google::protobuf::MethodDescriptor* method =
        test::EchoService::descriptor()->method(0);
    Controller cntl;
    ASSERT_EQ(0, Socket::Address(_id, &cntl._current_call.sending_sock));
    ASSERT_TRUE(cntl.has_device_channel());

    butil::IOBuf body;
    body.append("body");
    butil::IOBuf req_buf;
    SocketMessage* packet = nullptr;
    policy::PackRpcRequest(&req_buf, &packet, 1, method, &cntl, body, nullptr);
    ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();
    // No SocketMessage detour either: nothing has to be deferred.
    ASSERT_TRUE(nullptr == packet);

    char magic[4];
    ASSERT_EQ(4u, req_buf.copy_to(magic, 4));
    ASSERT_EQ(0, memcmp(magic, "PRPC", 4));
    policy::RpcMeta meta;
    std::string payload;
    uint32_t device_size = 12345;
    ASSERT_TRUE(SplitFrame(&req_buf, &meta, &payload, &device_size));
    ASSERT_EQ("body", payload);
    ASSERT_EQ(0u, device_size);
    ASSERT_EQ(0u, req_buf.size());

    cntl._current_call.sending_sock.reset();
}

TEST_F(GdrProtocolTest, pack_request_falls_back_to_tcp_without_a_device_channel) {
    // Same GDRB frame as on a device channel -- the magic says the message
    // has a device half and how long it is, not which line it came down. With
    // no second channel here, that half goes right behind the body.
    SocketId id;
    SocketUniquePtr sock;
    rdma::RdmaEndpoint* ep = nullptr;
    ASSERT_EQ(0, MakeSocket(false, &id, &sock, &ep));

    Controller cntl;
    ASSERT_EQ(0, Socket::Address(id, &cntl._current_call.sending_sock));
    ASSERT_FALSE(cntl.has_device_channel());
    const std::string device_bytes(4096, 'q');
    ASSERT_NO_FATAL_FAILURE(MakeDeviceAttachment(
            &cntl.request_device_attachment(), device_bytes));

    butil::IOBuf req_buf;
    butil::IOBuf body;
    body.append("body");
    SocketMessage* packet = nullptr;
    policy::PackRpcRequest(&req_buf, &packet, 1,
                           test::EchoService::descriptor()->method(0),
                           &cntl, body, nullptr);
    // No fast-fail: whether the bytes will go out on a QP or inline is not
    // settled until AppendAndDestroySelf() sees the socket.
    ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();
    ASSERT_TRUE(packet != nullptr);
    ASSERT_EQ(0u, req_buf.size());

    butil::IOBuf out;
    ASSERT_TRUE(packet->AppendAndDestroySelf(&out, sock.get()).ok());
    butil::IOBuf wire(out);
    char magic[4];
    ASSERT_EQ(4u, out.copy_to(magic, 4));
    ASSERT_EQ(0, memcmp(magic, "GDRB", 4));
    policy::RpcMeta meta;
    std::string payload;
    uint32_t device_size = 0;
    ASSERT_TRUE(SplitFrame(&out, &meta, &payload, &device_size));
    ASSERT_EQ("body", payload);
    ASSERT_EQ((uint32_t)device_bytes.size(), device_size);
    // body_size still counts host bytes only, so what SplitFrame leaves is
    // exactly the device half, staged through the host on the way out.
    ASSERT_EQ(device_bytes, out.to_string());

    // And a receiver on such a connection takes it back off the same stream,
    // so neither end needs to know the other fell back.
    ParseResult pr = policy::ParseRpcMessage(&wire, sock.get(), false, nullptr);
    ASSERT_TRUE(pr.is_ok());
    ASSERT_TRUE(pr.message() != nullptr);
    ASSERT_EQ(device_bytes.size(), AsMsg(pr)->device_payload.size());
    ASSERT_EQ(device_bytes, DeviceToString(AsMsg(pr)->device_payload));
    ASSERT_EQ(0u, wire.size());
    pr.message()->Destroy();

    cntl._current_call.sending_sock.reset();
    sock.reset();
    Socket::SetFailed(id);
}

TEST_F(GdrProtocolTest, estimated_byte_size_counts_the_bytes_that_get_written) {
    // Socket::Write() sizes its write queue from EstimatedByteSize(), which
    // runs before AppendAndDestroySelf(). It could only ever be a guess while
    // the header and meta were serialized inside AppendAndDestroySelf(); the
    // frame is built in the constructor now, so it is exact.
    Controller cntl;
    ASSERT_EQ(0, Socket::Address(_id, &cntl._current_call.sending_sock));
    ASSERT_TRUE(cntl.request_device_attachment().append_new(4096) != nullptr);

    butil::IOBuf body;
    body.append("body");
    butil::IOBuf req_buf;
    SocketMessage* packet = nullptr;
    policy::PackRpcRequest(&req_buf, &packet, 1,
                           test::EchoService::descriptor()->method(0),
                           &cntl, body, nullptr);
    ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();
    ASSERT_TRUE(packet != nullptr);

    // Host bytes only: the 4096 device bytes leave on the other QP.
    const size_t estimated = packet->EstimatedByteSize();
    ASSERT_GT(estimated, 16u + strlen("body"));
    ASSERT_LT(estimated, 4096u);

    butil::IOBuf out;
    ASSERT_TRUE(packet->AppendAndDestroySelf(&out, _sock.get()).ok());
    ASSERT_EQ(estimated, out.size());

    cntl._current_call.sending_sock.reset();
}

TEST_F(GdrProtocolTest, frame_does_not_depend_on_where_the_device_half_goes) {
    // This is what let the serialization move off the write path at all:
    // GDRB says how long the device half is, not which line it came down, so
    // the header and meta bytes are decided entirely by the request. Only the
    // PLACEMENT needs the socket, and that is all AppendAndDestroySelf() does.
    SocketId tcp_id;
    SocketUniquePtr tcp_sock;
    rdma::RdmaEndpoint* tcp_ep = nullptr;
    ASSERT_EQ(0, MakeSocket(false, &tcp_id, &tcp_sock, &tcp_ep));

    const std::string device_bytes(4096, 'z');
    butil::IOBuf out[2];
    size_t estimated[2] = { 0, 0 };
    for (int i = 0; i < 2; ++i) {
        Socket* sock = (i == 0) ? _sock.get() : tcp_sock.get();
        Controller cntl;
        ASSERT_EQ(0, Socket::Address(sock->id(),
                                     &cntl._current_call.sending_sock));
        ASSERT_EQ(i == 0, cntl.has_device_channel());
        ASSERT_NO_FATAL_FAILURE(MakeDeviceAttachment(
                &cntl.request_device_attachment(), device_bytes));

        butil::IOBuf body;
        body.append("body");
        butil::IOBuf req_buf;
        SocketMessage* packet = nullptr;
        policy::PackRpcRequest(&req_buf, &packet, 7,
                               test::EchoService::descriptor()->method(0),
                               &cntl, body, nullptr);
        ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();
        ASSERT_TRUE(packet != nullptr);
        estimated[i] = packet->EstimatedByteSize();
        ASSERT_TRUE(packet->AppendAndDestroySelf(&out[i], sock).ok());
        cntl._current_call.sending_sock.reset();
    }

    // Byte for byte the same frame; the fallback just carries the device half
    // behind it instead of on the second QP.
    ASSERT_EQ(estimated[0], estimated[1]);
    const std::string framed = out[0].to_string();
    ASSERT_EQ(estimated[0], framed.size());
    ASSERT_EQ(framed + device_bytes, out[1].to_string());

    _ep->DiscardQueuedDeviceData();
    tcp_sock.reset();
    Socket::SetFailed(tcp_id);
}

TEST_F(GdrProtocolTest, abandoned_packet_releases_its_device_memory) {
    Controller cntl;
    ASSERT_EQ(0, Socket::Address(_id, &cntl._current_call.sending_sock));
    void* p = cntl.request_device_attachment().append_new(4096);
    ASSERT_TRUE(p != nullptr);

    butil::IOBuf req_buf;
    butil::IOBuf body;
    SocketMessage* packet = nullptr;
    policy::PackRpcRequest(&req_buf, &packet, 1,
                           test::EchoService::descriptor()->method(0),
                           &cntl, body, nullptr);
    ASSERT_TRUE(packet != nullptr);

    // Socket::Write() rejected it: AppendAndDestroySelf(nullptr) must still run,
    // and the reference it holds must go back to the pool.
    cntl.request_device_attachment().clear();
    butil::IOBuf out;
    ASSERT_TRUE(packet->AppendAndDestroySelf(&out, nullptr).ok());
    ASSERT_EQ(0u, out.size());
    DeviceAttachment probe;
    ASSERT_EQ(p, probe.append_new(4096));

    cntl._current_call.sending_sock.reset();
}

TEST_F(GdrProtocolTest, packed_request_round_trips_through_the_parser) {
    Controller cntl;
    ASSERT_EQ(0, Socket::Address(_id, &cntl._current_call.sending_sock));
    ASSERT_TRUE(cntl.request_device_attachment().append_new(4096) != nullptr);
    cntl.request_attachment().append("attached");

    butil::IOBuf req_buf;
    butil::IOBuf body;
    body.append("body");
    SocketMessage* packet = nullptr;
    policy::PackRpcRequest(&req_buf, &packet, 7,
                           test::EchoService::descriptor()->method(0),
                           &cntl, body, nullptr);
    ASSERT_TRUE(packet != nullptr);
    butil::IOBuf wire;
    ASSERT_TRUE(packet->AppendAndDestroySelf(&wire, _sock.get()).ok());

    // Feed the produced frame back to the parser of a receiver whose device
    // stream is at offset 0, as the peer's would be.
    ArriveDeviceBytes(4096);
    ParseResult pr = Parse(&wire);
    ASSERT_TRUE(pr.is_ok());
    policy::MostCommonMessage* msg = AsMsg(pr);
    ASSERT_TRUE(msg != nullptr);
    ASSERT_EQ("bodyattached", msg->payload.to_string());
    ASSERT_EQ(4096u, msg->device_payload.size());
    msg->Destroy();

    cntl._current_call.sending_sock.reset();
}

TEST_F(GdrProtocolTest, gdr_channel_rejects_pooled_connections) {
    // A message whose device half has not arrived keeps a parsing context,
    // and Socket::ReturnToPool() aborts the process when it finds one.
    // Init() does not connect, so any address will do.
    butil::EndPoint ep;
    ASSERT_EQ(0, butil::str2endpoint("127.0.0.1:8713", &ep));

    Channel channel;
    ChannelOptions options;
    options.socket_mode = SOCKET_MODE_RDMA_AND_DEVICE;
    options.connection_type = CONNECTION_TYPE_POOLED;
    ASSERT_EQ(-1, channel.Init(ep, &options));

    options.connection_type = CONNECTION_TYPE_SHORT;
    ASSERT_EQ(-1, channel.Init(ep, &options));

    options.connection_type = CONNECTION_TYPE_SINGLE;
    ASSERT_EQ(0, channel.Init(ep, &options));
}

TEST_F(GdrProtocolTest, pooled_is_fine_without_device_channel_mode) {
    // The restriction above is a property of the second channel, not of RDMA
    // and not of the process: a channel that never asks for one has no
    // half-arrived messages to park and may pool its connections, even in a
    // process where GDR is on.
    butil::EndPoint ep;
    ASSERT_EQ(0, butil::str2endpoint("127.0.0.1:8713", &ep));

    ChannelOptions options;
    options.socket_mode = SOCKET_MODE_RDMA;
    options.connection_type = CONNECTION_TYPE_POOLED;
    Channel pooled;
    ASSERT_EQ(0, pooled.Init(ep, &options));

    options.connection_type = CONNECTION_TYPE_SHORT;
    Channel short_conn;
    ASSERT_EQ(0, short_conn.Init(ep, &options));
}

// ComputeChannelSignature() is static in channel.cpp, so the signature is
// observed where it actually matters: two single-connection Channels to the
// same address land on the same SocketMap entry iff their signatures agree.
class SocketIdPeekingChannel : public Channel {
public:
    SocketId server_id() const { return _server_id; }
};

TEST_F(GdrProtocolTest, device_channel_mode_isolates_connections) {
    // GDR forces connection_type=single, and single Sockets are shared across
    // Channels through the SocketMap. If the requirement did not reach the
    // signature, whichever Channel connected first would decide whether
    // everyone's attachments work -- and the connection-level enforcement
    // would then take down unrelated host traffic. Neither is an
    // optimization; both are why this isolation had to land first.
    //
    // Init() does not connect, so nothing needs to be listening.
    butil::EndPoint ep;
    ASSERT_EQ(0, butil::str2endpoint("127.0.0.1:8713", &ep));

    ChannelOptions plain;
    plain.socket_mode = SOCKET_MODE_RDMA;
    plain.connection_type = CONNECTION_TYPE_SINGLE;

    ChannelOptions requiring = plain;
    requiring.socket_mode = SOCKET_MODE_RDMA_AND_DEVICE;

    SocketIdPeekingChannel plain_ch;
    ASSERT_EQ(0, plain_ch.Init(ep, &plain));
    SocketIdPeekingChannel requiring_ch;
    ASSERT_EQ(0, requiring_ch.Init(ep, &requiring));
    ASSERT_NE(plain_ch.server_id(), requiring_ch.server_id());

    // The same demand must still share, or every requiring channel in a
    // process would open its own connection to the same server. This is also
    // what proves the ids above differ because of the option rather than
    // because two Init()s never share anything.
    SocketIdPeekingChannel requiring_ch2;
    ASSERT_EQ(0, requiring_ch2.Init(ep, &requiring));
    ASSERT_EQ(requiring_ch.server_id(), requiring_ch2.server_id());

    SocketIdPeekingChannel plain_ch2;
    ASSERT_EQ(0, plain_ch2.Init(ep, &plain));
    ASSERT_EQ(plain_ch.server_id(), plain_ch2.server_id());
}

// Each of the three below used to be a silent downgrade: the endpoint gave
// up on the device channel mid-handshake and the connection then failed with
// EDEVICECHANNEL, which says nothing about the configuration that caused it.
// A process that cannot possibly bring a device channel up must say so once,
// at startup, on the thing that asked for one.
TEST_F(GdrProtocolTest, device_channel_mode_needs_the_v3_handshake) {
    // The device qp_num has nowhere to ride in a v2 hello.
    butil::EndPoint ep;
    ASSERT_EQ(0, butil::str2endpoint("127.0.0.1:8713", &ep));

    ChannelOptions options;
    options.socket_mode = SOCKET_MODE_RDMA_AND_DEVICE;
    rdma::FLAGS_rdma_client_handshake_version = 2;
    Channel v2_channel;
    ASSERT_EQ(-1, v2_channel.Init(ep, &options));

    // Only the pairing is rejected: v2 is still fine without a device
    // channel, and a device channel is still fine on v3.
    ChannelOptions plain = options;
    plain.socket_mode = SOCKET_MODE_RDMA;
    Channel v2_plain;
    ASSERT_EQ(0, v2_plain.Init(ep, &plain));

    rdma::FLAGS_rdma_client_handshake_version = 3;
    Channel v3_channel;
    ASSERT_EQ(0, v3_channel.Init(ep, &options));

    // The server never picks the version, so it has nothing to reject here.
}

TEST_F(GdrProtocolTest, device_channel_mode_is_refused_in_polling_mode) {
    // GDR has no comp channel to share in polling mode
    // (docs/cn/gdr_design.md section 10).
    butil::EndPoint ep;
    ASSERT_EQ(0, butil::str2endpoint("127.0.0.1:8713", &ep));

    const bool saved = rdma::FLAGS_rdma_use_polling;
    rdma::FLAGS_rdma_use_polling = true;

    ChannelOptions options;
    options.socket_mode = SOCKET_MODE_RDMA_AND_DEVICE;
    Channel channel;
    ASSERT_EQ(-1, channel.Init(ep, &options));

    Server server;
    ServerOptions server_options;
    server_options.socket_mode = SOCKET_MODE_RDMA_AND_DEVICE;
    ASSERT_EQ(-1, server.Start(8714, &server_options));

    rdma::FLAGS_rdma_use_polling = saved;
}

TEST_F(GdrProtocolTest, device_channel_mode_is_refused_without_the_gdr_pool) {
    butil::EndPoint ep;
    ASSERT_EQ(0, butil::str2endpoint("127.0.0.1:8713", &ep));

    rdma::FLAGS_rdma_enable_gdr = false;

    ChannelOptions options;
    options.socket_mode = SOCKET_MODE_RDMA_AND_DEVICE;
    Channel channel;
    ASSERT_EQ(-1, channel.Init(ep, &options));

    Server server;
    ServerOptions server_options;
    server_options.socket_mode = SOCKET_MODE_RDMA_AND_DEVICE;
    ASSERT_EQ(-1, server.Start(8714, &server_options));

    // TearDown() clears the flag anyway; put it back so nothing in between
    // sees a half-off pool.
    rdma::FLAGS_rdma_enable_gdr = true;
}

// The negotiation itself, on the wire and without a card. Between them these
// two cover the chain the optional replaced -- the client proposes a device
// channel because it is configured for one, the peer's answer decides, and an
// unmet demand fails the connection -- which the end-to-end GDR tests can only
// reach with real hardware on both ends.
TEST_F(GdrProtocolTest, device_mode_client_hello_carries_the_device_channel) {
    butil::ip_t ip;
    ASSERT_EQ(0, butil::str2ip(g_ip.c_str(), &ip));
    const butil::EndPoint ep(ip, PORT);

    butil::fd_guard listen_fd(butil::tcp_listen(ep));
    ASSERT_TRUE(listen_fd >= 0);

    Channel channel;
    ChannelOptions chan_options;
    chan_options.socket_mode = SOCKET_MODE_RDMA_AND_DEVICE;
    chan_options.connect_timeout_ms = 1000;
    chan_options.timeout_ms = 1000;
    chan_options.max_retry = 0;
    ASSERT_EQ(0, channel.Init(ep, &chan_options));

    Controller cntl;
    test::EchoRequest req;
    test::EchoResponse res;
    req.set_message(__FUNCTION__);
    ::test::EchoService::Stub(&channel).Echo(&cntl, &req, &res, DoNothing());

    butil::fd_guard acc_fd(accept(listen_fd, nullptr, nullptr));
    ASSERT_TRUE(acc_fd >= 0);
    rdma::RdmaHello msg;
    ASSERT_NO_FATAL_FAILURE(ReadV3Hello(acc_fd, &msg));

    // The proposal has to be complete: the server sizes the device windows
    // from these three numbers in ApplyRemoteHello(), before it has allocated
    // anything of its own.
    ASSERT_TRUE(msg.has_device());
    ASSERT_EQ((uint32_t)rdma::FLAGS_rdma_device_sq_size, msg.device().sq_size());
    ASSERT_EQ((uint32_t)rdma::FLAGS_rdma_device_rq_size, msg.device().rq_size());
    // The device block size is the device RQ's alone, and comes from its own
    // flag. Advertising the host one here is what PR #3144 did, and the peer
    // then put 64x too much in a single device WR.
    ASSERT_EQ((uint32_t)rdma::FLAGS_rdma_gdr_recv_block_size,
              msg.device().block_size());
    ASSERT_TRUE(msg.has_qp_num());

    // Nothing here is going to answer, so hang up rather than make the test
    // wait out the RPC timeout.
    acc_fd.reset(-1);
    bthread_id_join(cntl.call_id());
}

TEST_F(GdrProtocolTest, server_hello_without_a_device_field_fails_the_connection) {
    butil::ip_t ip;
    ASSERT_EQ(0, butil::str2ip(g_ip.c_str(), &ip));
    const butil::EndPoint ep(ip, PORT);

    butil::fd_guard listen_fd(butil::tcp_listen(ep));
    ASSERT_TRUE(listen_fd >= 0);

    Channel channel;
    ChannelOptions chan_options;
    chan_options.socket_mode = SOCKET_MODE_RDMA_AND_DEVICE;
    chan_options.connect_timeout_ms = 1000;
    chan_options.timeout_ms = 1000;
    chan_options.max_retry = 0;
    ASSERT_EQ(0, channel.Init(ep, &chan_options));

    Controller cntl;
    test::EchoRequest req;
    test::EchoResponse res;
    req.set_message(__FUNCTION__);
    ::test::EchoService::Stub(&channel).Echo(&cntl, &req, &res, DoNothing());

    butil::fd_guard acc_fd(accept(listen_fd, nullptr, nullptr));
    ASSERT_TRUE(acc_fd >= 0);
    rdma::RdmaHello client_hello;
    ASSERT_NO_FATAL_FAILURE(ReadV3Hello(acc_fd, &client_hello));
    ASSERT_TRUE(client_hello.has_device());

    // A peer that is otherwise fully capable and simply did not enable the
    // second channel: a valid v3 hello with no device field. This is the
    // mixed-deployment case, GDR rolled out to the clients before the servers.
    const std::string reply = MakeV3Packet(MakeValidV3Hello());
    ASSERT_TRUE(WriteAll(acc_fd, reply.data(), reply.size()));

    // The client hands its speculative device QP back in ApplyRemoteHello()
    // and then finds itself without the channel it was configured to require.
    // Reporting that per RPC instead would have to answer before the handshake
    // finished, where the honest answer is "not yet" -- so it fails the
    // connection once, with an error code that names the reason.
    bthread_id_join(cntl.call_id());
    ASSERT_TRUE(cntl.Failed());
    ASSERT_EQ(EDEVICECHANNEL, cntl.ErrorCode()) << cntl.ErrorText();
}

TEST(GdrDisabledTest, attachment_falls_back_to_host_memory_when_gdr_is_off) {
    ASSERT_FALSE(rdma::IsGdrAvailable());
    DeviceAttachment da;
    // An attachment still has to be buildable here, or a process that talks
    // to a GDR peer over plain TCP could not materialize what it receives --
    // and one body of application code could not run on both kinds of peer.
    void* p = da.append_new(4096);
    ASSERT_TRUE(p != nullptr);
    ASSERT_EQ(1u, da.segment_count());
    ASSERT_EQ(4096u, da.size());
    // Ordinary host memory: nothing registered it, and it says so, which is
    // what lets copy_to() memcpy from it.
    ASSERT_TRUE(da.segment(0).is_host);
    ASSERT_EQ(0u, da.segment(0).lkey);
    ASSERT_TRUE(da.is_host_readable());
    memset(p, 'h', 4096);
    ASSERT_EQ(std::string(4096, 'h'), DeviceToString(da));
    da.clear();
    ASSERT_TRUE(da.empty());

    // Both user-data forms take it too. Registration is what this process
    // cannot do, and on the TCP fallback there is nothing to register for:
    // the bytes are read by the CPU either way.
    int stack_var = 0;
    ASSERT_EQ(0, da.append_user_data(&stack_var, sizeof(stack_var), nullptr));
    ASSERT_TRUE(da.segment(0).is_host);
    ASSERT_EQ(0, da.append_user_data_with_lkey(&stack_var, sizeof(stack_var),
                                               /*lkey=*/1, /*is_host=*/true,
                                               nullptr));
    ASSERT_EQ(2u, da.segment_count());
    ASSERT_EQ(2 * sizeof(stack_var), da.size());
    da.clear();

    // Everything that does not need memory keeps working, so callers that
    // never populate an attachment need no #ifdef.
    DeviceAttachment other;
    ASSERT_EQ(0u, da.cutn(&other, 100));
    ASSERT_EQ(0u, da.pop_front(100));
    da.clear();
}

TEST(GdrDisabledTest, iobuf_round_trip_without_a_pool) {
    // The two halves of the TCP fallback, back to back and in the process
    // that needs them most: no pool, no card, no GPU.
    ASSERT_FALSE(rdma::IsGdrAvailable());
    std::string expected;
    for (int i = 0; i < 40000; ++i) {
        expected.push_back((char)('a' + i % 26));
    }
    // Several source blocks, so append_from_iobuf() has to walk them: a
    // destination chunk generally straddles a block boundary.
    butil::IOBuf from;
    for (size_t off = 0; off < expected.size(); off += 3000) {
        from.append(expected.data() + off,
                    std::min((size_t)3000, expected.size() - off));
    }
    ASSERT_LT(1u, from.backing_block_num());

    // Smaller than the 1MB default so that this actually produces several
    // destination chunks, each straddling a source block boundary.
    const int saved_block_size = rdma::FLAGS_rdma_gdr_recv_block_size;
    rdma::FLAGS_rdma_gdr_recv_block_size = 4096;
    DeviceAttachment da;
    const int rc = da.append_from_iobuf(&from, expected.size());
    rdma::FLAGS_rdma_gdr_recv_block_size = saved_block_size;
    ASSERT_EQ(0, rc);
    ASSERT_EQ(0u, from.size());
    ASSERT_EQ(expected.size(), da.size());
    // Chunked by -rdma_gdr_recv_block_size, so a fallback-received attachment
    // has the same shape as one the device channel filled in and application
    // code walking segments needs no branch.
    ASSERT_EQ((expected.size() + 4095) / 4096, da.segment_count());
    ASSERT_EQ(expected, DeviceToString(da));

    // Asking for more than is there changes nothing at all.
    butil::IOBuf few;
    few.append("abc");
    DeviceAttachment da2;
    ASSERT_EQ(-1, da2.append_from_iobuf(&few, 4));
    ASSERT_EQ(EINVAL, errno);
    ASSERT_EQ(3u, few.size());
    ASSERT_TRUE(da2.empty());
    ASSERT_EQ(0, da2.append_from_iobuf(&few, 0));
    ASSERT_TRUE(da2.empty());
}

TEST(GdrDisabledTest, initialize_is_a_noop_unless_flag_is_set) {
    ASSERT_FALSE(rdma::FLAGS_rdma_enable_gdr);
    ASSERT_EQ(0, rdma::GlobalGdrInitialize());
    ASSERT_FALSE(rdma::IsGdrAvailable());
}

// Bounces the attachment back with no regard for whether this connection has
// a second channel, which on plain TCP it does not. Deliberately not
// MyGdrEchoService: that one refuses to reply without a device channel, and
// the point here is that application code does not have to.
class TcpFallbackEchoService : public ::test::EchoService {
    void Echo(google::protobuf::RpcController* cntl_base,
              const ::test::EchoRequest* req,
              ::test::EchoResponse* res,
              google::protobuf::Closure* done) {
        Controller* cntl = static_cast<Controller*>(cntl_base);
        ClosureGuard done_guard(done);
        res->set_message("MyEchoService");
        res->add_code_list(req->code());
        cntl->response_attachment().append(cntl->request_attachment());
        cntl->response_device_attachment().append_ref(
                cntl->request_device_attachment());
    }
};

// The whole round trip with no second channel anywhere: GDR off, no card, no
// GPU. The attachment still goes out and comes back, inline behind each
// message body (docs/cn/gdr_design.md section 7.3). That is the
// compatibility path -- what lets one body of application code talk to a peer
// with GDR and to one without -- so it is asserted in the build everyone
// runs, not only under --gdr_test_real_device.
TEST(GdrDisabledTest, device_attachment_round_trips_over_plain_tcp) {
    ASSERT_FALSE(rdma::IsGdrAvailable());
    butil::ip_t ip;
    ASSERT_EQ(0, butil::str2ip(g_ip.c_str(), &ip));
    const butil::EndPoint ep(ip, PORT);

    Server server;
    TcpFallbackEchoService svc;
    ASSERT_EQ(0, server.AddService(&svc, SERVER_DOESNT_OWN_SERVICE));
    {
        ServerOptions options;
        options.enabled_protocols = "baidu_std";
        options.internal_port = -1;
        ASSERT_EQ(0, server.Start(PORT, &options));
    }

    {
        Channel channel;
        ChannelOptions chan_options;
        chan_options.connection_type = CONNECTION_TYPE_SINGLE;
        chan_options.timeout_ms = 5000;
        chan_options.max_retry = 0;
        ASSERT_EQ(0, channel.Init(ep, &chan_options));

        // Sizes picked around the 1MB chunk append_from_iobuf() cuts at, so
        // the reply comes back in a different number of pieces than it went
        // out in. The 0 interleaves a PRPC frame among the GDRB ones on a
        // single connection, in both directions.
        const size_t DEV_SIZES[] = { 8, 4096, 1024 * 1024 + 7, 0,
                                     300 * 1024 - 13 };
        const int RPC_COUNT = arraysize(DEV_SIZES);
        const size_t HOST_SIZE = 4096;
        Controller cntl[RPC_COUNT];
        test::EchoRequest req[RPC_COUNT];
        test::EchoResponse res[RPC_COUNT];
        std::vector<std::string> pattern(RPC_COUNT);

        for (int i = 0; i < RPC_COUNT; ++i) {
            // Position-dependent so that a cut off by a whole chunk shows up
            // as a mismatch rather than as an identical fill.
            pattern[i].resize(DEV_SIZES[i]);
            for (size_t k = 0; k < DEV_SIZES[i]; ++k) {
                pattern[i][k] = (char)((k + i) % 251);
            }
            ASSERT_NO_FATAL_FAILURE(MakeDeviceAttachment(
                    &cntl[i].request_device_attachment(), pattern[i]));
            cntl[i].request_attachment().resize(HOST_SIZE, (char)('A' + i));
            req[i].set_message(__FUNCTION__);
            req[i].set_code(i + 1);
            ::test::EchoService::Stub(&channel).Echo(
                    &cntl[i], &req[i], &res[i], DoNothing());
        }

        for (int i = 0; i < RPC_COUNT; ++i) {
            bthread_id_join(cntl[i].call_id());
            ASSERT_FALSE(cntl[i].Failed())
                    << "rpc[" << i << "]: " << cntl[i].ErrorText();
            // No fast-fail and no error code: the fallback is silent by
            // design, which is also why --rdma_enable_gdr not taking effect
            // is worth a startup check of its own.
            ASSERT_FALSE(cntl[i].has_device_channel());
            ASSERT_EQ(1, res[i].code_list_size());
            ASSERT_EQ(i + 1, res[i].code_list(0));
            ASSERT_EQ(HOST_SIZE, cntl[i].response_attachment().size());
            ASSERT_EQ(pattern[i].size(),
                      cntl[i].response_device_attachment().size())
                    << "rpc[" << i << "]";
            ASSERT_TRUE(pattern[i] ==
                        DeviceToString(cntl[i].response_device_attachment()))
                    << "rpc[" << i << "] of " << pattern[i].size()
                    << " device bytes came back wrong";
        }
    }

    server.Stop(0);
    server.Join();
}

// ---------------------- needs a real GPU and a card ----------------------
//
// Everything above runs on a machine with neither. What follows does not, and
// includes the only test that puts device bytes on the wire: these need a GDR
// build, --rdma_test_enable --gdr_test_real_device, a CUDA device, a RoCE card
// and a peer-memory module. Skipped otherwise.

#if BRPC_WITH_GDR

TEST(GdrDeviceIdTest, blocks_land_on_the_requested_gpu) {
    if (!FLAGS_rdma_test_enable || !FLAGS_gdr_test_real_device) {
        return;
    }
    int device_count = 0;
    ASSERT_EQ(cudaSuccess, cudaGetDeviceCount(&device_count));
    if (device_count < 2) {
        // With one GPU the flag cannot be distinguished from the default.
        return;
    }
    // Not device 0: that is where cudaMalloc lands anyway, so pinning to it
    // would pass even if ScopedCudaDevice did nothing.
    const int target = device_count - 1;
    GdrDeviceIdFlag device_guard(target);
    ASSERT_NO_FATAL_FAILURE(EnableGdrForTest());

    uint32_t lkey = 0;
    void* p = rdma::AllocDeviceBlock(1 << 20, &lkey);
    ASSERT_TRUE(p != nullptr);
    ASSERT_NE(0u, lkey);
    cudaPointerAttributes attr;
    ASSERT_EQ(cudaSuccess, cudaPointerGetAttributes(&attr, p));
    ASSERT_EQ(cudaMemoryTypeDevice, attr.type);
    ASSERT_EQ(target, attr.device);

    // And the caller's own current device is left where it was.
    int current = -1;
    ASSERT_EQ(cudaSuccess, cudaGetDevice(&current));

    ASSERT_EQ(0, rdma::DeallocDeviceBlock(p));
    rdma::GlobalGdrRelease();
    rdma::FLAGS_rdma_enable_gdr = false;
    int after = -1;
    ASSERT_EQ(cudaSuccess, cudaGetDevice(&after));
    ASSERT_EQ(current, after);
}

TEST(GdrDeviceIdTest, out_of_range_device_id_is_rejected) {
    if (!FLAGS_rdma_test_enable || !FLAGS_gdr_test_real_device) {
        return;
    }
    int device_count = 0;
    ASSERT_EQ(cudaSuccess, cudaGetDeviceCount(&device_count));
    GdrDeviceIdFlag device_guard(device_count);
    rdma::g_skip_device_alloc_for_test = false;
    rdma::FLAGS_rdma_enable_gdr = true;
    ASSERT_EQ(-1, rdma::GlobalGdrInitialize());
    ASSERT_FALSE(rdma::IsGdrAvailable());
    rdma::FLAGS_rdma_enable_gdr = false;
}

// --------------------------- end to end ---------------------------

// Bounces the device attachment straight back. append_ref() shares the blocks
// the request landed in instead of copying them, so the reply is sent out of
// the very device memory the NIC wrote into and nothing here ever touches the
// data from the host -- which is the entire point of the feature.
class MyGdrEchoService : public ::test::EchoService {
    void Echo(google::protobuf::RpcController* cntl_base,
              const ::test::EchoRequest* req,
              ::test::EchoResponse* res,
              google::protobuf::Closure* done) {
        Controller* cntl = static_cast<Controller*>(cntl_base);
        ClosureGuard done_guard(done);
        res->set_message("MyEchoService");
        res->add_code_list(req->code());
        if (!cntl->has_device_channel()) {
            cntl->SetFailed(EDEVICECHANNEL, "No device channel on this "
                            "connection");
            return;
        }
        cntl->response_attachment().append(cntl->request_attachment());
        cntl->response_device_attachment().append_ref(
                cntl->request_device_attachment());
    }
};

// Copy the whole attachment out to the host, segment by segment. The reply
// generally comes back in different-sized pieces than it went out in -- it is
// cut out of the receive stream's blocks -- so this walks segments rather
// than assuming one.
//
// Plain memcpy for a host segment, and not merely as an optimization:
// cudaMemcpy would happily accept those too, so taking the bit at its word is
// the only thing here that would notice is_host lying.
static void CopyDeviceAttachmentToHost(const DeviceAttachment& da, char* out) {
    size_t off = 0;
    for (size_t i = 0; i < da.segment_count(); ++i) {
        const DeviceAttachment::Segment& seg = da.segment(i);
        if (seg.is_host) {
            memcpy(out + off, seg.ptr, seg.length);
        } else {
            const cudaError_t err = cudaMemcpy(out + off, seg.ptr, seg.length,
                                               cudaMemcpyDeviceToHost);
            ASSERT_EQ(cudaSuccess, err) << cudaGetErrorString(err);
        }
        off += seg.length;
    }
    ASSERT_EQ(da.size(), off);
}

TEST(GdrRpcTest, device_attachment_round_trips_over_the_wire) {
    if (!FLAGS_rdma_test_enable || !FLAGS_gdr_test_real_device) {
        return;
    }
    ASSERT_NO_FATAL_FAILURE(EnableGdrForTest());

    // Not g_ep: that one is filled in by RdmaTest's constructor, and this test
    // has to work under --gtest_filter='Gdr*' where none of those ever run.
    butil::ip_t ip;
    ASSERT_EQ(0, butil::str2ip(g_ip.c_str(), &ip));
    const butil::EndPoint ep(ip, PORT);

    // One size per round, picked against the 1MB device receive block:
    // comfortably inside one, exactly one, just over one (so a message
    // straddles two blocks), and several that leave the next message starting
    // at a different offset within a block every round. A single round always
    // starts from a fresh, block-aligned stream, which is precisely the state
    // in which a cut-accounting bug is invisible.
    //
    // The tiny ones are not padding: mlx5 hands a small enough payload to the
    // driver inside the CQE, which then memcpy's it into the receive scatter
    // entry from the host and faults on GPU memory. 8 and 40 hit that as a
    // whole message; 1024*1024+7 hits it with the 7-byte tail work request of
    // a big one. See the scatter-to-CQE note in rdma_helper.cpp.
    const size_t DEV_SIZES[] = {
        64 * 1024,
        8,
        4096,
        1024 * 1024 + 7,
        40,
        700 * 1024 - 13,
        1536 * 1024,
        128 * 1024,
        1024 * 1024,
        33 * 1024 + 1,
    };
    const int ROUND_COUNT = arraysize(DEV_SIZES);
    // Above the device SQ depth (16) once the larger rounds split a message
    // into several WRs, so the send path has to queue on the device stream
    // and drain from completions rather than posting everything up front.
    const int RPC_COUNT = 16;
    const size_t HOST_SIZE = 4096;

    // The device channel only exists in v3; the v2 hello has nowhere to put a
    // device qp_num, so the client never asks for one and every RPC below
    // would come back EDEVICECHANNEL.
    HandshakeVersionFlag version_guard(3);

    Server server;
    MyGdrEchoService svc;
    ASSERT_EQ(0, server.AddService(&svc, SERVER_DOESNT_OWN_SERVICE));
    {
        ServerOptions options;
        options.enabled_protocols = "baidu_std";
        options.socket_mode = SOCKET_MODE_RDMA_AND_DEVICE;
        options.internal_port = -1;
        ASSERT_EQ(0, server.Start(PORT, &options));
    }

    {
        Channel channel;
        ChannelOptions chan_options;
        chan_options.socket_mode = SOCKET_MODE_RDMA_AND_DEVICE;
        chan_options.connection_type = CONNECTION_TYPE_SINGLE;
        // Plain SOCKET_MODE_RDMA would leave the client never proposing a
        // device channel, and every RPC below would come back
        // EDEVICECHANNEL. The mode also makes the failure legible: if the
        // second channel cannot be brought up, the first RPC fails to connect
        // rather than the test failing 160 times.
        chan_options.connect_timeout_ms = 1000;
        chan_options.timeout_ms = 5000;
        chan_options.max_retry = 0;
        ASSERT_EQ(0, channel.Init(ep, &chan_options));

        // One connection, many rounds. Everything a single round cannot show
        // lives here: device receive blocks being recycled and re-posted
        // (the rounds together move far more than the 16MB receive window),
        // device credits coming back, the pending list draining to empty so
        // host credits stop being withheld, and Controller::Reset() actually
        // dropping both device attachments instead of accumulating them.
        Controller cntl[RPC_COUNT];
        test::EchoRequest req[RPC_COUNT];
        test::EchoResponse res[RPC_COUNT];
        std::vector<std::string> pattern(RPC_COUNT);
        std::string got;

        for (int round = 0; round < ROUND_COUNT; ++round) {
            const size_t dev_size = DEV_SIZES[round];
            for (int i = 0; i < RPC_COUNT; ++i) {
                cntl[i].Reset();
                res[i].Clear();
                // Every fourth RPC carries no device data at all, so PRPC and
                // GDRB frames are interleaved on a live connection and the
                // device-free ones get their chance to jump a pending list
                // that is waiting on someone else's tensor.
                const bool with_device = (i % 4 != 3);
                const int tag = round * RPC_COUNT + i;
                if (with_device) {
                    // Position-dependent, not a constant fill: a constant fill
                    // hides a cut that is off by a whole block, which is the
                    // one failure the frame header cannot rule out (see
                    // docs/cn/gdr_design.md section 13). 251 is prime, so any
                    // shift by less than 251 blocks shows up as a mismatch.
                    pattern[i].resize(dev_size);
                    for (size_t k = 0; k < dev_size; ++k) {
                        pattern[i][k] = (char)((k + tag) % 251);
                    }
                    void* dptr =
                        cntl[i].request_device_attachment().append_new(dev_size);
                    ASSERT_TRUE(dptr != nullptr)
                            << "round " << round << " rpc[" << i << "]: "
                            << berror(errno);
                    const cudaError_t err =
                        cudaMemcpy(dptr, pattern[i].data(), dev_size,
                                   cudaMemcpyHostToDevice);
                    ASSERT_EQ(cudaSuccess, err) << cudaGetErrorString(err);
                } else {
                    pattern[i].clear();
                }
                // Host payload alongside it, so both QPs carry data at once
                // and the two streams have a chance to get out of step.
                cntl[i].request_attachment().resize(HOST_SIZE, (char)('A' + tag % 26));
                req[i].set_message(__FUNCTION__);
                req[i].set_code(tag + 1);
                ::test::EchoService::Stub(&channel).Echo(
                        &cntl[i], &req[i], &res[i], DoNothing());
            }

            for (int i = 0; i < RPC_COUNT; ++i) {
                bthread_id_join(cntl[i].call_id());
                const int tag = round * RPC_COUNT + i;
                ASSERT_FALSE(cntl[i].Failed())
                        << "round " << round << " rpc[" << i << "]: "
                        << cntl[i].ErrorText();
                ASSERT_EQ(1, res[i].code_list_size())
                        << "round " << round << " rpc[" << i << "]";
                ASSERT_EQ(tag + 1, res[i].code_list(0));
                ASSERT_EQ(HOST_SIZE, cntl[i].response_attachment().size());
                ASSERT_EQ(pattern[i].size(),
                          cntl[i].response_device_attachment().size())
                        << "round " << round << " rpc[" << i << "]";
                if (pattern[i].empty()) {
                    continue;
                }
                got.assign(pattern[i].size(), '\0');
                ASSERT_NO_FATAL_FAILURE(CopyDeviceAttachmentToHost(
                        cntl[i].response_device_attachment(), &got[0]));
                ASSERT_TRUE(pattern[i] == got)
                        << "round " << round << " rpc[" << i << "] of "
                        << pattern[i].size() << " device bytes came back wrong";
            }
        }
    }

    // Every round above ran over the device QP, and the 8- and 40-byte ones
    // are exactly the payloads scatter-to-CQE would have memcpy'd into GPU
    // memory from the host. So the per-QP disable took effect -- and it did
    // so without the process-wide env var, meaning the host QP still has the
    // feature. If this fires, CreateDeviceQp() fell back to setenv(); the
    // reason is a WARNING near the start of the log.
    ASSERT_TRUE(nullptr == getenv("MLX5_SCATTER_TO_CQE"));

    server.Stop(0);
    server.Join();
    // No GlobalGdrRelease(): the client socket outlives the Channel in the
    // socket map with device receive blocks still posted to its QP, and
    // freeing the pool underneath it would leave the NIC writing into
    // cudaFree'd memory.
}

// The four combinations of source memory kind and landing memory kind.
//
// Source is per-append: append_user_data_with_lkey() is told which kind it
// is being handed, and both are accepted whatever this process is configured
// to allocate. Landing is -rdma_attachment_memory, which is process-wide and
// decided before the pool comes up, so the full matrix takes two runs of this
// binary:
//
//   (default) --rdma_attachment_memory=device  ->  host->device, device->device
//             --rdma_attachment_memory=host    ->  host->host,   device->host
//
// The test asserts which half it is running, so a run that silently landed in
// the wrong kind of memory fails rather than passing twice over the same
// path. What makes all four work is that neither is on the wire: the hello
// carries a block size and nothing about memory kinds, and the landing kind
// is settled entirely by which blocks the receiver posted to its own RQ.
TEST(GdrRpcTest, source_and_landing_memory_kinds_are_independent) {
    if (!FLAGS_rdma_test_enable || !FLAGS_gdr_test_real_device) {
        return;
    }
    ASSERT_NO_FATAL_FAILURE(EnableGdrForTest());
    const bool expect_host_landing =
        (rdma::FLAGS_rdma_attachment_memory == "host");
    // Both ends are this process, so the peer's landing kind is ours too.
    // That is the one thing a single-process test cannot vary independently,
    // and also the one thing neither end can observe about the other.
    ASSERT_EQ(!expect_host_landing, rdma::IsAttachmentMemoryDevice());

    butil::ip_t ip;
    ASSERT_EQ(0, butil::str2ip(g_ip.c_str(), &ip));
    const butil::EndPoint ep(ip, PORT);

    HandshakeVersionFlag version_guard(3);

    // One registration each, sliced per RPC with append_user_data_with_lkey().
    // Registering per RPC instead would burn a g_regions slot every time and
    // run out after 64.
    const size_t MAX_SIZE = 1536 * 1024;
    void* host_src = nullptr;
    ASSERT_EQ(0, posix_memalign(&host_src, 4096, MAX_SIZE));
    const uint32_t host_lkey = rdma::RegisterHostMemory(host_src, MAX_SIZE);
    ASSERT_NE(0u, host_lkey);
    void* dev_src = nullptr;
    ASSERT_EQ(cudaSuccess, cudaMalloc(&dev_src, MAX_SIZE));
    const uint32_t dev_lkey = rdma::RegisterDeviceMemory(dev_src, MAX_SIZE);
    ASSERT_NE(0u, dev_lkey);

    Server server;
    MyGdrEchoService svc;
    ASSERT_EQ(0, server.AddService(&svc, SERVER_DOESNT_OWN_SERVICE));
    {
        ServerOptions options;
        options.enabled_protocols = "baidu_std";
        options.socket_mode = SOCKET_MODE_RDMA_AND_DEVICE;
        options.internal_port = -1;
        ASSERT_EQ(0, server.Start(PORT, &options));
    }

    {
        Channel channel;
        ChannelOptions chan_options;
        chan_options.socket_mode = SOCKET_MODE_RDMA_AND_DEVICE;
        chan_options.connection_type = CONNECTION_TYPE_SINGLE;
        chan_options.connect_timeout_ms = 1000;
        chan_options.timeout_ms = 5000;
        chan_options.max_retry = 0;
        ASSERT_EQ(0, channel.Init(ep, &chan_options));
        // Under, exactly at, and over the 1MB device receive block, plus a
        // couple of small ones: a source-kind bug that only shows up when a
        // message straddles blocks would otherwise hide behind the small
        // cases, and vice versa for scatter-to-CQE.
        const size_t SIZES[] = {8, 64 * 1024, 1024 * 1024,
                                1024 * 1024 + 7, 40, MAX_SIZE};
        std::string pattern;
        std::string got;
        for (size_t s = 0; s < arraysize(SIZES); ++s) {
            const size_t size = SIZES[s];
            for (int host_source = 0; host_source < 2; ++host_source) {
                pattern.resize(size);
                for (size_t k = 0; k < size; ++k) {
                    pattern[k] = (char)((k + s * 2 + host_source) % 251);
                }
                if (host_source) {
                    memcpy(host_src, pattern.data(), size);
                } else {
                    ASSERT_EQ(cudaSuccess,
                              cudaMemcpy(dev_src, pattern.data(), size,
                                         cudaMemcpyHostToDevice));
                }

                Controller cntl;
                test::EchoRequest req;
                test::EchoResponse res;
                req.set_message("mixed");
                req.set_code(1);
                ASSERT_EQ(0, cntl.request_device_attachment()
                                     .append_user_data_with_lkey(
                                         host_source ? host_src : dev_src,
                                         size,
                                         host_source ? host_lkey : dev_lkey,
                                         host_source != 0, nullptr));
                ASSERT_EQ(host_source != 0,
                          cntl.request_device_attachment().segment(0).is_host);

                test::EchoService::Stub(&channel).Echo(&cntl, &req, &res, nullptr);
                ASSERT_FALSE(cntl.Failed())
                        << size << " bytes from "
                        << (host_source ? "host" : "device") << ": "
                        << cntl.ErrorText();

                const DeviceAttachment& out = cntl.response_device_attachment();
                ASSERT_EQ(size, out.size());
                // The reply came out of the blocks the request landed in, so
                // this is the landing kind at both ends -- and it is the
                // configured one no matter which kind was sent.
                ASSERT_EQ(expect_host_landing, out.is_host_readable())
                        << size << " bytes from "
                        << (host_source ? "host" : "device");
                got.assign(size, '\0');
                ASSERT_NO_FATAL_FAILURE(
                        CopyDeviceAttachmentToHost(out, &got[0]));
                ASSERT_TRUE(pattern == got)
                        << size << " bytes from "
                        << (host_source ? "host" : "device")
                        << " came back wrong";
            }
        }
    }

    server.Stop(0);
    server.Join();
    // The attachments are gone, so the registrations they borrowed can go.
    // Not the pool: the client socket outlives the Channel with receive
    // blocks still posted to its QP.
    rdma::DeregisterDeviceMemory(dev_src);
    ASSERT_EQ(cudaSuccess, cudaFree(dev_src));
    rdma::DeregisterHostMemory(host_src);
    free(host_src);
}

TEST(GdrRpcTest, device_channel_mode_fails_the_connection) {
    if (!FLAGS_rdma_test_enable || !FLAGS_gdr_test_real_device) {
        return;
    }
    ASSERT_NO_FATAL_FAILURE(EnableGdrForTest());

    butil::ip_t ip;
    ASSERT_EQ(0, butil::str2ip(g_ip.c_str(), &ip));
    const butil::EndPoint ep(ip, PORT);

    // A server that simply has not enabled the second channel. Both ends are
    // v3 and both are otherwise fully capable -- the client's configuration is
    // one this process accepts (see device_channel_mode_needs_the_v3_handshake
    // for the ones it rejects at startup) -- which is the point: what is being
    // tested is that an unmet demand fails the connection, not that a crippled
    // local configuration is caught. This is the mixed-deployment case: rolling
    // GDR out to the clients before the servers.
    HandshakeVersionFlag version_guard(3);

    Server server;
    // The plain echo service, not MyGdrEchoService: nothing here sends device
    // data, and the second half below has to be able to succeed.
    MyEchoService svc;
    ASSERT_EQ(0, server.AddService(&svc, SERVER_DOESNT_OWN_SERVICE));
    {
        ServerOptions options;
        options.enabled_protocols = "baidu_std";
        options.socket_mode = SOCKET_MODE_RDMA;
        options.internal_port = -1;
        ASSERT_EQ(0, server.Start(PORT, &options));
    }

    {
        Channel channel;
        ChannelOptions chan_options;
        chan_options.socket_mode = SOCKET_MODE_RDMA_AND_DEVICE;
        chan_options.connection_type = CONNECTION_TYPE_SINGLE;
        chan_options.connect_timeout_ms = 1000;
        chan_options.timeout_ms = 5000;
        chan_options.max_retry = 0;
        // Init() does not connect, so the requirement cannot be reported
        // here. It is a property of the connection, and there is none yet.
        ASSERT_EQ(0, channel.Init(ep, &chan_options));

        Controller cntl;
        test::EchoRequest req;
        test::EchoResponse res;
        req.set_message("no device channel here");
        req.set_code(1);
        test::EchoService::Stub(&channel).Echo(&cntl, &req, &res, nullptr);
        ASSERT_TRUE(cntl.Failed());
        // The connection never reached ESTABLISHED, so this is a connect
        // failure carrying our errno rather than a reply. Without the
        // requirement the same RPC would have succeeded host-only, which is
        // exactly the silent degradation being ruled out.
        ASSERT_EQ(EDEVICECHANNEL, cntl.ErrorCode()) << cntl.ErrorText();
    }

    // And a channel that did not ask is unaffected on the same server, which
    // is what the SocketMap isolation buys: the failure above is scoped to the
    // channel that demanded something, not to the address.
    {
        Channel channel;
        ChannelOptions chan_options;
        chan_options.socket_mode = SOCKET_MODE_RDMA;
        chan_options.connection_type = CONNECTION_TYPE_SINGLE;
        chan_options.connect_timeout_ms = 1000;
        chan_options.timeout_ms = 5000;
        chan_options.max_retry = 0;
        ASSERT_EQ(0, channel.Init(ep, &chan_options));

        Controller cntl;
        test::EchoRequest req;
        test::EchoResponse res;
        req.set_message("no device channel needed");
        req.set_code(1);
        test::EchoService::Stub(&channel).Echo(&cntl, &req, &res, nullptr);
        ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();
        ASSERT_EQ("MyEchoService", res.message());
    }

    server.Stop(0);
    server.Join();
}

// The shape of a GDR connection: two QPs, four CQs, one comp channel
// (docs/cn/gdr_design.md section 10). It is the design sentence itself, and
// it is what keeps a connection at one fd and one epoll registration however
// many QPs it runs -- PollCq() drains all four CQs on a single wakeup.
//
// Also the only place the CQ depths are checked against the queues feeding
// them. A CQ shallower than its queue overflows once the queue fills, which
// puts the CQ into an error state and takes the QP with it; AllocateQpCq()
// used to size every host CQ at -rdma_prepared_qp_size no matter how deep the
// queues were, and the connections that missed the prepared pool are exactly
// the ones with queues too deep for it.
TEST(GdrRpcTest, four_cqs_report_to_one_comp_channel) {
    if (!FLAGS_rdma_test_enable || !FLAGS_gdr_test_real_device) {
        return;
    }
    ASSERT_NO_FATAL_FAILURE(EnableGdrForTest());

    butil::ip_t ip;
    ASSERT_EQ(0, butil::str2ip(g_ip.c_str(), &ip));
    const butil::EndPoint ep(ip, PORT);

    HandshakeVersionFlag version_guard(3);

    Server server;
    MyGdrEchoService svc;
    ASSERT_EQ(0, server.AddService(&svc, SERVER_DOESNT_OWN_SERVICE));
    {
        ServerOptions options;
        options.enabled_protocols = "baidu_std";
        options.socket_mode = SOCKET_MODE_RDMA_AND_DEVICE;
        options.internal_port = -1;
        ASSERT_EQ(0, server.Start(PORT, &options));
    }

    {
        Channel channel;
        ChannelOptions chan_options;
        chan_options.socket_mode = SOCKET_MODE_RDMA_AND_DEVICE;
        chan_options.connection_type = CONNECTION_TYPE_SINGLE;
        chan_options.connect_timeout_ms = 1000;
        chan_options.timeout_ms = 5000;
        chan_options.max_retry = 0;
        ASSERT_EQ(0, channel.Init(ep, &chan_options));

        // One RPC only, to get a connection that finished its handshake.
        Controller cntl;
        test::EchoRequest req;
        test::EchoResponse res;
        req.set_message(__FUNCTION__);
        req.set_code(1);
        test::EchoService::Stub(&channel).Echo(&cntl, &req, &res, nullptr);
        ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();

        SocketUniquePtr s;
        ASSERT_EQ(0, Socket::Address(cntl._single_server_id, &s));
        rdma::RdmaEndpoint* rep = RdmaTransportOf(s)->_rdma_ep;
        ASSERT_TRUE(rep != NULL);
        ASSERT_TRUE(rep->has_device_channel());
        ASSERT_TRUE(rep->_comp_channel != NULL);

        rdma::RdmaResource* host = rep->_host.resource;
        rdma::RdmaResource* dev = rep->_device->resource;
        ASSERT_TRUE(host != NULL);
        ASSERT_TRUE(dev != NULL);

        // Two QPs, and neither channel is quietly posting on the other's.
        ASSERT_TRUE(host->qp != NULL);
        ASSERT_TRUE(dev->qp != NULL);
        ASSERT_NE(host->qp, dev->qp);

        // Four distinct CQs. No polling CQ on either: GDR rules polling mode
        // out at startup, and this is where that is observable.
        ibv_cq* cqs[4] = { host->send_cq, host->recv_cq,
                           dev->send_cq, dev->recv_cq };
        ASSERT_TRUE(host->polling_cq == NULL);
        ASSERT_TRUE(dev->polling_cq == NULL);
        for (int i = 0; i < 4; ++i) {
            ASSERT_TRUE(cqs[i] != NULL) << "cq " << i;
            for (int j = i + 1; j < 4; ++j) {
                ASSERT_NE(cqs[i], cqs[j]) << "cq " << i << " vs " << j;
            }
            // The one comp channel, which is the whole point.
            ASSERT_EQ(rep->_comp_channel->channel, cqs[i]->channel)
                << "cq " << i;
        }

        // And each CQ is at least as deep as the queue that feeds it. The
        // driver may round up, never down.
        ASSERT_GE(host->send_cq->cqe, (int)rep->_host.sq_size);
        ASSERT_GE(host->recv_cq->cqe, (int)rep->_host.rq_size);
        ASSERT_GE(dev->send_cq->cqe, (int)rep->_device->sq_size);
        ASSERT_GE(dev->recv_cq->cqe, (int)rep->_device->rq_size);
    }

    server.Stop(0);
    server.Join();
}

// The device SQ holds --rdma_device_sq_size (16) work requests and the peer
// posts --rdma_device_rq_size (16) receive blocks, so a message of more than
// ~16MB cannot go out in one go: the writer posts what the credits allow,
// parks, and resumes when the poller returns them. Nothing else drives that
// loop -- the host bytes of these RPCs are a few hundred and drain on the
// first DoWrite() -- so this is the test that would hang if the device queue
// ever lost its consumer.
//
// It is the regression guard for making the device send path lock-free: the
// poller used to post the backlog itself, and now only returns credits and
// calls WakeAsEpollOut(), leaving Socket::IsWriteComplete() ->
// Transport::HasPendingWrite() to keep KeepWrite alive. Get that wrong in
// either direction and this either hangs (KeepWrite exits, backlog stranded)
// or spins a core (KeepWrite loops without parking).
TEST(GdrRpcTest, big_device_attachment_drains_through_backpressure) {
    if (!FLAGS_rdma_test_enable || !FLAGS_gdr_test_real_device) {
        return;
    }
    ASSERT_NO_FATAL_FAILURE(EnableGdrForTest());

    butil::ip_t ip;
    ASSERT_EQ(0, butil::str2ip(g_ip.c_str(), &ip));
    const butil::EndPoint ep(ip, PORT);

    HandshakeVersionFlag version_guard(3);

    Server server;
    MyGdrEchoService svc;
    ASSERT_EQ(0, server.AddService(&svc, SERVER_DOESNT_OWN_SERVICE));
    {
        ServerOptions options;
        options.enabled_protocols = "baidu_std";
        options.socket_mode = SOCKET_MODE_RDMA_AND_DEVICE;
        options.internal_port = -1;
        ASSERT_EQ(0, server.Start(PORT, &options));
    }

    // 64MB is 64 work requests against a 16-deep SQ and a 16-block peer RQ,
    // so the writer has to park and resume at least three times per message
    // -- in each direction, since the service echoes it back.
    const size_t BIG_SIZE = 64u << 20;
    // And a second phase where several oversized messages are queued before
    // any of them can drain, which is what proves the backlog stays FIFO
    // across AppendForSend() calls rather than just across work requests.
    const size_t CONCURRENT_SIZE = 12u << 20;
    const int CONCURRENT_COUNT = 4;

    {
        Channel channel;
        ChannelOptions chan_options;
        chan_options.socket_mode = SOCKET_MODE_RDMA_AND_DEVICE;
        chan_options.connection_type = CONNECTION_TYPE_SINGLE;
        chan_options.connect_timeout_ms = 1000;
        // Generous: a hang shows up as this firing, not as a stuck test.
        chan_options.timeout_ms = 30000;
        chan_options.max_retry = 0;
        ASSERT_EQ(0, channel.Init(ep, &chan_options));
        // Position-dependent fill, prime stride: a constant fill would hide a
        // backlog replayed out of order, which is exactly what this test is
        // about.
        std::string pattern;
        std::string got;
        void* dptr = nullptr;

        pattern.resize(BIG_SIZE);
        for (size_t k = 0; k < BIG_SIZE; ++k) {
            pattern[k] = (char)(k % 251);
        }
        {
            Controller cntl;
            test::EchoRequest req;
            test::EchoResponse res;
            req.set_message(__FUNCTION__);
            req.set_code(1);
            dptr = cntl.request_device_attachment().append_new(BIG_SIZE);
            ASSERT_TRUE(dptr != nullptr) << berror(errno);
            ASSERT_EQ(cudaSuccess,
                      cudaMemcpy(dptr, pattern.data(), BIG_SIZE,
                                 cudaMemcpyHostToDevice));
            // No host attachment on purpose: the host stream must run dry
            // long before the device one does.
            test::EchoService::Stub(&channel).Echo(&cntl, &req, &res, nullptr);
            ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();
            ASSERT_EQ(BIG_SIZE, cntl.response_device_attachment().size());
            got.assign(BIG_SIZE, '\0');
            ASSERT_NO_FATAL_FAILURE(CopyDeviceAttachmentToHost(
                    cntl.response_device_attachment(), &got[0]));
            ASSERT_TRUE(pattern == got) << BIG_SIZE
                    << " device bytes came back wrong after backpressure";
        }

        Controller cntl[CONCURRENT_COUNT];
        test::EchoRequest req[CONCURRENT_COUNT];
        test::EchoResponse res[CONCURRENT_COUNT];
        std::vector<std::string> want(CONCURRENT_COUNT);
        for (int i = 0; i < CONCURRENT_COUNT; ++i) {
            want[i].resize(CONCURRENT_SIZE);
            for (size_t k = 0; k < CONCURRENT_SIZE; ++k) {
                want[i][k] = (char)((k + i) % 251);
            }
            dptr = cntl[i].request_device_attachment().append_new(CONCURRENT_SIZE);
            ASSERT_TRUE(dptr != nullptr) << "rpc[" << i << "]: " << berror(errno);
            ASSERT_EQ(cudaSuccess,
                      cudaMemcpy(dptr, want[i].data(), CONCURRENT_SIZE,
                                 cudaMemcpyHostToDevice));
            req[i].set_message(__FUNCTION__);
            req[i].set_code(i + 1);
            test::EchoService::Stub(&channel).Echo(
                    &cntl[i], &req[i], &res[i], DoNothing());
        }
        for (int i = 0; i < CONCURRENT_COUNT; ++i) {
            bthread_id_join(cntl[i].call_id());
            ASSERT_FALSE(cntl[i].Failed())
                    << "rpc[" << i << "]: " << cntl[i].ErrorText();
            ASSERT_EQ(CONCURRENT_SIZE, cntl[i].response_device_attachment().size())
                    << "rpc[" << i << "]";
            got.assign(CONCURRENT_SIZE, '\0');
            ASSERT_NO_FATAL_FAILURE(CopyDeviceAttachmentToHost(
                    cntl[i].response_device_attachment(), &got[0]));
            ASSERT_TRUE(want[i] == got)
                    << "rpc[" << i << "] came back with someone else's bytes";
        }
    }

    server.Stop(0);
    server.Join();
    // No GlobalGdrRelease(), for the reason given at the end of the previous
    // test. This is the last test in the file.
}

#endif  // if BRPC_WITH_GDR

#endif  // if BRPC_WITH_RDMA

int main(int argc, char* argv[]) {
    // gflags before gtest, and on a copy of argv.
    //
    // .bazelrc passes --define absl=1, which builds googletest with
    // GTEST_HAS_ABSL=1, which makes InitGoogleTest() parse the command line
    // with Abseil's flag parser instead of its own. That parser rewrites argv
    // down to the positional arguments and silently discards every
    // "--flag=value" it does not recognise -- i.e. every gflags flag, ours
    // included. The two-token form "--flag value" survives as two positionals,
    // which is why the breakage looks selective rather than total:
    // --rdma_test_enable arrives, --rdma_attachment_memory=host is thrown away
    // and the flag reads as its default, with nothing logged either way.
    //
    // Parsing before InitGoogleTest() is what fixes it. Parsing a copy keeps
    // gtest's view of argv pristine, because gflags permutes positional
    // arguments to the end even when asked not to remove anything. And
    // AllowCommandLineReparsing() is what stops gflags from dying on
    // --gtest_*, which is not its flag to know.
    std::vector<char*> gflags_argv(argv, argv + argc);
    gflags_argv.push_back(nullptr);
    int gflags_argc = argc;
    char** gflags_argv_data = gflags_argv.data();
    GFLAGS_NAMESPACE::AllowCommandLineReparsing();
    GFLAGS_NAMESPACE::ParseCommandLineFlags(&gflags_argc, &gflags_argv_data,
                                            false);
    testing::InitGoogleTest(&argc, argv);
#if BRPC_WITH_RDMA
    rdma::FLAGS_rdma_trace_verbose = true;
    rdma::FLAGS_rdma_memory_pool_max_regions = 2;
    FLAGS_log_idle_connection_close = true;
    if (!FLAGS_rdma_test_enable) {
        // skip UT requiring rdma runtime environment
        rdma::g_rdma_available.store(true, butil::memory_order_relaxed);
        rdma::g_skip_rdma_init = true;
    }
#endif  // if BRPC_WITH_RDMA
    return RUN_ALL_TESTS();
}
