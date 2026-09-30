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

#ifndef BRPC_SOCKET_MODE_H
#define BRPC_SOCKET_MODE_H
namespace brpc {
enum SocketMode {
    SOCKET_MODE_TCP = 0,
    SOCKET_MODE_RDMA = 1,
    SOCKET_MODE_UBRING = 2,
    SOCKET_MODE_URMA = 3,
    // RDMA, plus the second data channel that carries DeviceAttachment (see
    // docs/cn/gdr_design.md). Only one end can propose the channel, so the
    // mode reads slightly differently at the two:
    //   * on a Channel it is a demand -- the handshake asks for a device
    //     channel, and a connection whose peer does not agree to one fails
    //     rather than coming up without it. That is what lets an RPC hand off
    //     a device attachment without asking first.
    //   * on a Server it is permission to agree. A client that asks is given
    //     a device channel, one that does not is served host-only. Without
    //     the mode the server declines, because agreeing costs rq_size *
    //     -rdma_gdr_recv_block_size of registered memory per connection and
    //     no client should be able to make a server spend that by asking.
    //
    // Being a mode rather than a flag is deliberate on the client side: it
    // puts the demand in the SocketMap key, so these connections never end up
    // shared with a channel that did not ask.
    SOCKET_MODE_RDMA_AND_DEVICE = 4
};

// True for both modes that speak RDMA. Most code cares only about that, and
// gets this wrong by testing == SOCKET_MODE_RDMA.
inline bool IsRdmaSocketMode(SocketMode mode) {
    return mode == SOCKET_MODE_RDMA || mode == SOCKET_MODE_RDMA_AND_DEVICE;
}
}  // namespace brpc

#endif  // BRPC_SOCKET_MODE_H
