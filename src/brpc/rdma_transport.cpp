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

#include "brpc/rdma_transport.h"
#include "brpc/event_dispatcher.h"
#include "brpc/tcp_transport.h"
#include "brpc/input_messenger.h"
#include "brpc/rdma/rdma_endpoint.h"
#include "brpc/rdma/rdma_handshake.h"
#include "brpc/rdma/device_memory.h"
#include "brpc/rdma/rdma_helper.h"

namespace brpc {
DECLARE_bool(usercode_in_coroutine);
DECLARE_bool(usercode_in_pthread);

extern SocketVarsCollector *g_vars;

void RdmaTransport::Init(Socket *socket, const SocketOptions &options) {
    CHECK(_rdma_ep == nullptr);
    if (IsRdmaSocketMode(options.socket_mode)) {
        // The endpoint reads socket_mode() for itself, including whether this
        // connection runs a device channel. Socket::Create() has already
        // stored it, which is what makes that readable this early.
        _rdma_ep = new rdma::RdmaEndpoint(socket);
        _rdma_state = RDMA_UNKNOWN;
    } else {
        _rdma_state = RDMA_OFF;
        socket->_socket_mode = SOCKET_MODE_TCP;
    }
    _socket = socket;
    _default_connect = options.app_connect;
    _on_edge_trigger = options.on_edge_triggered_events;
    if (options.need_on_edge_trigger && _on_edge_trigger == nullptr) {
        if (_rdma_ep != nullptr) {
            _on_edge_trigger = rdma::RdmaEndpoint::OnNewDataFromTcp;
        } else {
            _on_edge_trigger = InputMessenger::OnNewMessages;
        }
    }
    _tcp_transport = std::make_shared<TcpTransport>();
    _tcp_transport->Init(socket, options);
}

void RdmaTransport::Release() {
    if (_rdma_ep) {
        delete _rdma_ep;
        _rdma_ep = nullptr;
        _rdma_state = RDMA_UNKNOWN;
    }
}

int RdmaTransport::Reset(int32_t expected_nref) {
    if (_rdma_ep) {
        _rdma_ep->Reset();
        _rdma_state = RDMA_UNKNOWN;
    }
    return 0;
}

std::shared_ptr<AppConnect> RdmaTransport::Connect() {
    if (_default_connect == nullptr) {
        return  std::make_shared<rdma::RdmaConnect>();
    }
    return _default_connect;
}

int RdmaTransport::CutFromIOBuf(butil::IOBuf *buf) {
    // Only send over the RDMA channel once the handshake has NEGOTIATED it
    // (RDMA_ON). While the state is still RDMA_UNKNOWN (handshake in progress,
    // or a server connection that turned out to be plain TCP and never
    // handshook) or RDMA_OFF (fell back), the QP is not usable and everything
    // must go over the TCP fd. Mirrors the RDMA_ON check in WaitEpollOut().
    if (_rdma_ep && _rdma_state == RDMA_ON) {
        butil::IOBuf *data_arr[1] = {buf};
        return _rdma_ep->CutFromIOBufList(data_arr, 1);
    } else {
        return _tcp_transport->CutFromIOBuf(buf);
    }
}

ssize_t RdmaTransport::CutFromIOBufList(butil::IOBuf **buf, size_t ndata) {
    if (_rdma_ep && _rdma_state == RDMA_ON) {
        return _rdma_ep->CutFromIOBufList(buf, ndata);
    }
    return _tcp_transport->CutFromIOBufList(buf, ndata);
}

int RdmaTransport::WaitEpollOut(butil::atomic<int> *_epollout_butex,
                                    bool pollin, const timespec duetime) {
    if (_rdma_state == RDMA_ON) {
        const int expected_val = _epollout_butex->load(butil::memory_order_acquire);
        CHECK(_rdma_ep != nullptr);
        if (!_rdma_ep->IsWritable()) {
            g_vars->nwaitepollout << 1;
            if (bthread::butex_wait(_epollout_butex, expected_val, &duetime) < 0) {
                if (errno != EAGAIN && errno != ETIMEDOUT) {
                    const int saved_errno = errno;
                    PLOG(WARNING) << "Fail to wait rdma window of " << _socket;
                    _socket->SetFailed(saved_errno,
                                       "Fail to wait rdma window of %s: %s",
                                       _socket->description().c_str(),
                                       berror(saved_errno));
                }
                if (_socket->Failed()) {
                    // NOTE:
                    // Different from TCP, we cannot find the RDMA channel
                    // failed by writing to it. Thus we must check if it
                    // is already failed here.
                    return 1;
                }
            }
        }
    } else {
        return _tcp_transport->WaitEpollOut(_epollout_butex, pollin, duetime);
    }
    return 0;
}

void RdmaTransport::ProcessEvent(bthread_attr_t attr) {
    bthread_t tid;
    if (FLAGS_usercode_in_coroutine) {
        OnEdge(_socket);
    } else if (!EventDispatcherUnsched()) {
        auto rc = bthread_start_urgent(&tid, &attr, OnEdge, _socket);
        if (rc != 0) {
            LOG(FATAL) << "Fail to start ProcessEvent";
            OnEdge(_socket);
        }
    } else if (bthread_start_background(&tid, &attr, OnEdge, _socket) != 0) {
        LOG(FATAL) << "Fail to start ProcessEvent";
        OnEdge(_socket);
    }
}

void RdmaTransport::QueueMessage(InputMessageClosure& input_msg,
                                 int* num_bthread_created, bool last_msg) {
    if (last_msg && !rdma::FLAGS_rdma_use_polling) {
        return;
    }
    InputMessageBase* to_run_msg = input_msg.release();
    if (!to_run_msg) {
        return;
    }

    if (rdma::FLAGS_rdma_disable_bthread) {
        ProcessInputMessage(to_run_msg);
        return;
    }
    // Create bthread for last_msg. The bthread is not scheduled
    // until bthread_flush() is called (in the worse case).

    // TODO(gejun): Join threads.
    bthread_t th;
    bthread_attr_t tmp = (FLAGS_usercode_in_pthread ?
                                      BTHREAD_ATTR_PTHREAD :
                                                                    BTHREAD_ATTR_NORMAL) | BTHREAD_NOSIGNAL;
    tmp.keytable_pool = _socket->keytable_pool();
    tmp.tag = bthread_self_tag();
    bthread_attr_set_name(&tmp, "ProcessInputMessage");

    if (!FLAGS_usercode_in_coroutine && bthread_start_background(
            &th, &tmp, ProcessInputMessage, to_run_msg) == 0) {
        ++*num_bthread_created;
    } else {
        ProcessInputMessage(to_run_msg);
    }
}

void RdmaTransport::Debug(std::ostream &os) {
    if (_rdma_state == RDMA_ON && _rdma_ep) {
        _rdma_ep->DebugInfo(os);
    }
}

DeviceStream* RdmaTransport::GetDeviceStream() {
    if (_rdma_state != RDMA_ON || _rdma_ep == nullptr ||
        !_rdma_ep->has_device_channel()) {
        return nullptr;
    }
    return _rdma_ep->device_stream();
}

DeviceChannelState RdmaTransport::GetDeviceChannelState() {
    if (_rdma_state == RDMA_UNKNOWN) {
        // Handshake still in flight. Whether it ends in a device channel
        // depends on what the peer answers, which has not happened yet.
        return DEVICE_CHANNEL_UNDECIDED;
    }
    return GetDeviceStream() ? DEVICE_CHANNEL_ON : DEVICE_CHANNEL_OFF;
}

bool RdmaTransport::HasPendingWrite() const {
    // Deliberately not gated on _rdma_state: only CutFromIOBufList() can
    // empty this queue, so if the state somehow moved away from RDMA_ON with
    // bytes still in it, hiding them here would strand them.
    return _rdma_ep != nullptr && _rdma_ep->HasQueuedDeviceData();
}

void RdmaTransport::DiscardPendingWrite() {
    if (_rdma_ep != nullptr) {
        _rdma_ep->DiscardQueuedDeviceData();
    }
}

int RdmaTransport::ContextInitOrDie(bool serverOrNot, const void* _options) {
    SocketMode socket_mode = SOCKET_MODE_TCP;
    if (serverOrNot) {
        const ServerOptions* opt = static_cast<const ServerOptions*>(_options);
        socket_mode = opt->socket_mode;
        if (!OptionsAvailableOverRdma(opt)) {
            return -1;
        }
        rdma::GlobalRdmaInitializeOrDie();
        if (!rdma::InitPollingModeWithTag(opt->bthread_tag)) {
            return -1;
        }
    } else {
        const ChannelOptions* opt =
            static_cast<const ChannelOptions*>(_options);
        socket_mode = opt->socket_mode;
        if (!OptionsAvailableForRdma(opt)) {
            return -1;
        }
        rdma::GlobalRdmaInitializeOrDie();
        if (!rdma::InitPollingModeWithTag(bthread_self_tag())) {
            return -1;
        }
    }

    // Belt and braces for the checks above: they test the configuration, this
    // tests the result. GlobalRdmaInitializeOrDie() exits the process on most
    // GDR startup failures, but not on all of them -- a build without
    // BRPC_WITH_GDR, for instance, simply has no pool to offer. Either way the
    // operator hears about it here, at startup, instead of from a connection
    // that will not establish.
    if (socket_mode == SOCKET_MODE_RDMA_AND_DEVICE && !rdma::IsGdrAvailable()) {
        LOG(ERROR) << "SOCKET_MODE_RDMA_AND_DEVICE needs the second channel's "
                      "memory pool, which failed to initialize";
        return -1;
    }
    return 0;
}

// Reject a device-channel configuration that cannot possibly come up.
//
// Each of these used to be a silent downgrade: the endpoint gave up on the
// device channel during the handshake, the end-of-handshake demand check then
// failed the connection with EDEVICECHANNEL, and the operator was left with
// connections that refuse to establish and no hint as to why. A configuration
// mistake belongs in the startup log, once. Shared by both directions because
// every one of them is a property of this process, not of the peer.
//
// `is_client` gates only the handshake version, which is the one thing a
// server does not choose.
static bool DeviceChannelOptionsAvailable(SocketMode socket_mode,
                                          bool is_client) {
    if (socket_mode != SOCKET_MODE_RDMA_AND_DEVICE) {
        return true;
    }
    if (!rdma::FLAGS_rdma_enable_gdr) {
        LOG(ERROR) << "SOCKET_MODE_RDMA_AND_DEVICE needs the second channel's "
                      "memory pool: set -rdma_enable_gdr";
        return false;
    }
    if (rdma::FLAGS_rdma_use_polling) {
        LOG(ERROR) << "SOCKET_MODE_RDMA_AND_DEVICE does not support "
                      "-rdma_use_polling: the device CQs share the host "
                      "connection's comp_channel, which polling mode does not "
                      "use. Unset one of the two";
        return false;
    }
    if (is_client && rdma::FLAGS_rdma_client_handshake_version < 3) {
        // The device qp_num has nowhere to ride in a v2 hello, so a v2 client
        // can never advertise a device channel.
        LOG(ERROR) << "SOCKET_MODE_RDMA_AND_DEVICE needs the v3 handshake: set "
                      "-rdma_client_handshake_version=3 (now "
                   << rdma::FLAGS_rdma_client_handshake_version << ")";
        return false;
    }
    return true;
}

bool RdmaTransport::OptionsAvailableForRdma(const ChannelOptions* opt) {
    if (opt->has_ssl_options()) {
        LOG(WARNING) << "Cannot use SSL and RDMA at the same time";
        return false;
    }
    if (!rdma::SupportedByRdma(opt->protocol.name())) {
        LOG(WARNING) << "Cannot use " << opt->protocol.name()
                     << " over RDMA";
        return false;
    }
    if (!DeviceChannelOptionsAvailable(opt->socket_mode, true)) {
        return false;
    }
    return true;
}

bool RdmaTransport::OptionsAvailableOverRdma(const ServerOptions* opt) {
    if (opt->rtmp_service) {
        LOG(WARNING) << "RTMP is not supported by RDMA";
        return false;
    }
    if (opt->has_ssl_options()) {
        LOG(WARNING) << "SSL is not supported by RDMA";
        return false;
    }
    if (opt->nshead_service) {
        LOG(WARNING) << "NSHEAD is not supported by RDMA";
        return false;
    }
    if (opt->mongo_service_adaptor) {
        LOG(WARNING) << "MONGO is not supported by RDMA";
        return false;
    }
    if (!DeviceChannelOptionsAvailable(opt->socket_mode, false)) {
        return false;
    }
    return true;
}
} // namespace brpc
#endif
