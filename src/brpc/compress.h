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


#ifndef BRPC_COMPRESS_H
#define BRPC_COMPRESS_H

#include <google/protobuf/message.h>              // Message
#include "butil/iobuf.h"                           // butil::IOBuf
#include "json2pb/pb_to_json.h"
#include "json2pb/json_to_pb.h"
#include "brpc/options.pb.h"                     // CompressType

namespace brpc {

// Base class for CompressCallback and DecompressCallback.
class CompressBase {
public:
    void append_error(const std::string& error) {
        if (_error.empty()) {
            _error = error;
        } else {
            _error.append(", ").append(error);
        }
    }

    const std::string& get_error() const {
        return _error;
    }
protected:
    // Error messages.
    std::string _error;
};

// CompressCallback provides raw data for compression,
// and a buffer for storing compressed data.
class CompressCallback : public CompressBase {
public:
    // Converts the data into `output' for later compression.
    virtual bool Convert(google::protobuf::io::ZeroCopyOutputStream* output) = 0;
    // Returns the buffer for storing compressed data.
    virtual butil::IOBuf& Buffer() = 0;
};

// DecompressCallback provides raw data stored in a buffer for decompression,
// and handles the decompressed data.
class DecompressCallback : public CompressBase {
public:
    // Converts the decompressed `input'.
    virtual bool Convert(google::protobuf::io::ZeroCopyInputStream* intput) = 0;
    // Returns the buffer containing compressed data.
    virtual const butil::IOBuf& Buffer() = 0;
};

struct CompressHandler {
    // Compress data from CompressCallback::Convert() into CompressCallback::Buffer().
    bool (*Compress)(CompressCallback& callback);

    // Decompress data from DecompressCallback::Buffer() into DecompressCallback::Convert().
    bool (*Decompress)(DecompressCallback& callback);

    // Name of the compression algorithm, must be string constant.
    const char* name;
};

// [NOT thread-safe] Register `handler' using key=`type'
// Returns 0 on success, -1 otherwise
int RegisterCompressHandler(CompressType type, CompressHandler handler);

// Returns CompressHandler pointer of `type' if registered, NULL otherwise.
const CompressHandler* FindCompressHandler(CompressType type);

// Returns the `name' of the CompressType if registered
const char* CompressTypeToCStr(CompressType type);

// Put all registered handlers into `vec'.
void ListCompressHandler(std::vector<CompressHandler>* vec);

// CompressCallback for Protobuf messages.
class PBCompressCallback : public CompressCallback {
public:
    PBCompressCallback(const google::protobuf::Message& msg, butil::IOBuf* buf)
        : _msg(msg), _buf(buf) {}
    bool Convert(google::protobuf::io::ZeroCopyOutputStream* output) override {
        return _msg.SerializeToZeroCopyStream(output);
    }
    butil::IOBuf& Buffer() override { return *_buf; }

private:
    const google::protobuf::Message& _msg;
    butil::IOBuf* _buf;
};

// DecompressCallback for Protobuf messages.
class PBDecompressCallback : public DecompressCallback {
public:
    PBDecompressCallback(const butil::IOBuf& buf, google::protobuf::Message* msg) : _buf(buf), _msg(msg) {}
    bool Convert(google::protobuf::io::ZeroCopyInputStream* input) override {
        return _msg->ParseFromZeroCopyStream(input);
    }
    const butil::IOBuf& Buffer() override { return _buf; }

private:
    const butil::IOBuf& _buf;
    google::protobuf::Message* _msg;
};

// Parse decompressed `data' as `msg' using registered `compress_type'.
// Returns true on success, false otherwise
bool ParseFromCompressedData(const butil::IOBuf& data,
                             google::protobuf::Message* msg,
                             CompressType compress_type);

// Compress serialized `msg' into `buf' using registered `compress_type'.
// Returns true on success, false otherwise
bool SerializeAsCompressedData(const google::protobuf::Message& msg,
                               butil::IOBuf* buf,
                               CompressType compress_type);

} // namespace brpc


#endif // BRPC_COMPRESS_H
