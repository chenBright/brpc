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


#include <deque>

#include <google/protobuf/descriptor.h>         // MethodDescriptor
#include <google/protobuf/message.h>            // Message
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/text_format.h>

#include "butil/iobuf.h"                         // butil::IOBuf
#include "butil/logging.h"                       // LOG()
#include "butil/memory/scope_guard.h"
#include "butil/raw_pack.h"                      // RawPacker RawUnpacker
#include "butil/strings/string_util.h"

#include "json2pb/json_to_pb.h"
#include "json2pb/pb_to_json.h"
#include "brpc/controller.h"                    // Controller
#include "brpc/destroyable.h"                   // Destroyable
#include "brpc/socket.h"                        // Socket
#include "brpc/server.h"                        // Server
#include "brpc/span.h"
#include "brpc/compress.h"                      // ParseFromCompressedData
#include "brpc/checksum.h"
#include "brpc/stream_impl.h"
#include "brpc/rpc_dump.h"                      // SampledRequest
#include "brpc/rpc_pb_message_factory.h"
#include "brpc/policy/baidu_rpc_meta.pb.h"      // RpcRequestMeta
#include "brpc/policy/baidu_rpc_protocol.h"
#include "brpc/policy/most_common_message.h"
#include "brpc/policy/streaming_rpc_protocol.h"
#include "brpc/details/usercode_backup_pool.h"
#include "brpc/details/controller_private_accessor.h"
#include "brpc/details/server_private_accessor.h"

extern "C" {
void bthread_assign_data(void* data);
}


namespace brpc {
namespace policy {

DEFINE_bool(baidu_protocol_use_fullname, true,
            "If this flag is true, baidu_std puts service.full_name in requests"
            ", otherwise puts service.name (required by jprotobuf).");

DEFINE_bool(baidu_std_protocol_deliver_timeout_ms, false,
            "If this flag is true, baidu_std puts timeout_ms in requests.");

DECLARE_bool(pb_enum_as_number);

// Notes:
// 1. 12-byte header [PRPC][body_size][meta_size]
// 2. body_size and meta_size are in network byte order
// 3. Use service->full_name() + method_name to specify the method to call
// 4. `attachment_size' is set iff request/response has attachment
// 5. Not supported: chunk_info

// A second framing shares this parser on GDR-negotiated connections:
//
//   PRPC  12B  ['P''R''P''C'][body_size:u32][meta_size:u32]
//   GDRB  16B  ['G''D''R''B'][body_size:u32][meta_size:u32][device_size:u32]
//
// GDRB is used only by the messages that actually carry GPU memory; every
// other message on the same connection still uses PRPC, so a small RPC pays
// nothing. Keeping device_size at a fixed byte offset is the whole point:
// the parser can tell whether a message has a device half by arithmetic
// alone. The alternative -- reading it out of the meta -- would put a
// protobuf parse inside the socket's serialized input section, on every
// message of the connection rather than just the ones carrying tensors.
// See docs/cn/gdr_design.md section 7.1.
static const size_t RPC_HEADER_SIZE = 12;
static const size_t GDR_HEADER_SIZE = 16;

// Pack header into `buf'. `device_size' > 0 selects the GDRB framing.
// Returns the number of bytes written.
//
// device_size is deliberately NOT folded into body_size: those bytes travel
// on the connection's second QP and never enter the host stream.
inline size_t PackRpcHeader(char* rpc_header, uint32_t meta_size,
                            int payload_size, uint32_t device_size) {
    uint32_t* dummy = (uint32_t*)rpc_header;  // suppress strict-alias warning
    if (BAIDU_LIKELY(device_size == 0)) {
        *dummy = *(uint32_t*)"PRPC";
        butil::RawPacker(rpc_header + 4)
            .pack32(meta_size + payload_size)
            .pack32(meta_size);
        return RPC_HEADER_SIZE;
    }
    *dummy = *(uint32_t*)"GDRB";
    butil::RawPacker(rpc_header + 4)
        .pack32(meta_size + payload_size)
        .pack32(meta_size)
        .pack32(device_size);
    return GDR_HEADER_SIZE;
}

static void SerializeRpcHeaderAndMeta(
    butil::IOBuf* out, const RpcMeta& meta, int payload_size,
    uint32_t device_size = 0) {
    const uint32_t meta_size = GetProtobufByteSize(meta);
    const size_t header_size =
        (device_size == 0 ? RPC_HEADER_SIZE : GDR_HEADER_SIZE);
    if (meta_size <= 244) { // most common cases
        char header_and_meta[header_size + meta_size];
        PackRpcHeader(header_and_meta, meta_size, payload_size, device_size);
        ::google::protobuf::io::ArrayOutputStream arr_out(
            header_and_meta + header_size, meta_size);
        ::google::protobuf::io::CodedOutputStream coded_out(&arr_out);
        meta.SerializeWithCachedSizes(&coded_out); // not calling ByteSize again
        CHECK(!coded_out.HadError());
        CHECK_EQ(0, out->append(header_and_meta, sizeof(header_and_meta)));
    } else {
        char header[GDR_HEADER_SIZE];
        PackRpcHeader(header, meta_size, payload_size, device_size);
        CHECK_EQ(0, out->append(header, header_size));
        butil::IOBufAsZeroCopyOutputStream buf_stream(out);
        ::google::protobuf::io::CodedOutputStream coded_out(&buf_stream);
        meta.SerializeWithCachedSizes(&coded_out);
        CHECK(!coded_out.HadError());
    }
}

// A baidu_std message that carries a DeviceAttachment.
//
// Why this cannot be a plain IOBuf: on a connection with a second QP the
// device stream has no framing, so the k-th device segment on the wire
// belongs to the k-th device-carrying host message. Socket::Write() is a
// lock-free MPSC enqueue -- the order concurrent writers call it in is NOT
// the order they reach the wire -- so the device append has to happen where
// the order is already fixed. AppendAndDestroySelf() is that place: Socket
// runs it over the already-linked list from oldest to newest, i.e. in exact
// write order. See docs/cn/gdr_design.md section 8.1.
//
// It is also the first and only point on the send path that can see the
// socket, which makes it the only place that can choose between the two
// PLACEMENTS of the device half: the second QP, or -- on a connection with
// no second channel -- inline behind the body (docs/cn/gdr_design.md
// section 7.3), two copies more expensive, and the difference between "this
// RPC costs more here" and "this RPC does not run here".
//
// The BYTES, however, are the same either way, so none of them are built
// there. GDRB says how long the device half is, not which line it came down:
// body_size counts host bytes only in both placements, and device_size sits
// at a fixed offset. So the header and the meta are serialized in the
// constructor, on the calling thread, and AppendAndDestroySelf() is left
// with nothing to do but pick a placement -- which is what it is for.
// Socket's single writer runs it for every queued message in turn, and
// protobuf serialization does not belong in that loop.
class DeviceMessage : public SocketMessage {
public:
    // Takes over both `body' (the serialized response/request plus any host
    // attachment) and `device_data'. Only ever constructed with a non-empty
    // `device_data', hence always the GDRB framing.
    DeviceMessage(const RpcMeta& meta, butil::IOBuf* body,
                  DeviceAttachment* device_data) {
        _device_data.swap(*device_data);
        DCHECK(!_device_data.empty());
        SerializeRpcHeaderAndMeta(&_host, meta, body->size(),
                                  _device_data.size());
        _host.append(body->movable());
    }

    butil::Status AppendAndDestroySelf(butil::IOBuf* out, Socket* sock) override {
        std::unique_ptr<DeviceMessage> destroy_self(this);
        if (sock == nullptr) {
            // Abandoned before reaching the wire. ~DeviceAttachment frees the
            // GPU memory, which is the whole point of routing it through here.
            return butil::Status::OK();
        }
        DeviceStream* device_stream = sock->device_stream();
        if (device_stream != nullptr) {
            device_stream->AppendForSend(std::move(_device_data));
            out->append(_host.movable());
            return butil::Status::OK();
        }
        // Fallback: no second channel, so the device half travels inline on
        // this connection, behind the body. Staged into its own IOBuf first
        // because the D2H copy can fail, and the host half must not already
        // be in `out` when it does -- Socket has no way to take those bytes
        // back off the write queue.
        butil::IOBuf device_buf;
        if (_device_data.copy_to(&device_buf) != 0) {
            const int saved_errno = errno;
            return butil::Status(saved_errno,
                                 "Fail to copy device data for %s: %s",
                                 sock->description().c_str(),
                                 berror(saved_errno));
        }
        _device_data.clear();
        out->append(_host.movable());
        out->append(device_buf.movable());
        return butil::Status::OK();
    }

