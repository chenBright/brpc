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


#ifndef BRPC_EXTENSION_H
#define BRPC_EXTENSION_H

#include <string>
#include "butil/scoped_lock.h"
#include "butil/logging.h"
#include "butil/containers/case_ignored_flat_map.h"
#include "butil/memory/singleton_on_pthread_once.h"

namespace butil {
template <typename T> class GetLeakySingleton;
}


namespace brpc {

// A global map from string to user-extended instances (typed T).
// It's used by NamingService and LoadBalancer to maintain globally
// available instances.
// All names are case-insensitive. Names are printed in lowercases.

template <typename T>
class Extension {
public:
    static Extension<T>* instance();

    int Register(const std::string& name, T* instance);
    int RegisterOrDie(const std::string& name, T* instance);
    T* Find(const char* name);
    void List(std::ostream& os, char separator);

private:
template <typename U> friend U* butil::create_leaky_singleton_obj();
    Extension() = default;
    butil::CaseIgnoredFlatMap<T*> _instance_map;
    butil::Mutex _map_mutex;
};

} // namespace brpc


#include "brpc/extension_inl.h"

#endif  // BRPC_EXTENSION_H
