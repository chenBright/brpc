#include "baidu_generic_message.h"

#include "brpc/channel.h"
#include "brpc/proto_base.pb.h"

namespace brpc {

const ::google::protobuf::Descriptor* BaiduGenericMessage::descriptor() {
    return BaiduGenericMessageBase::descriptor();
}

::google::protobuf::Metadata BaiduGenericMessage::GetMetadata() const {
    ::google::protobuf::Metadata metadata{};
    metadata.descriptor = BaiduGenericMessage::descriptor();
    metadata.reflection = NULL;
    return metadata;
}

void BaiduGenericStub::CallMethod(const BaiduGenericMethod* method,
                                  Controller* cntl,
                                  const std::string* request,
                                  std::string* response,
                                  ::google::protobuf::Closure* done) {
    if (!method) {
        cntl->SetFailed(EINVAL,
            "Fail to CallGenericMethod with NULL GenericMethod");
    } else if (_channel->options().protocol != PROTOCOL_BAIDU_STD) {
        // std::string err = butil::string_printf(
        //     "Fail to CallGenericMethod with invalid protocol=%s",
        //     ProtocolTypeToString(_options.protocol));
        cntl->SetFailed(EINVAL,
            "Fail to CallGenericMethod with invalid protocol=%s",
            ProtocolTypeToString(_channel->options().protocol));
    }
    if (cntl->Failed()) {
        if(done) {
            done->Run();
        }
        return;
    }
    cntl->set_baidu_generic_method(method);

    BaiduGenericMessage request_wrapper(const_cast<std::string*>(request));
    if (!done) {
        BaiduGenericMessage response_wrapper(response);
        _channel->CallMethod(NULL, cntl, &request_wrapper, &response_wrapper, done);
    } else {
        auto done_wrapper = new BaiduGenericDoneWrapper(done);
        done_wrapper->response_wrapper.raw_data() = response;
        _channel->CallMethod(NULL, cntl, &request_wrapper, &done_wrapper->response_wrapper, done_wrapper);
    }
}

}