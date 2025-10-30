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


#include <gtest/gtest.h>
#include <gflags/gflags.h>
#include "brpc/controller.h"
#include "brpc/server.h"
#include "brpc/channel.h"
#include "brpc/thrift_service.h"
#include "brpc/thrift_message.h"
#include "gen-cpp/echo_types.h"

int main(int argc, char* argv[]) {
    testing::InitGoogleTest(&argc, argv);
    GFLAGS_NAMESPACE::ParseCommandLineFlags(&argc, &argv, true);
    return RUN_ALL_TESTS();
}

namespace {

const std::string g_server_addr = "127.0.0.1:8011";
const std::string echo_message = "hello";
const std::string echo_message_suffix = " (Echo)";

class EchoServiceImpl : public brpc::ThriftService {
public:
    void ProcessThriftFramedRequest(brpc::Controller* cntl,
                                    brpc::ThriftFramedMessage* req,
                                    brpc::ThriftFramedMessage* res,
                                    google::protobuf::Closure* done) override {
        // Dispatch calls to different methods
        if (cntl->thrift_method_name() == "Echo") {
            return Echo(cntl, req->Cast<example::EchoRequest>(),
                        res->Cast<example::EchoResponse>(), done);
        }

        brpc::ClosureGuard done_guard(done);
        cntl->SetFailed(brpc::ENOMETHOD, "Fail to find method=%s",
                        cntl->thrift_method_name().c_str());
    }

    void Echo(brpc::Controller* cntl,
              const example::EchoRequest* req,
              example::EchoResponse* res,
              google::protobuf::Closure* done) {
        // This object helps you to call done->Run() in RAII style. If you need
        // to process the request asynchronously, pass done_guard.release().
        brpc::ClosureGuard done_guard(done);

        if (req->sleep_us > 0) {
            bthread_usleep(req->sleep_us);
        }

        res->message = req->message;
        res->message.append(echo_message_suffix);
    }
};

class ThriftTest : public ::testing::Test {
protected:
    ThriftTest() {
        brpc::ServerOptions server_options;
        server_options.thrift_service = new EchoServiceImpl;
        EXPECT_EQ(0, _server.Start(g_server_addr.c_str(), &server_options));

        brpc::ChannelOptions options;
        options.protocol = brpc::PROTOCOL_THRIFT;
        EXPECT_EQ(0, _channel.Init(g_server_addr.c_str(), "", &options));
    }

    void CallMethod(brpc::ConnectionType type = brpc::CONNECTION_TYPE_POOLED) {
        example::EchoRequest req;
        example::EchoResponse res;
        brpc::Controller cntl;
        cntl.set_connection_type(type);
        req.__set_message(echo_message);
        req.__set_sleep_us(butil::fast_rand_in(100, 1000));

        brpc::ThriftStub stub(&_channel);

        stub.CallMethod("Echo", &cntl, &req, &res, NULL);
        ASSERT_FALSE(cntl.Failed()) << cntl.ErrorCode() << ": " << cntl.ErrorText();
        ASSERT_EQ(res.message, echo_message + echo_message_suffix);
    }

    brpc::Server _server;
    brpc::Channel _channel;
};

TEST_F(ThriftTest, sanity) {
    std::vector<bthread_t> threads;
    threads.reserve(100);
    for (int i = 0; i < 100; ++i) {
        bthread_t tid;
        ASSERT_EQ(0, bthread_start_background(&tid, NULL, [](void* arg) -> void* {
            auto t = (ThriftTest*)arg;
            for (int j = 0; j < 1000; ++j) {
                t->CallMethod();
            }
            return NULL;
        }, this));
        threads.push_back(tid);
    }
    for (auto t : threads) {
        bthread_join(t, NULL);
    }
}

TEST_F(ThriftTest, single) {
    std::vector<bthread_t> threads;
    threads.reserve(100);
    for (int i = 0; i < 100; ++i) {
        bthread_t tid;
        ASSERT_EQ(0, bthread_start_background(&tid, NULL, [](void* arg) -> void* {
            auto t = (ThriftTest*)arg;
            for (int j = 0; j < 1000; ++j) {
                t->CallMethod(brpc::CONNECTION_TYPE_SINGLE);
            }
            return NULL;
        }, this));
        threads.push_back(tid);
    }
    for (auto t : threads) {
        bthread_join(t, NULL);
    }
}

} // namespace 