    size_t EstimatedByteSize() override {
        // Exact for the second-QP placement, which is the one this class
        // exists for. The fallback under-reports by device_size, and
        // deliberately: counting a tensor as host bytes would distort the
        // write-queue accounting of the fast path to protect the slow one.
        return _host.size();
    }

private:
    // Header + meta + body, already serialized. Host bytes only.
    butil::IOBuf _host;
    DeviceAttachment _device_data;
};

// Messages whose host half has been parsed but whose device half has not
// arrived yet. Hangs on Socket::parsing_context() so that a dying connection
// frees the parked GPU memory for free.
//
// Strict FIFO on purpose: the device stream carries no framing, so the only
// thing that says which bytes belong to which message is the order they were
// cut in. Messages WITHOUT a device attachment never enter this list and are
// dispatched immediately -- they consume zero device bytes, so letting them
// jump the queue cannot disturb the alignment. That is what keeps one large
// tensor transfer from adding latency to every small RPC sharing the
// connection. See docs/cn/gdr_design.md section 8.2.
//
// Not locked: a Socket's input is serialized by Socket::_nevent, so only one
// thread is ever inside the parser for a given connection.
class GdrPendingList : public Destroyable {
public:
    struct Entry {
        MostCommonMessage* msg;
        uint64_t device_size;
    };

    GdrPendingList() : _pending_bytes(0) {}

    // @Destroyable
    void Destroy() override { delete this; }

    ~GdrPendingList() override {
        // Deliberately does not report the drained pending count to the
        // DeviceStream: Socket::ResetFileDescriptor() releases the transport
        // (and with it the RdmaEndpoint) before it resets the parsing
        // context, so the stream may already be gone. The endpoint zeroes its
        // own counters on Reset(), and SetPending() is absolute rather than
        // incremental, so nothing can drift.
        for (size_t i = 0; i < _list.size(); ++i) {
            _list[i].msg->Destroy();
        }
    }

    bool empty() const { return _list.empty(); }
    size_t size() const { return _list.size(); }
    const Entry& front() const { return _list.front(); }
    uint64_t pending_bytes() const { return _pending_bytes; }

    void push_back(const Entry& e) {
        _list.push_back(e);
        _pending_bytes += e.device_size;
    }

