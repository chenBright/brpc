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

#include "brpc/ssl_context_factory.h"

#ifdef USE_MESALINK
#include <mesalink/openssl/ssl.h>
#include <mesalink/openssl/err.h>
#include <mesalink/openssl/x509.h>
#endif
#if defined(OS_MACOSX)
#include <sys/event.h>
#endif
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <gflags/gflags.h>
#include "bthread/unstable.h"
#include "brpc/socket.h"
#include "brpc/details/ssl_helper.h"

namespace brpc {

DEFINE_int32(ssl_bio_buffer_size, 16 * 1024, "Set buffer size for SSL read/write");

SocketSSLContext::SocketSSLContext() : raw_ctx(NULL) {}

SocketSSLContext::~SocketSSLContext() {
    if (raw_ctx) {
        SSL_CTX_free(raw_ctx);
    }
}

static SSL* SSLHandshakeImpl(const std::shared_ptr<SocketSSLContext>& ssl_ctx,
                             SocketUniquePtr& sock, int fd, bool server_mode) {
    SSL* ssl_session = CreateSSLSession(ssl_ctx->raw_ctx, sock->id(), fd, server_mode);
    if (NULL == ssl_session) {
        LOG(ERROR) << "Fail to CreateSSLSession";
        return NULL;
    }

#if defined(SSL_CTRL_SET_TLSEXT_HOSTNAME) || defined(USE_MESALINK)
    if (!ssl_ctx->sni_name.empty()) {
        SSL_set_tlsext_host_name(ssl_session, ssl_ctx->sni_name.c_str());
    }
#endif
    // Loop until SSL handshake has completed. For SSL_ERROR_WANT_READ/WRITE,
    // we use bthread_fd_wait as polling mechanism instead of EventDispatcher
    // as it may confuse the origin event processing code.
    while (true) {
        ERR_clear_error();
        int rc = SSL_do_handshake(ssl_session);
        if (rc == 1) {
            // In client, check if server returned ALPN selection is acceptable.
            if (!server_mode && !ssl_ctx->alpn_protocols.empty()) {
                const unsigned char *alpn_proto;
                unsigned int alpn_proto_length;
                SSL_get0_alpn_selected(ssl_session, &alpn_proto, &alpn_proto_length);
                if (!alpn_proto) {
                    LOG(ERROR) << "Server returned no ALPN protocol";
                    return NULL;
                }

                std::string alpn_protocol(
                    reinterpret_cast<char const *>(alpn_proto), alpn_proto_length);
                if (std::find(ssl_ctx->alpn_protocols.begin(),
                    ssl_ctx->alpn_protocols.end(),
                    alpn_protocol) == ssl_ctx->alpn_protocols.end()) {
                    LOG(ERROR) << "Server returned unacceptable ALPN protocol: "
                               << alpn_protocol;
                    return NULL;
                }
            }

            AddBIOBuffer(ssl_session, fd, FLAGS_ssl_bio_buffer_size);
            return ssl_session;
        }

        int ssl_error = SSL_get_error(ssl_session, rc);
        switch (ssl_error) {
        case SSL_ERROR_WANT_READ:
#if defined(OS_LINUX)
            if (bthread_fd_wait(fd, EPOLLIN) != 0) {
#elif defined(OS_MACOSX)
            if (bthread_fd_wait(fd, EVFILT_READ) != 0) {
#endif
                return NULL;
            }
            break;

        case SSL_ERROR_WANT_WRITE:
#if defined(OS_LINUX)
            if (bthread_fd_wait(fd, EPOLLOUT) != 0) {
#elif defined(OS_MACOSX)
            if (bthread_fd_wait(fd, EVFILT_WRITE) != 0) {
#endif
                return NULL;
            }
            break;

        default: {
            const unsigned long e = ERR_get_error();
            if (ssl_error == SSL_ERROR_ZERO_RETURN || e == 0) {
                errno = ECONNRESET;
                LOG(ERROR) << "SSL connection was shutdown by peer: " << sock->remote_side();
            } else if (ssl_error == SSL_ERROR_SYSCALL) {
                PLOG(ERROR) << "Fail to SSL_do_handshake";
            } else {
                errno = ESSL;
                LOG(ERROR) << "Fail to SSL_do_handshake: " << SSLError(e);
            }
            return NULL;
        }
        }
    }
}


SSL* DefaultSSLContextFactory::SSLHandshake(SocketUniquePtr& sock, int fd) {
    return SSLHandshakeImpl(_ssl_ctx, sock, fd, _server_mode);
}

void DefaultSSLContextFactory::Shutdown(SSL* ssl_session) {
    if (ssl_session) {
        SSL_free(ssl_session);
    }
}

}
