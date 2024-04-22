#ifndef BRPC_BAIDU_GENERIC_MESSAGE_H
#define BRPC_BAIDU_GENERIC_MESSAGE_H

#include <string>
#include <google/protobuf/message.h>
#include "brpc/pb_compat.h"
#include "butil/logging.h"

namespace brpc {
class Channel;
class Controller;

struct BaiduGenericMethod {
    std::string name;
    std::string full_name;
    std::string service_name;
    std::string service_full_name;
};

class BaiduGenericMessage : public ::google::protobuf::Message {
public:
    BaiduGenericMessage() : _raw_data(NULL) {}
    explicit BaiduGenericMessage(std::string* raw_data) : _raw_data(raw_data) {}

    std::string*& raw_data() {
        return _raw_data;
    }

    // todo 好像不太好
    const std::string* raw_data() const {
        return _raw_data;
    }

    // implements Message ----------------------------------------------

    BaiduGenericMessage* New() const PB_319_OVERRIDE {
        return new BaiduGenericMessage;
    }
#if GOOGLE_PROTOBUF_VERSION >= 3006000
    BaiduGenericMessage* New(::google::protobuf::Arena* arena) const override {
        return CreateMaybeMessage<BaiduGenericMessage>(arena);
    }
#endif
    void CopyFrom(const ::google::protobuf::Message& from) PB_321_OVERRIDE {
        CHECK_NE(&from, this);
        LOG(ERROR) << "ThriftFramedMessage does not support MergeFrom";
    }

    void MergeFrom(const ::google::protobuf::Message& from) override {
        CHECK_NE(&from, this);
        LOG(ERROR) << "ThriftFramedMessage does not support MergeFrom";
    }

    void CopyFrom(const BaiduGenericMessage& from) {
        if (&from == this) {
            return;
        }
        LOG(ERROR) << "ThriftFramedMessage does not support CopyFrom";
    }

    void MergeFrom(const BaiduGenericMessage& from) {
        if (&from == this) {
            return;
        }
        LOG(ERROR) << "ThriftFramedMessage does not support CopyFrom";
    }

    void Clear() override {}
    bool IsInitialized() const override { return _raw_data != NULL; }

    int ByteSize() const {
        return _raw_data ? static_cast<int>(_raw_data->size()) : 0;
    }

    bool MergePartialFromCodedStream(
        ::google::protobuf::io::CodedInputStream* input) PB_310_OVERRIDE {
        LOG(WARNING) << "You're not supposed to parse a RedisRequest";
        return false;
    }

    void SerializeWithCachedSizes(
        ::google::protobuf::io::CodedOutputStream* output) const PB_310_OVERRIDE {
        LOG(WARNING) << "You're not supposed to serialize a RedisRequest";
    }

    ::google::protobuf::uint8* SerializeWithCachedSizesToArray(::google::protobuf::uint8* target) const PB_310_OVERRIDE {
        return target;
    }

    int GetCachedSize() const override { return ByteSize(); }

    static const ::google::protobuf::Descriptor* descriptor();

protected:
    ::google::protobuf::Metadata GetMetadata() const override;

private:
    std::string* _raw_data;
};

// A wrapper closure to own additional stuffs required by BaiduGenericStub.
class BaiduGenericDoneWrapper : public ::google::protobuf::Closure {
public:
    explicit BaiduGenericDoneWrapper(::google::protobuf::Closure* done)
        : _done(done) {}

    void Run() override {
        _done->Run();
        delete this;
    }
private:
    ::google::protobuf::Closure* _done;
public:
    BaiduGenericMessage response_wrapper;
};

class BaiduGenericStub {
public:
    explicit BaiduGenericStub(Channel* channel) : _channel(channel) {}

    void CallMethod(const BaiduGenericMethod* method,
                    Controller* cntl,
                    const std::string* request,
                    std::string* response,
                    ::google::protobuf::Closure* done);

private:
    Channel* _channel;
};

}

#endif //BRPC_BAIDU_GENERIC_MESSAGE_H