    void pop_front() {
        _pending_bytes -= _list.front().device_size;
        _list.pop_front();
    }

private:
    std::deque<Entry> _list;
    uint64_t _pending_bytes;
};

// Cut `entry`'s device bytes out of `stream` into its message.
static void CutDeviceAttachment(const GdrPendingList::Entry& entry,
                                DeviceStream* stream) {
    const size_t moved =
        stream->cutn(&entry.msg->device_payload, entry.device_size);
    CHECK_EQ(moved, entry.device_size);
}

ParseResult ParseRpcMessage(butil::IOBuf* source, Socket* socket,
                            bool /*read_eof*/, const void*) {
    // nullptr for every connection but a GDR-negotiated RDMA one, and the single
    // thing that decides where a GDRB message's device half is read from:
    // non-nullptr means the second channel (with the pending list below), nullptr
    // means inline behind the body. Everything specific to the second channel
    // hangs off this test, so the common path is unchanged.
    DeviceStream* const device_stream = socket->device_stream();
    GdrPendingList* pending = nullptr;
    if (BAIDU_UNLIKELY(device_stream != nullptr)) {
        pending = static_cast<GdrPendingList*>(socket->parsing_context());
        if (pending == nullptr) {
            // Also covers the case where InputMessenger dropped the context
            // while messages were still parked (it does that when it
            // re-detects the protocol). Reporting zero here is what makes
            // that survivable: otherwise the endpoint would go on withholding
            // host credits for a pending list that no longer exists.
            device_stream->SetPending(0, 0);
        }
        // Phase 1: drain whatever became complete since the last call. One
        // per call is enough -- InputMessenger keeps calling us until we
        // report NOT_ENOUGH_DATA.
        if (pending != nullptr && !pending->empty()) {
            const GdrPendingList::Entry& head = pending->front();
            if (device_stream->size() >= head.device_size) {
                CutDeviceAttachment(head, device_stream);
                MostCommonMessage* msg = head.msg;
                pending->pop_front();
                device_stream->SetPending(pending->size(),
                                          pending->pending_bytes());
                return MakeMessage(msg);
            }
        }
    }

    char header_buf[GDR_HEADER_SIZE];
    const size_t n = source->copy_to(header_buf, sizeof(header_buf));
    bool is_gdr = false;
    if (n >= 4) {
        void* dummy = header_buf;
        if (*(const uint32_t*)dummy == *(const uint32_t*)"PRPC") {
            // Nothing to do.
        } else if (*(const uint32_t*)dummy == *(const uint32_t*)"GDRB") {
            is_gdr = true;
        } else {
            return MakeParseError(PARSE_ERROR_TRY_OTHERS);
        }
    } else {
        // A prefix of either magic is still ours; only give up once it can be
        // neither.
        if (memcmp(header_buf, "PRPC", n) != 0 &&
            memcmp(header_buf, "GDRB", n) != 0) {
            return MakeParseError(PARSE_ERROR_TRY_OTHERS);
        }
        return MakeParseError(PARSE_ERROR_NOT_ENOUGH_DATA);
    }
    const size_t header_size = (is_gdr ? GDR_HEADER_SIZE : RPC_HEADER_SIZE);
    if (n < header_size) {
        return MakeParseError(PARSE_ERROR_NOT_ENOUGH_DATA);
    }
    uint32_t body_size;
    uint32_t meta_size;
    uint32_t device_size = 0;
    butil::RawUnpacker(header_buf + 4).unpack32(body_size).unpack32(meta_size);
    if (is_gdr) {
        butil::RawUnpacker(header_buf + 12).unpack32(device_size);
        if (device_size == 0) {
            // The sender only picks GDRB when it has device bytes to send,
            // so this is a malformed peer. Rejecting it keeps "GDRB implies a
            // device half" an invariant the code below can rely on.
            LOG(ERROR) << "GDRB message from " << socket->remote_side()
                       << " declares no device bytes";
            return MakeParseError(PARSE_ERROR_ABSOLUTELY_WRONG,
                                  "GDRB message with device_size=0");
        }
    }
    // GDRB says how long the device half is, not which line it came down.
    // That is decided by whether this connection has a second channel, and
    // both ends read the same answer off the same handshake, so no bit on the
    // wire has to distinguish the two placements. With no second channel the
    // device bytes follow the body right here (docs/cn/gdr_design.md 7.3).
    const size_t inline_device_size =
        (device_stream == nullptr) ? device_size : 0;
    if ((uint64_t)body_size + inline_device_size > (uint64_t)FLAGS_max_body_size) {
        // We need this log to report the body_size to give users some clues
        // which is not printed in InputMessenger.
        //
        // Inline device bytes count towards the limit because they really are
        // host bytes piling up in the read buffer; the ones on a device
        // channel do not, their volume is bounded by that channel's own flow
        // control.
        LOG(ERROR) << "body_size=" << body_size << " device_size="
                   << inline_device_size << " from "
                   << socket->remote_side() << " is too large";
        return MakeParseError(PARSE_ERROR_TOO_BIG_DATA);
    } else if (source->length() < header_size + body_size + inline_device_size) {
        // On a device channel the device bytes are NOT part of this test: the
        // host half being complete is enough to build the message, and a
        // missing device half parks it below instead of rewinding, which we
        // could not do anyway once the host bytes are consumed. Inline, there
        // is nothing to park -- the bytes are on their way down this very
        // socket, so waiting for them is both possible and simpler.
        return MakeParseError(PARSE_ERROR_NOT_ENOUGH_DATA);
    }
    if (meta_size > body_size) {
        LOG(ERROR) << "meta_size=" << meta_size << " is bigger than body_size="
                   << body_size;
        // Pop the message
        source->pop_front(header_size + body_size + inline_device_size);
        return MakeParseError(PARSE_ERROR_TRY_OTHERS);
    }
    source->pop_front(header_size);
    MostCommonMessage* msg = MostCommonMessage::Get();
    source->cutn(&msg->meta, meta_size);
    source->cutn(&msg->payload, body_size - meta_size);
    if (BAIDU_LIKELY(!is_gdr)) {
        // Zero device bytes consumed, so dispatching ahead of the pending
        // list cannot disturb anyone's alignment. This is also every message
        // on a non-GDR connection, which is why the framing rather than the
        // meta has to say whether there is a device half: reaching this point
        // must not cost a protobuf parse.
        return MakeMessage(msg);
    }
    if (device_stream == nullptr) {
        // Fallback: the device half is sitting right behind the body. H2D
        // here, so that the application sees the same DeviceAttachment it
        // would have got over a device channel.
        if (msg->device_payload.append_from_iobuf(source, device_size) != 0) {
            const int saved_errno = errno;
            source->pop_front(device_size);
            msg->Destroy();
            LOG(ERROR) << "Fail to receive " << device_size
                       << " inline device bytes from " << socket->remote_side()
                       << ": " << berror(saved_errno);
            // The framing is intact, so this could be turned into a dropped
            // message instead. It is not: the caller would see a timeout with
            // nothing in its own logs, and running out of attachment memory is
            // worth shedding the connection over.
            return MakeParseError(PARSE_ERROR_ABSOLUTELY_WRONG,
                                  "Fail to receive inline device bytes");
        }
        return MakeMessage(msg);
    }

    GdrPendingList::Entry entry = { msg, device_size };
    if ((pending == nullptr || pending->empty()) &&
        device_stream->size() >= entry.device_size) {
        // Fast path: the device half is already here.
        CutDeviceAttachment(entry, device_stream);
        return MakeMessage(msg);
    }
    if (pending == nullptr) {
        pending = new GdrPendingList;
        socket->reset_parsing_context(pending);
    }
    pending->push_back(entry);
    device_stream->SetPending(pending->size(), pending->pending_bytes());
    // No message this round, but the parse itself succeeded: returning an
    // error here would make InputMessenger try another protocol and throw
    // away the context we just installed.
    return MakeMessage(nullptr);
}

bool SerializeRpcMessage(const google::protobuf::Message& message,
                         Controller& cntl, ContentType content_type,
                         CompressType compress_type, ChecksumType checksum_type,
                         butil::IOBuf* buf,
                         const butil::IOBuf* checksum_attachment) {
    auto serialize = [&](Serializer& serializer) -> bool {
        bool ok;
        if (COMPRESS_TYPE_NONE == compress_type) {
            butil::IOBufAsZeroCopyOutputStream stream(buf);
            ok = serializer.SerializeTo(&stream);
        } else {
            const CompressHandler* handler = FindCompressHandler(compress_type);
            if (nullptr == handler) {
                return false;
            }
            ok = handler->Compress(serializer, buf);
        }
        ChecksumIn checksum_in{buf, &cntl, checksum_attachment};
        ComputeDataChecksum(checksum_in, checksum_type);
        return ok;
    };

    if (CONTENT_TYPE_PB == content_type) {
        Serializer serializer([&message](google::protobuf::io::ZeroCopyOutputStream* output) -> bool {
            return message.SerializeToZeroCopyStream(output);
        });
        return serialize(serializer);
    } else if (CONTENT_TYPE_JSON == content_type) {
        Serializer serializer([&message, &cntl](google::protobuf::io::ZeroCopyOutputStream* output) -> bool {
            json2pb::Pb2JsonOptions options;
            options.bytes_to_base64 = cntl.has_pb_bytes_to_base64();
            options.jsonify_empty_array = cntl.has_pb_jsonify_empty_array();
            options.always_print_primitive_fields = cntl.has_always_print_primitive_fields();
            options.single_repeated_to_array = cntl.has_pb_single_repeated_to_array();
            options.enum_option = FLAGS_pb_enum_as_number
                                  ? json2pb::OUTPUT_ENUM_BY_NUMBER
                                  : json2pb::OUTPUT_ENUM_BY_NAME;
            std::string error;
            bool ok = json2pb::ProtoMessageToJson(message, output, options, &error);
            if (!ok) {
                LOG(INFO) << "Fail to serialize message="
                          << message.GetDescriptor()->full_name()
                          << " to json :" << error;
            }
            return ok;
        });
        return serialize(serializer);
    } else if (CONTENT_TYPE_PROTO_JSON == content_type) {
        Serializer serializer([&message, &cntl](google::protobuf::io::ZeroCopyOutputStream* output) -> bool {
            json2pb::Pb2ProtoJsonOptions options;
            options.always_print_enums_as_ints = FLAGS_pb_enum_as_number;
            AlwaysPrintPrimitiveFields(options) = cntl.has_always_print_primitive_fields();
            std::string error;
            bool ok = json2pb::ProtoMessageToProtoJson(message, output, options, &error);
            if (!ok) {
                LOG(INFO) << "Fail to serialize message="
                          << message.GetDescriptor()->full_name()
                          << " to proto-json :" << error;
            }
            return ok;
        });
        return serialize(serializer);
    } else if (CONTENT_TYPE_PROTO_TEXT == content_type) {
        Serializer serializer([&message](google::protobuf::io::ZeroCopyOutputStream* output) -> bool {
            return google::protobuf::TextFormat::Print(message, output);
        });
        return serialize(serializer);
    }
    return false;
}

static bool SerializeResponse(const google::protobuf::Message& res,
                              Controller& cntl, butil::IOBuf& buf) {
    if (res.GetDescriptor() == SerializedResponse::descriptor()) {
        buf.swap(((SerializedResponse&)res).serialized_data());
        return true;
    }

    if (!res.IsInitialized()) {
        cntl.SetFailed(ERESPONSE, "Missing required fields in response: %s",
                       res.InitializationErrorString().c_str());
        return false;
    }

    ContentType content_type = cntl.response_content_type();
    CompressType compress_type = cntl.response_compress_type();
    ChecksumType checksum_type = cntl.response_checksum_type();
    const butil::IOBuf* checksum_attachment = nullptr;
    if (cntl.response_checksum_attachment()) {
        // See the same check in SerializeRpcRequest() for the rationale;
        // baidu_std never sets this flag itself but we defend anyway.
        if (!cntl.is_response_read_progressively()) {
            checksum_attachment = &cntl.response_attachment();
        }
    }
    if (!SerializeRpcMessage(res, cntl, content_type, compress_type,
                             checksum_type, &buf, checksum_attachment)) {
        cntl.SetFailed(ERESPONSE,
                       "Fail to serialize response=%s, "
                       "ContentType=%s, CompressType=%s, ChecksumType=%s",
                       butil::EnsureString(res.GetDescriptor()->full_name()).c_str(),
                       ContentTypeToCStr(content_type),
                       CompressTypeToCStr(compress_type),
                       ChecksumTypeToCStr(checksum_type));
        return false;
    }
    return true;
}

namespace {
struct BaiduProxyPBMessages : public RpcPBMessages {
    static BaiduProxyPBMessages* Get() {
        return butil::get_object<BaiduProxyPBMessages>();
    }

    static void Return(BaiduProxyPBMessages* messages) {
        messages->Clear();
        butil::return_object(messages);
    }

    void Clear() {
        request.Clear();
        response.Clear();
    }

    ::google::protobuf::Message* Request() override { return &request; }
    ::google::protobuf::Message* Response() override { return &response; }

