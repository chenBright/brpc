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

#ifndef BRPC_RDMA_HELPER_H
#define BRPC_RDMA_HELPER_H

#if BRPC_WITH_RDMA

#include <infiniband/verbs.h>
#include <string>
#include <functional>
#include "bthread/types.h"


namespace brpc {
namespace rdma {

static constexpr size_t RDMA_MAX_DEVICES = 16;

struct RdmaDevice {
    std::string name; // HCA device name, e.g. "mlx5_0"
    ibv_context* context{NULL};
    ibv_pd* pd{NULL};
    ibv_gid gid{};
    uint16_t lid{0};
    uint8_t gid_index{0};
    uint8_t port_num{1};
    int gid_tbl_len{0};
    int max_sge{0};
    uint8_t pd_index{0}; // Index into the per-device lkey arrays

    ~RdmaDevice();

    // Non-copyable, non-movable (ibverbs resources are not transferable)
    RdmaDevice() = default;
    DISALLOW_COPY_AND_ASSIGN(RdmaDevice);

    int GetCompVector() const;
private:
    mutable uint8_t comp_vector_index{0};
};

std::ostream& operator<<(std::ostream& os, const RdmaDevice& dev);


// Initialize RDMA environment
// Exit if failed
void GlobalRdmaInitializeOrDie();

// Initialize RDMA polling mode with tag
bool InitPollingModeWithTag(bthread_tag_t tag,
                            std::function<void(void)> callback = nullptr,
                            std::function<void(void)> init_fn = nullptr,
                            std::function<void(void)> release_fn = nullptr);

void ReleasePollingModeWithTag(bthread_tag_t tag);

// Register the given memory.
// Return the memory lkey for the given memory, Return 0 when fails.
// To use the memory in IOBuf, append_user_data_with_meta must be called
// and take the lkey as the data meta.
struct RegisterResult {
    explicit RegisterResult(int rc) : rc(rc) {}

    int rc{0};
    // Per-device lkey/rkey arrays, indexed by RdmaDevice::pd_index.
    // For single-device deployments (or when callers only need the
    // default device), use index 0.
    uint32_t all_lkeys[RDMA_MAX_DEVICES]{};
    uint32_t all_rkeys[RDMA_MAX_DEVICES]{};
};

RegisterResult RegisterMemoryForAllRdmaDevices(void* buf, size_t len);

// Register the given memory
// Return the memory lkey for the given memory, Return 0 when fails
// To use the memory in IOBuf, append_user_data_with_meta must be called
// and take the lkey as the data meta
uint32_t RegisterMemoryForRdma(void* buf, size_t len);

// Deregister the given memory
void DeregisterMemoryForRdma(void* buf);

// Return lkey of the given address on the target pd.
uint32_t GetLKey(void* buf, uint8_t pd_index);

// If the RDMA environment is available
bool IsRdmaAvailable();

// Disable RDMA in the remaining lifetime of the process
void GlobalDisableRdma();

// If the given protocol supported by RDMA
bool SupportedByRdma(std::string protocol);

// Get the number of initialized RDMA devices.
size_t GetRdmaDeviceCount();

// Get the list of initialized RDMA device names (in the order they were
// opened, the first one is the default device).
const std::vector<std::string>& GetRdmaDeviceNames();

// Get RdmaDevice by name. Returns nullptr if not found.
// An empty name returns the default (first) device.
RdmaDevice const* GetRdmaDevice(const std::string& device_name);

// Get RdmaDevice by index (pd_index). Returns nullptr if out of range.
RdmaDevice const* GetRdmaDeviceByIndex(int index);

// Get the default (first) RdmaDevice.
RdmaDevice const* GetDefaultRdmaDevice();

}  // namespace rdma
}  // namespace brpc
#else
namespace brpc {
namespace rdma {

// Initialize RDMA environment
// Exit if failed
void GlobalRdmaInitializeOrDie();

}  // namespace rdma
}  // namespace brpc
#endif  // if BRPC_WITH_RDMA

#endif  // BRPC_RDMA_HELPER_H
