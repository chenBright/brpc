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

#if BRPC_WITH_RDMA

#include <dlfcn.h>                                // dlopen
#include <pthread.h>
#include <stdlib.h>
#include <vector>
#include <memory>                                 // std::unique_ptr
#include <iomanip>                                // std::hex, std::setw, std::setfill
#include <gflags/gflags.h>
#include "butil/containers/flat_map.h"            // butil::FlatMap
#include "butil/fd_guard.h"
#include "butil/fd_utility.h"                     // butil::make_non_blocking
#include "butil/logging.h"
#include "brpc/socket.h"
#include "brpc/rdma/block_pool.h"
#include "brpc/rdma/rdma_endpoint.h"
#include "brpc/rdma/rdma_helper.h"


namespace butil {
namespace iobuf {
// declared in iobuf.cpp
extern void* (*blockmem_allocate)(size_t);
extern void  (*blockmem_deallocate)(void*);
}
}

namespace brpc {
namespace rdma {

void* g_handle_ibverbs = NULL;
bool g_skip_rdma_init = false;

ibv_device** (*IbvGetDeviceList)(int*) = NULL;
void (*IbvFreeDeviceList)(ibv_device**) = NULL;
ibv_context* (*IbvOpenDevice)(ibv_device*) = NULL;
int (*IbvCloseDevice)(ibv_context*) = NULL;
const char* (*IbvGetDeviceName)(ibv_device*) = NULL;
int (*IbvForkInit)(void) = NULL;
int (*IbvQueryDevice)(ibv_context*, ibv_device_attr*) = NULL;
int (*IbvQueryPort)(ibv_context*, uint8_t, ibv_port_attr*) = NULL;
int (*IbvQueryGid)(ibv_context*, uint8_t, int, ibv_gid*) = NULL;
ibv_pd* (*IbvAllocPd)(ibv_context*) = NULL;
int (*IbvDeallocPd)(ibv_pd*) = NULL;
ibv_cq* (*IbvCreateCq)(ibv_context*, int, void*, ibv_comp_channel*, int) = NULL;
int (*IbvDestroyCq)(ibv_cq*) = NULL;
ibv_qp* (*IbvCreateQp)(ibv_pd*, ibv_qp_init_attr*) = NULL;
int (*IbvModifyQp)(ibv_qp*, ibv_qp_attr*, ibv_qp_attr_mask) = NULL;
int (*IbvQueryQp)(ibv_qp*, ibv_qp_attr*, ibv_qp_attr_mask, ibv_qp_init_attr*) = NULL;
int (*IbvDestroyQp)(ibv_qp*) = NULL;
ibv_comp_channel* (*IbvCreateCompChannel)(ibv_context*) = NULL;
int (*IbvDestroyCompChannel)(ibv_comp_channel*) = NULL;
ibv_mr* (*IbvRegMr)(ibv_pd*, void*, size_t, int) = NULL;
int (*IbvDeregMr)(ibv_mr*) = NULL;
int (*IbvGetCqEvent)(ibv_comp_channel*, ibv_cq**, void**) = NULL;
void (*IbvAckCqEvents)(ibv_cq*, unsigned int) = NULL;
int (*IbvGetAsyncEvent)(ibv_context*, ibv_async_event*) = NULL;
void (*IbvAckAsyncEvent)(ibv_async_event*) = NULL;
const char* (*IbvEventTypeStr)(ibv_event_type) = NULL;
int (*IbvQueryEce)(ibv_qp*, ibv_ece*) = NULL;
int (*IbvSetEce)(ibv_qp*, ibv_ece*) = NULL;

// NOTE:
// ibv_post_send, ibv_post_recv, ibv_poll_cq, ibv_req_notify_cq are all inline function
// defined in infiniband/verbs.h.

butil::atomic<bool> g_rdma_available(false);

DEFINE_int32(rdma_max_sge, 0, "Max SGE num in a WR");
DEFINE_string(rdma_device, "", "The name of the HCA device(s) used. "
                               "Multiple devices can be separated by comma, e.g. "
                               "\"mlx5_0,mlx5_1\", or \"all\" to open every "
                               "available device. "
                               "(Empty means using the first active device)");
DEFINE_int32(rdma_port, 1, "The port number to use. For RoCE, it is always 1.");
DEFINE_int32(rdma_gid_index, -1, "The GID index to use. -1 means using the last one.");

// static const size_t SYSFS_SIZE = 4096;
static ibv_device** g_devices = NULL;
static std::vector<ibv_mr*>* g_mrs = NULL; // mr registered by brpc

// Per-device user MR maps: index 0 = default device, 1..N-1 = extra devices.
static butil::FlatMap<void*, ibv_mr*>* g_user_mr_maps[RDMA_MAX_DEVICES] = {};
static butil::Mutex* g_user_mr_maps_lock = NULL;

// All opened RDMA devices. The first one is the default device.
static std::vector<RdmaDevice*> g_rdma_devices;
// Device names in order (for GetRdmaDeviceNames())
static std::vector<std::string> g_rdma_device_names;
// Map from device name to RdmaDevice*
static butil::FlatMap<std::string, RdmaDevice*>* g_rdma_device_map = NULL;
// Async sockets for each device (one per device)
static std::vector<SocketId> g_async_sockets;

// Store the original IOBuf memalloc and memdealloc functions
static void* (*g_mem_alloc)(size_t) = NULL;
static void (*g_mem_dealloc)(void*) = NULL;

namespace {
struct IbvDeviceDeleter {
    void operator()(ibv_device** device_list) {
      IbvFreeDeviceList(device_list);
    }
};

struct IbvContextDeleter {
    void operator() (ibv_context* context) {
        IbvCloseDevice(context);
    }
};
}  // namespace

static void GlobalRelease() {
    g_rdma_available.store(false, butil::memory_order_release);
    usleep(100000);  // to avoid unload library too early

    // We do not set async sockets to failed explicitly to avoid
    // close async_fd twice.

    RdmaEndpoint::GlobalRelease();

    if (g_user_mr_maps_lock) {
        BAIDU_SCOPED_LOCK(*g_user_mr_maps_lock);
        // Deregister and clean up all device user MRs
        for (auto& user_mr_map : g_user_mr_maps) {
            if (user_mr_map) {
                for (auto it = user_mr_map->begin();
                     it != user_mr_map->end(); ++it) {
                    IbvDeregMr(it->second);
                     }
                user_mr_map->clear();
                delete user_mr_map;
                user_mr_map = NULL;
            }
        }
    }
    delete g_user_mr_maps_lock;
    g_user_mr_maps_lock = NULL;

    if (g_mrs) {
        for (auto& mr : *g_mrs) {
            IbvDeregMr(mr);
        }
        delete g_mrs;
        g_mrs = NULL;
    }

    // Release all RDMA devices (destructor handles pd/context cleanup)
    for (auto dev : g_rdma_devices) {
        delete dev;
    }
    g_rdma_devices.clear();
    g_rdma_device_names.clear();
    delete g_rdma_device_map;
    g_rdma_device_map = NULL;
    g_async_sockets.clear();

    if (g_devices) {
        IbvFreeDeviceList(g_devices);
        g_devices = NULL;
    }
}

void* UserExtendBlockPool(void* region_base, size_t region_size,
                          int block_type) {
    return ExtendBlockPoolByUser(region_base, region_size, block_type);
}

// Register memory on all RDMA devices for block_pool.
// Returns vector of lkeys indexed by pd_index. Entry is 0 on failure.
RegisterIds RdmaRegisterMemory(void* buf, size_t size) {
    RegisterIds result(g_rdma_devices.size(), 0);
    for (size_t i = 0; i < g_rdma_devices.size(); ++i) {
        RdmaDevice* dev = g_rdma_devices[i];
        CHECK_EQ(i, dev->pd_index) << "pd_index mismatch for device " << dev->name;
        ibv_mr* mr = IbvRegMr(dev->pd, buf, size, IBV_ACCESS_LOCAL_WRITE);
        if (!mr) {
            PLOG(ERROR) << "Fail to register memory on device " << dev->name;
            continue;
        }

        g_mrs->push_back(mr);
        result[dev->pd_index] = mr->lkey;
        LOG(INFO) << "Register memory(block_pool) addr=" << buf << " len=" << size
                  << " device=" << dev->name
                  << " pd_index=" << dev->pd_index
                  << " lkey=" << mr->lkey << " rkey=" << mr->rkey;
    }
    LOG(INFO) << "Register memory(block_pool) on " << g_rdma_devices.size() << " device(s)";

    return result;
}

static void* BlockAllocate(size_t len) {
    if (len == 0) {
        errno = EINVAL;
        return NULL;
    }
    void* ptr = AllocBlock(len);
    if (!ptr) {
        LOG(ERROR) << "Fail to get block from memory pool";
    }

    return ptr;
}

void BlockDeallocate(void* buf) {
    if (!buf) {
        errno = EINVAL;
        return;
    }
    DeallocBlock(buf);
}

RdmaDevice::~RdmaDevice() {
    if (pd) {
        IbvDeallocPd(pd);
        pd = nullptr;
    }
    if (context) {
        IbvCloseDevice(context);
        context = nullptr;
    }
}

int RdmaDevice::GetCompVector() const {
    CHECK(context);
    return (comp_vector_index++) % context->num_comp_vectors;
}

std::ostream& operator<<(std::ostream& os, const RdmaDevice& dev) {
    // Save the stream's formatting state so we don't leak hex/fill changes
    // back to the caller.
    const std::ios::fmtflags saved_flags = os.flags();
    const char saved_fill = os.fill();

    os << "RdmaDevice{name=" << (dev.name.empty() ? "<unnamed>" : dev.name)
       << ", pd_index=" << dev.pd_index
       << ", port=" << dev.port_num
       << ", gid_index=" << dev.gid_index
       << ", gid=";

    // GID is 16 bytes; print as 8 groups of 4 hex digits separated by ':',
    // matching the conventional InfiniBand/RoCE GID textual form.
    os << std::hex << std::setfill('0');
    for (int i = 0; i < 8; ++i) {
        if (i != 0) {
            os << ':';
        }
        uint hi = dev.gid.raw[i * 2];
        uint lo = dev.gid.raw[i * 2 + 1];
        os << std::setw(2) << hi << std::setw(2) << lo;
    }
    // Restore formatting before printing the remaining decimal fields.
    os.flags(saved_flags);
    os.fill(saved_fill);

    os << ", lid=" << dev.lid
       << ", gid_tbl_len=" << dev.gid_tbl_len
       << ", max_sge=" << dev.max_sge
       << ", context=" << dev.context
       << ", pd=" << dev.pd
       << "}";
    return os;
}

static void FindRdmaLidForDevice(RdmaDevice* dev) {
    ibv_port_attr attr;
    if (IbvQueryPort(dev->context, dev->port_num, &attr) != 0) {
        return;
    }
    dev->lid = attr.lid;
}

static bool FindRdmaGidForDevice(RdmaDevice* dev) {
    bool found = false;
    for (int i = dev->gid_tbl_len - 1; i >= 0; --i) {
        ibv_gid gid;
        if (IbvQueryGid(dev->context, dev->port_num, i, &gid) != 0) {
            continue;
        }
        if (gid.global.interface_id == 0) {
            continue;
        }
        if (FLAGS_rdma_gid_index == i) {
            dev->gid = gid;
            dev->gid_index = i;
            return true;
        }
        // For infiniband, there is only one GID for each port.
        // For RoCE, there are 2 GIDs for each MAC and 2 GIDs for each IP.
        // Generally, the last GID is a RoCEv2-type GID generated by IP.
        if (!found) {
            dev->gid = gid;
            dev->gid_index = i;
            found = true;
        }
    }
    if (FLAGS_rdma_gid_index >= 0 && dev->gid_index != FLAGS_rdma_gid_index) {
        found = false;
    }

    return found;
}

// Find the RdmaDevice whose context->async_fd matches the socket's fd.
// Returns NULL if not found (should not happen).
static RdmaDevice* FindDeviceByAsyncFd(int fd) {
    for (auto dev : g_rdma_devices) {
        if (dev->context->async_fd == fd) {
            return dev;
        }
    }
    return NULL;
}

static void OnRdmaAsyncEvent(Socket* m) {
    // Determine which device this async socket belongs to
    RdmaDevice* dev = FindDeviceByAsyncFd(m->fd());
    CHECK(dev) << "Socket " << m->description() << " does not belong to any RDMA device";

    int progress = Socket::PROGRESS_INIT;
    do {
        ibv_async_event event;
        if (IbvGetAsyncEvent(dev->context, &event) != 0) {
            break;
        }
        LOG(WARNING) << "rdma async event: " << IbvEventTypeStr(event.event_type);
        switch (event.event_type) {
        case IBV_EVENT_QP_REQ_ERR:
        case IBV_EVENT_QP_ACCESS_ERR:
        case IBV_EVENT_QP_FATAL: {
            SocketId sid = (SocketId)event.element.qp->qp_context;
            SocketUniquePtr s;
            if (Socket::Address(sid, &s) == 0) {
                s->SetFailed(ERDMA, "QP fatal error");
                LOG(WARNING) << "Receive a QP fatal error on " << s->description();
            }
            // NOTE:
            // We must ack the async event here, before `s' is recycled.
            // Otherwise there will be an deadlock.
            // Please check the use of ibv_ack_async_event at:
            // http://www.rdmamojo.com/2012/08/16/ibv_ack_async_event/
            IbvAckAsyncEvent(&event);
            break;
        }
        case IBV_EVENT_CQ_ERR: {
            LOG(WARNING) << "CQ overruns, the connection will be stopped.";
            IbvAckAsyncEvent(&event);
            break;
        }
        case IBV_EVENT_COMM_EST:
        case IBV_EVENT_SQ_DRAINED:
        case IBV_EVENT_QP_LAST_WQE_REACHED: {
            // just ignore the event
            IbvAckAsyncEvent(&event);
            break;
        }
        case IBV_EVENT_SRQ_ERR:
        case IBV_EVENT_SRQ_LIMIT_REACHED: {
            // SRQ not used, should not happen
            IbvAckAsyncEvent(&event);
            break;
        }
        case IBV_EVENT_LID_CHANGE: {
            FindRdmaLidForDevice(dev);
            LOG(INFO) << "RDMA LID changed on " << *dev;

            IbvAckAsyncEvent(&event);
            break;
        }
        case IBV_EVENT_PATH_MIG:
        case IBV_EVENT_PATH_MIG_ERR:
        case IBV_EVENT_PKEY_CHANGE:
        case IBV_EVENT_SM_CHANGE:
        case IBV_EVENT_CLIENT_REREGISTER: {
            // for IB only, we haven't test these events carefully
            IbvAckAsyncEvent(&event);
            break;
        }
        case IBV_EVENT_PORT_ACTIVE:
        case IBV_EVENT_PORT_ERR: {
            // Port up/down will lead these two events.
            // The port error is recoverable.
            IbvAckAsyncEvent(&event);
            break;
        }
        case IBV_EVENT_GID_CHANGE: {
            FindRdmaGidForDevice(dev);
            LOG(INFO) << "RDMA GID changed on " << *dev;
            IbvAckAsyncEvent(&event);
            break;
        }
        case IBV_EVENT_DEVICE_FATAL: {
            // because the memory resources are related to rdma device
            // we view this error unrecoverable
            GlobalDisableRdma();
            IbvAckAsyncEvent(&event);
            break;
        }
        default:
            // should not happen
            IbvAckAsyncEvent(&event);
            break;
        }
        if (!m->MoreReadEvents(&progress)) {
            break;
        }
    } while (true);
}

#define LoadSymbol(handle, func, symbol) \
    *(void**)(&func) = dlsym(handle, symbol); \
    if (!func) { \
        LOG(ERROR) << "Fail to find symbol: " << symbol; \
        return -1; \
    }

static int ReadRdmaDynamicLib() {
    const static char* const kRdmaLibs[] = {
        "libibverbs.so",
        "libibverbs.so.1"
    };
    for (const char* lib : kRdmaLibs) {
        dlerror();  // Clear existing error
        g_handle_ibverbs = dlopen(lib, RTLD_LAZY);
        if (g_handle_ibverbs) {
            LOG(INFO) << "Successfully loaded " << lib;
            break;
        }
        LOG(WARNING) << "Failed to load " << lib << ": " << dlerror();
    }
    if (!g_handle_ibverbs) {
        LOG(ERROR) << "Failed to load any of the RDMA libraries";
        return -1;
    }

    LoadSymbol(g_handle_ibverbs, IbvGetDeviceList, "ibv_get_device_list");
    LoadSymbol(g_handle_ibverbs, IbvFreeDeviceList, "ibv_free_device_list");
    LoadSymbol(g_handle_ibverbs, IbvOpenDevice, "ibv_open_device");
    LoadSymbol(g_handle_ibverbs, IbvCloseDevice, "ibv_close_device");
    LoadSymbol(g_handle_ibverbs, IbvGetDeviceName, "ibv_get_device_name");
    LoadSymbol(g_handle_ibverbs, IbvForkInit, "ibv_fork_init");
    LoadSymbol(g_handle_ibverbs, IbvQueryDevice, "ibv_query_device");
    LoadSymbol(g_handle_ibverbs, IbvQueryPort, "ibv_query_port");
    LoadSymbol(g_handle_ibverbs, IbvQueryGid, "ibv_query_gid");
    LoadSymbol(g_handle_ibverbs, IbvAllocPd, "ibv_alloc_pd");
    LoadSymbol(g_handle_ibverbs, IbvDeallocPd, "ibv_dealloc_pd");
    LoadSymbol(g_handle_ibverbs, IbvCreateCq, "ibv_create_cq");
    LoadSymbol(g_handle_ibverbs, IbvDestroyCq, "ibv_destroy_cq");
    LoadSymbol(g_handle_ibverbs, IbvCreateQp, "ibv_create_qp");
    LoadSymbol(g_handle_ibverbs, IbvModifyQp, "ibv_modify_qp");
    LoadSymbol(g_handle_ibverbs, IbvQueryQp, "ibv_query_qp");
    LoadSymbol(g_handle_ibverbs, IbvDestroyQp, "ibv_destroy_qp");
    LoadSymbol(g_handle_ibverbs, IbvCreateCompChannel, "ibv_create_comp_channel");
    LoadSymbol(g_handle_ibverbs, IbvDestroyCompChannel, "ibv_destroy_comp_channel");
    LoadSymbol(g_handle_ibverbs, IbvRegMr, "ibv_reg_mr");
    LoadSymbol(g_handle_ibverbs, IbvDeregMr, "ibv_dereg_mr");
    LoadSymbol(g_handle_ibverbs, IbvGetCqEvent, "ibv_get_cq_event");
    LoadSymbol(g_handle_ibverbs, IbvAckCqEvents, "ibv_ack_cq_events");
    LoadSymbol(g_handle_ibverbs, IbvGetAsyncEvent, "ibv_get_async_event");
    LoadSymbol(g_handle_ibverbs, IbvAckAsyncEvent, "ibv_ack_async_event");
    LoadSymbol(g_handle_ibverbs, IbvEventTypeStr, "ibv_event_type_str");
    LoadSymbol(g_handle_ibverbs, IbvQueryEce, "ibv_query_ece");
    LoadSymbol(g_handle_ibverbs, IbvSetEce, "ibv_set_ece");

    return 0;
}

static void ExitWithError() {
    GlobalRelease(); 
    exit(1);
}

// Parse comma-separated device names from FLAGS_rdma_device.
// Sets *use_all to true when the flag value is "all" (case-insensitive),
// meaning every available device should be opened.
static std::vector<std::string> ParseDeviceNames(bool* use_all) {
    *use_all = false;
    std::vector<std::string> names;
    if (FLAGS_rdma_device.empty()) {
        return names;  // empty means auto-detect (first available)
    }
    // Trim and check for "all"
    butil::StringPiece input(FLAGS_rdma_device);
    input.trim_spaces();
    if (input == "all") {
        *use_all = true;
        return names;  // empty list, but use_all=true means open everything
    }
    // Split by ',' and trim each token
    butil::StringPiece remaining(FLAGS_rdma_device);
    while (!remaining.empty()) {
        size_t pos = remaining.find(',');
        butil::StringPiece token = pos == butil::StringPiece::npos
            ? remaining : remaining.substr(0, pos);
        token.trim_spaces();
        if (!token.empty()) {
            names.push_back(token.as_string());
        }
        if (pos == butil::StringPiece::npos) {
            break;
        }
        remaining.remove_prefix(pos + 1);
    }
    return names;
}

//
// @brief Open RDMA devices according to FLAGS_rdma_device.
//
// - Empty: open the first device with an active port.
// - "all": open every device with an active port.
// - Comma-separated names: open the listed devices.
//
// Opened devices are appended to g_rdma_devices. The first one becomes
// the default device (g_rdma_devices[0]).
//
// @param num_total Total number returned by ibv_get_device_list
static void OpenDevice(int num_total) {
    bool use_all = false;
    std::vector<std::string> requested_names = ParseDeviceNames(&use_all);
    // "all" or multiple names → multi-device mode
    bool multi_device = use_all || requested_names.size() > 1;

    for (int i = 0; i < num_total; ++i) {
        std::unique_ptr<ibv_context, IbvContextDeleter> context{
            IbvOpenDevice(g_devices[i]), IbvContextDeleter()};
        const char* dev_name = IbvGetDeviceName(g_devices[i]);
        if (!context) {
            PLOG(ERROR) << "Fail to open rdma device " << dev_name;
            continue;
        }
        ibv_port_attr attr;
        if (IbvQueryPort(context.get(), uint8_t(FLAGS_rdma_port), &attr) != 0) {
            PLOG(WARNING) << "Fail to query port " << FLAGS_rdma_port << " on "
                          << dev_name;
            continue;
        }
        if (attr.state != IBV_PORT_ACTIVE) {
            LOG(WARNING) << "Device " << dev_name << " port not active";
            continue;
        }

        if (multi_device) {
            if (!use_all) {
                if (std::find(requested_names.begin(),
                              requested_names.end(),
                              dev_name) == requested_names.end()) {
                    LOG(INFO) << "Device " << dev_name << " not in requested list, skipping";
                    continue;
                }
            }
        } else if (!requested_names.empty()) {
            if (requested_names[0] != dev_name) {
                LOG(INFO) << "Device name not match: " << dev_name
                          << " vs " << requested_names[0];
                continue;
            }
        }

        std::unique_ptr<RdmaDevice> rdma_device(new RdmaDevice);
        rdma_device->name = dev_name;
        rdma_device->context = context.release();
        rdma_device->lid = attr.lid;
        rdma_device->port_num = FLAGS_rdma_port;
        rdma_device->gid_tbl_len = attr.gid_tbl_len;
        if (!FindRdmaGidForDevice(rdma_device.get())) {
            LOG(ERROR) << "Fail to find GID for device " << dev_name;
            continue;
        }
        g_rdma_devices.push_back(rdma_device.release());
        g_rdma_device_names.push_back(dev_name);

        // Single-device mode: stop after the first match
        if (!multi_device) {
            break;
        }
    }
}

static void GlobalRdmaInitializeOrDieImpl() {
    if (BAIDU_UNLIKELY(g_skip_rdma_init)) {
        // Just for UT
        return;
    }

    if (ReadRdmaDynamicLib() < 0) { 
        LOG(ERROR) << "Fail to load rdma dynamic lib";
        ExitWithError();
    }

    // ibv_fork_init is very important. If we don't call this API,
    // we may get some very, very strange problems if the program
    // calls fork().
    if (IbvForkInit()) {
        PLOG(ERROR) << "Fail to ibv_fork_init";
        ExitWithError();
    }

    int num = 0;
    g_devices = IbvGetDeviceList(&num);
    if (num == 0) {
        LOG(ERROR) << "Fail to find rdma device";
        ExitWithError();
    }

    // OpenDevice populates g_rdma_devices; the first entry is the default.
    OpenDevice(num);

    if (g_rdma_devices.empty()) {
        LOG(ERROR) << "Fail to find available RDMA device " << FLAGS_rdma_device;
        ExitWithError();
    }
    if (num > 1 && g_rdma_devices.size() == 1 && FLAGS_rdma_device.empty()) {
        LOG(INFO) << "This server has more than one RDMA device. Only "
                  << "the first one (" << g_rdma_devices[0]->name
                  << ") will be used. If you want to use other device, please "
                  << "specify it with --rdma_device.";
    }
    // Initialize device map
    g_rdma_device_map = new butil::FlatMap<std::string, RdmaDevice*>();
    if (g_rdma_device_map->init(64) < 0) {
        LOG(ERROR) << "Fail to initialize rdma device map";
        ExitWithError();
    }

    // Initialize user MR maps and lock
    g_user_mr_maps_lock = new butil::Mutex;

    // Initialize all devices: assign pd_index, allocate PD, query attributes, user MR maps
    for (size_t i = 0; i < g_rdma_devices.size(); ++i) {
        RdmaDevice* dev = g_rdma_devices[i];
        dev->pd_index = i;
        dev->pd = IbvAllocPd(dev->context);
        if (!dev->pd) {
            PLOG(ERROR) << "Fail to allocate PD for device " << dev->name;
            ExitWithError();
        }

        ibv_device_attr dev_attr;
        if (IbvQueryDevice(dev->context, &dev_attr) != 0) {
            PLOG(ERROR) << "Fail to query device " << dev->name;
            ExitWithError();
        }
        if (FLAGS_rdma_max_sge > 0) {
            dev->max_sge = dev_attr.max_sge < FLAGS_rdma_max_sge ?
                           dev_attr.max_sge : FLAGS_rdma_max_sge;
        } else {
            dev->max_sge = dev_attr.max_sge;
        }

        (*g_rdma_device_map)[dev->name] = dev;

        // Initialize per-device user MR map
        g_user_mr_maps[i] = new butil::FlatMap<void*, ibv_mr*>();
        if (g_user_mr_maps[i]->init(65536) < 0) {
            PLOG(WARNING) << "Fail to initialize g_user_mrs for device "
                          << dev->name;
            ExitWithError();
        }

        LOG(INFO) << "RDMA device[" << i << "]" << (i == 0 ? "(default): " : ": ") << *dev;
    }

    g_mrs = new std::vector<ibv_mr*>;

    // Initialize RDMA memory pool (block_pool)
    butil::SetDefaultBlockSize(GetRdmaBlockSize());
    if (!InitBlockPool(RdmaRegisterMemory)) {
        PLOG(ERROR) << "Fail to initialize RDMA memory pool";
        ExitWithError();
    }

    if (RdmaEndpoint::GlobalInitialize() < 0) {
        LOG(ERROR) << "rdma_recv_block_type incorrect "
                   << "(valid value: default/large/huge)";
        ExitWithError();
    }

    atexit(GlobalRelease);

    // Create async event sockets for all devices
    for (auto dev : g_rdma_devices) {
        SocketOptions sopt;
        sopt.fd = dev->context->async_fd;
        butil::make_close_on_exec(sopt.fd);
        if (butil::make_non_blocking(sopt.fd) < 0) {
            PLOG(WARNING) << "Fail to set async_fd to nonblocking for device "
                          << dev->name;
            ExitWithError();
        }
        sopt.on_edge_triggered_events = OnRdmaAsyncEvent;
        SocketId async_sid;
        if (Socket::Create(sopt, &async_sid) < 0) {
            LOG(WARNING) << "Fail to create socket for async event of device "
                         << dev->name;
            ExitWithError();
        }
        g_async_sockets.push_back(async_sid);
    }

    g_mem_alloc = butil::iobuf::blockmem_allocate;
    g_mem_dealloc = butil::iobuf::blockmem_deallocate;
    butil::iobuf::blockmem_allocate = BlockAllocate;
    butil::iobuf::blockmem_deallocate = BlockDeallocate;

    g_rdma_available.store(true, butil::memory_order_relaxed);
}

static pthread_once_t initialize_rdma_once = PTHREAD_ONCE_INIT;

void GlobalRdmaInitializeOrDie() {
    if (pthread_once(&initialize_rdma_once, GlobalRdmaInitializeOrDieImpl) != 0) {
        LOG(FATAL) << "Fail to pthread_once GlobalRdmaInitializeOrDie";
        exit(1);
    }
}

RegisterResult RegisterMemoryForAllRdmaDevices(void* buf, size_t len) {
    auto flags = static_cast<ibv_access_flags>(
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
    bool success = false;
    ibv_mr* all_mrs[RDMA_MAX_DEVICES] = {};
    size_t inserted_count = 0;  // how many entries inserted into g_user_mrs

    // On scope exit, rollback if not successful
    BRPC_SCOPE_EXIT {
        if (success) {
            return;
        }
        // Erase already-inserted map entries under lock
        if (inserted_count > 0) {
            BAIDU_SCOPED_LOCK(*g_user_mr_maps_lock);
            for (size_t i = 0; i < inserted_count; ++i) {
                uint8_t pd_index = g_rdma_devices[i]->pd_index;
                if (g_user_mr_maps[pd_index]) {
                    g_user_mr_maps[pd_index]->erase(buf);
                }
            }
        }
        // Deregister all registered MRs
        for (auto dev : g_rdma_devices) {
            uint8_t pd_index = dev->pd_index;
            if (all_mrs[pd_index]) {
                IbvDeregMr(all_mrs[pd_index]);
            }
        }
    };

    // Register on all devices (outside lock — IbvRegMr is a slow kernel call)
    for (auto dev : g_rdma_devices) {
        ibv_mr* mr = IbvRegMr(dev->pd, buf, len, flags);
        if (!mr) {
            PLOG(ERROR) << "Fail to register user memory on device " << dev->name;
            return RegisterResult(-1);
        }
        all_mrs[dev->pd_index] = mr;
    }

    // Insert into per-device MR maps under lock
    {
        BAIDU_SCOPED_LOCK(*g_user_mr_maps_lock);
        for (auto dev : g_rdma_devices) {
            uint8_t pd_index = dev->pd_index;
            if (!g_user_mr_maps[pd_index]->insert(buf, all_mrs[pd_index])) {
                LOG(WARNING) << "Fail to insert to user mr map for device "
                             << dev->name << " (now there are "
                             << g_user_mr_maps[pd_index]->size() << " mrs already)";
                return RegisterResult(-1);
            }
            ++inserted_count;
        }
    }

    // Fill per-device lkey/rkey arrays. Index 0 is the default device.
    RegisterResult result(0);
    for (auto dev : g_rdma_devices) {
        uint8_t pd_index = dev->pd_index;
        result.all_lkeys[pd_index] = all_mrs[pd_index]->lkey;
        result.all_rkeys[pd_index] = all_mrs[pd_index]->rkey;
    }

    LOG(INFO) << "Register memory addr=" << buf << " len=" << len
              << " lkey=" << result.all_lkeys[0]
              << " rkey=" << result.all_rkeys[0]
              << " on " << g_rdma_devices.size() << " device(s)";

    success = true;
    return result;
}

uint32_t RegisterMemoryForRdma(void* buf, size_t len) {
    RegisterResult result = RegisterMemoryForAllRdmaDevices(buf, len);
    return 0 != result.rc ? 0 : result.all_lkeys[0];
}

void DeregisterMemoryForRdma(void* buf) {
    BAIDU_SCOPED_LOCK(*g_user_mr_maps_lock);
    for (auto dev : g_rdma_devices) {
        uint8_t pd_index = dev->pd_index;
        if (!g_user_mr_maps[pd_index]) {
            continue;
        }
        ibv_mr* mr = NULL;
        size_t removed = g_user_mr_maps[pd_index]->erase(buf, &mr);
        if (removed == 0) {
            LOG(WARNING) << "Try to deregister a buffer which "
                            "is not registered on device " << dev->name;
            continue;
        }
        if (IbvDeregMr(mr)) {
            PLOG(ERROR) << "Failed to deregister memory at: " << mr->addr
                        << " on device " << dev->name;
        }
    }
}

// Search a FlatMap for an MR whose registered range covers `buf`.
// This handles the case where `buf` is an offset within a larger
// registered region (e.g. a BuddyChunk sub-block).
static ibv_mr* FindMrByRange(butil::FlatMap<void*, ibv_mr*>* mrs, void* buf) {
    if (!mrs) return NULL;
    // 1. Try exact match first (fast path)
    ibv_mr** mr_ptr = mrs->seek(buf);
    if (mr_ptr) return *mr_ptr;
    // 2. Fall back to range search (few entries expected)
    uintptr_t addr = (uintptr_t)buf;
    for (auto it = mrs->begin(); it != mrs->end(); ++it) {
        ibv_mr* mr = it->second;
        uintptr_t mr_start = (uintptr_t)mr->addr;
        if (addr >= mr_start && addr < mr_start + mr->length) {
            return mr;
        }
    }
    return NULL;
}

uint32_t GetLKey(void* buf, uint8_t pd_index) {
    BAIDU_SCOPED_LOCK(*g_user_mr_maps_lock);
    if (pd_index <= 0 || pd_index >= RDMA_MAX_DEVICES) {
        // Default device
        pd_index = 0;
    }
    // Non-default device
    ibv_mr* mr = FindMrByRange(g_user_mr_maps[pd_index], buf);
    return mr ? mr->lkey : 0;
}

bool IsRdmaAvailable() {
    return g_rdma_available.load(butil::memory_order_acquire);
}

void GlobalDisableRdma() {
    if (g_rdma_available.exchange(false, butil::memory_order_acquire)) {
        LOG(FATAL) << "RDMA is disabled due to some unrecoverable problem";
    }
}

bool SupportedByRdma(std::string protocol) {
    if (protocol.compare("baidu_std") == 0) {
        // Since rdma is used for high performance scenario,
        // we consider baidu_std for the only protocol to support.
        return true;
    }
    return false;
}

bool InitPollingModeWithTag(bthread_tag_t tag,
                            std::function<void(void)> callback,
                            std::function<void(void)> init_fn,
                            std::function<void(void)> release_fn) {
    if (RdmaEndpoint::PollingModeInitialize(tag, callback, init_fn,
                                            release_fn) == 0) {
        return true;
    }
    return false;
}

void ReleasePollingModeWithTag(bthread_tag_t tag) {
    RdmaEndpoint::PollingModeRelease(tag);
}

size_t GetRdmaDeviceCount() {
    return (int)g_rdma_devices.size();
}

const std::vector<std::string>& GetRdmaDeviceNames() {
    return g_rdma_device_names;
}

RdmaDevice const* GetRdmaDevice(const std::string& device_name) {
    if (device_name.empty()) {
        return GetDefaultRdmaDevice();
    }
    if (!g_rdma_device_map) {
        return NULL;
    }
    RdmaDevice** dev = g_rdma_device_map->seek(device_name);
    if (dev) {
        return *dev;
    }
    return NULL;
}

RdmaDevice const* GetRdmaDeviceByIndex(int index) {
    if (index < 0 || (size_t)index >= g_rdma_devices.size()) {
        return NULL;
    }
    return g_rdma_devices[index];
}

RdmaDevice const* GetDefaultRdmaDevice() {
    if (g_rdma_devices.empty()) {
        return NULL;
    }
    return g_rdma_devices[0];
}

}  // namespace rdma
}  // namespace brpc

#else

#include <stdlib.h>
#include "butil/logging.h"

namespace brpc {
namespace rdma {
void GlobalRdmaInitializeOrDie() {
    LOG(ERROR) << "brpc is not compiled with rdma. To enable it, please refer to "
               << "https://github.com/apache/brpc/blob/master/docs/en/rdma.md";
    exit(1);
}
}
}

#endif  // if BRPC_WITH_RDMA
