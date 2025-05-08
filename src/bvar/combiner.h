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

#ifndef BRPC_COMBINER_H
#define BRPC_COMBINER_H

#include "bvar/detail/combiner.h"

namespace bvar {

// Combiner is a thread-safe class that combines Element to global Value.
template <typename Value, typename Item>
class Combiner {
    // Local submit operator used in operator<<
    struct LocalSubmitOp {
        void operator()(Value& r, const Item& e) const {
            r.Submit(e);
        }
    };

    // Combine two Value into one.
    struct CombineOp {
        void operator()(Value& r1, Value r2) const {
            if (&r1 == &r2) {
                return;
            }
            r1.Merge(r2);
        }
    };

    using AgentCombiner = bvar::detail::AgentCombiner<Value, Value, CombineOp>;
    using Agent = typename AgentCombiner::Agent;

public:
    Combiner() : _combiner(std::make_shared<AgentCombiner>()) {}

    Combiner& operator<<(const Item& item) {
        // It's wait-free for most time
        Agent* agent = _combiner->get_or_create_tls_agent();
        if (BAIDU_UNLIKELY(NULL == agent)) {
            LOG(FATAL) << "Fail to create agent";
        } else {
            agent->element.modify(_local_op, item);
        }
        return *this;
    }

    Value get_value() const {
        return _combiner->combine_agents();
    }

    template<typename Callback>
    void get_value_with_callback(Callback&& callback) {
        _combiner->combine_agents_with_callback(true, callback);
    }

    Value get_and_reset_value() {
        return _combiner->reset_all_agents();
    }

private:
    typename AgentCombiner::self_shared_type _combiner;

    LocalSubmitOp _local_op;
};

} // namespace bvar

#endif //BRPC_COMBINER_H
