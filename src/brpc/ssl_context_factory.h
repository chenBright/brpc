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

#ifndef BRPC_SSL_FACTORY_H
#define BRPC_SSL_FACTORY_H

#include <memory>
#include <openssl/types.h>
#include "butil/macros.h"
#include "brpc/socket_id.h"

namespace brpc {

struct SocketSSLContext {
    SocketSSLContext();
    ~SocketSSLContext();

    SSL_CTX* raw_ctx;                        // owned
    std::string sni_name;                    // useful for clients
    std::vector<std::string> alpn_protocols; // useful for clients
};

class SSLContextFactory {
public:
    virtual ~SSLContextFactory() = default;

    virtual SSL* SSLHandshake(SocketUniquePtr& sock, int fd) = 0;

    virtual void Shutdown(SSL* ssl_seesion) = 0;
};

class DefaultSSLContextFactory : public SSLContextFactory {
public:
    explicit DefaultSSLContextFactory(const std::shared_ptr<SocketSSLContext>& ssl_ctx, bool server_mode)
        : _ssl_ctx(ssl_ctx), _server_mode(server_mode) {
        RELEASE_ASSERT(NULL != _ssl_ctx);
    }

    ~DefaultSSLContextFactory() override = default;

    SSL* SSLHandshake(SocketUniquePtr& sock, int fd) override;

    void Shutdown(SSL* ssl_session) override;

private:
    std::shared_ptr<SocketSSLContext> _ssl_ctx;
    // Whether the SSL context is used for server or client.
    bool _server_mode;
};

}

#endif // BRPC_SSL_FACTORY_H
