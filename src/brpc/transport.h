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

#ifndef BRPC_TRANSPORT_H
#define BRPC_TRANSPORT_H
#include "brpc/input_messenger.h"
#include "brpc/socket.h"
#include "server.h"

namespace brpc {
using OnEdgeTrigger = std::function<void (Socket*)>;
class Transport {
    friend class TransportFactory;
public:
    static void* OnEdge(void* arg) {
        // the enclosed Socket is valid and free to access inside this function.
        SocketUniquePtr s(static_cast<Socket*>(arg));
        const OnEdgeTrigger on_edge_trigger = s->_transport->GetOnEdgeTrigger();
        on_edge_trigger(s.get());
        return nullptr;
    }

    static void* ProcessInputMessage(void* void_arg) {
        InputMessageBase* msg = static_cast<InputMessageBase*>(void_arg);
        msg->_process(msg);
        return nullptr;
    }
    virtual ~Transport() = default;
    virtual void Init(Socket* socket, const SocketOptions& options) = 0;
    virtual void Release() = 0;
    virtual int Reset(int32_t expected_nref) = 0;
    virtual std::shared_ptr<AppConnect> Connect() = 0;
    virtual int CutFromIOBuf(butil::IOBuf* buf) = 0;
    virtual ssize_t CutFromIOBufList(butil::IOBuf** buf, size_t ndata) = 0;
    virtual int WaitEpollOut(butil::atomic<int>* _epollout_butex, bool pollin, timespec duetime) = 0;
    virtual void ProcessEvent(bthread_attr_t attr) = 0;
    virtual void QueueMessage(InputMessageClosure& input_msg, int* num_bthread_created, bool last_msg) = 0;
    virtual void Debug(std::ostream &os) = 0;

    // The device (GPU memory) side channel of this connection, or nullptr when
    // the transport has none. Only RdmaTransport can ever return non-nullptr,
    // and only after a successful device-channel negotiation.
    virtual DeviceStream* GetDeviceStream() { return nullptr; }

    // Whether GetDeviceStream() can still start returning non-nullptr. A
    // transport that never negotiates anything is OFF from the start; only
    // RdmaTransport spends time UNDECIDED, between Init() and the end of its
    // handshake.
    virtual DeviceChannelState GetDeviceChannelState() {
        return DEVICE_CHANNEL_OFF;
    }

    // Whether the transport is still holding bytes that only the socket's
    // writer can push out. TCP holds nothing -- whatever CutFromIOBufList()
    // accepted is already in the kernel. RdmaTransport's device channel does:
    // a DeviceAttachment queued while the device credits were shut waits
    // there. Socket::IsWriteComplete() consults this so that KeepWrite stays
    // alive to post the rest; declaring the write finished would leave the
    // queue with nobody to drain it.
    virtual bool HasPendingWrite() const { return false; }

    // Drop what HasPendingWrite() reports. Called once the socket has given
    // up on the write, where the bytes can never go out anyway and holding
    // them would pin device memory until the socket is recycled.
    virtual void DiscardPendingWrite() {}

    bool HasOnEdgeTrigger() {
        return _on_edge_trigger != nullptr;
    }
    OnEdgeTrigger GetOnEdgeTrigger() {
        return _on_edge_trigger;
    }
protected:
    Socket* _socket;
    std::shared_ptr<AppConnect> _default_connect;
    OnEdgeTrigger _on_edge_trigger;
};
}
#endif //BRPC_TRANSPORT_H