    SerializedRequest request;
    SerializedResponse response;
};
}

static bool IsBaiduMasterService(const Server* server,
                                 const butil::EndPoint& local_side) {
    return nullptr != server->options().baidu_master_service &&
           !IsInternalPort(*server, local_side);
}

// Used by UT, can't be static.
void SendRpcResponse(int64_t correlation_id, Controller* cntl,
                     RpcPBMessages* messages, const Server* server,
                     MethodStatus* method_status, int64_t received_us,
                     std::shared_ptr<Span> span) {
    ControllerPrivateAccessor accessor(cntl);
    if (span) {
        span->set_start_send_us(butil::cpuwide_time_us());
    }
    Socket* sock = accessor.get_sending_socket();

    const google::protobuf::Message* req = nullptr == messages ? nullptr : messages->Request();
    const google::protobuf::Message* res = nullptr == messages ? nullptr : messages->Response();

    // Recycle resources at the end of this function.
    BRPC_SCOPE_EXIT {
        {
            // Remove concurrency and record latency at first.
            ConcurrencyRemover concurrency_remover(method_status, cntl, received_us);
        }

        std::unique_ptr<Controller, LogErrorTextAndDelete> recycle_cntl(cntl);

        if (nullptr == messages) {
            return;
        }

        cntl->CallAfterRpcResp(req, res);
        if (IsBaiduMasterService(server, cntl->local_side())) {
            BaiduProxyPBMessages::Return(static_cast<BaiduProxyPBMessages*>(messages));
        } else {
            server->options().rpc_pb_message_factory->Return(messages);
        }
    };
    
    StreamIds response_stream_ids = accessor.response_streams();

    if (cntl->IsCloseConnection()) {
        for(size_t i = 0; i < response_stream_ids.size(); ++i) {
            StreamClose(response_stream_ids[i]);
        }
        sock->SetFailed();
        return;
    }
    if (BAIDU_UNLIKELY(!cntl->response_device_attachment().empty() &&
                       cntl->has_remote_stream())) {
        // The one case the TCP fallback cannot rescue: SendStreamData()
        // re-frames the buffer, and both placements of the device half are
        // accounted per message. Report it as an ordinary RPC error instead
        // of letting the write fail later -- a failed write only reaches the
        // client as a timeout, and the service author needs to see which
        // mistake this is. A connection with no device channel is no longer
        // one of them: it sends the bytes inline (gdr_design.md 7.3).
        cntl->response_device_attachment().clear();
        cntl->SetFailed(EDEVICECHANNEL,
                        "A stream cannot carry response_device_attachment");
    }

    bool append_body = false;
    butil::IOBuf res_body;
    // `res' can be nullptr here, in which case we don't serialize it
    // If user calls `SetFailed' on Controller, we don't serialize
    // response either
    if (res != nullptr && !cntl->Failed()) {
        append_body = SerializeResponse(*res, *cntl, res_body);
    }

    // Don't use res->ByteSize() since it may be compressed
    size_t res_size = 0;
    size_t attached_size = 0;
    if (append_body) {
        res_size = res_body.length();
        attached_size = cntl->response_attachment().length();
    }

    int error_code = cntl->ErrorCode();
    if (error_code == -1) {
        // replace general error (-1) with INTERNAL_SERVER_ERROR to make a
        // distinction between server error and client error
        error_code = EINTERNAL;
    }
    RpcMeta meta;
    RpcResponseMeta* response_meta = meta.mutable_response();
    response_meta->set_error_code(error_code);
    if (!cntl->ErrorText().empty()) {
        // Only set error_text when it's not empty since protobuf Message
        // always new the string no matter if it's empty or not.
        response_meta->set_error_text(cntl->ErrorText());
    }
    meta.set_correlation_id(correlation_id);
    meta.set_compress_type(cntl->response_compress_type());
    meta.set_content_type(cntl->response_content_type());
    meta.set_checksum_type(cntl->response_checksum_type());
    meta.set_checksum_value(accessor.checksum_value());
    if (cntl->response_checksum_attachment()) {
        meta.set_checksum_with_attachment(true);
    }
    if (attached_size > 0) {
        meta.set_attachment_size(attached_size);
    }
    StreamId response_stream_id = INVALID_STREAM_ID;
    StreamUniquePtr stream_ptr;
    if (!response_stream_ids.empty()) {
        response_stream_id = response_stream_ids[0];
        if (Stream::Address(response_stream_id, &stream_ptr) == 0) {
            Stream* s = stream_ptr.get();
            StreamSettings *stream_settings = meta.mutable_stream_settings();
            s->FillSettings(stream_settings);
            if (s->SetHostSocket(sock) != 0) {
                cntl->SetFailed(EINVAL, "Fail to bind stream=%" PRIu64
                                " to %s", response_stream_id,
                                sock->description().c_str());
                Stream::SetFailed(response_stream_ids, EINVAL,
                                  "%s", cntl->ErrorText().c_str());
                return;
            }
            for (size_t i = 1; i < response_stream_ids.size(); ++i) {
                stream_settings->mutable_extra_stream_ids()->Add(response_stream_ids[i]);
            }
        } else {
            LOG(WARNING) << "Stream=" << response_stream_id 
                         << " was closed before sending response";
        }
    }

    if (cntl->has_response_user_fields() &&
        !cntl->response_user_fields()->empty()) {
        ::google::protobuf::Map<std::string, std::string>& user_fields
            = *meta.mutable_user_fields();
        user_fields.insert(cntl->response_user_fields()->begin(),
                           cntl->response_user_fields()->end());

    }

    butil::IOBuf res_buf;
    // Non-empty only for a response with a device attachment, which cannot be
    // serialized here: where its device half goes depends on the socket, and
    // on a device channel it has to be queued from the socket's single
    // writer, in the outgoing order that pairs the two streams.
    SocketMessagePtr<DeviceMessage> device_msg;
    if (BAIDU_UNLIKELY(!cntl->response_device_attachment().empty())) {
        butil::IOBuf body;
        if (append_body) {
            body.append(res_body.movable());
            if (attached_size > 0) {
                body.append(cntl->response_attachment().movable());
            }
        }
        device_msg.reset(new DeviceMessage(
            meta, &body, &cntl->response_device_attachment()));
    } else {
        SerializeRpcHeaderAndMeta(&res_buf, meta, res_size + attached_size);
        if (append_body) {
            res_buf.append(res_body.movable());
            if (attached_size > 0) {
                res_buf.append(cntl->response_attachment().movable());
            }
        }
    }

    ResponseWriteInfo args;
    bthread_id_t response_id = INVALID_BTHREAD_ID;
    if (span) {
        span->set_response_size(device_msg ? device_msg->EstimatedByteSize()
                                           : res_buf.size());
        CHECK_EQ(0, bthread_id_create(&response_id, &args, HandleResponseWritten));
    }

    // Send rpc response over stream even if server side failed to create
    // stream for some reason.
    if (cntl->has_remote_stream()) {
        // Send the response over stream to notify that this stream connection
        // is successfully built.
        // Response_stream can be INVALID_STREAM_ID when error occurs.
        if (SendStreamData(sock, &res_buf,
                           accessor.remote_stream_settings()->stream_id(),
                           response_stream_id, response_id) != 0) {
            error_code = errno;
            PLOG_IF(WARNING, error_code != EPIPE)
                << "Fail to write into " << sock->description();
            cntl->SetFailed(error_code,  "Fail to write into %s",
                            sock->description().c_str());
            Stream::SetFailed(response_stream_ids, error_code,
                              "Fail to write into %s",
                              sock->description().c_str());
            return;
        }

        // Now it's ok the mark these server-side streams as connected as all the
        // written user data would follower the RPC response.
        // Reuse stream_ptr to avoid address first stream id again
        if (stream_ptr) {
            stream_ptr->SetConnected();
        }
        for (size_t i = 1; i < response_stream_ids.size(); ++i) {
            StreamId extra_stream_id = response_stream_ids[i];
            StreamUniquePtr extra_stream_ptr;
            if (Stream::Address(extra_stream_id, &extra_stream_ptr) == 0) {
                Stream* extra_stream = extra_stream_ptr.get();
                if (extra_stream->SetHostSocket(sock) == 0) {
                    extra_stream->SetConnected();
                } else {
                    Stream::SetFailed(extra_stream_id, EINVAL,
                                      "Fail to bind stream to %s",
                                      sock->description().c_str());
                }
            } else {
                LOG(WARNING) << "Stream=" << extra_stream_id
                             << " was closed before sending response";
            }
        }
    } else{
        // Have the risk of unlimited pending responses, in which case, tell
        // users to set max_concurrency.
        Socket::WriteOptions wopt;
        wopt.ignore_eovercrowded = true;
        if (INVALID_BTHREAD_ID != response_id) {
            wopt.id_wait = response_id;
            wopt.notify_on_success = true;
        }
        const int rc = device_msg ? sock->Write(device_msg, &wopt)
                                  : sock->Write(&res_buf, &wopt);
        if (rc != 0) {
            const int errcode = errno;
            PLOG_IF(WARNING, errcode != EPIPE) << "Fail to write into " << *sock;
            cntl->SetFailed(errcode, "Fail to write into %s",
                            sock->description().c_str());
            return;
        }
    }

    if (span) {
        bthread_id_join(response_id);
        // Do not care about the result of background writing.
        // TODO: this is not sent
        span->set_sent_us(args.sent_us);
    }
}

namespace {
struct CallMethodInBackupThreadArgs {
    ::google::protobuf::Service* service;
    const ::google::protobuf::MethodDescriptor* method;
    ::google::protobuf::RpcController* controller;
    const ::google::protobuf::Message* request;
    ::google::protobuf::Message* response;
    ::google::protobuf::Closure* done;
};
}

static void CallMethodInBackupThread(void* void_args) {
    CallMethodInBackupThreadArgs* args = (CallMethodInBackupThreadArgs*)void_args;
    args->service->CallMethod(args->method, args->controller, args->request,
                              args->response, args->done);
    delete args;
}

// Used by other protocols as well.
void EndRunningCallMethodInPool(
    ::google::protobuf::Service* service,
    const ::google::protobuf::MethodDescriptor* method,
    ::google::protobuf::RpcController* controller,
    const ::google::protobuf::Message* request,
    ::google::protobuf::Message* response,
    ::google::protobuf::Closure* done) {
    CallMethodInBackupThreadArgs* args = new CallMethodInBackupThreadArgs;
    args->service = service;
    args->method = method;
    args->controller = controller;
    args->request = request;
    args->response = response;
    args->done = done;
    return EndRunningUserCodeInPool(CallMethodInBackupThread, args);
};

bool DeserializeRpcMessage(const butil::IOBuf& data, Controller& cntl,
                           ContentType content_type, CompressType compress_type,
                           ChecksumType checksum_type,
                           google::protobuf::Message* message,
                           const butil::IOBuf* checksum_attachment) {
    auto deserialize = [&](Deserializer& deserializer) -> bool {
        ChecksumIn checksum_in{&data, &cntl, checksum_attachment};
        bool ok = VerifyDataChecksum(checksum_in, checksum_type);
        if (!ok) {
            return ok;
        }
        if (COMPRESS_TYPE_NONE == compress_type) {
            butil::IOBufAsZeroCopyInputStream stream(data);
            ok = deserializer.DeserializeFrom(&stream);
        } else {
            const CompressHandler* handler = FindCompressHandler(compress_type);
            if (nullptr == handler) {
                return false;
            }
            ok = handler->Decompress(data, &deserializer);
        }
        return ok;
    };

    if (CONTENT_TYPE_PB == content_type) {
        Deserializer deserializer([message](
            google::protobuf::io::ZeroCopyInputStream* input) -> bool {
            return message->ParseFromZeroCopyStream(input);
        });
        return deserialize(deserializer);
    } else if (CONTENT_TYPE_JSON == content_type) {
        Deserializer deserializer([message, &cntl](
            google::protobuf::io::ZeroCopyInputStream* input) -> bool {
            json2pb::Json2PbOptions options;
            options.base64_to_bytes = cntl.has_pb_bytes_to_base64();
            options.array_to_single_repeated = cntl.has_pb_single_repeated_to_array();
            std::string error;
            bool ok = json2pb::JsonToProtoMessage(input, message, options, &error);
            if (!ok) {
                LOG(INFO) << "Fail to parse json to "
                          << message->GetDescriptor()->full_name()
                          << ": "<< error;
            }
            return ok;
        });
        return deserialize(deserializer);
    } else if (CONTENT_TYPE_PROTO_JSON == content_type) {
        Deserializer deserializer([message](
            google::protobuf::io::ZeroCopyInputStream* input) -> bool {
            json2pb::ProtoJson2PbOptions options;
            options.ignore_unknown_fields = true;
            std::string error;
            bool ok = json2pb::ProtoJsonToProtoMessage(input, message, options, &error);
            if (!ok) {
                LOG(INFO) << "Fail to parse proto-json to "
                          << message->GetDescriptor()->full_name()
                          << ": "<< error;
            }
            return ok;
        });
        return deserialize(deserializer);
    } else if (CONTENT_TYPE_PROTO_TEXT == content_type) {
        Deserializer deserializer([message](
            google::protobuf::io::ZeroCopyInputStream* input) -> bool {
            return google::protobuf::TextFormat::Parse(input, message);
        });
        return deserialize(deserializer);
    }
    return false;
}

void ProcessRpcRequest(InputMessageBase* msg_base) {
    const int64_t start_parse_us = butil::cpuwide_time_us();
    DestroyingPtr<MostCommonMessage> msg(static_cast<MostCommonMessage*>(msg_base));
    SocketUniquePtr socket_guard(msg->ReleaseSocket());
    Socket* socket = socket_guard.get();
    const Server* server = static_cast<const Server*>(msg_base->arg());
    ScopedNonServiceError non_service_error(server);

    RpcMeta meta;
    if (!ParsePbFromIOBuf(&meta, msg->meta)) {
        LOG(WARNING) << "Fail to parse RpcMeta from " << *socket;
        socket->SetFailed(EREQUEST, "Fail to parse RpcMeta from %s",
                          socket->description().c_str());
        return;
    }
    const RpcRequestMeta &request_meta = meta.request();

    SampledRequest* sample = AskToBeSampled();
    if (sample) {
        sample->meta.set_service_name(request_meta.service_name());
        sample->meta.set_method_name(request_meta.method_name());
        sample->meta.set_compress_type((CompressType)meta.compress_type());
        sample->meta.set_protocol_type(PROTOCOL_BAIDU_STD);
        sample->meta.set_attachment_size(meta.attachment_size());
        sample->meta.set_authentication_data(meta.authentication_data());
        sample->request = msg->payload;
        sample->submit(start_parse_us);
    }

    std::unique_ptr<Controller> cntl(new Controller);

    RpcPBMessages* messages = nullptr;

    ServerPrivateAccessor server_accessor(server);
    ControllerPrivateAccessor accessor(cntl.get());
    const bool security_mode = server->options().security_mode() &&
                               socket->user() == server_accessor.acceptor();
    if (request_meta.has_log_id()) {
        cntl->set_log_id(request_meta.log_id());
    }
    if (request_meta.has_request_id()) {
        cntl->set_request_id(request_meta.request_id());
    }
    if (request_meta.has_timeout_ms()) {
        cntl->set_timeout_ms(request_meta.timeout_ms());
    }
    cntl->set_request_content_type(meta.content_type());
    cntl->set_request_compress_type((CompressType)meta.compress_type());
    cntl->set_request_checksum_type((ChecksumType)meta.checksum_type());
    cntl->set_request_checksum_attachment(meta.checksum_with_attachment());
    cntl->set_rpc_received_us(msg->received_us());
    if (BAIDU_UNLIKELY(!msg->device_payload.empty())) {
        // Already cut out of the device stream by the parser, so nothing here
        // can block or fail. Moved rather than referenced so that the GPU
        // memory is released when the Controller dies, even if the service
        // never looks at it.
        cntl->request_device_attachment().append(
            std::move(msg->device_payload));
    }
    accessor.set_checksum_value(meta.checksum_value());
    accessor.set_server(server)
        .set_security_mode(security_mode)
        .set_peer_id(socket->id())
        .set_remote_side(socket->remote_side())
        .set_local_side(socket->local_side())
        .set_auth_context(socket->auth_context())
        .set_request_protocol(PROTOCOL_BAIDU_STD)
        .set_begin_time_us(msg->received_us())
        .move_in_server_receiving_sock(socket_guard);

    if (meta.has_stream_settings()) {
        accessor.set_remote_stream_settings(meta.release_stream_settings());
    }

    if (!meta.user_fields().empty()) {
        for (const auto& it : meta.user_fields()) {
            (*cntl->request_user_fields())[it.first] = it.second;
        }
    }

    // Tag the bthread with this server's key for thread_local_data().
    if (server->thread_local_options().thread_local_data_factory) {
        bthread_assign_data((void*)&server->thread_local_options());
    }

    std::shared_ptr<Span> span;
    if (IsTraceable(request_meta.has_trace_id())) {
        span = Span::CreateServerSpan(
            request_meta.trace_id(), request_meta.span_id(),
            request_meta.parent_span_id(), msg->base_real_us());
        accessor.set_span(span);
        span->set_log_id(request_meta.log_id());
        span->set_remote_side(cntl->remote_side());
        span->set_protocol(PROTOCOL_BAIDU_STD);
        span->set_received_us(msg->received_us());
        span->set_start_parse_us(start_parse_us);
        // A GDR request was framed with the longer header. The device bytes
        // themselves are still not counted: they never entered the host
        // stream, so this number understates a tensor RPC by design.
        span->set_request_size(msg->payload.size() + msg->meta.size() +
                               (cntl->request_device_attachment().empty()
                                ? RPC_HEADER_SIZE : GDR_HEADER_SIZE));
    }

    MethodStatus* method_status = nullptr;
    do {
        if (!server->IsRunning()) {
            cntl->SetFailed(ELOGOFF, "Server is stopping");
            break;
        }

        if (!server_accessor.AddConcurrency(cntl.get())) {
            cntl->SetFailed(
                ELIMIT, "Reached server's max_concurrency=%d",
                server->options().max_concurrency);
            break;
        }

        if (FLAGS_usercode_in_pthread && TooManyUserCode()) {
            cntl->SetFailed(ELIMIT, "Too many user code to run when"
                            " -usercode_in_pthread is on");
            break;
        }

        const int req_size = static_cast<int>(msg->payload.size());
        if (meta.has_attachment_size()) {
            if (req_size < meta.attachment_size()) {
                cntl->SetFailed(EREQUEST,
                    "attachment_size=%d is larger than request_size=%d",
                    meta.attachment_size(), req_size);
                break;
            }
        }

        google::protobuf::Service* svc = nullptr;
        google::protobuf::MethodDescriptor* method = nullptr;
        if (IsBaiduMasterService(server, cntl->local_side())) {
            if (socket->is_overcrowded() &&
              !server->options().ignore_eovercrowded &&
              !server->options().baidu_master_service->ignore_eovercrowded()) {
                  cntl->SetFailed(EOVERCROWDED, "Connection to %s is overcrowded",
                                  butil::endpoint2str(socket->remote_side()).c_str());
                  break;
            }
            svc = server->options().baidu_master_service;
            auto sampled_request = new SampledRequest;
            sampled_request->meta.set_service_name(request_meta.service_name());
            sampled_request->meta.set_method_name(request_meta.method_name());
            cntl->reset_sampled_request(sampled_request);
            // Switch to service-specific error.
            non_service_error.release();
            method_status = server->options().baidu_master_service->_status;
            if (method_status) {
                int rejected_cc = 0;
                if (!method_status->OnRequested(&rejected_cc, cntl.get())) {
                    cntl->SetFailed(ELIMIT, "Rejected by %s's ConcurrencyLimiter, concurrency=%d",
                                    butil::class_name<BaiduMasterService>(), rejected_cc);
                    break;
                }
            }
            if (span) {
                span->ResetServerSpanName(sampled_request->meta.method_name());
            }

            messages = BaiduProxyPBMessages::Get();
            msg->payload.cutn(&((SerializedRequest*)messages->Request())->serialized_data(),
                              req_size - meta.attachment_size());
            if (!msg->payload.empty()) {
                cntl->request_attachment().swap(msg->payload);
            }
        } else {
            // NOTE(gejun): jprotobuf sends service names without packages. So the
            // name should be changed to full when it's not.
            butil::StringPiece svc_name(request_meta.service_name());
            if (svc_name.find('.') == butil::StringPiece::npos) {
                const Server::ServiceProperty* sp =
                    server_accessor.FindServicePropertyByName(svc_name);
                if (nullptr == sp) {
                    cntl->SetFailed(ENOSERVICE, "Fail to find service=%s",
                                    request_meta.service_name().c_str());
                    break;
                }
                svc_name = sp->service->GetDescriptor()->full_name();
            }
            const Server::MethodProperty* mp =
                server_accessor.FindMethodPropertyByFullName(
                    svc_name, request_meta.method_name());
            if (nullptr == mp) {
                cntl->SetFailed(ENOMETHOD, "Fail to find method=%s/%s",
                                request_meta.service_name().c_str(),
                                request_meta.method_name().c_str());
                break;
            }
            if (RejectBuiltinAccess(cntl.get(), *server, mp) ||
                RejectNonBuiltinAccessFromInternalPort(cntl.get(), *server, mp)) {
                break;
            }
            if (mp->service->GetDescriptor() == BadMethodService::descriptor()) {
                BadMethodRequest breq;
                BadMethodResponse bres;
                breq.set_service_name(request_meta.service_name());
                mp->service->CallMethod(mp->method, cntl.get(), &breq, &bres, nullptr);
                break;
            }
            if (socket->is_overcrowded() &&
                !server->options().ignore_eovercrowded &&
                !mp->ignore_eovercrowded) {
              cntl->SetFailed(
                  EOVERCROWDED, "Connection to %s is overcrowded",
                  butil::endpoint2str(socket->remote_side()).c_str());
              break;
            }
            // Switch to service-specific error.
            non_service_error.release();
            method_status = mp->status;
            if (method_status) {
                int rejected_cc = 0;
                if (!method_status->OnRequested(&rejected_cc, cntl.get())) {
                    cntl->SetFailed(
                        ELIMIT,
                        "Rejected by %s's ConcurrencyLimiter, concurrency=%d",
                        butil::EnsureString(mp->method->full_name()).c_str(), rejected_cc);
                    break;
                }
            }
            svc = mp->service;
            method = const_cast<google::protobuf::MethodDescriptor*>(mp->method);
            accessor.set_method(method);

            if (span) {
                span->ResetServerSpanName(butil::EnsureString(method->full_name()));
            }

            if (!server->AcceptRequest(cntl.get())) {
                break;
            }

            butil::IOBuf req_buf;
            int body_without_attachment_size = req_size - meta.attachment_size();
            msg->payload.cutn(&req_buf, body_without_attachment_size);
            if (meta.attachment_size() > 0) {
                cntl->request_attachment().swap(msg->payload);
            }

            ContentType content_type = meta.content_type();
            auto compress_type =
                static_cast<CompressType>(meta.compress_type());
            auto checksum_type =
                static_cast<ChecksumType>(meta.checksum_type());
            messages =
                server->options().rpc_pb_message_factory->Get(*svc, *method);
            // request_attachment() has already been filled in above (swapped
            // out of msg->payload) before we get here, so it's safe to fold
            // it into the checksum now when the client asked us to.
            const butil::IOBuf* checksum_attachment =
                cntl->request_checksum_attachment() ?
                &cntl->request_attachment() : nullptr;
            if (!DeserializeRpcMessage(req_buf, *cntl, content_type,
                                       compress_type, checksum_type,
                                       messages->Request(),
                                       checksum_attachment)) {
                cntl->SetFailed(
                    EREQUEST,
                    "Fail to parse request=%s, ContentType=%s, "
                    "CompressType=%s, ChecksumType=%s, request_size=%d",
                    butil::EnsureString(messages->Request()->GetDescriptor()->full_name()).c_str(),
                    ContentTypeToCStr(content_type),
                    CompressTypeToCStr(compress_type),
                    ChecksumTypeToCStr(checksum_type), req_size);
                break;
            }
            req_buf.clear();
        }

        // `socket' will be held until response has been sent
        google::protobuf::Closure* done = ::brpc::NewCallback<
            int64_t, Controller*, RpcPBMessages*,
            const Server*, MethodStatus*, int64_t, std::shared_ptr<Span>>(
                &SendRpcResponse, meta.correlation_id(),cntl.get(),
                messages, server, method_status, msg->received_us(), span);

        // optional, just release resource ASAP
        msg.reset();

        if (span) {
            span->set_start_callback_us(butil::cpuwide_time_us());
            span->AsParent();
        }
        if (!FLAGS_usercode_in_pthread) {
            return svc->CallMethod(method, cntl.release(), 
                                   messages->Request(),
                                   messages->Response(), done);
        }
        if (BeginRunningUserCode()) {
            svc->CallMethod(method, cntl.release(), 
                            messages->Request(),
                            messages->Response(), done);
            return EndRunningUserCodeInPlace();
        } else {
            return EndRunningCallMethodInPool(
                svc, method, cntl.release(),
                messages->Request(),
                messages->Response(), done);
        }
    } while (false);
    
    // `cntl', `req' and `res' will be deleted inside `SendRpcResponse'
    // `socket' will be held until response has been sent

    SendRpcResponse(meta.correlation_id(),
                    cntl.release(), messages,
                    server, method_status,
                    msg->received_us(), span);
}

bool VerifyRpcRequest(const InputMessageBase* msg_base) {
    const MostCommonMessage* msg =
        static_cast<const MostCommonMessage*>(msg_base);
    const Server* server = static_cast<const Server*>(msg->arg());
    Socket* socket = msg->socket();
    
    RpcMeta request_meta;
    if (!ParsePbFromIOBuf(&request_meta, msg->meta)) {
        LOG(WARNING) << "Fail to parse RpcRequestMeta";
        return false;
    }
    const Authenticator* auth = server->options().auth;
    if (nullptr == auth) {
        // Fast pass (no authentication)
        return true;
    }
    if (auth->VerifyCredential(request_meta.authentication_data(),
                               socket->remote_side(),
                               socket->mutable_auth_context()) == 0) {
        return true;
    }

    // Send `ERPCAUTH' to client.
    RpcMeta response_meta;
    response_meta.set_correlation_id(request_meta.correlation_id());
    response_meta.mutable_response()->set_error_code(ERPCAUTH);
    response_meta.mutable_response()->set_error_text("Fail to authenticate");
    std::string user_error_text = auth->GetUnauthorizedErrorText();
    if (!user_error_text.empty()) {
        response_meta.mutable_response()->mutable_error_text()->append(": ");
        response_meta.mutable_response()->mutable_error_text()->append(user_error_text);
    }
    butil::IOBuf res_buf;
    SerializeRpcHeaderAndMeta(&res_buf, response_meta, 0);
    Socket::WriteOptions opt;
    opt.ignore_eovercrowded = true;
    if (socket->Write(&res_buf, &opt) != 0) {
        PLOG_IF(WARNING, errno != EPIPE) << "Fail to write into " << *socket;
    }

    return false;
}

void ProcessRpcResponse(InputMessageBase* msg_base) {
    const int64_t start_parse_us = butil::cpuwide_time_us();
    DestroyingPtr<MostCommonMessage> msg(static_cast<MostCommonMessage*>(msg_base));
    RpcMeta meta;
    if (!ParsePbFromIOBuf(&meta, msg->meta)) {
        LOG(WARNING) << "Fail to parse from response meta";
        return;
    }

    const bthread_id_t cid = { static_cast<uint64_t>(meta.correlation_id()) };
    Controller* cntl = nullptr;

    StreamId remote_stream_id = meta.has_stream_settings() ? meta.stream_settings().stream_id(): INVALID_STREAM_ID;

    const int rc = bthread_id_lock(cid, (void**)&cntl);
    if (rc != 0) {
        LOG_IF(ERROR, rc != EINVAL && rc != EPERM)
            << "Fail to lock correlation_id=" << cid << ": " << berror(rc);
        if (remote_stream_id != INVALID_STREAM_ID) {
            SendStreamRst(msg->socket(), remote_stream_id);
            const auto & extra_stream_ids = meta.stream_settings().extra_stream_ids();
            for (int i = 0; i < extra_stream_ids.size(); ++i) {
                policy::SendStreamRst(msg->socket(), extra_stream_ids[i]);
            }
        }
        return;
    }
    
    ControllerPrivateAccessor accessor(cntl);
    if (remote_stream_id != INVALID_STREAM_ID) {
        accessor.set_remote_stream_settings(
                new StreamSettings(meta.stream_settings()));
    }

    if (!meta.user_fields().empty()) {
        for (const auto& it : meta.user_fields()) {
            (*cntl->response_user_fields())[it.first] = it.second;
        }
    }

    cntl->set_rpc_received_us(msg->received_us());
    if (BAIDU_UNLIKELY(!msg->device_payload.empty())) {
        // Handed over before the error branches below, so that a response
        // that also reports a failure still frees its GPU memory with the
        // Controller instead of leaking it into ~MostCommonMessage.
        cntl->response_device_attachment().append(
            std::move(msg->device_payload));
    }
    if (auto span = accessor.span()) {
        span->set_base_real_us(msg->base_real_us());
        span->set_received_us(msg->received_us());
        // See the matching comment in ProcessRpcRequest(): host bytes only,
        // and the longer header when the reply carried GPU memory.
        span->set_response_size(msg->meta.size() + msg->payload.size() +
                                (cntl->response_device_attachment().empty()
                                 ? RPC_HEADER_SIZE : GDR_HEADER_SIZE));
        span->set_start_parse_us(start_parse_us);
    }
    const RpcResponseMeta &response_meta = meta.response();
    const int saved_error = cntl->ErrorCode();
    do {
        if (response_meta.error_code() != 0) {
            // If error_code is unset, default is 0 = success.
            cntl->SetFailed(response_meta.error_code(), 
                                  "%s", response_meta.error_text().c_str());
            break;
        } 
        // Parse response message iff error code from meta is 0
        butil::IOBuf res_buf;
        const int res_size = msg->payload.length();
        butil::IOBuf* res_buf_ptr = &msg->payload;
        if (meta.has_attachment_size()) {
            if (meta.attachment_size() > res_size) {
                cntl->SetFailed(
                    ERESPONSE, "attachment_size=%d is larger than response_size=%d",
                    meta.attachment_size(), res_size);
                break;
            }
            int body_without_attachment_size = res_size - meta.attachment_size();
            msg->payload.cutn(&res_buf, body_without_attachment_size);
            res_buf_ptr = &res_buf;
            cntl->response_attachment().swap(msg->payload);
        }

        ContentType content_type = meta.content_type();
        auto compress_type = (CompressType)meta.compress_type();
        auto checksum_type = (ChecksumType)meta.checksum_type();
        cntl->set_response_content_type(content_type);
        cntl->set_response_compress_type(compress_type);
        cntl->set_response_checksum_type(checksum_type);
        cntl->set_response_checksum_attachment(meta.checksum_with_attachment());
        accessor.set_checksum_value(meta.checksum_value());
        if (cntl->response()) {
            // response_attachment() has already been filled in above (swapped
            // out of msg->payload) before we get here, so it's safe to fold
            // it into the checksum now when the server told us to.
            const butil::IOBuf* checksum_attachment =
                cntl->response_checksum_attachment() ?
                &cntl->response_attachment() : nullptr;
            if (cntl->response()->GetDescriptor() == SerializedResponse::descriptor()) {
                ((SerializedResponse*)cntl->response())->
                    serialized_data().append(*res_buf_ptr);
            } else if (!DeserializeRpcMessage(*res_buf_ptr, *cntl, content_type,
                                              compress_type, checksum_type,
                                              cntl->response(),
                                              checksum_attachment)) {
                cntl->SetFailed(
                    EREQUEST,
                    "Fail to parse response=%s, ContentType=%s, "
                    "CompressType=%s, ChecksumType=%s, request_size=%d",
                    butil::EnsureString(cntl->response()->GetDescriptor()->full_name()).c_str(),
                    ContentTypeToCStr(content_type),
                    CompressTypeToCStr(compress_type),
                    ChecksumTypeToCStr(checksum_type), res_size);
            }
        } // else silently ignore the response.
    } while (0);
    // Unlocks correlation_id inside. Revert controller's
    // error code if it version check of `cid' fails
    msg.reset();  // optional, just release resource ASAP
    accessor.OnResponse(cid, saved_error);
}

void SerializeRpcRequest(butil::IOBuf* request_buf, Controller* cntl,
                         const google::protobuf::Message* request) {
    // Check sanity of request.
    if (nullptr == request) {
        return cntl->SetFailed(EREQUEST, "`request' is NULL");
    }
    if (request->GetDescriptor() == SerializedRequest::descriptor()) {
        request_buf->append(((SerializedRequest*)request)->serialized_data());
        return;
    }
    if (!request->IsInitialized()) {
        return cntl->SetFailed(EREQUEST, "Missing required fields in request: %s",
                               request->InitializationErrorString().c_str());
    }

    ContentType content_type = cntl->request_content_type();
    CompressType compress_type = cntl->request_compress_type();
    ChecksumType checksum_type = cntl->request_checksum_type();
    const butil::IOBuf* checksum_attachment = nullptr;
    if (cntl->request_checksum_attachment()) {
        // Progressive reading (HTTP-only feature) hands the attachment to
        // the user piece by piece as it arrives, so there's no single,
        // complete IOBuf to fold into the checksum here. baidu_std (this
        // protocol) never sets FLAGS_READ_PROGRESSIVELY itself, but guard
        // against a Controller that's reused/misconfigured across protocols.
        if (!cntl->is_response_read_progressively()) {
            checksum_attachment = &cntl->request_attachment();
        }
    }
    if (!SerializeRpcMessage(*request, *cntl, content_type, compress_type,
                             checksum_type, request_buf, checksum_attachment)) {
        return cntl->SetFailed(
            EREQUEST,
            "Fail to compress request=%s, "
            "ContentType=%s, CompressType=%s, ChecksumType=%s",
            butil::EnsureString(request->GetDescriptor()->full_name()).c_str(),
            ContentTypeToCStr(content_type), CompressTypeToCStr(compress_type),
            ChecksumTypeToCStr(checksum_type));
    }
}

void PackRpcRequest(butil::IOBuf* req_buf,
                    SocketMessage** user_packet,
                    uint64_t correlation_id,
                    const google::protobuf::MethodDescriptor* method,
                    Controller* cntl,
                    const butil::IOBuf& request_body,
                    const Authenticator* auth) {
    RpcMeta meta;
    if (auth && auth->GenerateCredential(
            meta.mutable_authentication_data()) != 0) {
        return cntl->SetFailed(EREQUEST, "Fail to generate credential");
    }

    ControllerPrivateAccessor accessor(cntl);
    RpcRequestMeta* request_meta = meta.mutable_request();
    if (method) {
        request_meta->set_service_name(FLAGS_baidu_protocol_use_fullname ?
                                       method->service()->full_name() :
                                       method->service()->name());
        request_meta->set_method_name(method->name());
        meta.set_compress_type(cntl->request_compress_type());
        meta.set_checksum_type(cntl->request_checksum_type());
        meta.set_checksum_value(accessor.checksum_value());
        if (cntl->request_checksum_attachment()) {
            meta.set_checksum_with_attachment(true);
        }
    } else if (nullptr != cntl->sampled_request()) {
        // Replaying. Keep service-name as the one seen by server.
        request_meta->set_service_name(cntl->sampled_request()->meta.service_name());
        request_meta->set_method_name(cntl->sampled_request()->meta.method_name());
        meta.set_compress_type(cntl->sampled_request()->meta.has_compress_type() ?
                               cntl->sampled_request()->meta.compress_type() :
                               cntl->request_compress_type());
    } else {
        return cntl->SetFailed(ENOMETHOD, "%s.method is NULL", __func__ );
    }
    if (cntl->has_log_id()) {
        request_meta->set_log_id(cntl->log_id());
    }
    if (!cntl->request_id().empty()) {
        request_meta->set_request_id(cntl->request_id());
    }
    meta.set_correlation_id(correlation_id);
    StreamIds request_stream_ids = accessor.request_streams();
    if (!request_stream_ids.empty()) {
        StreamSettings* stream_settings = meta.mutable_stream_settings();
        StreamId request_stream_id = request_stream_ids[0];
        StreamUniquePtr ptr;
        if (Stream::Address(request_stream_id, &ptr) != 0) {
            return cntl->SetFailed(EREQUEST, "Stream=%" PRIu64 " was closed",
                                   request_stream_id);
        }
        Stream* s = ptr.get();
        s->FillSettings(stream_settings);
        for (size_t i = 1; i < request_stream_ids.size(); ++i) {
            stream_settings->mutable_extra_stream_ids()->Add(request_stream_ids[i]);
        }
    }

    if (cntl->has_request_user_fields() && !cntl->request_user_fields()->empty()) {
        ::google::protobuf::Map<std::string, std::string>& user_fields
            = *meta.mutable_user_fields();
        user_fields.insert(cntl->request_user_fields()->begin(),
                           cntl->request_user_fields()->end());
    }

    // Don't use res->ByteSize() since it may be compressed
    const size_t req_size = request_body.length(); 
    const size_t attached_size = cntl->request_attachment().length();
    if (attached_size) {
        meta.set_attachment_size(attached_size);
    }

    if (FLAGS_baidu_std_protocol_deliver_timeout_ms) {
        if (accessor.real_timeout_ms() > 0) {
            request_meta->set_timeout_ms(accessor.real_timeout_ms());
        }
    }
    meta.set_content_type(cntl->request_content_type());

    if (auto span = accessor.span()) {
        request_meta->set_trace_id(span->trace_id());
        request_meta->set_span_id(span->span_id());
        request_meta->set_parent_span_id(span->parent_span_id());
    }

    if (BAIDU_UNLIKELY(!cntl->request_device_attachment().empty())) {
        // Deferred to AppendAndDestroySelf() rather than serialized here.
        // That is the only point on this path that can see the socket, and
        // therefore the only one that can tell whether the device half goes
        // on a second channel or inline behind the body. On a second channel
        // it also has to be queued from the socket's single writer, where the
        // outgoing order is already fixed (that ordering is the only thing
        // pairing the two streams), and queuing it can fail -- at which point
        // the host half must not already be on the wire.
        //
        // Deliberately no fast-fail on device_channel_state() == OFF: OFF now
        // means "this RPC will use the fallback", not "this RPC cannot run".
        butil::IOBuf body;
        body.append(request_body);
        if (attached_size) {
            body.append(cntl->request_attachment());
        }
        // A reference rather than a move, for the same reason the host
        // attachment above is copied and not moved: PackRpcRequest() runs
        // again on every retry, and the blocks are refcounted so this costs
        // one atomic per segment.
        DeviceAttachment device_data;
        device_data.append_ref(cntl->request_device_attachment());
        *user_packet = new DeviceMessage(meta, &body, &device_data);
        return;
    }

    SerializeRpcHeaderAndMeta(req_buf, meta, req_size + attached_size);
    req_buf->append(request_body);
    if (attached_size) {
        req_buf->append(cntl->request_attachment());
    }
}

const char* ContentTypeToCStr(ContentType content_type) {
    switch (content_type) {
    case CONTENT_TYPE_PB:
        return "pb";
    case CONTENT_TYPE_JSON:
        return "json";
    case CONTENT_TYPE_PROTO_JSON:
        return "proto-json";
    case CONTENT_TYPE_PROTO_TEXT:
        return "proto-text";
    default:
        return "unknown";
    }
}

}  // namespace policy
} // namespace brpc
