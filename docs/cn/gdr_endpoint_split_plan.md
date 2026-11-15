# RdmaEndpoint 字段彻底按 host / device 拆分 —— 改动计划

## 目标

上一轮只把「流控计数器」抽成了 `QpFlowControl`（`_host_fc` / `_device_fc`），
其余 host 字段和 device 字段仍然平铺在 `RdmaEndpoint` 里，靠 `_device_` 前缀
区分。这一轮彻底做完：**一条通道的全部状态放进一个结构体**，
`RdmaEndpoint` 只留两端共用的字段。

判断标准很简单：一个字段如果只有 host 路径读写，它属于 host 通道；只有
device 路径读写，属于 device 通道；两边都要用（或与通道无关，比如握手、
poller）才留在 `RdmaEndpoint`。

## 目标形态

```cpp
// 原 QpFlowControl 改名：它现在不只是 flow control，而是「一条 QP 通道」的基类。
// 内容不变——17 个计数器 + is_device + name() / IsWindowOpen() / ResetCounters()。
struct QpChannel { ... };

// host 通道：每条连接都有，承载所有消息。
struct HostChannel : public QpChannel {
    RdmaResource* resource;
    InputMessengerProcessor input_processor;
    std::vector<butil::IOBuf> sbuf;
    std::vector<butil::IOBuf> rbuf;
    std::vector<void*> rbuf_data;
    uint16_t unsolicited;
    uint32_t unsolicited_bytes;
    ibv_qp* qp() const;   // resource ? resource->qp : nullptr
};

// device 通道：协商成功才有。
struct DeviceChannel : public QpChannel {
    RdmaDeviceResource* resource;
    uint32_t recv_block_size;
    bool pending_over_watermark;
    bool channel_mode;        // 配置：socket_mode == SOCKET_MODE_RDMA_AND_DEVICE
    bool wanted;              // 协商中：本端愿意开
    bool established;         // 协商结果：两端都开且 device QP 到 RTS
    std::vector<DeviceAttachment> sbuf;
    std::vector<void*> rbuf_data;
    std::vector<uint32_t> rbuf_lkey;
    DeviceStream stream;
    ibv_qp* qp() const;
};

class RdmaEndpoint {
    ...
    HostChannel   _host;
    DeviceChannel _device;
    // 共用字段，仅此六个（外加 poller 静态成员）
    Socket* _socket;
    butil::atomic<State> _state;
    int _handshake_version;
    butil::optional<ibv_ece> _outgoing_ece;
    SocketId _cq_sid;
    butil::atomic<int>* _read_butex;
};
```

`is_device` 由两个派生结构体各自的构造函数写死（现在是 `RdmaEndpoint`
构造函数里手工给 `_device_fc.is_device = true`），少一处可以写错的地方。

**不加虚函数**。`qp()` 在两个派生结构体里各写一份非虚成员函数，因为
`RdmaResource` 与 `RdmaDeviceResource` 是两个类型（前者池化预分配、带
`next`，后者按连接分配）。共用辅助函数的签名仍然是
`(QpChannel& ch, ibv_qp* qp, ...)`：QP 由调用方从对应通道取出传入，这样
单测里没有真实 QP 的场景仍然能传 nullptr。

## 改名对照表

host（`_host_fc.X` → `_host.X`，以下是其余字段）：

| 现在 | 改为 |
| --- | --- |
| `_resource` | `_host.resource` |
| `_input_processor` | `_host.input_processor` |
| `_sbuf` | `_host.sbuf` |
| `_rbuf` | `_host.rbuf` |
| `_rbuf_data` | `_host.rbuf_data` |
| `_unsolicited` | `_host.unsolicited` |
| `_unsolicited_bytes` | `_host.unsolicited_bytes` |

device（`_device_fc.X` → `_device.X`，以下是其余字段）：

| 现在 | 改为 |
| --- | --- |
| `_device_resource` | `_device.resource` |
| `_device_recv_block_size` | `_device.recv_block_size` |
| `_device_pending_over_watermark` | `_device.pending_over_watermark` |
| `_device_channel_mode` | `_device.channel_mode` |
| `_want_device_channel` | `_device.wanted` |
| `_device_channel_on` | `_device.established` |
| `_device_sbuf` | `_device.sbuf` |
| `_device_rbuf_data` | `_device.rbuf_data` |
| `_device_rbuf_lkey` | `_device.rbuf_lkey` |
| `_device_stream` | `_device.stream` |

类型改名：`QpFlowControl` → `QpChannel`（含 `SendImm` / `TakeAcks` /
`SendAck` / `ReturnSendCredits` 的形参类型，以及注释里提到
`QpFlowControl::name()` 的地方）。

访问器不变（对外行为不变）：`host_qp()` / `device_qp()` 转发到
`_host.qp()` / `_device.qp()`，`has_device_channel()` 读
`_device.established`，`device_stream()` 返回 `&_device.stream`。

## 涉及文件

- `src/brpc/rdma/rdma_endpoint.h` —— 结构体定义、成员、辅助函数签名、注释。
- `src/brpc/rdma/rdma_endpoint.cpp` —— 约 250 处调用点（含构造函数初始化
  列表、`Reset()`、`DeallocateResources()`、`DebugInfo()`）。
- `src/brpc/rdma/rdma_handshake.cpp` —— 8 处（v2/v3 填 hello 时读
  `sq_size`/`rq_size`/`qp_num`/`block_size`）。
- `test/brpc_rdma_unittest.cpp` —— 约 17 处。
- `docs/cn/gdr_design.md` —— §7.2 / §9 / §9.2 / §9.3 里出现的字段名。

**不要动 `src/brpc/urma/urma_endpoint.{h,cpp}`**：那是另一个类，恰好有同名
的 `_resource` / `_sbuf` / `_rbuf` / `_rbuf_data` / `_input_processor` 成员
（`_resource` 在它那里就有 57 处）。全局替换必然误伤，只能按文件改。

## 纯重构，不改行为

这一轮不改任何逻辑：没有新增/删除字段，没有改流控规则、握手流程、
`IsWritable()` 的判定。因此不新增单测，用现有测试矩阵验证等价性：

- 无硬件 `--config=gdr`：165/165
- 无硬件 `--config=rdma`（GDR 编译掉）：159/159
- 真卡 device 落地：159/159（带三个既有硬件 bug 的排除过滤器）
- 真卡 §13.2 内存种类矩阵：两半各 1/1
- `brpc_socket_unittest` / `brpc_channel_unittest` / `brpc_server_unittest`
