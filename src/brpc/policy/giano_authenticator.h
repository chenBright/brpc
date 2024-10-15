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

#ifdef BAIDU_INTERNAL


#ifndef BRPC_POLICY_GIANO_AUTHENTICATOR_H
#define BRPC_POLICY_GIANO_AUTHENTICATOR_H

#include <baas-lib-c/baas.h>                   // Giano stuff
#include "brpc/authenticator.h"

namespace brpc {
namespace policy {

class GianoAuthenticator: public Authenticator {
public:
    // Either `gen' or `ver' can be nullptr (but not at the same time),
    // in which case it can only verify/generate credential data
    explicit GianoAuthenticator(const baas::CredentialGenerator* gen,
                                const baas::CredentialVerifier* ver);

    ~GianoAuthenticator();

    int GenerateCredential(std::string* auth_str) const;

    int VerifyCredential(const std::string& auth_str,
                         const butil::EndPoint& client_addr,
                         AuthContext* out_ctx) const;

private:
    baas::CredentialGenerator* _generator;
    baas::CredentialVerifier* _verifier;
};


}  // namespace policy
} // namespace brpc


#endif // BRPC_POLICY_GIANO_AUTHENTICATOR_H
#endif // BAIDU_INTERNAL
